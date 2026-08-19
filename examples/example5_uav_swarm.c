// Fleece Example 5: UAV Swarm + Web Dashboard.
//
// Pure Fleece + GOAP dogfooding: three UAVs (each its own runtime + brain)
// patrol a GPS world, scan a zone for targets, hunt them, return to base and
// recharge - decided entirely by GOAP. No named "locations": the brain thinks
// in continuous GPS coordinates (bb.self.x/.y/.home) and distances
// (Math.hypot). The only non-GOAP code is the *hardware*: the autopilot and
// sensor simulation behind the "platform" registry, exactly as you'd write it
// for real avionics.
//
// The loop per UAV is the same as example 4: plan -> select first action ->
// run its exec() body each tick (committing so the world sees progress) ->
// replan when done. What's new:
//   - exec bodies call real "hardware" verbs (platform.uav_waypoint,
//     platform.uav_scan, ...) and wait on them, so the brain is embodied.
//   - goals are GPS-utility driven: "forage" (deliver a target) vs
//     "recharge" (battery topped up); the battery drains continuously in C
//     (the "hardware"), so utility shifts and the swarm degrades gracefully.
//   - if the top goal is currently unplannable (e.g. battery too low to scan),
//     the brain falls back to the next-best goal instead of idling.
//
// A web dashboard (examples/webui) visualizes the swarm: open
//     http://127.0.0.1:8080
// GET /state returns the telemetry JSON, POST /cmd controls it.
//
// Usage:
//   ./example5_uav_swarm [port] [webroot]
//   (defaults: port 8080, webroot "examples/webui" - run from the repo root)

#define _DEFAULT_SOURCE  // M_PI etc. (POSIX_C_SOURCE suppresses them)
#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "runtime/fleece_runtime.h"
#include "comms/fleece_comms.h"
#include "state/fleece_state_manager.h"
#include "planner/fleece_planner.h"
#include "embedded/fleece_goap_js.h"
#include "platform/fleece_platform.h"
#include "http_server.h"

#define UAV_COUNT 3
#define TARGET_COUNT 3

// GPS world (world units, [0,640)x[0,480) - arbitrary units of "meters").
#define WORLD_W 640.0
#define WORLD_H 480.0
#define HOME_X 60.0     // base pad GPS
#define HOME_Y 420.0
#define ZONE_X 500.0    // patrol zone centre
#define ZONE_Y 120.0
#define ZONE_R 90.0

#define MAX_SPEED 5.0       // world units per tick at speed 1.0
#define BATTERY_DRAIN 0.25  // passive drain per airborne tick
#define BATTERY_MAX 100.0
#define BATTERY_WARNING 30.0   // below this the brain heads home (return pre)
#define RECHARGE_GOAL 90.0
#define SENSOR_RANGE 140.0
#define PICKUP_RANGE 30.0
#define ARRIVE_DIST 14.0

// ---------------------------------------------------------------------------
// Shared simulation state (the "hardware" backing the platform functions).
// UAV threads own their uav[i]; the httpd thread reads everything under lock.
// ---------------------------------------------------------------------------

typedef struct {
    double x, y;        // GPS position
    double heading;     // radians, current course
    double battery;
    double wp_x, wp_y;  // autopilot waypoint
    int wp_set;
    int cargo;          // carrying a target
    int airborne;
} UavPhysics;

typedef struct {
    double x, y;
    char id[8];  // "t0".."tN"
    int alive;
} Target;

typedef struct {
    pthread_mutex_t lock;
    volatile int paused;
    volatile int step_once;
    volatile int reset;
    double speed;            // physics multiplier (0.2..4.0)
    uint64_t tick;
    int world_delivered;
    volatile int stopping;

    UavPhysics uavs[UAV_COUNT];
    char uav_goal[UAV_COUNT][24];
    char uav_action[UAV_COUNT][24];
    int uav_progress[UAV_COUNT];
    int uav_cargo[UAV_COUNT];
    int uav_detect[UAV_COUNT];
    // Mesh bandwidth per unit (mirrored from the uav threads under lock).
    uint64_t uav_rx_bytes[UAV_COUNT];
    uint64_t uav_tx_bytes[UAV_COUNT];
    uint64_t uav_rx_frames[UAV_COUNT];
    uint64_t uav_tx_frames[UAV_COUNT];
    double uav_rx_rate[UAV_COUNT];  // bytes/sec over the last sample window
    double uav_tx_rate[UAV_COUNT];
    Target targets[TARGET_COUNT];
} Sim;

// In-process mesh: each runtime's comms broadcasts gossip frames into the
// OTHER runtimes' mailboxes (mimicking a radio: async, lossy-ish, ring buffer
// bound). Each runtime drains its own mailbox on its own thread in Phase 1
// (process_input -> poll callback) so state-manager access stays on the owner
// thread - the same design as example2's UDP multicast, minus the sockets.
#define MESH_MAILBOX_DEPTH 32

typedef struct {
    uint8_t* frame;
    uint32_t size;
} MeshSlot;

typedef struct {
    MeshSlot slots[MESH_MAILBOX_DEPTH];
    int head;
    int count;
    pthread_mutex_t lock;
} MeshMailbox;

typedef struct UavCtx {
    Sim* sim;
    int index;
    FleeceRuntime* runtime;
    uint64_t self_id;
    FleeceStateManager* sm;
    MeshMailbox mbox;  // frames queued by other UAVs' send callbacks
    // Bandwidth accounting (written only by this uav's own thread).
    uint64_t rx_bytes, tx_bytes;
    uint64_t rx_frames, tx_frames;
    uint64_t bw_last_rx, bw_last_tx;  // sample for rate computation
    double bw_last_time;
} UavCtx;

