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
#include "fleece_cbor.h"
#include "fleece_comms.h"
#include "fleece_embedded.h"
#include "platform/fleece_platform.h"
#include "planner/fleece_planner.h"
#include "embedded/fleece_goap_js.h"

// Runtime implementation

// On-demand resync protocol (replaces the periodic full-state broadcast).
// Delta gossip is the steady-state traffic; a node that detects divergence
// (see fleece_state_manager_import_ex's digest check) runs a TARGETED repair
// against the peer instead of pulling a blind full snapshot:
//
//   1. send an INDEX REQUEST ('FX' control frame, tag 0)
//   2. peer replies with its stream index [1, [[key_hash, ts], ...]]
//   3. we diff locally: keys missing here, or newer at the peer -> VALUE
//      REQUEST [2, hashes]
//   4. peer replies with a NORMAL gossip frame carrying just those records,
//      which merges through the regular import path
//
// A digest mismatch is usually benign concurrency (each side holds a fresh
// write the other hasn't heard), so step 3 often finds nothing - the whole
// repair costs one tiny request plus an index instead of O(world) airtime.
// A liveness probe re-runs the handshake from peers that haven't confirmed
// sync within the probe interval.
#define FLEECE_RESYNC_MAGIC0 'F'
#define FLEECE_RESYNC_MAGIC1 'X'
#define FLEECE_RESYNC_VERSION FLEECE_CONTROL_PROTOCOL_VERSION  // shared with index/value frames
#define FLEECE_RESYNC_TAG_INDEX_REQ 0
#define FLEECE_RESYNC_TAG_INDEX_REPLY 1
#define FLEECE_RESYNC_TAG_VALUE_REQ 2
#define FLEECE_RESYNC_PROBE_TICKS 100  // re-request from a peer not heard from this long
#define FLEECE_RESYNC_RETRY_TICKS 8    // re-send an unanswered index request after this many ticks
// Repairs during local churn are allowed again: with unicast replies a round
// costs one index + one targeted value frame, so the cure is now cheaper than
// the divergence it fixes. The retry cadence (above) still bounds the rate.
#define FLEECE_RESYNC_QUIESCE_TICKS 0
#define FLEECE_RESYNC_CLEAR_STREAK 8  // consecutive current reports before a repair flag clears
#define FLEECE_MAX_RESYNC_TARGETS 32   // matches FLEECE_MAX_TRACKED_PEERS in the state manager
#define FLEECE_MAX_PENDING_TX 16       // deferred control-frame slots
#define FLEECE_MAX_FETCH_KEYS 64       // max keys fetched per repair round

// v2 wire format: ['F']['X'][2] + CBOR [tag, origin_node_id, items...].
//
// origin_node_id is what makes UNICAST replies possible on transports that
// can route to a single peer (e.g. Reticulum, via fleece_reticulum_send_to_node):
// a responder answers an index/value request addressed to the requester alone,
// instead of fanning the reply out to every peer - which under a busy mesh
// multiplies repair traffic by N and was measured to dominate channel load.
// Transports without peer-addressed sends ignore the id and keep replying
// through the plain comms path (destination strings pass through unchanged).
#define FLEECE_MAX_FETCH_KEYS 64       // max keys fetched per repair round

struct FleeceRuntime {
    volatile sig_atomic_t is_running;
    FleeceStateManager* state_manager;
    FleeceComms* comms;
    FleeceEmbedded* embedded;
    FleecePlatform* platform;
    pthread_t main_thread;
    int script_fd;
    uint64_t shared_gossip_watermark;   // local timestamp as of the last shared/"world"-stream gossip send
    uint32_t gossip_tick_count;
    FleeceGoapBrain* goap_brain;        // optional behavior-loop driver (see fleece_runtime_set_goap)
    void (*tick_cb)(FleeceRuntime*, void*);  // optional per-tick C hook (see fleece_runtime_set_tick_callback)
    void* tick_ud;

