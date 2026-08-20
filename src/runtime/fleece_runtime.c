#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>

#include "fleece_runtime.h"
#include "fleece_alloc.h"
#include "fleece_state_manager.h"
#include "fleece_comms.h"
#include "fleece_embedded.h"
#include "platform/fleece_platform.h"
#include "planner/fleece_planner.h"
#include "embedded/fleece_goap_js.h"

// Runtime implementation

// On-demand resync protocol (replaces the periodic full-state broadcast).
// Delta gossip is the steady-state traffic; a node that detects it is behind
// on a peer's stream (see fleece_state_manager_import_ex) requests a full
// snapshot from that peer with a tiny unicast control frame, and peers reply
// with a full export. A liveness probe re-requests from peers that haven't
// confirmed sync within the probe interval, so a node that never hears from a
// peer at all still recovers instead of stalling forever.
#define FLEECE_RESYNC_REQUEST_MAGIC0 'F'
#define FLEECE_RESYNC_REQUEST_MAGIC1 'R'
#define FLEECE_RESYNC_VERSION 1
#define FLEECE_RESYNC_STREAM_SELF 0
#define FLEECE_RESYNC_STREAM_SHARED 1
#define FLEECE_RESYNC_PROBE_TICKS 100  // re-request from a peer not heard from this long
#define FLEECE_MAX_RESYNC_TARGETS 32   // matches FLEECE_MAX_TRACKED_PEERS in the state manager

struct FleeceRuntime {
    volatile sig_atomic_t is_running;
    FleeceStateManager* state_manager;
    FleeceComms* comms;
    FleeceEmbedded* embedded;
    FleecePlatform* platform;
    pthread_t main_thread;
    int script_fd;
    uint64_t self_gossip_watermark;     // local timestamp as of the last self-stream gossip send
    uint64_t shared_gossip_watermark;   // local timestamp as of the last shared/"world"-stream gossip send
    uint32_t gossip_tick_count;
    FleeceGoapBrain* goap_brain;        // optional behavior-loop driver (see fleece_runtime_set_goap)
    void (*tick_cb)(FleeceRuntime*, void*);  // optional per-tick C hook (see fleece_runtime_set_tick_callback)
    void* tick_ud;

    // On-demand resync bookkeeping, keyed by the comms source address of each
    // peer we gossip with. A target is created when a gossip frame arrives from
    // a source; need_self/need_shared are set when import_ex reports we are
    // behind on that source's stream, and cleared when a frame confirms we are
    // current. The probe (FLEECE_RESYNC_PROBE_TICKS) re-requests from targets
    // we haven't confirmed sync with in a while - handles "never heard from".
    struct ResyncTarget {
        char source[64];
        uint64_t last_heard_tick;  // last tick a gossip frame arrived from this source
        bool need_self;
        bool need_shared;
        bool exists;
    } resync_targets[FLEECE_MAX_RESYNC_TARGETS];
};

static FleeceRuntime* global_runtime = NULL;

static void signal_handler(int signum) {
    (void)signum;
    if (global_runtime) {
        global_runtime->is_running = 0;
    }
}

// Locates (creating if needed) the resync bookkeeping entry for `source`.
static struct ResyncTarget* resync_target_for(FleeceRuntime* runtime, const char* source) {
    if (!source || !source[0]) return NULL;

    for (int i = 0; i < FLEECE_MAX_RESYNC_TARGETS; i++) {
        if (runtime->resync_targets[i].exists && strcmp(runtime->resync_targets[i].source, source) == 0) {
            return &runtime->resync_targets[i];
        }
    }
    for (int i = 0; i < FLEECE_MAX_RESYNC_TARGETS; i++) {
        if (!runtime->resync_targets[i].exists) {
            strncpy(runtime->resync_targets[i].source, source, sizeof(runtime->resync_targets[i].source) - 1);
            runtime->resync_targets[i].source[sizeof(runtime->resync_targets[i].source) - 1] = '\0';
            runtime->resync_targets[i].last_heard_tick = runtime->gossip_tick_count;
            runtime->resync_targets[i].need_self = false;
            runtime->resync_targets[i].need_shared = false;
            runtime->resync_targets[i].exists = true;
            return &runtime->resync_targets[i];
        }
    }
    return NULL;  // table full - silently skip; gap detection is best-effort
}