static Sim g_sim;
static FleeceHttpServer* g_srv;
static FleeceRuntime* g_runtimes[UAV_COUNT];
static UavCtx g_ctx[UAV_COUNT];
static UavCtx* g_mesh_nodes[UAV_COUNT];  // populated during init (single-threaded)

// ---------------------------------------------------------------------------
// State manager helpers (JSON bytes in, doubles out - same as example 4).
// ---------------------------------------------------------------------------

static bool env_get_double(FleeceStateManager* sm, uint64_t owner, const char* name, double* out) {
    uint8_t* data = NULL;
    uint32_t size = 0;
    if (fleece_state_manager_get_named(sm, owner, name, &data, &size) != 0 || !data) {
        free(data);
        return false;
    }
    char buf[64];
    if (size >= sizeof(buf)) { free(data); return false; }
    memcpy(buf, data, size);
    buf[size] = '\0';
    free(data);
    char* end = NULL;
    *out = strtod(buf, &end);
    return end != buf && *end == '\0';
}

static void env_set_double(FleeceStateManager* sm, const char* name, double v) {
    char buf[48];
    int n = snprintf(buf, sizeof(buf), "%.17g", v);
    fleece_state_manager_set_named(sm, name, (const uint8_t*)buf, (uint32_t)n);
}

// ---------------------------------------------------------------------------
// Platform functions - the UAV's "hardware", one set per UAV bound to its
// UavCtx. These are the only C verb the JS brain is allowed to touch.
// ---------------------------------------------------------------------------

static int platform_uav_waypoint(const uint8_t* args, uint32_t args_size, uint8_t** result, uint32_t* result_size, void* ud) {
    UavCtx* ctx = (UavCtx*)ud;
    Sim* sim = ctx->sim;
    UavPhysics* uav = &sim->uavs[ctx->index];

    if (!args || args_size == 0 || args_size >= 128) return -1;
    char buf[128];
    memcpy(buf, args, args_size);
    buf[args_size] = '\0';

    double x, y;
    if (sscanf(buf, "[%lf,%lf]", &x, &y) != 2) return -1;

    pthread_mutex_lock(&sim->lock);
    uav->wp_x = x;
    uav->wp_y = y;
    uav->wp_set = 1;
    uav->airborne = 1;
    pthread_mutex_unlock(&sim->lock);

    char out[128];
    int n = snprintf(out, sizeof(out), "{\"x\":%.4f,\"y\":%.4f}", x, y);
    *result = (uint8_t*)malloc((size_t)n);
    if (!*result) return -1;
    memcpy(*result, out, (size_t)n);
    *result_size = (uint32_t)n;
    return 0;
}

static int platform_uav_arrived(const uint8_t* args, uint32_t args_size, uint8_t** result, uint32_t* result_size, void* ud) {
    (void)args; (void)args_size;
    UavCtx* ctx = (UavCtx*)ud;
    Sim* sim = ctx->sim;
    UavPhysics* uav = &sim->uavs[ctx->index];

    pthread_mutex_lock(&sim->lock);
    bool arrived = !uav->wp_set ||
        (sqrt((uav->x - uav->wp_x) * (uav->x - uav->wp_x) + (uav->y - uav->wp_y) * (uav->y - uav->wp_y)) <= ARRIVE_DIST);
    pthread_mutex_unlock(&sim->lock);

    char out[32];
    int n = snprintf(out, sizeof(out), "{\"arrived\":%s}", arrived ? "true" : "false");
    *result = (uint8_t*)malloc((size_t)n);
    if (!*result) return -1;
    memcpy(*result, out, (size_t)n);
    *result_size = (uint32_t)n;
    return 0;
}

// platform.uav_scan() -> {detect, targets:[{id,x,y}...]} - raw sensor readout:
// every target this UAV's sensor can see right now. NO coordination logic -
// deciding which one to go for is the brain's job (it has bb.world claims).
static int platform_uav_scan(const uint8_t* args, uint32_t args_size, uint8_t** result, uint32_t* result_size, void* ud) {
    (void)args; (void)args_size;
    UavCtx* ctx = (UavCtx*)ud;
    Sim* sim = ctx->sim;
    UavPhysics* uav = &sim->uavs[ctx->index];

    char out[512];
    int n = snprintf(out, sizeof(out), "{\"detect\":false,\"targets\":[]}");
    pthread_mutex_lock(&sim->lock);
    int first = 1;
    int written = 0;
    n = snprintf(out, sizeof(out), "{\"detect\":");
    for (int i = 0; i < TARGET_COUNT; i++) {
        Target* t = &sim->targets[i];
        if (!t->alive) continue;
        double d = sqrt((t->x - uav->x) * (t->x - uav->x) + (t->y - uav->y) * (t->y - uav->y));
        if (d > SENSOR_RANGE) continue;
        if (written == 0) {
            written = snprintf(out + n, sizeof(out) - (size_t)n, "true,\"targets\":[");
            n += written;
        }
        written = snprintf(out + n, sizeof(out) - (size_t)n, "%s{\"id\":\"%s\",\"x\":%.4f,\"y\":%.4f}",
                           first ? "" : ",", t->id, t->x, t->y);
        first = 0;
        n += written;
    }
    if (written == 0) {
        n = snprintf(out, sizeof(out), "{\"detect\":false,\"targets\":[]}");
    } else {
        n += snprintf(out + n, sizeof(out) - (size_t)n, "]}");
    }
    pthread_mutex_unlock(&sim->lock);

    *result = (uint8_t*)malloc((size_t)n);
    if (!*result) return -1;
    memcpy(*result, out, (size_t)n);
    *result_size = (uint32_t)n;
    return 0;
}