    // On-demand resync bookkeeping, keyed by the comms source address of each
    // peer we gossip with. A target is created when a gossip frame arrives from
    // a source; need_shared is set when import_ex reports we are behind on the
    // shared/"world" stream, and cleared when a frame confirms we are current.
    // The probe (FLEECE_RESYNC_PROBE_TICKS) re-requests from targets we haven't
    // confirmed sync with in a while - handles "never heard from".
    //
    // Only the world stream exists on the wire now: "self" is node-local
    // storage (a node publishes selected fields into world explicitly), so the
    // old self-stream need flag is gone.
    // Deferred control-frame transmit queue. Replies are composed while we
    // are INSIDE the transport's inbound-dispatch callback; sending a Packet
    // re-entrantly at that moment hands it to an interface whose transport is
    // mid-cycle - measured live, such packets vanish without failing. Queue
    // here, flush from the main loop.
    struct PendingTx {
        char dest[48];
        uint8_t* data;
        uint32_t size;
        uint64_t ready_tick;  // earliest main-loop tick allowed to transmit
        bool used;
    } pending_tx[FLEECE_MAX_PENDING_TX];
    uint32_t pending_tx_dropped;

    struct ResyncTarget {
        char source[64];
        uint64_t last_heard_tick;  // last tick a gossip frame arrived from this source
        bool need_shared;          // digest divergence seen - repair pending
        bool awaiting_index;       // index request sent, reply not yet processed
        uint64_t last_index_req_tick;  // throttle for index-request retries
        uint32_t current_streak;   // consecutive "current" reports (sticky-clear)
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
            runtime->resync_targets[i].need_shared = false;
            runtime->resync_targets[i].awaiting_index = false;
            runtime->resync_targets[i].last_index_req_tick = 0;
            runtime->resync_targets[i].current_streak = 0;
            runtime->resync_targets[i].exists = true;
            return &runtime->resync_targets[i];
        }
    }
    return NULL;  // table full - silently skip; gap detection is best-effort
}

// Sends an 'FX' v2 control frame. tag 0 = index request (empty items); tag 2
// = value request carrying key hashes. `origin` is this node's id, so the
// responder can unicast the reply back to us alone.
static void send_control(FleeceRuntime* runtime, const char* target, uint8_t tag,
                         const uint32_t* hashes, uint32_t count) {
    uint8_t buf[16 + FLEECE_MAX_FETCH_KEYS * 5 + 16];
    size_t pos = 0;
    buf[pos++] = FLEECE_RESYNC_MAGIC0;
    buf[pos++] = FLEECE_RESYNC_MAGIC1;
    buf[pos++] = FLEECE_RESYNC_VERSION;
    fleece_cbor_write_array_header(buf, &pos, 3);
    fleece_cbor_write_uint(buf, &pos, tag);
    fleece_cbor_write_uint(buf, &pos, fleece_state_manager_get_node_id(runtime->state_manager));
    fleece_cbor_write_array_header(buf, &pos, count);
    for (uint32_t i = 0; i < count; i++) {
        fleece_cbor_write_uint(buf, &pos, hashes[i]);
    }
    fleece_comms_send(runtime->comms, target, buf, (uint32_t)pos);
}

// Formats the peer-addressed destination string understood by transports with
// per-peer routing (see the bench/reticulum router). Returns false when the
// frame carried no usable origin id.
static bool unicast_dest_for(uint64_t origin, char* out, size_t out_size) {
    if (origin == 0) return false;
    snprintf(out, out_size, "node:%016llx", (unsigned long long)origin);
    return true;
}

