// Fleece gossip-bandwidth benchmark (lossy channels, world-object protocol)
//
// Drives the CURRENT gossip design - a single shared/"world" stream, delta
// push every tick, and digest-divergence-triggered pull repair (see
// fleece_runtime.c Phase 3 and fleece_state_manager_import_ex) - and reports
// what it costs on lossy links:
//
//   - ticks and wall time until every node holds an identical world object
//   - radio bytes spent until convergence, per node and vs the theoretical
//     minimum payload (the "overhead" of fighting packet loss)
//   - steady-state cost: air bytes/tick, ingested bytes/tick, largest frame,
//     resync pulls/tick
//
// No transport, no QuickJS: N FleeceStateManager instances in one process
// exchange frames directly. Per-link loss is simulated with a deterministic
// RNG (xorshift64) so every run is reproducible.
//
// Loss model, mirroring the real runtime's two detection paths:
//   - DELIVERED frames are merged via import_ex(); its digest-based behind
//     flag is exact - any divergence schedules a unicast full-snapshot pull.
//   - DROPPED frames are invisible to the receiver by definition; like the
//     runtime's liveness probe, the sim marks a gap when the dropped frame
//     was "meaningful" (the sender's advertised view changed since its last
//     broadcast). Pull requests/replies are themselves subject to loss and
//     retry until they land.
//
// Usage:
//   bench_gossip [--csv]                       sweep N x loss table
//   bench_gossip [--csv] N loss [k [vlen]]     single configuration
//
// Workload: each node publishes k=3 world keys (~vlen=24-byte JSON-ish
// values), then rewrites them round-robin in the steady phase, like
// telemetry. All world fields share the manager's fixed 128-slot pool
// (FIELD_CAPACITY), so N*k must stay well below it - tombstones included.

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>

#include "state/fleece_state_manager.h"
#include "fleece_cbor.h"
#include "fleece_alloc.h"

#define MAX_NODES 64
#define STORE_CAPACITY 128    // FIELD_CAPACITY in fleece_state_manager.c
#define CONVERGE_TICK_CAP 5000
#define STEADY_TICKS 200      // steady-state measurement window
#define PULL_REQUEST_BYTES 6  // tiny 'FR' control frame, as the runtime sends

// Radio physics. A frame larger than the MTU is sent as ceil(size/MTU)
// packets; a packet-based link loses the WHOLE frame if any fragment drops
// (dumb fragmentation, no per-fragment ACKs - pessimistic but honest).
// Airtime is bytes-on-air divided by the raw channel rate (115200 baud,
// 8N1 => 11520 bytes/s; FEC/framing/CCSDS overhead ignored, so real
// goodput will be somewhat lower - treat these figures as upper bounds
// on achievable throughput).
#define RADIO_MTU 240
#define AIR_BYTES_PER_SEC 11520
#define PULL_COOLDOWN_TICKS 10  // min spacing between pulls per (receiver, sender)

// Per-frame transmission accounting: how many MTU-sized packets a frame
// occupies, and whether it survives the link (all fragments must arrive).
static uint32_t frame_packets(uint32_t size) {
    return (size + RADIO_MTU - 1) / RADIO_MTU;
}

// --- Deterministic RNG ------------------------------------------------------

static uint64_t rng_state;

static void rng_seed(uint64_t s) { rng_state = s ? s : 1; }

static uint64_t rng_next(void) {
    uint64_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    rng_state = x;
    return x;
}

static bool rng_drop(double p) {
    if (p <= 0.0) return false;
    if (p >= 1.0) return true;
    return (rng_next() % 1000000u) < (uint64_t)(p * 1000000.0);
}

// Whether a frame survives the link: every MTU fragment must arrive, so the
// effective drop probability compounds per packet.
static bool frame_survives(uint32_t size, double loss) {
    uint32_t packets = frame_packets(size);
    double keep = 1.0;
    for (uint32_t i = 0; i < packets; i++) keep *= (1.0 - loss);
    double drop = 1.0 - keep;
    return !rng_drop(drop > 1.0 ? 1.0 : drop);
}