// platform.uav_pickup(targetId) -> {picked} - grab that specific target if the
// autopilot has flown us within pickup range of it. Pure actuator.
static int platform_uav_pickup(const uint8_t* args, uint32_t args_size, uint8_t** result, uint32_t* result_size, void* ud) {
    UavCtx* ctx = (UavCtx*)ud;
    Sim* sim = ctx->sim;
    UavPhysics* uav = &sim->uavs[ctx->index];

    if (!args || args_size == 0 || args_size >= 64) return -1;
    char buf[64];
    memcpy(buf, args, args_size);
    buf[args_size] = '\0';
    char tid[8];
    if (sscanf(buf, "[\"%7[^\"]\"]", tid) != 1) return -1;

    bool picked = false;
    pthread_mutex_lock(&sim->lock);
    for (int i = 0; i < TARGET_COUNT; i++) {
        Target* t = &sim->targets[i];
        if (strcmp(t->id, tid) != 0) continue;
        double d = sqrt((t->x - uav->x) * (t->x - uav->x) + (t->y - uav->y) * (t->y - uav->y));
        if (t->alive && d <= PICKUP_RANGE) {
            t->alive = 0;
            uav->cargo = 1;
            picked = true;
            printf("[uav-%d] PICKED UP target %s\n", ctx->index + 1, tid);
        }
        break;
    }
    pthread_mutex_unlock(&sim->lock);

    char out[32];
    int n = snprintf(out, sizeof(out), "{\"picked\":%s}", picked ? "true" : "false");
    *result = (uint8_t*)malloc((size_t)n);
    if (!*result) return -1;
    memcpy(*result, out, (size_t)n);
    *result_size = (uint32_t)n;
    return 0;
}

// platform.uav_deliver() -> {delivered} - drop a target at the base pad.
static int platform_uav_deliver(const uint8_t* args, uint32_t args_size, uint8_t** result, uint32_t* result_size, void* ud) {
    (void)args; (void)args_size;
    UavCtx* ctx = (UavCtx*)ud;
    Sim* sim = ctx->sim;
    UavPhysics* uav = &sim->uavs[ctx->index];

    int delivered = 0;
    pthread_mutex_lock(&sim->lock);
    double d = sqrt((uav->x - HOME_X) * (uav->x - HOME_X) + (uav->y - HOME_Y) * (uav->y - HOME_Y));
    if (d <= ARRIVE_DIST && uav->cargo >= 1) {
        uav->cargo = 0;
        delivered = ++sim->world_delivered;
        printf("[uav-%d] DELIVERED (%d total)\n", ctx->index + 1, delivered);
    } else {
        delivered = sim->world_delivered;
    }
    pthread_mutex_unlock(&sim->lock);

    char out[48];
    int n = snprintf(out, sizeof(out), "{\"delivered\":%d}", delivered);
    *result = (uint8_t*)malloc((size_t)n);
    if (!*result) return -1;
    memcpy(*result, out, (size_t)n);
    *result_size = (uint32_t)n;
    return 0;
}

// ---------------------------------------------------------------------------
// In-process mesh transport (see example2 for the same shape over UDP).
// comms_send -> enqueue into every peer's mailbox (copy, so the sender's frame
// buffer is freed immediately). poll callback -> drain own mailbox and import
// straight into the state manager, exactly like the UDP recvfrom loop.
// ---------------------------------------------------------------------------

// comms send callback: invoked on the *sender's* thread during gossip (Phase 3).
static void mesh_on_send(const char* destination, const uint8_t* data, uint32_t size, void* ud) {
    (void)destination;
    UavCtx* self = (UavCtx*)ud;
    if (!data || size == 0) return;
    self->tx_bytes += size;   // the unit's radio transmits this gossip frame once
    self->tx_frames++;
    for (int i = 0; i < UAV_COUNT; i++) {
        UavCtx* peer = g_mesh_nodes[i];
        if (!peer || peer == self) continue;
        MeshMailbox* m = &peer->mbox;
        pthread_mutex_lock(&m->lock);
        if (m->count < MESH_MAILBOX_DEPTH) {
            int pos = (m->head + m->count) % MESH_MAILBOX_DEPTH;
            uint8_t* copy = (uint8_t*)malloc(size);
            if (copy) {
                memcpy(copy, data, size);
                m->slots[pos].frame = copy;
                m->slots[pos].size = size;
                m->count++;
            }
        }  // else: mailbox full - drop the frame (that's what radios do)
        pthread_mutex_unlock(&m->lock);
    }
}

// comms poll callback: invoked on the *receiver's* thread in Phase 1.
static void mesh_on_poll(void* ud) {
    UavCtx* self = (UavCtx*)ud;
    MeshMailbox* m = &self->mbox;
    for (;;) {
        uint8_t* frame = NULL;
        uint32_t size = 0;
        pthread_mutex_lock(&m->lock);
        if (m->count > 0) {
            int pos = m->head;
            frame = m->slots[pos].frame;
            size = m->slots[pos].size;
            m->head = (m->head + 1) % MESH_MAILBOX_DEPTH;
            m->count--;
        }
        pthread_mutex_unlock(&m->lock);
        if (!frame) break;
        fleece_state_manager_import(self->sm, frame, size);  // merges peer self + world streams (LWW)
        self->rx_bytes += size;
        self->rx_frames++;
        free(frame);
    }
}

// ---------------------------------------------------------------------------
// Env tick: the autopilot (physics) + sensors + battery, run by each UAV's
// own runtime before its brain sees the world. Also mirrors state the
// dashboard wants to read from another thread.
// ---------------------------------------------------------------------------