// Diff step of the repair: given a parsed index reply's [hash, ts] pairs,
// collect the keys we are missing or stale on and request them from the
// responder alone. `reply_origin` is the responder's node id (0 = unknown,
// fall back to fan-out).
static bool request_missing_keys(FleeceRuntime* runtime, uint64_t reply_origin,
                                 const uint64_t* hashes, const uint64_t* tss, uint32_t count) {
    uint32_t wanted[FLEECE_MAX_FETCH_KEYS];
    uint32_t n_wanted = 0;
    for (uint32_t i = 0; i < count && n_wanted < FLEECE_MAX_FETCH_KEYS; i++) {
        // Keys outside uint32 range cannot be name hashes - skip defensively.
        if (hashes[i] > 0xFFFFFFFFULL) continue;
        int have = fleece_state_manager_shared_at_least(runtime->state_manager,
                                                        (uint32_t)hashes[i], tss[i]);
        if (have == 0) wanted[n_wanted++] = (uint32_t)hashes[i];
    }
    // NOTE: an empty diff does NOT clear the repair flag. This responder's
    // index only proves WE have everything THEY have - another peer may hold
    // fresher data (measured live: clearing here left a stale value stuck
    // forever because the stalest responder always answered fastest).
    // Disarming happens exclusively via note_behind_shared's sustained
    // current-report streak, which is what actually certifies convergence.
    if (n_wanted > 0) {
        char dest[40];
        if (!unicast_dest_for(reply_origin, dest, sizeof dest)) {
            snprintf(dest, sizeof dest, "mesh");
        }
        if (getenv("FX_DEBUG")) fprintf(stderr, "[fx] me=%llu fetching %u keys from %s (%llx)\n",
            (unsigned long long)fleece_state_manager_get_node_id(runtime->state_manager),
            n_wanted, dest, (unsigned long long)reply_origin);
        send_control(runtime, dest, FLEECE_RESYNC_TAG_VALUE_REQ, wanted, n_wanted);
        return true;
    }
    if (getenv("FX_DEBUG")) fprintf(stderr, "[fx] me=%llu diff empty vs %llx\n",
        (unsigned long long)fleece_state_manager_get_node_id(runtime->state_manager),
        (unsigned long long)reply_origin);
    return false;
}

static void enqueue_tx(FleeceRuntime* runtime, const char* dest, const uint8_t* data, uint32_t size) {
    for (int i = 0; i < FLEECE_MAX_PENDING_TX; i++) {
        struct PendingTx* t = &runtime->pending_tx[i];
        if (t->used) continue;
        t->data = (uint8_t*)fleece_malloc(size);
        if (!t->data) return;
        memcpy(t->data, data, size);
        snprintf(t->dest, sizeof t->dest, "%s", dest);
        t->size = size;
        // Hold until the NEXT tick: transmitting inside the same transport
        // cycle that delivered the triggering packet loses the frame
        // (confirmed live), even from the main loop.
        t->ready_tick = runtime->gossip_tick_count + 1;
        t->used = true;
        return;
    }
    runtime->pending_tx_dropped++;  // queue full: newest dropped, retries will recover
}

static void flush_pending_tx(FleeceRuntime* runtime) {
    for (int i = 0; i < FLEECE_MAX_PENDING_TX; i++) {
        struct PendingTx* t = &runtime->pending_tx[i];
        if (!t->used || runtime->gossip_tick_count < t->ready_tick) continue;
        fleece_comms_send(runtime->comms, t->dest, t->data, t->size);
        fleece_free(t->data);
        t->data = NULL;
        t->used = false;
    }
}