// --- Simulated node ---------------------------------------------------------

typedef struct {
    FleeceStateManager* m;
    uint64_t id;
    uint64_t wm;                    // delta watermark, like FleeceRuntime.shared_gossip_watermark
    uint64_t last_advertised;       // digest this node last put on the air
    uint32_t k;                     // world keys owned
    bool need[MAX_NODES];           // need[s]: owe a full pull from node s
    uint32_t cooldown[MAX_NODES];   // ticks until pair (this, s) may pull again
    char (*keys)[FLEECE_FIELD_NAME_MAX];  // this node's own key names
    uint32_t steady_cursor;         // round-robin write cursor for the steady phase
} SimNode;

static void node_init(SimNode* n, uint32_t idx, uint32_t k, size_t vlen, uint64_t seed) {
    n->id = (uint64_t)(idx + 1);
    n->m = fleece_state_manager_create_with_node_id(n->id);
    n->k = k;
    n->keys = calloc(k, sizeof(*n->keys));

    // k uniquely-owned world keys with vlen-byte JSON-ish payloads.
    for (uint32_t f = 0; f < k; f++) {
        snprintf(n->keys[f], FLEECE_FIELD_NAME_MAX, "n%u-%u", idx, f);
        char val[128];
        snprintf(val, sizeof val, "{\"v\":%u,\"s\":%llu}", f, (unsigned long long)(seed & 0xFFFF));
        size_t len = strlen(val);
        while (len < vlen) val[len++] = 'x';  // pad to target payload size
        fleece_state_manager_set_shared(n->m, n->keys[f], (const uint8_t*)val, (uint32_t)vlen);
    }
    fleece_state_manager_stream_digest(n->m, FLEECE_SHARED_OWNER_ID, &n->last_advertised);
}

static void node_free(SimNode* n) {
    free(n->keys);
    fleece_state_manager_destroy(n->m);
}

// Steady-phase churn: rewrite the next owned key with fresh telemetry.
static void node_write_steady(SimNode* n, uint32_t tick) {
    char val[128];
    snprintf(val, sizeof val, "{\"t\":%u}", tick);
    size_t len = strlen(val);
    while (len < 24) val[len++] = 'x';
    fleece_state_manager_set_shared(n->m, n->keys[n->steady_cursor % n->k], (const uint8_t*)val, (uint32_t)len);
    n->steady_cursor++;
}