static void respawn_targets(Sim* sim) {
    int alive = 0;
    for (int i = 0; i < TARGET_COUNT; i++) if (sim->targets[i].alive) alive++;
    for (int i = 0; i < TARGET_COUNT && alive < TARGET_COUNT; i++) {
        if (sim->targets[i].alive) continue;
        // Random point inside the patrol zone (keep off the very edge).
        double r = (double)(rand() % 10000) / 10000.0 * (ZONE_R - 10.0);
        double a = (double)(rand() % 10000) / 10000.0 * 2.0 * M_PI;
        sim->targets[i].x = ZONE_X + r * cos(a);
        sim->targets[i].y = ZONE_Y + r * sin(a);
        snprintf(sim->targets[i].id, sizeof(sim->targets[i].id), "t%d", i);
        sim->targets[i].alive = 1;
        alive++;
    }
}

static void env_tick(FleeceRuntime* runtime, void* ud) {
    UavCtx* ctx = (UavCtx*)ud;
    Sim* sim = ctx->sim;
    UavPhysics* uav = &sim->uavs[ctx->index];
    FleeceStateManager* sm = ctx->sm;

    double bat = BATTERY_MAX;
    env_get_double(sm, ctx->self_id, "battery", &bat);

    pthread_mutex_lock(&sim->lock);
    uav->battery = bat;

    if (sim->reset) {
        uav->x = HOME_X; uav->y = HOME_Y;
        uav->wp_set = 0;
        uav->cargo = 0;
        uav->battery = BATTERY_MAX;
        sim->world_delivered = 0;
        sim->reset = 0;
    }

    // Autopilot: advance at most max_speed * speed toward the commanded waypoint.
    if (!sim->paused || sim->step_once) {
        sim->step_once = 0;
        if (uav->wp_set) {
            double dx = uav->wp_x - uav->x;
            double dy = uav->wp_y - uav->y;
            double d = sqrt(dx * dx + dy * dy);
            uav->heading = atan2(dy, dx);
            double step = MAX_SPEED * sim->speed;
            if (d <= step || d < 1e-9) {
                uav->x = uav->wp_x;
                uav->y = uav->wp_y;
                uav->wp_set = 0;
            } else {
                uav->x += dx / d * step;
                uav->y += dy / d * step;
            }
            uav->airborne = 1;
        }
        uav->battery -= BATTERY_DRAIN;
        if (uav->battery < 0.0) uav->battery = 0.0;
    }

    respawn_targets(sim);
    sim->tick++;
    pthread_mutex_unlock(&sim->lock);

    // Expose GPS + battery to the brain (bb.self.x/.y/.battery) and the world.
    env_set_double(sm, "battery", uav->battery);
    env_set_double(sm, "x", uav->x);
    env_set_double(sm, "y", uav->y);

    // Mirror the brain's current decision for the dashboard (read-only copy
    // so the httpd thread never touches live brain/state-manager memory).
    FleeceGoapBrain* brain = (FleeceGoapBrain*)fleece_runtime_get_goap_brain(runtime);
    pthread_mutex_lock(&sim->lock);
    const char* g = brain ? fleece_goap_brain_goal_id(brain) : NULL;
    const char* a = brain ? fleece_goap_brain_action_id(brain) : NULL;
    snprintf(sim->uav_goal[ctx->index], 24, "%s", g ? g : "idle");
    snprintf(sim->uav_action[ctx->index], 24, "%s", a ? a : "idle");
    sim->uav_progress[ctx->index] = brain ? (int)fleece_goap_brain_action_progress(brain) : 0;
    double cargo = 0, detect = 0;
    env_get_double(sm, ctx->self_id, "cargo", &cargo);
    env_get_double(sm, ctx->self_id, "detect", &detect);
    sim->uav_cargo[ctx->index] = cargo >= 1.0 ? 1 : 0;
    sim->uav_detect[ctx->index] = detect >= 1.0 ? 1 : 0;
    // Bandwidth telemetry: mirror counters and refresh the per-unit rate every
    // ~2s of wall clock (the mesh may deliver faster than the 100ms loop tick).
    sim->uav_rx_bytes[ctx->index] = ctx->rx_bytes;
    sim->uav_tx_bytes[ctx->index] = ctx->tx_bytes;
    sim->uav_rx_frames[ctx->index] = ctx->rx_frames;
    sim->uav_tx_frames[ctx->index] = ctx->tx_frames;
    struct timespec bw_now_ts;
    clock_gettime(CLOCK_MONOTONIC, &bw_now_ts);
    double bw_now = (double)bw_now_ts.tv_sec + (double)bw_now_ts.tv_nsec / 1e9;
    if (ctx->bw_last_time > 0.0) {
        double dt = bw_now - ctx->bw_last_time;
        if (dt >= 2.0) {
            sim->uav_rx_rate[ctx->index] = (double)(ctx->rx_bytes - ctx->bw_last_rx) / dt;
            sim->uav_tx_rate[ctx->index] = (double)(ctx->tx_bytes - ctx->bw_last_tx) / dt;
            ctx->bw_last_rx = ctx->rx_bytes;
            ctx->bw_last_tx = ctx->tx_bytes;
            ctx->bw_last_time = bw_now;
        }
    } else {
        ctx->bw_last_rx = ctx->rx_bytes;
        ctx->bw_last_tx = ctx->tx_bytes;
        ctx->bw_last_time = bw_now;
    }
    pthread_mutex_unlock(&sim->lock);
}