// FX control-frame core, shared by the comms-receive path and the transport-
// agnostic public hook. `reply_source` is where replies go: under comms it is
// the peer's address; under transports that fan out to all peers anyway it is
// the pseudo-source "mesh" (ignored by such senders).
static void handle_fx_frame(FleeceRuntime* runtime, const uint8_t* data, uint32_t size,
                            const char* reply_source) {
    size_t pos = 3;
    uint8_t major;
    uint64_t value;

    int dbg = getenv("FX_DEBUG") ? 1 : 0;
    if (!fleece_cbor_read_head(data, size, &pos, &major, &value)) { if(dbg)fprintf(stderr,"[fx] me=%llu parse: outer head\n",(unsigned long long)fleece_state_manager_get_node_id(runtime->state_manager)); return; }
    if (major != 4 || value != 3) { if(dbg)fprintf(stderr,"[fx] me=%llu parse: outer arity=%u major=%u\n",(unsigned long long)fleece_state_manager_get_node_id(runtime->state_manager),(unsigned)value,major); return; }
    if (!fleece_cbor_read_head(data, size, &pos, &major, &value) || major != 0) return;
    uint64_t tag = value;

    if (!fleece_cbor_read_head(data, size, &pos, &major, &value) || major != 0) return;
    uint64_t origin = value;  // sender's node id - who replies go back to

    if (!fleece_cbor_read_head(data, size, &pos, &major, &value) || major != 4) return;
    uint64_t items = value;
    if (dbg) fprintf(stderr, "[fx] me=%llu got tag=%u origin=%llx items=%u\n",
        (unsigned long long)fleece_state_manager_get_node_id(runtime->state_manager),
        (unsigned)tag, (unsigned long long)origin, (unsigned)items);

    // Where replies to THIS peer go: peer-addressed when the transport can
    // route by node id (and the frame told us who sent it), else the legacy
    // source string / fan-out.
    char udest[40];
    bool have_unicast = unicast_dest_for(origin, udest, sizeof udest);
    const char* reply_dest = have_unicast ? udest : reply_source;

    if (tag == FLEECE_RESYNC_TAG_INDEX_REQ) {
        uint8_t* idx = NULL;
        uint32_t idx_size = 0;
        if (fleece_state_manager_export_shared_index(runtime->state_manager, &idx, &idx_size) == 0) {
            if (dbg) fprintf(stderr, "[fx] me=%llu replying index (%u B) to %s\n",
                (unsigned long long)fleece_state_manager_get_node_id(runtime->state_manager), idx_size, reply_dest);
            enqueue_tx(runtime, reply_dest, idx, idx_size);
            fleece_free(idx);
        }
        return;
    }

    if (tag == FLEECE_RESYNC_TAG_VALUE_REQ) {
        uint32_t hashes[FLEECE_MAX_FETCH_KEYS];
        uint32_t n = 0;
        for (uint64_t i = 0; i < items && n < FLEECE_MAX_FETCH_KEYS; i++) {
            if (!fleece_cbor_read_head(data, size, &pos, &major, &value) || major != 0) return;
            if (value <= 0xFFFFFFFFULL) hashes[n++] = (uint32_t)value;
        }
        if (n > 0) {
            uint8_t* frame = NULL;
            uint32_t frame_size = 0;
            if (fleece_state_manager_export_shared_by_hash(runtime->state_manager, hashes, n, &frame, &frame_size) == 0) {
                if (getenv("FX_DEBUG")) fprintf(stderr, "[fx] me=%llu serving %u keys to %s\n",
                    (unsigned long long)fleece_state_manager_get_node_id(runtime->state_manager), n, reply_dest);
                enqueue_tx(runtime, reply_dest, frame, frame_size);
                fleece_free(frame);
            }
        }
        return;
    }

    if (tag == FLEECE_RESYNC_TAG_INDEX_REPLY) {
        uint64_t hashes[FLEECE_MAX_RESYNC_TARGETS * 4];
        uint64_t tss[FLEECE_MAX_RESYNC_TARGETS * 4];
        uint32_t n = 0;
        for (uint64_t i = 0; i < items && n < FLEECE_MAX_RESYNC_TARGETS * 4; i++) {
            if (!fleece_cbor_read_head(data, size, &pos, &major, &value) || major != 4 || value != 2) return;
            if (!fleece_cbor_read_head(data, size, &pos, &major, &hashes[n]) || major != 0) return;
            if (!fleece_cbor_read_head(data, size, &pos, &major, &tss[n]) || major != 0) return;
            n++;
        }
        struct ResyncTarget* t = resync_target_for(runtime, "mesh");
        if (t) t->awaiting_index = false;
        request_missing_keys(runtime, origin, hashes, tss, n);
        return;
    }
}