// Sends a tiny unicast control frame asking `target` for a full snapshot of one
// stream (self or shared). The receiver answers with a full export (see
// runtime_gossip_receive).
static void send_resync_request(FleeceRuntime* runtime, const char* target, uint8_t stream) {
    uint8_t req[4] = {
        FLEECE_RESYNC_REQUEST_MAGIC0,
        FLEECE_RESYNC_REQUEST_MAGIC1,
        FLEECE_RESYNC_VERSION,
        stream
    };
    fleece_comms_send(runtime->comms, target, req, sizeof(req));
}

// Receives a gossip or resync-request frame. Requests are answered with a full
// export of the requested stream, unicast back to the requester. Gossip frames
// are merged and the receiver's resync bookkeeping is updated from the
// import_ex gap report: a stream we're behind on gets a need flag (so we pull),
// a stream we're current on clears it.
static void runtime_gossip_receive(const char* source, const uint8_t* data, uint32_t size, void* user_data) {
    FleeceRuntime* runtime = (FleeceRuntime*)user_data;
    if (!runtime || !data || size == 0) return;

    // Resync request control frame: ['F']['R'][version][stream] -> answer with
    // a full export of the requested stream, addressed back to the requester.
    if (size >= 4 && data[0] == FLEECE_RESYNC_REQUEST_MAGIC0 && data[1] == FLEECE_RESYNC_REQUEST_MAGIC1) {
        if (data[2] != FLEECE_RESYNC_VERSION) return;
        uint8_t* frame = NULL;
        uint32_t frame_size = 0;
        int rc = (data[3] == FLEECE_RESYNC_STREAM_SHARED)
            ? fleece_state_manager_export_shared(runtime->state_manager, &frame, &frame_size)
            : fleece_state_manager_export(runtime->state_manager, &frame, &frame_size);
        if (rc == 0) {
            fleece_comms_send(runtime->comms, source, frame, frame_size);
            fleece_free(frame);
        }
        return;
    }

    bool behind_self = false;
    bool behind_shared = false;
    if (fleece_state_manager_import_ex(runtime->state_manager, data, size, &behind_self, &behind_shared) != 0) {
        return;  // malformed frame - ignore
    }

    struct ResyncTarget* target = resync_target_for(runtime, source);
    if (!target) return;
    target->last_heard_tick = runtime->gossip_tick_count;
    if (behind_self) target->need_self = true;
    else target->need_self = false;
    if (behind_shared) target->need_shared = true;
    else target->need_shared = false;
}

// Phase 3 (on-demand part): request full snapshots from every peer we know we
// are behind on, and re-probe peers we haven't heard from in a while.
static void runtime_send_resync_requests(FleeceRuntime* runtime) {
    for (int i = 0; i < FLEECE_MAX_RESYNC_TARGETS; i++) {
        struct ResyncTarget* t = &runtime->resync_targets[i];
        if (!t->exists) continue;

        // Gap-driven: we imported a frame whose advertised hw exceeds what we
        // hold for this source's stream, so a delta was dropped - pull it now.
        if (t->need_self) send_resync_request(runtime, t->source, FLEECE_RESYNC_STREAM_SELF);
        if (t->need_shared) send_resync_request(runtime, t->source, FLEECE_RESYNC_STREAM_SHARED);

        // Liveness probe: haven't heard from this peer in a while - it may have
        // updates we never received (or we joined late). Cheap, throttled pull.
        if (runtime->gossip_tick_count - t->last_heard_tick >= FLEECE_RESYNC_PROBE_TICKS) {
            send_resync_request(runtime, t->source, FLEECE_RESYNC_STREAM_SELF);
            send_resync_request(runtime, t->source, FLEECE_RESYNC_STREAM_SHARED);
            t->last_heard_tick = runtime->gossip_tick_count;  // throttle to once per interval
        }
    }
}

static FleeceRuntime* runtime_create_internal(FleeceStateManager* state_manager) {
    FleeceRuntime* runtime = (FleeceRuntime*)fleece_calloc(1, sizeof(FleeceRuntime));
    if (!runtime) {
        fleece_state_manager_destroy(state_manager);
        return NULL;
    }

    runtime->is_running = 0;
    runtime->state_manager = state_manager;
    runtime->comms = fleece_comms_create();
    runtime->embedded = fleece_embedded_create();
    runtime->platform = fleece_platform_create();

    if (!runtime->state_manager || !runtime->comms || !runtime->embedded || !runtime->platform) {
        fleece_runtime_destroy(runtime);
        return NULL;
    }

    fleece_embedded_set_state_manager(runtime->embedded, runtime->state_manager);
    fleece_embedded_set_platform(runtime->embedded, runtime->platform);
    fleece_embedded_register_c_functions(runtime->embedded);

    // The runtime owns the comms receive slot: gossip frames from peers land here.
    fleece_comms_set_receive_callback(runtime->comms, runtime_gossip_receive, runtime);

    global_runtime = runtime;

    return runtime;
}