// ---------------------------------------------------------------------------
// Live telemetry injected into the brain's bb every tick, on both the exec and
// plan/select paths (fleece_goap_brain_set_world_model). This is the seam a
// real PX4/MAVLink adapter would use: a telemetry thread fills a ring buffer
// and this hook copies the latest sample into bb.self. distHome here replaces
// the old env_tick mirror - it gives the planner a model to reason over
// ("return" sets it to 0, "charge" requires it small).
static void brain_world_model(FleeceGoapBrain* brain, FleeceGoapBlackboard* bb, uint32_t tick, void* ud) {
    (void)brain; (void)tick;
    UavCtx* ctx = (UavCtx*)ud;
    UavPhysics* uav = &ctx->sim->uavs[ctx->index];  // same thread as env_tick, no lock needed
    char b[64];
    double dh = sqrt((uav->x - HOME_X) * (uav->x - HOME_X) + (uav->y - HOME_Y) * (uav->y - HOME_Y));
    snprintf(b, sizeof(b), "%.17g", dh);
    fleece_goap_bb_set(bb, "distHome", (const uint8_t*)b, (uint32_t)strlen(b), false);
    // Mock autopilot state - foreshadows the PX4 adapter streaming mode/etc.
    fleece_goap_bb_set(bb, "mode", (const uint8_t*)"\"offboard\"", 11, false);
}

// GOAP scenario: GPS-based behaviour authored as JS function sources.
//   - no named locations: preconditions/goals reason over bb.self.x/.y and
//     distances via Math.hypot; exec bodies command the autopilot by GPS.
//   - eff is only the planner's heuristic (never applied); exec is real work.
// ---------------------------------------------------------------------------

static void register_scenario(FleeceGoap* g) {
    fleece_goap_set_name(g, "UAV Swarm");

    FleeceGoapActionDef a = {0};

    // fly to the patrol zone, sweep the sensor for a target
    const char* pre_scan[] = {
        "function(bb){ return bb.self.detect === 0 && bb.self.cargo === 0 && bb.self.battery > 30; }"
    };
    const char* eff_scan[] = {
        "function(bb){ bb.self.detect = 1; return bb; }"
    };
    a.id = "scan"; a.name = "Patrol Zone";
    a.pre = pre_scan; a.pre_count = 1;
    a.eff = eff_scan; a.eff_count = 1;
    a.dest = "zone";
    // Brain-side target assignment: the sensor returns EVERYTHING it sees; the
    // brain picks the nearest target no live peer has claimed, then claims it
    // atomically in the shared world (worldCompareAndSet: claim-if-absent, or
    // take over a claim whose owner has gone silent / left the swarm). Claims
    // are gossiped to peers like any other world state.
    a.exec = "function(bb, tick){"
             " platform.uav_waypoint(500, 120);"
             " if (!platform.uav_arrived().arrived) return false;"
             " const s = platform.uav_scan();"
             " if (!s.detect) return false;"
             " let target = null;"
             " for (const t of s.targets) {"
             "   const owner = bb.world['claim_' + t.id];"
             "   if (owner && owner !== self.id && bb.swarm[owner]) continue;"
             "   if (!target || Math.hypot(t.x - bb.self.x, t.y - bb.self.y) < Math.hypot(target.x - bb.self.x, target.y - bb.self.y)) target = t;"
             " }"
             " if (!target) return false;"
             " const owner = bb.world['claim_' + target.id];"
             " const ok = owner === self.id ? true : worldCompareAndSet('claim_' + target.id, owner, self.id);"
             " if (!ok) return true;"
             " bb.self.target = target; bb.self.detect = 1;"
             " return true; }";
    fleece_goap_add_action(g, &a);

    // intercept the detected target at its GPS
    const char* pre_hunt[] = {
        "function(bb){ return bb.self.detect === 1 && bb.self.battery > 30; }"
    };
    const char* eff_hunt[] = {
        "function(bb){ bb.self.cargo = 1; bb.self.detect = 0; return bb; }"
    };
    memset(&a, 0, sizeof(a));
    a.id = "hunt"; a.name = "Intercept Target";
    a.pre = pre_hunt; a.pre_count = 1;
    a.eff = eff_hunt; a.eff_count = 1;
    a.dest = "target";
    a.exec = "function(bb, tick){"
             " const t = bb.self.target;"
             " if (!t) return true;"
             " platform.uav_waypoint(t.x, t.y);"
             " if (!platform.uav_arrived().arrived) return false;"
             " if (bb.world['claim_' + t.id] !== self.id) { bb.self.detect = 0; bb.self.target = null; return true; }"
             " const p = platform.uav_pickup(t.id);"
             " if (p.picked) { delete world['claim_' + t.id]; bb.self.cargo = 1; bb.self.detect = 0; bb.self.target = null; return true; }"
             " if (bb.world['claim_' + t.id] === self.id) delete world['claim_' + t.id];"
             " bb.self.detect = 0; bb.self.target = null;"
             " return true; }";
    fleece_goap_add_action(g, &a);

    // bring the target home (also the low-battery retreat)
    const char* pre_return[] = {
        "function(bb){ return bb.self.cargo === 1 || bb.self.battery < 30; }"
    };
    const char* eff_return[] = {
        "function(bb){ bb.self.delivered = 1; bb.self.cargo = 0; bb.self.distHome = 0; return bb; }"
    };
    memset(&a, 0, sizeof(a));
    a.id = "return"; a.name = "Return to Base";
    a.pre = pre_return; a.pre_count = 1;
    a.eff = eff_return; a.eff_count = 1;
    a.dest = "home";
    a.exec = "function(bb, tick){ platform.uav_waypoint(bb.self.home.x, bb.self.home.y);"
             " if (!platform.uav_arrived().arrived) return false;"
             " platform.uav_deliver();"
             " bb.self.delivered = 1; bb.self.cargo = 0;"
             " return true; }";
    fleece_goap_add_action(g, &a);

    // hover at the pad and top up; resetting delivered opens a new patrol cycle
    const char* pre_charge[] = {
        "function(bb){ return bb.self.distHome < 30; }"
    };
    const char* eff_charge[] = {
        "function(bb){ bb.self.battery = 100; bb.self.delivered = 0; return bb; }"
    };
    memset(&a, 0, sizeof(a));
    a.id = "charge"; a.name = "Recharge";
    a.pre = pre_charge; a.pre_count = 1;
    a.eff = eff_charge; a.eff_count = 1;
    a.dest = "home";
    a.exec = "function(bb, tick){ if (tick >= 3) { bb.self.battery = 100; bb.self.delivered = 0; return true; } return false; }";
    fleece_goap_add_action(g, &a);

    // Goals, utility-driven so the swarm shifts from work to recovery.
    FleeceGoapGoalDef goal = {0};
    goal.id = "forage";
    goal.name = "Deliver a target";
    goal.expr = "function(bb){ return bb.self.delivered >= 1; }";
    goal.priority = 2.0;
    goal.curve_id = "uDel";
    fleece_goap_add_goal(g, &goal);

    goal.id = "recharge";
    goal.name = "Battery topped up";
    goal.expr = "function(bb){ return bb.self.battery >= 90; }";
    goal.priority = 2.0;
    goal.curve_id = "uBat";
    fleece_goap_add_goal(g, &goal);

    FleeceGoapUtilityDef u = {0};
    FleeceGoapPoint u_del[] = { {0, 1.0}, {1, 0.0} };
    u.id = "uDel"; u.name = "Delivery duty";
    u.dim = "delivered"; u.x_min = 0; u.x_max = 1;
    u.points = u_del; u.point_count = 2;
    fleece_goap_add_utility(g, &u);

    FleeceGoapPoint u_bat[] = { {0, 1.0}, {100, 0.0} };
    u.id = "uBat"; u.name = "Battery urgency";
    u.dim = "battery"; u.x_min = 0; u.x_max = 100;
    u.points = u_bat; u.point_count = 2;
    fleece_goap_add_utility(g, &u);
}

