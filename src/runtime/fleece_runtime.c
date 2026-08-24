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
// Repairs during local churn wait for a short quiet period: while THIS node
// is itself writing, digest divergence is expected (peers legitimately lag
// behind us) and every peer repairs at once - at N>=6 that aggregate is what
// saturates the channel (measured: quiesce-free repairs drove >90% channel
// utilization with zero convergence). A lost-delta gap persists past the
// quiet point by definition, so nothing is missed; in-flight rounds continue.
#define FLEECE_RESYNC_QUIESCE_TICKS 5
// Bounded leak-through: at most this many NEW handshake rounds may start per
// tick across all peers. After a partition heals, every peer reports a gap on
// the same tick; without the cap they would all fire index requests at once
// and stampede the channel exactly when N peers' worth of backlogged gossip is
// already fighting for it (observed as the N=6 non-convergence). Two per tick
// drains the backlog quickly while keeping each tick's control burst tiny;
// in-flight rounds and their retries are NOT throttled by this - only starts.
#define FLEECE_RESYNC_STARTS_PER_TICK 2
#define FLEECE_MAX_RESYNC_TARGETS 32   // matches FLEECE_MAX_TRACKED_PEERS in the state manager
#define FLEECE_MAX_PENDING_TX 16       // deferred control-frame slots
#define FLEECE_MAX_FETCH_KEYS 64       // max keys fetched per repair round
// Steady-state gossip cadence: one batched delta frame every this many ticks.
// Wire cost per node is roughly frame_bytes * N_peers * RNS_overhead / (ticks
// between sends); at N=6 that lands under the 1/N channel share only at 10+
// ticks (measured: 5 ticks oversubscribed a 115200-baud channel ~2x once
// per-packet header/crypto overhead is counted).
//
// NOT a hard constant: fleece_runtime_set_gossip_cadence() overrides both this
// and the beacon interval per runtime, for transports with bandwidth to spare
// (native_sim pipelines, wired backhauls) that want sub-second propagation.
#define FLEECE_GOSSIP_EVERY_TICKS 10
static uint32_t s_gossip_every_ticks = FLEECE_GOSSIP_EVERY_TICKS;
// Wire budget for one gossip delta frame. Under the single-packet payload
// ceiling at the LoRa MTU (460), so frames never fall back to Resource/Link
// transfer even with per-record overhead at its worst.
#define FLEECE_GOSSIP_FRAME_BYTES 320
// Digest beacon cadence: a header-only frame (view digest, zero records) so
// peers keep detecting divergence even when nobody has fresh data - without
// it, a delta lost during churn leaves the loser silent AND blind: it cannot
// know its view differs until the next real frame arrives, which under
// empty-delta suppression may be never (measured: post-churn silences left
// stale values unrepaired past the convergence budget).
#define FLEECE_BEACON_EVERY_TICKS 20
static uint32_t s_beacon_every_ticks = FLEECE_BEACON_EVERY_TICKS;

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

    // On-demand resync bookkeeping, one entry per peer we gossip with (protocol
    // v5 frames carry the sender's node id, so divergence is attributable per
    // peer and repair traffic can be UNICAST to the offender). A target is
    // created when a gossip frame arrives; need_shared is set when that frame's
    // digest check reports we are behind on the shared/"world" stream relative
    // to THIS sender, and cleared when a later frame from the same sender
    // confirms we are current. The probe (FLEECE_RESYNC_PROBE_TICKS)
    // re-requests from targets we haven't confirmed sync with in a while -
    // handles "never heard from".
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
        uint64_t node_id;         // peer's node id (0 = anonymous "mesh" aggregate)
        char source[48];          // comms destination: "node:%016llx", or "mesh"
        uint64_t last_heard_tick;  // last tick a gossip frame arrived from this source
        bool need_shared;          // digest divergence seen - repair pending
        bool awaiting_index;       // index request sent, reply not yet processed
        uint64_t last_index_req_tick;  // throttle for index-request retries
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

// Locates (creating if needed) the resync bookkeeping entry for a peer by
// NODE ID - the primary key since protocol v5 frames carry the sender's id.
// `node_id` 0 means "unknown sender": everything collapses into one anonymous
// "mesh" target (legacy fan-out behavior).
static struct ResyncTarget* resync_target_for_id(FleeceRuntime* runtime, uint64_t node_id) {
    char source[48];
    if (node_id == 0) {
        snprintf(source, sizeof source, "mesh");
    } else {
        snprintf(source, sizeof source, "node:%016llx", (unsigned long long)node_id);
    }