// One gossip tick across the swarm: delta broadcast per node, per-link loss
// (MTU-fragmented frames), then throttled unicast pull repair for gaps that
// survive a delivered frame's digest check.
//
// Pull semantics mirror fleece_runtime.c: need is SET when an import reports
// divergence (or a meaningful frame was dropped), and CLEARED as soon as any
// delivered frame from that sender reports us current. A pull whose snapshot
// cannot satisfy the gap yet (the sender is itself behind) just fails to
// clear the flag - it retries after the cooldown, like the runtime's probe.
static void sim_tick(SimNode* nodes, uint32_t count, double loss,
                     uint64_t* out_air, uint64_t* out_rx, uint32_t* out_max_frame,
                     uint32_t* out_pulls) {
    uint64_t air = 0, rx = 0;
    uint32_t max_frame = 0, pulls = 0;

    // Broadcast phase: one delta frame per node onto the air (O(N) radio),
    // delivered to each peer subject to loss (O(N^2) ingest worst case).
    for (uint32_t i = 0; i < count; i++) {
        SimNode* src = &nodes[i];
        uint8_t* frame = NULL;
        uint32_t size = 0;
        if (fleece_state_manager_export_shared_delta(src->m, src->wm, &frame, &size) != 0) continue;

        uint64_t digest = 0;
        fleece_state_manager_stream_digest(src->m, FLEECE_SHARED_OWNER_ID, &digest);
        src->wm = fleece_state_manager_get_shared_hw(src->m);

        uint32_t packets = frame_packets(size);
        if (packets > max_frame) max_frame = packets * RADIO_MTU;
        air += (uint64_t)packets * RADIO_MTU;  // padded packets are real airtime
        bool meaningful = (digest != src->last_advertised);
        src->last_advertised = digest;

        for (uint32_t j = 0; j < count; j++) {
            if (j == i) continue;
            if (!frame_survives(size, loss)) {
                // Dropped: the receiver cannot know - except that the sender's
                // advertised view moved, which is what triggers the probe path.
                if (meaningful && nodes[j].cooldown[i] == 0) nodes[j].need[i] = true;
                continue;
            }
            rx += size;
            bool behind = false;
            fleece_state_manager_import_ex(nodes[j].m, frame, size, NULL, &behind);
            if (behind) {
                nodes[j].need[i] = true;
            } else {
                nodes[j].need[i] = false;  // confirmed current with this sender
            }
        }
        fleece_free(frame);
    }

    // Pull-repair phase: the runtime's targeted anti-entropy handshake -
    // index request -> index reply -> local diff -> value request -> normal
    // gossip frame with just the missing records. Each leg is unicast and
    // subject to loss; a failed leg leaves the need flag set (cooldown-gated
    // retries). Benign-concurrency mismatches cost one index exchange and
    // then clear - no full snapshots are ever pulled.
    for (uint32_t r = 0; r < count; r++) {
        for (uint32_t s = 0; s < count; s++) {
            if (r == s || !nodes[r].need[s]) continue;
            if (nodes[r].cooldown[s] > 0) {
                nodes[r].cooldown[s]--;
                continue;
            }
            nodes[r].cooldown[s] = PULL_COOLDOWN_TICKS;

            // Leg 1+2: index request, index reply. The request is tiny; the
            // reply scales with the sender's key count (~5 B/entry).
            uint8_t* idx = NULL;
            uint32_t idx_size = 0;
            if (!frame_survives(PULL_REQUEST_BYTES, loss)) {
                air += frame_packets(PULL_REQUEST_BYTES) * RADIO_MTU;
                continue;
            }
            air += frame_packets(PULL_REQUEST_BYTES) * RADIO_MTU;
            if (fleece_state_manager_export_shared_index(nodes[s].m, &idx, &idx_size) != 0) continue;
            pulls++;
            uint32_t packets = frame_packets(idx_size);
            if (packets > max_frame) max_frame = packets * RADIO_MTU;
            air += (uint64_t)packets * RADIO_MTU;
            if (!frame_survives(idx_size, loss)) {
                fleece_free(idx);
                continue;
            }
            rx += idx_size;

            // Diff locally against our own store (this is what the runtime's
            // receive path does with an index reply).
            size_t pos = 3;
            uint8_t major;
            uint64_t value;
            uint32_t wanted[STORE_CAPACITY];
            uint32_t n_wanted = 0;
            bool parse_ok = fleece_cbor_read_head(idx, idx_size, &pos, &major, &value)
                            && major == 4 && value == 2
                            && fleece_cbor_read_head(idx, idx_size, &pos, &major, &value)
                            && major == 0 && value == 1
                            && fleece_cbor_read_head(idx, idx_size, &pos, &major, &value)
                            && major == 4;
            if (!parse_ok) {
                fleece_free(idx);
                continue;
            }
            uint64_t entries = value;
            for (uint64_t e = 0; e < entries; e++) {
                if (!fleece_cbor_read_head(idx, idx_size, &pos, &major, &value) || major != 4 || value != 2) { parse_ok = false; break; }
                uint64_t h, ts;
                if (!fleece_cbor_read_head(idx, idx_size, &pos, &major, &h) || major != 0) { parse_ok = false; break; }
                if (!fleece_cbor_read_head(idx, idx_size, &pos, &major, &ts) || major != 0) { parse_ok = false; break; }
                if (h > 0xFFFFFFFFULL) continue;
                int rc_at = fleece_state_manager_shared_at_least(nodes[r].m, (uint32_t)h, ts);
                if (rc_at == 0 && n_wanted < STORE_CAPACITY) wanted[n_wanted++] = (uint32_t)h;
            }
            fleece_free(idx);
            if (!parse_ok) continue;

            // Nothing missing: benign divergence - repair complete.
            if (n_wanted == 0) {
                nodes[r].need[s] = false;
                continue;
            }

            // Leg 3: value request (tiny), leg 4: targeted gossip frame.
            if (!frame_survives(PULL_REQUEST_BYTES + n_wanted * 5, loss)) {
                air += frame_packets(PULL_REQUEST_BYTES + n_wanted * 5) * RADIO_MTU;
                continue;
            }
            air += frame_packets(PULL_REQUEST_BYTES + n_wanted * 5) * RADIO_MTU;

            uint8_t* resp = NULL;
            uint32_t resp_size = 0;
            if (fleece_state_manager_export_shared_by_hash(nodes[s].m, wanted, n_wanted, &resp, &resp_size) != 0) continue;
            packets = frame_packets(resp_size);
            if (packets > max_frame) max_frame = packets * RADIO_MTU;
            air += (uint64_t)packets * RADIO_MTU;
            if (!frame_survives(resp_size, loss)) {
                fleece_free(resp);
                continue;
            }
            rx += resp_size;
            fleece_state_manager_import(nodes[r].m, resp, resp_size);
            fleece_free(resp);
        }
    }

    *out_air = air;
    *out_rx = rx;
    *out_max_frame = max_frame;
    *out_pulls = pulls;
}