// ---------------------------------------------------------------------------
// HTTP handlers: telemetry + controls.
// ---------------------------------------------------------------------------

static void http_state(const char* method, const char* path, const char* body, uint32_t body_size,
                       char* scratch, size_t scratch_cap, FleeceHttpResponse* resp, void* ud) {
    (void)method; (void)path; (void)body; (void)body_size; (void)ud;
    Sim* sim = &g_sim;
    size_t off = 0;
    pthread_mutex_lock(&sim->lock);

    off += (size_t)snprintf(scratch + off, scratch_cap - off,
        "{\"tick\":%llu,\"paused\":%s,\"speed\":%.2f,\"worldDelivered\":%d,"
        "\"base\":{\"x\":%.1f,\"y\":%.1f},\"zone\":{\"x\":%.1f,\"y\":%.1f,\"r\":%.1f},"
        "\"world\":{\"w\":%.1f,\"h\":%.1f},\"uavs\":[",
        (unsigned long long)sim->tick, sim->paused ? "true" : "false", sim->speed,
        sim->world_delivered, HOME_X, HOME_Y, ZONE_X, ZONE_Y, ZONE_R, WORLD_W, WORLD_H);

    for (int i = 0; i < UAV_COUNT; i++) {
        UavPhysics* u = &sim->uavs[i];
        if (u->wp_set) {
            off += (size_t)snprintf(scratch + off, scratch_cap - off,
                "%s{\"id\":\"uav-%d\",\"x\":%.2f,\"y\":%.2f,\"heading\":%.3f,\"battery\":%.1f,"
                "\"cargo\":%d,\"detect\":%d,\"airborne\":true,\"wp\":[%.2f,%.2f],"
                "\"goal\":\"%s\",\"action\":\"%s\",\"progress\":%d,"
                "\"rxBytes\":%llu,\"txBytes\":%llu,\"rxRate\":%.1f,\"txRate\":%.1f}",
                i > 0 ? "," : "", i + 1,
                u->x, u->y, u->heading, u->battery,
                sim->uav_cargo[i], sim->uav_detect[i], u->wp_x, u->wp_y,
                sim->uav_goal[i], sim->uav_action[i], sim->uav_progress[i],
                (unsigned long long)sim->uav_rx_bytes[i], (unsigned long long)sim->uav_tx_bytes[i],
                sim->uav_rx_rate[i], sim->uav_tx_rate[i]);
        } else {
            off += (size_t)snprintf(scratch + off, scratch_cap - off,
                "%s{\"id\":\"uav-%d\",\"x\":%.2f,\"y\":%.2f,\"heading\":%.3f,\"battery\":%.1f,"
                "\"cargo\":%d,\"detect\":%d,\"airborne\":%s,\"wp\":null,"
                "\"goal\":\"%s\",\"action\":\"%s\",\"progress\":%d,"
                "\"rxBytes\":%llu,\"txBytes\":%llu,\"rxRate\":%.1f,\"txRate\":%.1f}",
                i > 0 ? "," : "", i + 1,
                u->x, u->y, u->heading, u->battery,
                sim->uav_cargo[i], sim->uav_detect[i], u->airborne ? "true" : "false",
                sim->uav_goal[i], sim->uav_action[i], sim->uav_progress[i],
                (unsigned long long)sim->uav_rx_bytes[i], (unsigned long long)sim->uav_tx_bytes[i],
                sim->uav_rx_rate[i], sim->uav_tx_rate[i]);
        }
    }

    off += (size_t)snprintf(scratch + off, scratch_cap - off, "],\"targets\":[");
    for (int i = 0; i < TARGET_COUNT; i++) {
        Target* t = &sim->targets[i];
        off += (size_t)snprintf(scratch + off, scratch_cap - off, "%s{\"x\":%.2f,\"y\":%.2f,\"alive\":%s}",
            i > 0 ? "," : "", t->x, t->y, t->alive ? "true" : "false");
    }
    off += (size_t)snprintf(scratch + off, scratch_cap - off, "]}");
    pthread_mutex_unlock(&sim->lock);

    resp->status = 200;
    resp->mime = "application/json";
    resp->body = scratch;
}