FleeceRuntime* fleece_runtime_create(void) {
    return runtime_create_internal(fleece_state_manager_create());
}

FleeceRuntime* fleece_runtime_create_with_node_id(uint64_t node_id) {
    return runtime_create_internal(fleece_state_manager_create_with_node_id(node_id));
}

void fleece_runtime_destroy(FleeceRuntime* runtime) {
    if (!runtime) return;

    fleece_runtime_stop(runtime);

    if (runtime->goap_brain) {
        fleece_goap_brain_destroy(runtime->goap_brain);
        runtime->goap_brain = NULL;
    }

    if (runtime->embedded) {
        fleece_embedded_destroy(runtime->embedded);
    }

    if (runtime->platform) {
        fleece_platform_destroy(runtime->platform);
    }

    if (runtime->comms) {
        fleece_comms_destroy(runtime->comms);
    }

    if (runtime->state_manager) {
        fleece_state_manager_destroy(runtime->state_manager);
    }

    fleece_free(runtime);
    global_runtime = NULL;
}

int fleece_runtime_load_script(FleeceRuntime* runtime, const char* source) {
    if (!runtime || !source) {
        return -1;
    }

    return fleece_embedded_load_script(runtime->embedded, source, "<script>");
}

int fleece_runtime_set_peer_ttl_ticks(FleeceRuntime* runtime, uint64_t ttl_ticks) {
    if (!runtime) return -1;

    return fleece_embedded_set_peer_ttl_ticks(runtime->embedded, ttl_ticks);
}

int fleece_runtime_start(FleeceRuntime* runtime) {
    if (!runtime || runtime->is_running) {
        return -1;
    }

    runtime->is_running = 1;

    // Setup signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    fleece_embedded_call_init(runtime->embedded);

    // Main runtime loop
    while (runtime->is_running) {
        fleece_state_manager_tick(runtime->state_manager);  // drives peer liveness (swarm TTL); see fleece_state_manager_ticks_since_seen

        // Phase 1: Input (Sensors/Radio)
        fleece_comms_process_input(runtime->comms);

        // Phase 2: C tick hook (optional) + Script Execution (QuickJS VM) - run
        // before gossip so any self.xxx/world.xxx changes made this tick are
        // broadcast this tick, not next.
        if (runtime->tick_cb) {
            runtime->tick_cb(runtime, runtime->tick_ud);
        }
        fleece_embedded_call_step(runtime->embedded);

        // Phase 2.5: GOAP brain (optional) - decides and applies/commits effects;
        // runs after script step so it sees this tick's sensor/state changes, and
        // before gossip so its decisions are broadcast this tick.
        if (runtime->goap_brain) {
            fleece_goap_brain_tick(runtime->goap_brain);
        }

        // Phase 3: Gossip (State Synchronization) - two independent streams,
        // each delta-by-default; on-demand full resync replaces the old periodic
        // full-state broadcast (anti-entropy), so steady-state traffic is just
        // the deltas plus the occasional pull when a gap is detected:
        //   - self: this node's own fields (owner = its own node id)
        //   - shared: the "world" collection (owner = FLEECE_SHARED_OWNER_ID),
        //     which any node may write/relay - not tied to any single node's liveness.
        // Peers' frames arrive via runtime_gossip_receive() and merge into
        // swarm/world; that handler also flags streams we are behind on, and
        // runtime_send_resync_requests() below pulls the missing snapshots.
        uint8_t* self_frame = NULL;
        uint32_t self_frame_size = 0;
        if (fleece_state_manager_export_delta(runtime->state_manager, runtime->self_gossip_watermark, &self_frame, &self_frame_size) == 0) {
            fleece_comms_send(runtime->comms, "broadcast", self_frame, self_frame_size);
            fleece_free(self_frame);
        }
        // The watermark must track this STREAM's own high-water mark (the
        // highest field.timestamp actually eligible for export_delta's
        // filter), not the manager's global local_timestamp clock. That
        // clock also advances on every import (merge_named/merge_shared's
        // own max(local, remote) bump - see fleece_state_manager.c) and on
        // every LOCAL field write regardless of stream, so a node that has
        // ticked a while before first hearing a given peer already has a
        // local_timestamp well past that peer's own (independently
        // clocked, so typically much lower) field timestamps. Advancing
        // the watermark to that global clock skips straight past those
        // merged-but-not-yet-exported fields: since export_delta only
        // ever looks at field.timestamp > since_timestamp, and a field's
        // own timestamp never changes once merged (until its origin
        // writes a newer one), the entry becomes permanently invisible to
        // every future export - found via a 3-node relay chain (A-B-C, A
        // and C not directly connected) where B correctly relayed one
        // neighbor's data onward but never the other's, depending purely
        // on which one's frame happened to arrive before B's own clock
        // ticked past its timestamp. get_self_hw()/get_shared_hw() report
        // the real per-stream ceiling instead, so the watermark can never
        // advance past data this export actually had a chance to include.
        runtime->self_gossip_watermark = fleece_state_manager_get_self_hw(runtime->state_manager);

        uint8_t* shared_frame = NULL;
        uint32_t shared_frame_size = 0;
        if (fleece_state_manager_export_shared_delta(runtime->state_manager, runtime->shared_gossip_watermark, &shared_frame, &shared_frame_size) == 0) {
            fleece_comms_send(runtime->comms, "broadcast", shared_frame, shared_frame_size);
            fleece_free(shared_frame);
        }
        runtime->shared_gossip_watermark = fleece_state_manager_get_shared_hw(runtime->state_manager);

        runtime_send_resync_requests(runtime);

        runtime->gossip_tick_count++;

        // Phase 4: Output (Actuators/Mesh Broadcast)
        fleece_comms_process_output(runtime->comms);

        // Pacing tick - avoid spinning the loop at full CPU
        struct timespec ts = {.tv_sec = 0, .tv_nsec = 100000000};  // 100ms
        nanosleep(&ts, NULL);
    }

    fleece_embedded_call_destroy(runtime->embedded);

    return 0;
}