// Converged = every node computes the SAME world-view digest and holds every
// expected key (digest equality alone could mask symmetric emptiness).
static bool swarm_converged(SimNode* nodes, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        char names[STORE_CAPACITY][FLEECE_FIELD_NAME_MAX];
        if (fleece_state_manager_list_fields(nodes[i].m, FLEECE_SHARED_OWNER_ID, names, STORE_CAPACITY) < (int)(count * nodes[i].k))
            return false;
    }
    uint64_t ref = 0;
    fleece_state_manager_stream_digest(nodes[0].m, FLEECE_SHARED_OWNER_ID, &ref);
    for (uint32_t i = 1; i < count; i++) {
        uint64_t d = 0;
        fleece_state_manager_stream_digest(nodes[i].m, FLEECE_SHARED_OWNER_ID, &d);
        if (d != ref) return false;
    }
    return true;
}

typedef struct {
    uint32_t conv_ticks;
    bool converged;
    uint64_t air_to_converge;      // padded radio bytes incl. pull repairs
    uint64_t rx_to_converge;
    double air_seconds_to_converge;  // air_to_converce / channel rate
    // Steady state (measured after convergence, same loss):
    uint64_t air_per_tick;
    double air_ms_per_tick;        // airtime consumed per gossip tick
    uint64_t rx_per_tick;
    uint32_t max_frame;
    uint32_t pulls_per_tick_x100;
} BenchResult;

// Total live world payload held by a node: names + values + per-record CBOR
// framing. The convergence yardstick is N copies of this - what a perfect
// protocol spends if every node simply floods the final object once.
static uint64_t world_payload_bytes(FleeceStateManager* m) {
    char names[STORE_CAPACITY][FLEECE_FIELD_NAME_MAX];
    uint32_t n = fleece_state_manager_list_fields(m, FLEECE_SHARED_OWNER_ID, names, STORE_CAPACITY);
    uint64_t total = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t* v = NULL;
        uint32_t vs = 0;
        if (fleece_state_manager_get_named(m, FLEECE_SHARED_OWNER_ID, names[i], &v, &vs) == 0) {
            free(v);
        }
        total += strlen(names[i]) + vs + 8;  // record array header + tombstone flag + ts
    }
    return total;
}