// Gossip import + resync bookkeeping core, shared by both inbound paths.
static void import_and_track(FleeceRuntime* runtime, const char* source,
                             const uint8_t* data, uint32_t size) {
    bool behind_self = false;
    bool behind_shared = false;
    if (fleece_state_manager_import_ex(runtime->state_manager, data, size, &behind_self, &behind_shared) != 0) {
        return;  // malformed frame - ignore
    }

    struct ResyncTarget* target = resync_target_for(runtime, source);
    if (!target) return;
    target->last_heard_tick = runtime->gossip_tick_count;
    if (behind_shared) target->need_shared = true;
    else target->need_shared = false;
}

// Receives gossip or 'FX' control frames from the built-in comms path.
static void runtime_gossip_receive(const char* source, const uint8_t* data, uint32_t size, void* user_data) {
    FleeceRuntime* runtime = (FleeceRuntime*)user_data;
    if (!runtime || !data || size == 0) return;

    if (size >= 3 && data[0] == FLEECE_RESYNC_MAGIC0 && data[1] == FLEECE_RESYNC_MAGIC1) {
        if (data[2] == FLEECE_RESYNC_VERSION) handle_fx_frame(runtime, data, size, source);
        return;
    }

    import_and_track(runtime, source, data, size);
}

void fleece_runtime_on_control_frame(FleeceRuntime* runtime, const uint8_t* data, uint32_t size) {
    if (!runtime || !data || size < 3) return;
    if (data[0] != FLEECE_RESYNC_MAGIC0 || data[1] != FLEECE_RESYNC_MAGIC1) return;
    if (data[2] != FLEECE_RESYNC_VERSION) return;
    // Replies fan out to all peers under such transports - "mesh" marks them.
    handle_fx_frame(runtime, data, size, "mesh");
}

void fleece_runtime_on_gossip_frame(FleeceRuntime* runtime, const uint8_t* data, uint32_t size) {
    if (!runtime || !data || size == 0) return;
    import_and_track(runtime, "mesh", data, size);
}

void fleece_runtime_note_behind_shared(FleeceRuntime* runtime, bool behind_shared) {
    if (!runtime) return;
    struct ResyncTarget* t = resync_target_for(runtime, "mesh");
    if (!t) return;
    t->last_heard_tick = runtime->gossip_tick_count;

    // A single transport (e.g. Reticulum fan-out) aggregates ALL peers into
    // this one target. Two consequences shape this logic:
    //   - One peer's "behind" report must arm the repair flag even though
    //     OTHER peers legitimately report "current" in the same tick - a
    //     single false report would otherwise flicker the flag back off
    //     before any handshake fires (measured: 170+ gap reports produced
    //     ~12 repair attempts). Hence STICKY clearing: only a sustained
    //     streak of current reports - no in-flight round, no divergence
    //     anywhere - clears it.
    //   - While a round is in flight, current reports mean little anyway:
    //     the handshake itself will close the gap.
    if (behind_shared) {
        t->need_shared = true;
        t->current_streak = 0;
    } else if (!t->awaiting_index) {
        if (++t->current_streak >= FLEECE_RESYNC_CLEAR_STREAK) {
            t->need_shared = false;
        }
    }
}