void fleece_runtime_stop(FleeceRuntime* runtime) {
    if (runtime) {
        runtime->is_running = 0;
    }
}

bool fleece_runtime_is_running(FleeceRuntime* runtime) {
    return runtime ? runtime->is_running : false;
}

int fleece_runtime_execute_script(FleeceRuntime* runtime, const char* script) {
    if (!runtime || !script) {
        return -1;
    }

    return fleece_embedded_execute(runtime->embedded, script);
}

void* fleece_runtime_get_state_manager(FleeceRuntime* runtime) {
    return runtime ? runtime->state_manager : NULL;
}

void* fleece_runtime_get_comms(FleeceRuntime* runtime) {
    return runtime ? runtime->comms : NULL;
}

void* fleece_runtime_get_embedded(FleeceRuntime* runtime) {
    return runtime ? runtime->embedded : NULL;
}

void* fleece_runtime_get_platform(FleeceRuntime* runtime) {
    return runtime ? runtime->platform : NULL;
}

int fleece_runtime_set_goap(FleeceRuntime* runtime, FleeceGoap* goap) {
    if (!runtime || !goap) return -1;
    if (runtime->goap_brain) return -1;  // already attached

    runtime->goap_brain = fleece_goap_brain_create(runtime->embedded, goap);
    return runtime->goap_brain ? 0 : -1;
}

void* fleece_runtime_get_goap_brain(FleeceRuntime* runtime) {
    return runtime ? runtime->goap_brain : NULL;
}

int fleece_runtime_replace_goap(FleeceRuntime* runtime, FleeceGoap* new_goap) {
    if (!runtime || !new_goap) return -1;
    if (runtime->goap_brain) {
        fleece_goap_brain_destroy(runtime->goap_brain);
        runtime->goap_brain = NULL;
    }
    runtime->goap_brain = fleece_goap_brain_create(runtime->embedded, new_goap);
    return runtime->goap_brain ? 0 : -1;
}

void fleece_runtime_set_tick_callback(FleeceRuntime* runtime, void (*cb)(FleeceRuntime*, void*), void* user_data) {
    if (!runtime) return;
    runtime->tick_cb = cb;
    runtime->tick_ud = user_data;
}