// POST /cmd  {"cmd":"pause"|"play"|"step"|"reset","speed":N}
static void http_cmd(const char* method, const char* path, const char* body, uint32_t body_size,
                     char* scratch, size_t scratch_cap, FleeceHttpResponse* resp, void* ud) {
    (void)ud;
    Sim* sim = &g_sim;
    if (strcmp(method, "POST") != 0) {
        resp->status = 405;
        resp->mime = "application/json";
        resp->body = "{\"ok\":false,\"error\":\"use POST\"}";
        return;
    }

    char b[512];
    if (!body || body_size >= sizeof(b)) {
        resp->status = 400;
        resp->mime = "application/json";
        resp->body = "{\"ok\":false,\"error\":\"bad body\"}";
        return;
    }
    memcpy(b, body, body_size);
    b[body_size] = '\0';

    char cmd[32] = {0};
    double speed = 0.0;
    const char* c = strstr(b, "\"cmd\"");
    if (c && sscanf(c, "\"cmd\":\"%31[^\"]\"", cmd) == 1) {
        if (strcmp(cmd, "play") == 0) sim->paused = 0;
        else if (strcmp(cmd, "pause") == 0) sim->paused = 1;
        else if (strcmp(cmd, "step") == 0) { sim->paused = 1; sim->step_once = 1; }
        else if (strcmp(cmd, "reset") == 0) sim->reset = 1;
        else {
            resp->status = 400;
            resp->mime = "application/json";
            resp->body = "{\"ok\":false,\"error\":\"unknown cmd\"}";
            return;
        }
    }
    const char* s = strstr(b, "\"speed\"");
    if (s && sscanf(s, "\"speed\":%lf", &speed) == 1) {
        if (speed < 0.2) speed = 0.2;
        if (speed > 4.0) speed = 4.0;
        sim->speed = speed;
    }

    (void)scratch; (void)scratch_cap;
    resp->status = 200;
    resp->mime = "application/json";
    resp->body = "{\"ok\":true}";
}

// ---------------------------------------------------------------------------
// Brain event logging (mirrors example 4).
// ---------------------------------------------------------------------------

static void brain_event(FleeceGoapBrain* brain, FleeceGoapBrainEvent event, void* ud) {
    UavCtx* ctx = (UavCtx*)ud;
    const char* goal = fleece_goap_brain_goal_id(brain);
    const char* action = fleece_goap_brain_action_id(brain);
    switch (event) {
        case FLEECE_GOAP_BRAIN_EVENT_REPLAN:
            printf("[uav-%d] tick %-4u goal '%-8s' -> execute '%-6s'\n", ctx->index + 1,
                   fleece_goap_brain_tick_count(brain), goal ? goal : "(none)", action ? action : "(none)");
            break;
        case FLEECE_GOAP_BRAIN_EVENT_ACTION_DONE:
            printf("[uav-%d] tick %-4u '%s' done, replanning\n", ctx->index + 1,
                   fleece_goap_brain_tick_count(brain), action ? action : "?");
            break;
        case FLEECE_GOAP_BRAIN_EVENT_ABORTED:
            printf("[uav-%d] tick %-4u '%s' aborted (watchdog), replanning\n", ctx->index + 1,
                   fleece_goap_brain_tick_count(brain), action ? action : "?");
            break;
        case FLEECE_GOAP_BRAIN_EVENT_IDLE:
            printf("[uav-%d] tick %-4u IDLE\n", ctx->index + 1, fleece_goap_brain_tick_count(brain));
            break;
    }
}

// ---------------------------------------------------------------------------
// Threads + main.
// ---------------------------------------------------------------------------

static void* uav_thread(void* ud) {
    UavCtx* ctx = (UavCtx*)ud;
    (void)fleece_runtime_start(ctx->runtime);  // blocks until fleece_runtime_stop
    return NULL;
}

static void on_signal(int signum) {
    (void)signum;
    g_sim.stopping = 1;
    fleece_http_server_stop(g_srv);
    for (int i = 0; i < UAV_COUNT; i++) {
        if (g_runtimes[i]) fleece_runtime_stop(g_runtimes[i]);
    }
}