    for (int i = 0; i < FLEECE_MAX_RESYNC_TARGETS; i++) {
        if (runtime->resync_targets[i].exists && runtime->resync_targets[i].node_id == node_id &&
            strcmp(runtime->resync_targets[i].source, source) == 0) {
            return &runtime->resync_targets[i];
        }
    }
    for (int i = 0; i < FLEECE_MAX_RESYNC_TARGETS; i++) {
        if (!runtime->resync_targets[i].exists) {
            struct ResyncTarget* t = &runtime->resync_targets[i];
            t->node_id = node_id;
            snprintf(t->source, sizeof t->source, "%s", source);
            t->last_heard_tick = runtime->gossip_tick_count;
            t->need_shared = false;
            t->awaiting_index = false;
            t->last_index_req_tick = 0;
            t->exists = true;
            return t;
        }
    }
    return NULL;  // table full - silently skip; gap detection is best-effort
}

static void enqueue_tx(FleeceRuntime* runtime, const char* dest, const uint8_t* data, uint32_t size);

// Sends an 'FX' v2 control frame. tag 0 = index request (empty items); tag 2
// = value request carrying key hashes. `origin` is this node's id, so the
// responder can unicast the reply back to us alone.
//
// defer: TRUE queues the frame for the next main-loop tick instead of
// transmitting inline. Anything composed while we are INSIDE the transport's
// inbound-dispatch callback MUST defer - a re-entrant Packet::send() there
// hands the frame to an interface whose transport is mid-cycle and it vanishes
// without failing (measured live: value requests fired from the index-reply
// handler never reached any responder, stalling every repair round).
static void send_control_ex(FleeceRuntime* runtime, const char* target, uint8_t tag,
                            const uint32_t* hashes, uint32_t count, bool defer) {
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
    if (defer) {
        enqueue_tx(runtime, target, buf, (uint32_t)pos);
    } else {
        fleece_comms_send(runtime->comms, target, buf, (uint32_t)pos);
    }
}

static void send_control(FleeceRuntime* runtime, const char* target, uint8_t tag,
                         const uint32_t* hashes, uint32_t count) {
    send_control_ex(runtime, target, tag, hashes, count, false);
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
// fall back to fan-out). Returns true when keys were requested.
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
    // An EMPTY diff means we hold everything THIS responder advertised:
    // pairwise sync with that peer is certified, so its repair flag disarms.
    // (Under the old single aggregated target this was unsound - one
    // responder proving nothing said nothing about OTHER peers, and clearing
    // then left stale values stuck forever behind the fastest responder.
    // Per-peer targets restored the soundness: any other peer's fresher data
    // keeps ITS OWN flag armed.) A non-empty diff keeps the flag armed until
    // the fetched values arrive and a later index exchange comes back clean.
    if (n_wanted > 0) {
        char dest[40];
        if (!unicast_dest_for(reply_origin, dest, sizeof dest)) {
            snprintf(dest, sizeof dest, "mesh");
        }
        if (getenv("FX_DEBUG")) {
            fprintf(stderr, "[fx] me=%llu fetching %u keys from %s (%llx):",
                (unsigned long long)fleece_state_manager_get_node_id(runtime->state_manager),
                n_wanted, dest, (unsigned long long)reply_origin);
            for (uint32_t i = 0; i < n_wanted; i++) fprintf(stderr, " %08x", wanted[i]);
            fprintf(stderr, "\n");
        }
        // Deferred transmit: we are inside the transport's inbound dispatch
        // (this handler runs from the index-reply callback chain) - an inline
        // send here vanishes at the transport layer. See send_control_ex.
        send_control_ex(runtime, dest, FLEECE_RESYNC_TAG_VALUE_REQ, wanted, n_wanted, true);
        return true;
    }
    struct ResyncTarget* t = resync_target_for_id(runtime, reply_origin);
    if (t) t->need_shared = false;
    if (getenv("FX_DEBUG")) fprintf(stderr, "[fx] me=%llu diff empty vs %llx - sync certified\n",
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
        // The responder's index reply closes OUR round against THAT peer.
        struct ResyncTarget* t = resync_target_for_id(runtime, origin);
        if (t) t->awaiting_index = false;
        request_missing_keys(runtime, origin, hashes, tss, n);
        return;
    }
}