// Phase 3 (on-demand part): run the targeted-repair handshake against every
// peer whose stream diverged from ours, and re-probe peers we haven't heard
// from in a while.
static void runtime_send_resync_requests(FleeceRuntime* runtime) {
    // First: drain replies composed during last tick's inbound dispatch -
    // safely outside the transport's callback context this time.
    flush_pending_tx(runtime);

    for (int i = 0; i < FLEECE_MAX_RESYNC_TARGETS; i++) {
        struct ResyncTarget* t = &runtime->resync_targets[i];
        if (!t->exists) continue;

        // Gap-driven: our view differs from what this peer advertised - run
        // the index/diff/fetch handshake. The request, its reply, or the
        // fetched values may all be lost on a lossy link, so retry with a
        // cooldown until an index reply confirms progress (or a frame
        // confirms we are current).
        //
        // Quiescence gate: while THIS node is itself writing, digest
        // divergence is expected (peers legitimately lag behind us), so
        // repairing then is pure churn - and on a congested channel it feeds
        // itself. Only start new handshakes once local writes have settled.
        // In-flight ones continue: their retries also respect the cooldown,
        // and a lost-delta gap persists into the quiet period by definition.
        if (t->need_shared) {
            bool quiet = fleece_state_manager_ticks_since_last_write(runtime->state_manager)
                         >= FLEECE_RESYNC_QUIESCE_TICKS;
            if (quiet &&
                (!t->awaiting_index ||
                 runtime->gossip_tick_count - t->last_index_req_tick >= FLEECE_RESYNC_RETRY_TICKS)) {
                uint32_t none = 0;
                send_control(runtime, t->source, FLEECE_RESYNC_TAG_INDEX_REQ, &none, 0);
                t->awaiting_index = true;
                t->last_index_req_tick = runtime->gossip_tick_count;
            }
        }

        // Liveness probe: haven't heard from this peer in a while - it may have
        // updates we never received (or we joined late). Cheap, throttled pull.
        if (runtime->gossip_tick_count - t->last_heard_tick >= FLEECE_RESYNC_PROBE_TICKS) {
            uint32_t none = 0;
            send_control(runtime, t->source, FLEECE_RESYNC_TAG_INDEX_REQ, &none, 0);
            t->awaiting_index = true;
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

    for (int i = 0; i < FLEECE_MAX_PENDING_TX; i++) {
        if (runtime->pending_tx[i].used) fleece_free(runtime->pending_tx[i].data);
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

        // Phase 3: Gossip (State Synchronization) - a single stream: the
        // shared/"world" collection (owner = FLEECE_SHARED_OWNER_ID), which is
        // THE swarm-replicated object. Any node may write/relay world fields,
        // and a node publishes its own sensor data by writing it into world
        // explicitly ("self" is node-local storage now, never broadcast).
        // Frames are delta-by-default; on-demand full resync replaces any
        // periodic full-state broadcast, so steady-state traffic is just the
        // deltas plus the occasional pull when the advertised view digest
        // diverges from ours. Peers' frames arrive via runtime_gossip_receive()
        // and merge into world; that handler also flags streams we are behind
        // on, and runtime_send_resync_requests() below pulls missing snapshots.
        uint8_t* shared_frame = NULL;
        uint32_t shared_frame_size = 0;
        if (fleece_state_manager_export_shared_delta(runtime->state_manager, runtime->shared_gossip_watermark, &shared_frame, &shared_frame_size) == 0) {
            fleece_comms_send(runtime->comms, "broadcast", shared_frame, shared_frame_size);
            fleece_free(shared_frame);
        }
        // The watermark must track this stream's own high-water mark (the
        // highest field.timestamp actually eligible for export_delta's
        // filter), not the manager's global local_timestamp clock. That
        // clock also advances on every import (merge_shared's own
        // max(local, remote) bump - see fleece_state_manager.c) and on
        // every LOCAL write regardless of destination, so a node that has
        // ticked a while before first hearing a given peer already has a
        // local_timestamp well past that peer's own (independently
        // clocked, so typically much lower) field timestamps. Advancing
        // the watermark to that global clock skips straight past those
        // merged-but-not-yet-exported fields: since export_delta only
        // ever looks at field.timestamp > since_timestamp, and a field's
        // own timestamp never changes once merged (until its origin
        // writes a newer one), the entry becomes permanently invisible to
        // every future export - found via a 3-node relay chain where B
        // correctly relayed one neighbor's data onward but never the
        // other's, depending purely on which one's frame happened to
        // arrive before B's own clock ticked past its timestamp.
        // get_shared_hw() reports the real per-stream ceiling instead, so
        // the watermark can never advance past data this export actually
        // had a chance to include.
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