static int init_uav(int index) {
    Sim* sim = &g_sim;
    UavCtx* ctx = &g_ctx[index];
    ctx->sim = sim;
    ctx->index = index;

    uint64_t node_id = 0xE5B0000000000000ULL | (uint64_t)(uint32_t)(index + 1);
    FleeceRuntime* runtime = fleece_runtime_create_with_node_id(node_id);
    if (!runtime) {
        fprintf(stderr, "uav-%d: failed to create runtime\n", index + 1);
        return -1;
    }
    ctx->runtime = runtime;
    ctx->self_id = node_id;
    ctx->sm = (FleeceStateManager*)fleece_runtime_get_state_manager(runtime);

    FleeceComms* comms = (FleeceComms*)fleece_runtime_get_comms(runtime);
    if (comms && fleece_comms_initialize(comms) != 0) {
        fprintf(stderr, "uav-%d: comms init failed\n", index + 1);
        return -1;
    }
    // In-process mesh: broadcast gossip frames to every other UAV, drain our
    // own mailbox on our own thread (see mesh_on_send/mesh_on_poll).
    fleece_comms_set_send_callback(comms, mesh_on_send, ctx);
    fleece_comms_set_poll_callback(comms, mesh_on_poll, ctx);
    g_mesh_nodes[index] = ctx;

    FleeceStateManager* sm = ctx->sm;
    UavPhysics* uav = &sim->uavs[index];
    env_set_double(sm, "x", uav->x);
    env_set_double(sm, "y", uav->y);
    env_set_double(sm, "battery", uav->battery);
    env_set_double(sm, "distHome", 0.0);
    env_set_double(sm, "cargo", 0.0);
    env_set_double(sm, "detect", 0.0);
    env_set_double(sm, "delivered", 0.0);
    fleece_state_manager_set_named(sm, "home", (const uint8_t*)"{\"x\":60,\"y\":420}", 16);
    fleece_state_manager_set_named(sm, "target", (const uint8_t*)"null", 4);

    fleece_runtime_set_tick_callback(runtime, env_tick, ctx);

    FleeceGoap* goap = fleece_goap_create();
    if (!goap) {
        fprintf(stderr, "uav-%d: failed to create goap\n", index + 1);
        return -1;
    }
    register_scenario(goap);
    if (fleece_runtime_set_goap(runtime, goap) != 0) {
        fprintf(stderr, "uav-%d: failed to attach brain\n", index + 1);
        fleece_goap_destroy(goap);
        return -1;
    }
    FleeceGoapBrain* brain = (FleeceGoapBrain*)fleece_runtime_get_goap_brain(runtime);
    fleece_goap_brain_set_event_callback(brain, brain_event, ctx);
    fleece_goap_brain_set_max_action_ticks(brain, 300);  // ~30s at 10Hz: drop wedged actions
    fleece_goap_brain_set_goal_cooldown_ticks(brain, 20);  // ~2s at 10Hz: don't re-search an unplannable goal every tick
    fleece_goap_brain_set_world_model(brain, brain_world_model, ctx);  // live telemetry -> bb.self

    // Hardware: the autopilot/sensor verbs this UAV's brain may call. Note the
    // platform is PURELY a flight controller + sensor - coordination (target
    // claiming) lives in the brain, over shared world state.
    FleecePlatform* platform = (FleecePlatform*)fleece_runtime_get_platform(runtime);
    fleece_platform_register(platform, "uav_waypoint", platform_uav_waypoint, ctx);
    fleece_platform_register(platform, "uav_arrived", platform_uav_arrived, ctx);
    fleece_platform_register(platform, "uav_scan", platform_uav_scan, ctx);
    fleece_platform_register(platform, "uav_pickup", platform_uav_pickup, ctx);
    fleece_platform_register(platform, "uav_deliver", platform_uav_deliver, ctx);

    return 0;
}

int main(int argc, char** argv) {
    uint16_t port = (uint16_t)(argc > 1 ? atoi(argv[1]) : 8080);
    const char* webroot = argc > 2 ? argv[2] : "examples/webui";

    printf("Fleece Example 5: UAV Swarm + Web Dashboard\n");
    printf("============================================\n\n");
    printf("3 UAVs, each with its own runtime + GOAP brain.\n");
    printf("All behaviour is GOAP; C only simulates the hardware behind\n");
    printf("the platform registry (autopilot, sensors, battery).\n\n");
    printf("Dashboard:  http://127.0.0.1:%u\n", (unsigned)port);
    printf("Press Ctrl+C to stop\n\n");

    srand(20260818u);
    memset(&g_sim, 0, sizeof(g_sim));
    pthread_mutex_init(&g_sim.lock, NULL);
    g_sim.speed = 1.0;

    // UAVs start parked on/around the base pad.
    const double starts[UAV_COUNT][2] = { {HOME_X, HOME_Y}, {HOME_X - 20, HOME_Y + 10}, {HOME_X + 20, HOME_Y - 10} };
    for (int i = 0; i < UAV_COUNT; i++) {
        UavPhysics* u = &g_sim.uavs[i];
        u->x = starts[i][0];
        u->y = starts[i][1];
        u->battery = BATTERY_MAX;
        u->heading = 0.0;
    }
    respawn_targets(&g_sim);

    g_srv = fleece_http_server_create(webroot, port);
    if (!g_srv) {
        fprintf(stderr, "Failed to start HTTP server on port %u (is it in use?)\n", (unsigned)port);
        return 1;
    }
    fleece_http_server_register(g_srv, "/state", http_state, NULL);
    fleece_http_server_register(g_srv, "/cmd", http_cmd, NULL);

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    pthread_t threads[UAV_COUNT];
    for (int i = 0; i < UAV_COUNT; i++) {
        if (init_uav(i) != 0) {
            fprintf(stderr, "Failed to init uav-%d\n", i + 1);
            return 1;
        }
        g_runtimes[i] = g_ctx[i].runtime;
        if (pthread_create(&threads[i], NULL, uav_thread, &g_ctx[i]) != 0) {
            fprintf(stderr, "Failed to start uav-%d thread\n", i + 1);
            return 1;
        }
    }

    // runtime_start installs its own SIGINT handler per thread - reinstall ours
    // so one Ctrl+C stops the whole swarm (and the web server).
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    int rc = fleece_http_server_run(g_srv);

    for (int i = 0; i < UAV_COUNT; i++) {
        fleece_runtime_stop(g_runtimes[i]);
        pthread_join(threads[i], NULL);
    }
    fleece_http_server_destroy(g_srv);

    for (int i = 0; i < UAV_COUNT; i++) {
        FleeceRuntime* runtime = g_ctx[i].runtime;
        FleeceComms* comms = (FleeceComms*)fleece_runtime_get_comms(runtime);
        if (comms) fleece_comms_close(comms);
        fleece_runtime_destroy(runtime);
    }

    printf("Swarm stopped.\n");
    return rc;
}