// Gossip import + resync bookkeeping core, shared by both inbound paths.
static void import_and_track(FleeceRuntime* runtime, const uint8_t* data, uint32_t size) {
    bool behind_shared = false;
    uint64_t sender = 0;
    if (fleece_state_manager_import_from(runtime->state_manager, data, size, NULL, &behind_shared, &sender) != 0) {
        if (getenv("FX_DEBUG")) fprintf(stderr, "[fx] me=%llu import FAILED (%u B)\n",
            (unsigned long long)fleece_state_manager_get_node_id(runtime->state_manager), size);
        return;  // malformed frame - ignore
    }
    if (getenv("FX_DEBUG")) fprintf(stderr, "[fx] me=%llu imported %u B from %llx behind=%d\n",
        (unsigned long long)fleece_state_manager_get_node_id(runtime->state_manager),
        size, (unsigned long long)sender, (int)behind_shared);

    struct ResyncTarget* target = resync_target_for_id(runtime, sender);
    if (!target) return;
    target->last_heard_tick = runtime->gossip_tick_count;

    // Per-peer flag semantics. With each peer's reports landing in its OWN
    // target, the old stickiness is no longer needed: the flicker hazard it
    // guarded against was one peer's "current" report disarming repair state
    // armed by a DIFFERENT peer in the same tick (all peers used to aggregate
    // under one anonymous "mesh" target). Now a digest match from peer X
    // genuinely certifies "we hold everything X holds", so clearing X's flag
    // immediately is sound - divergence elsewhere re-arms via that peer's own
    // frames. The awaiting_index guard keeps an in-flight round alive until
    // its reply (or retry timeout) resolves it.
    if (behind_shared) {
        target->need_shared = true;
    } else if (!target->awaiting_index) {
        target->need_shared = false;
    }
}

// Receives gossip or 'FX' control frames from the built-in comms path.
static void runtime_gossip_receive(const char* source, const uint8_t* data, uint32_t size, void* user_data) {
    FleeceRuntime* runtime = (FleeceRuntime*)user_data;
    if (!runtime || !data || size == 0) return;

    if (size >= 3 && data[0] == FLEECE_RESYNC_MAGIC0 && data[1] == FLEECE_RESYNC_MAGIC1) {
        if (data[2] == FLEECE_RESYNC_VERSION) handle_fx_frame(runtime, data, size, source);
        return;
    }

    // The v5 sender header identifies the peer; the comms source string is
    // not needed for attribution.
    import_and_track(runtime, data, size);
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
    // The v5 sender header identifies the peer; no source string needed.
    import_and_track(runtime, data, size);
}

void fleece_runtime_note_behind_from(FleeceRuntime* runtime, bool behind_shared, uint64_t sender_node_id) {
    if (!runtime) return;
    struct ResyncTarget* t = resync_target_for_id(runtime, sender_node_id);
    if (!t) return;
    t->last_heard_tick = runtime->gossip_tick_count;

    // Same per-peer semantics as import_and_track: a gap report arms THIS
    // sender's repair flag; a current report disarms it (unless a round to
    // this very peer is in flight). Transports that cannot identify the
    // reporting peer pass sender_node_id = 0 and get the anonymous "mesh"
    // target - correct, just less targeted.
    if (behind_shared) {
        t->need_shared = true;
    } else if (!t->awaiting_index) {
        t->need_shared = false;
    }
}