static void run_config(uint32_t count, double loss, uint32_t k, size_t vlen, BenchResult* res) {
    memset(res, 0, sizeof(*res));
    rng_seed(0xC0FFEE123456789AULL);

    SimNode nodes[MAX_NODES];
    for (uint32_t i = 0; i < count; i++) {
        memset(&nodes[i], 0, sizeof(SimNode));
        node_init(&nodes[i], i, k, vlen, (uint64_t)(i * 131 + 11));
    }

    // Yardstick: every node floods the final world object exactly once.
    uint64_t baseline = (uint64_t)count * world_payload_bytes(nodes[0].m);

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    uint32_t t = 0;
    while (t < CONVERGE_TICK_CAP) {
        uint64_t a = 0, r = 0;
        uint32_t m = 0, p = 0;
        sim_tick(nodes, count, loss, &a, &r, &m, &p);
        res->air_to_converge += a;
        res->rx_to_converge += r;
        t++;
        if (swarm_converged(nodes, count)) break;
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    res->conv_ticks = t;
    res->converged = swarm_converged(nodes, count);
    double wall_ms = (double)((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000);
    (void)wall_ms;
    res->air_seconds_to_converge = (double)res->air_to_converge / AIR_BYTES_PER_SEC;

    // Steady-state window at the same loss level: continuous telemetry churn.
    uint64_t air_acc = 0, rx_acc = 0;
    uint32_t mf = 0, pulls_acc = 0;
    for (uint32_t s = 0; s < STEADY_TICKS; s++) {
        for (uint32_t i = 0; i < count; i++) node_write_steady(&nodes[i], s);
        uint64_t a = 0, r = 0;
        uint32_t m = 0, p = 0;
        sim_tick(nodes, count, loss, &a, &r, &m, &p);
        air_acc += a;
        rx_acc += r;
        pulls_acc += p;
        if (m > mf) mf = m;
    }
    res->air_per_tick = air_acc / STEADY_TICKS;
    res->air_ms_per_tick = ((double)res->air_per_tick / AIR_BYTES_PER_SEC) * 1000.0;
    res->rx_per_tick = rx_acc / STEADY_TICKS;
    res->max_frame = mf;
    res->pulls_per_tick_x100 = (uint32_t)((pulls_acc * 100ULL) / STEADY_TICKS);

    for (uint32_t i = 0; i < count; i++) node_free(&nodes[i]);
}

int main(int argc, char** argv) {
    bool csv = false;
    int a = 1;
    for (; a < argc; a++) {
        if (strcmp(argv[a], "--csv") == 0) csv = true;
        else break;
    }

    uint32_t k = 3;
    size_t vlen = 24;
    bool single_mode = false;
    uint32_t n = 0;
    double loss = 0.0;
    if (argc - a >= 2) {
        single_mode = true;
        n = (uint32_t)atol(argv[a]);
        loss = atof(argv[a + 1]);
        if (argc - a >= 4) {
            k = (uint32_t)atol(argv[a + 2]);
            vlen = (size_t)atol(argv[a + 3]);
        }
    } else if (argc > a) {
        printf("usage: %s [--csv] [N loss [k [vlen]]]\n", argv[0]);
        return 1;
    }

    static const uint32_t N_SWEEP[] = {4, 8, 16, 24, 32, 48};
    static const double LOSS_SWEEP[] = {0.0, 0.1, 0.3, 0.5, 0.7};
    const uint32_t* ns = single_mode ? &n : N_SWEEP;
    const double* losses = single_mode ? &loss : LOSS_SWEEP;
    uint32_t n_ns = single_mode ? 1 : sizeof(N_SWEEP) / sizeof(N_SWEEP[0]);
    uint32_t n_losses = single_mode ? 1 : sizeof(LOSS_SWEEP) / sizeof(LOSS_SWEEP[0]);

    if (!csv) {
        printf("Fleece gossip bandwidth benchmark (lossy channels)\n");
        printf("===================================================\n");
        printf("protocol : world-only delta gossip + digest-gap pull repair\n");
        printf("radio    : MTU %u B/frame (fragmented frames lost whole), %d B/s channel\n",
               RADIO_MTU, AIR_BYTES_PER_SEC);
        printf("workload : %u world keys/node, %zu-byte values (%u slots/node, %d-slot pool)\n",
               k, vlen, k, STORE_CAPACITY);
        printf("steady   : %d ticks of telemetry churn, measured at each loss level\n", STEADY_TICKS);
        printf("RNG      : deterministic, reproducible\n\n");
    } else {
        printf("N,loss,k,vlen,converged,conv_ticks,air_seconds_to_conv,"
               "air_to_conv_total,payload_min_bytes,flood_baseline,overhead_x_flood,"
               "steady_air_B_per_tick,steady_air_ms_per_tick,max_frame_packets_xMTU,pulls_per_100ticks\n");
    }

    for (uint32_t si = 0; si < n_ns; si++) {
        uint32_t N = ns[si];
        if (!csv) printf("=== N=%u ===\n", N);
        for (uint32_t li = 0; li < n_losses; li++) {
            double loss_rate = losses[li];
            BenchResult res;
            run_config(N, loss_rate, k, vlen, &res);

            // Baselines: payload floor (raw unique data) and the flood baseline
            // (every node transmits the final object once - what a naive
            // periodic full-state broadcast costs per round).
            uint64_t payload_min = (uint64_t)N * k * (vlen + 10);

            if (csv) {
                // Recompute flood baseline cheaply: N copies of the world object.
                uint64_t flood = 0;
                {
                    SimNode tmp;
                    memset(&tmp, 0, sizeof(tmp));
                    node_init(&tmp, 0, k, vlen, 11);  // representative single-node state
                    flood = (uint64_t)N * world_payload_bytes(tmp.m);
                    node_free(&tmp);
                }
                printf("%u,%.2f,%u,%zu,%s,%u,%.3f,%llu,%llu,%llu,%.2f,%llu,%.2f,%u,%u\n",
                       N, loss_rate, k, vlen, res.converged ? "yes" : "no",
                       res.conv_ticks, res.air_seconds_to_converge,
                       (unsigned long long)res.air_to_converge,
                       (unsigned long long)payload_min,
                       (unsigned long long)flood,
                       flood ? (double)res.air_to_converge / (double)flood : 0.0,
                       (unsigned long long)res.air_per_tick,
                       res.air_ms_per_tick,
                       frame_packets(res.max_frame),
                       res.pulls_per_tick_x100);
            } else {
                printf("  loss=%3.0f%%: %s in %u ticks, %.2f s airtime",
                       loss_rate * 100.0,
                       res.converged ? "converged" : "DID NOT CONVERGE",
                       res.conv_ticks, res.air_seconds_to_converge);
                if (res.converged) {
                    SimNode tmp;
                    memset(&tmp, 0, sizeof(tmp));
                    node_init(&tmp, 0, k, vlen, 11);
                    uint64_t flood = (uint64_t)N * world_payload_bytes(tmp.m);
                    node_free(&tmp);
                    printf(", air-to-converge %llu B (%.1fx one-shot flood)",
                           (unsigned long long)res.air_to_converge,
                           flood ? (double)res.air_to_converge / (double)flood : 0.0);
                }
                printf("\n");
                printf("    steady: tx %llu B/tick (%.1f ms airtime/tick), ingest %llu B/tick, max %u x %u B frames, %u pulls/100 ticks\n",
                       (unsigned long long)res.air_per_tick, res.air_ms_per_tick,
                       (unsigned long long)res.rx_per_tick,
                       frame_packets(res.max_frame), RADIO_MTU,
                       res.pulls_per_tick_x100);
            }
        }
        if (!csv) printf("\n");
    }
    return 0;
}