// Phase 3 (on-demand part): run the targeted-repair handshake against every
// peer whose stream diverged from ours, and re-probe peers we haven't heard
// from in a while.
static void runtime_send_resync_requests(FleeceRuntime* runtime) {
    // First: drain replies composed during last tick's inbound dispatch -
    // safely outside the transport's callback context this time.
    flush_pending_tx(runtime);

    uint32_t starts_this_tick = 0;

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
            bool can_start = starts_this_tick < FLEECE_RESYNC_STARTS_PER_TICK;
            if (quiet && can_start &&
                (!t->awaiting_index ||
                 runtime->gossip_tick_count - t->last_index_req_tick >= FLEECE_RESYNC_RETRY_TICKS)) {
                uint32_t none = 0;
                send_control(runtime, t->source, FLEECE_RESYNC_TAG_INDEX_REQ, &none, 0);
                t->awaiting_index = true;
                t->last_index_req_tick = runtime->gossip_tick_count;
                starts_this_tick++;
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
        // Gossip cadence + empty-delta suppression. Two hard limits collide on
        // a shared LoRa-class channel as N grows:
        //   - every frame costs a full N-peer fan-out of RNS packets, each
        //     carrying ~80B of header/crypto before any payload;
        //   - each node owns only 1/N of the channel.
        // Per-tick delta gossip crosses per-node fair share already at N=6
        // (measured: ticks stretched 20x, zero airtime left for repairs).
        // So deltas are BATCHED every FLEECE_GOSSIP_EVERY_TICKS ticks and
        // empty sends are skipped entirely: one amortized frame instead of k
        // heartbeat frames. Peers detect missed data via the view digest in
        // whichever frames do arrive, plus the unicast liveness probe.
        bool gossip_due = (runtime->gossip_tick_count % s_gossip_every_ticks) == 0;
        // Beacon phase offset by node id: synchronized beacons collide into
        // one N-fold burst every interval, exactly the kind of spike that
        // trips pool-pressure shedding on constrained radios.
        bool beacon_due = ((runtime->gossip_tick_count + (uint32_t)fleece_state_manager_get_node_id(runtime->state_manager))
                           % s_beacon_every_ticks) == 0;
        bool have_new = fleece_state_manager_count_new_shared(runtime->state_manager, runtime->shared_gossip_watermark) > 0;
        if (gossip_due && have_new) {
            // Bounded export: never exceed the transport's single-packet
            // ceiling, so steady-state gossip NEVER falls back to
            // Resource/Link transfer - pending Resources pin fixed-pool
            // memory and their shedding spiral silences a node's entire
            // fan-out while its unicast control path keeps working (measured:
            // one wedged node gossiped nothing for a whole run). Whatever
            // doesn't fit stays eligible: the watermark advances only to the
            // highest record ACTUALLY carried.
            uint64_t included_max_ts = 0;
            if (fleece_state_manager_export_shared_delta_bounded(runtime->state_manager,
                                                                 runtime->shared_gossip_watermark,
                                                                 FLEECE_GOSSIP_FRAME_BYTES,
                                                                 &shared_frame, &shared_frame_size,
                                                                 &included_max_ts) == 0) {
                fleece_comms_send(runtime->comms, "broadcast", shared_frame, shared_frame_size);
                if (getenv("FX_DEBUG")) fprintf(stderr, "[fx] me=%llu gossip %u B wm=%llu -> %llu\n",
                    (unsigned long long)fleece_state_manager_get_node_id(runtime->state_manager),
                    shared_frame_size,
                    (unsigned long long)runtime->shared_gossip_watermark,
                    (unsigned long long)included_max_ts);
                fleece_free(shared_frame);
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
                //
                // With the bounded exporter the ceiling is exact: the highest
                // timestamp actually carried. Records beyond it (truncated this
                // round) stay eligible for the next slot.
                runtime->shared_gossip_watermark = included_max_ts;
            }
        } else if (beacon_due) {
            // Digest beacon: header-only frame (zero records) carrying this
            // node's view digest, so peers keep detecting divergence even when
            // nobody has fresh data to gossip. Without it, a delta lost during
            // churn leaves both sides silent AND blind - the loser cannot know
            // its view differs until it sees a digest that disagrees with it.
            if (fleece_state_manager_export_shared_delta_bounded(runtime->state_manager,
                                                                 UINT64_MAX, 0,
                                                                 &shared_frame, &shared_frame_size,
                                                                 NULL) == 0) {
                fleece_comms_send(runtime->comms, "broadcast", shared_frame, shared_frame_size);
                fleece_free(shared_frame);
            }
        }
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

int fleece_runtime_set_gossip_cadence(FleeceRuntime* runtime, uint32_t gossip_every_ticks,
                                      uint32_t beacon_every_ticks) {
    if (!runtime) return -1;
    if (gossip_every_ticks == 0 || beacon_every_ticks == 0) return -1;
    s_gossip_every_ticks = gossip_every_ticks;
    s_beacon_every_ticks = beacon_every_ticks;
    return 0;
}
