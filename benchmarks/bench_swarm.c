// Fleece headless swarm benchmark
//
// Sweeps node count x packet-loss rate over the same gossip machinery the
// runtime drives (delta gossip with a periodic full resync - see
// src/runtime/fleece_runtime.c Phase 3), and reports:
//   - ticks to converge every node's view of the swarm (link-loss resilience)
//   - steady-state gossip bytes sent per tick and largest frame (scale)
//   - export+import wall time per tick and allocator churn (CPU/RAM cost)
//   - fields stored vs the fixed 128-field store (capacity ceiling)
//
// No transport, no QuickJS: N FleeceStateManager instances in one process
// exchange frames directly. Per-link loss is simulated with a deterministic
// RNG so every run is reproducible.
//
// Usage:
//   bench_swarm [--csv]                     full sweep (N x loss table)
//   bench_swarm [--csv] N loss [nfields [shared]]   single configuration
//
// Default workload: nfields=3 fields per node (1 probe + 2 sensors) plus
// shared_count=2 world fields. All nodes' data shares one 128-slot pool
// (FIELD_CAPACITY in src/state/fleece_state_manager.c), so the swarm hits a
// hard ceiling when N*nfields + shared_count exceeds 128 - the benchmark
// reports exactly where.

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <sys/resource.h>

#include "state/fleece_state_manager.h"
#include "fleece_alloc.h"

#define FULL_RESYNC_TICKS 50    // periodic-sim mode only; the library now uses on-demand resync
#define CONVERGE_TICK_CAP 1000  // >1000 ticks => reported as FAIL
#define STEADY_TICKS 100        // length of the steady-state throughput phase
#define MAX_NODES 128
#define STORE_CAPACITY 128      // FIELD_CAPACITY in fleece_state_manager.c
#define REQUEST_FRAME_SIZE 6    // tiny "send me your state" frame (magic+version+type+owner+target)

// Anti-entropy policy:
//   GOSSIP_PERIODIC - full resync broadcast every FULL_RESYNC_TICKS (current
//                     runtime behavior; wasteful but recovers any dropped delta).
//   GOSSIP_ONDEMAND - delta push only; a node that detects it missed a frame
//                     asks the sender for a full snapshot (pull repair). No
//                     periodic resync at all - the "ask for it" proposal.
typedef enum { GOSSIP_PERIODIC, GOSSIP_ONDEMAND } GossipMode;

// --- Deterministic RNG (xorshift64) so runs are reproducible ---------------

static uint64_t rng_state = 0x9E3779B97F4A7C15ULL;

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

// --- Counting allocator (churn measurement) --------------------------------

static uint64_t alloc_calls = 0;

static void* count_malloc(size_t s) { alloc_calls++; return malloc(s); }
static void* count_calloc(size_t c, size_t s) { alloc_calls++; return calloc(c, s); }
static void* count_realloc(void* p, size_t s) { alloc_calls++; return realloc(p, s); }
static void count_free(void* p) { free(p); }

// --- Simulated swarm node ---------------------------------------------------

typedef struct {
    FleeceStateManager* m;
    uint64_t id;
    uint64_t self_wm;    // gossip watermark, like FleeceRuntime.self_gossip_watermark
    uint64_t shared_wm;  // ... and shared_gossip_watermark
    // On-demand pull state.
    uint64_t self_hw;          // this node's current self-stream high-water mark
    uint64_t shared_hw;        // ... shared-stream high-water mark
    uint64_t last_self_sent_hw;   // hw at the last self frame this node sent
    uint64_t last_shared_sent_hw;
    bool need_self[MAX_NODES];    // [s] set when this node missed a self frame from node s
    bool need_shared[MAX_NODES];  // ... shared frame
} SimNode;

static void node_write_init(SimNode* n, uint32_t nfields, uint32_t shared_count, uint64_t seed) {
    char name[FLEECE_FIELD_NAME_MAX];

    // One stable, uniquely-identified "probe" field: convergence is defined as
    // every node holding every peer's probe. Its value never changes, so a
    // single successful delivery is enough - any non-convergence is then purely
    // a dropped-frame / capacity problem, not an LWW race.
    uint8_t probe = (uint8_t)(n->id & 0xFF);
    fleece_state_manager_set_named(n->m, "probe", &probe, 1);
    n->self_hw = fleece_state_manager_get_local_timestamp(n->m);  // set_named stamps this value

    // Remaining self fields are "sensors", bumped every tick in the steady
    // phase so delta gossip keeps carrying traffic.
    for (uint32_t k = 1; k < nfields; k++) {
        snprintf(name, sizeof name, "s%u", k);
        uint8_t val = (uint8_t)(seed & 0xFF);
        fleece_state_manager_set_named(n->m, name, &val, 1);
        n->self_hw = fleece_state_manager_get_local_timestamp(n->m);
    }

    // Shared "world" fields: every node writes the same names, so they cost
    // shared_count slots in the pool total, not N*shared_count.
    for (uint32_t k = 0; k < shared_count; k++) {
        snprintf(name, sizeof name, "world%u", k);
        uint8_t val = (uint8_t)(seed & 0xFF);
        fleece_state_manager_set_shared(n->m, name, &val, 1);
        n->shared_hw = fleece_state_manager_get_local_timestamp(n->m);
    }
}

static void node_write_tick(SimNode* n, uint32_t nfields, uint32_t tick) {
    char name[FLEECE_FIELD_NAME_MAX];
    for (uint32_t k = 1; k < nfields; k++) {
        snprintf(name, sizeof name, "s%u", k);
        uint8_t val = (uint8_t)(tick + k);
        fleece_state_manager_set_named(n->m, name, &val, 1);
        n->self_hw = fleece_state_manager_get_local_timestamp(n->m);
    }
    if ((tick % 10) == 0) {  // shared world churn every 10 ticks
        uint8_t val = (uint8_t)tick;
        fleece_state_manager_set_shared(n->m, "world0", &val, 1);
        n->shared_hw = fleece_state_manager_get_local_timestamp(n->m);
    }
}

// One runtime tick across the whole swarm. Each node exports its self stream
// (delta, or full on the resync cadence in PERIODIC mode) and its shared
// stream, then transmits each frame once on the air (a broadcast, like the
// runtime's single fleece_comms_send("broadcast", ...)) and delivers it to
// each peer with per-link loss probability `loss`.
//
// Two costs are reported separately, because they scale differently:
//   air_bytes - bytes actually transmitted on the radio. One frame per node
//               per stream, so O(N) - independent of how many peers receive.
//   rx_bytes  - bytes ingested by receiving nodes (import/merge work). Each
//               delivered frame is parsed once per peer, so O(N) per node and
//               O(N^2) across the swarm - inherent to every node replicating
//               every peer's state.
//
// In ONDEMAND mode there is no periodic resync at all. A node that missed a
// meaningful frame (the sender's high-water mark advanced past what it holds)
// sets a need flag and pulls a full snapshot from that sender with a small
// unicast request, retrying each tick until the full export arrives. That
// bounds anti-entropy traffic to actual gaps instead of broadcasting full
// state every FULL_RESYNC_TICKS.
static void sim_tick(SimNode* nodes, uint32_t count, double loss, uint32_t tick_index,
                     uint32_t nfields, uint32_t shared_count, GossipMode mode,
                     uint64_t* out_air_bytes, uint64_t* out_rx_bytes, uint32_t* out_max_frame) {
    (void)nfields; (void)shared_count;
    bool full_resync = (mode == GOSSIP_PERIODIC) && (tick_index % FULL_RESYNC_TICKS) == 0;
    uint64_t air = 0;
    uint64_t rx = 0;
    uint32_t max_frame = 0;

    for (uint32_t i = 0; i < count; i++) {
        SimNode* src = &nodes[i];
        fleece_state_manager_tick(src->m);  // peer liveness, as the runtime does

        uint8_t* self_frame = NULL;
        uint32_t self_size = 0;
        int rc = full_resync
            ? fleece_state_manager_export(src->m, &self_frame, &self_size)
            : fleece_state_manager_export_delta(src->m, src->self_wm, &self_frame, &self_size);
        src->self_wm = fleece_state_manager_get_local_timestamp(src->m);
        if (rc == 0) {
            if (self_size > max_frame) max_frame = self_size;
            air += self_size;  // one broadcast transmission, regardless of peers
            bool meaningful = (src->self_hw > src->last_self_sent_hw);
            for (uint32_t j = 0; j < count; j++) {
                if (j == i) continue;
                if (rng_drop(loss)) {
                    // R knows it fell behind A only if the frame carried new data.
                    if (mode == GOSSIP_ONDEMAND && meaningful) nodes[j].need_self[i] = true;
                } else {
                    fleece_state_manager_import(nodes[j].m, self_frame, self_size);
                    rx += self_size;
                }
            }
            src->last_self_sent_hw = src->self_hw;
            fleece_free(self_frame);
        }

        uint8_t* shared_frame = NULL;
        uint32_t shared_size = 0;
        rc = full_resync
            ? fleece_state_manager_export_shared(src->m, &shared_frame, &shared_size)
            : fleece_state_manager_export_shared_delta(src->m, src->shared_wm, &shared_frame, &shared_size);
        src->shared_wm = fleece_state_manager_get_local_timestamp(src->m);
        if (rc == 0) {
            if (shared_size > max_frame) max_frame = shared_size;
            air += shared_size;
            bool meaningful = (src->shared_hw > src->last_shared_sent_hw);
            for (uint32_t j = 0; j < count; j++) {
                if (j == i) continue;
                if (rng_drop(loss)) {
                    if (mode == GOSSIP_ONDEMAND && meaningful) nodes[j].need_shared[i] = true;
                } else {
                    fleece_state_manager_import(nodes[j].m, shared_frame, shared_size);
                    rx += shared_size;
                }
            }
            src->last_shared_sent_hw = src->shared_hw;
            fleece_free(shared_frame);
        }
    }

    // On-demand pull repair: every node with an outstanding gap asks the
    // sender for a full snapshot (unicast request, unicast response). A request
    // or response that drops leaves the need flag set - retried next tick.
    if (mode == GOSSIP_ONDEMAND) {
        for (uint32_t r = 0; r < count; r++) {
            for (uint32_t s = 0; s < count; s++) {
                if (r == s) continue;

                if (nodes[r].need_self[s]) {
                    air += REQUEST_FRAME_SIZE;  // request r->s (unicast)
                    if (!rng_drop(loss)) {
                        uint8_t* resp = NULL;
                        uint32_t resp_size = 0;
                        if (fleece_state_manager_export(nodes[s].m, &resp, &resp_size) == 0) {
                            if (resp_size > max_frame) max_frame = resp_size;
                            air += resp_size;  // full snapshot s->r (unicast)
                            if (!rng_drop(loss)) {
                                fleece_state_manager_import(nodes[r].m, resp, resp_size);
                                rx += resp_size;
                                nodes[r].need_self[s] = false;  // gap closed
                            }
                            fleece_free(resp);
                        }
                    }
                }

                if (nodes[r].need_shared[s]) {
                    air += REQUEST_FRAME_SIZE;
                    if (!rng_drop(loss)) {
                        uint8_t* resp = NULL;
                        uint32_t resp_size = 0;
                        if (fleece_state_manager_export_shared(nodes[s].m, &resp, &resp_size) == 0) {
                            if (resp_size > max_frame) max_frame = resp_size;
                            air += resp_size;
                            if (!rng_drop(loss)) {
                                fleece_state_manager_import(nodes[r].m, resp, resp_size);
                                rx += resp_size;
                                nodes[r].need_shared[s] = false;
                            }
                            fleece_free(resp);
                        }
                    }
                }
            }
        }
    }

    if (out_air_bytes) *out_air_bytes = air;
    if (out_rx_bytes) *out_rx_bytes = rx;
    if (out_max_frame) *out_max_frame = max_frame;
}

static bool swarm_converged(SimNode* nodes, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        for (uint32_t j = 0; j < count; j++) {
            if (i == j) continue;
            if (!fleece_state_manager_exists_named(nodes[i].m, nodes[j].id, "probe"))
                return false;
        }
    }
    return true;
}

// Fields actually stored in this node's pool (own + peers + shared) - the
// benchmark's view of how much of the 128-slot store the swarm is using.
static uint32_t count_stored_fields(SimNode* n) {
    uint64_t owners[MAX_NODES];
    char names[128][FLEECE_FIELD_NAME_MAX];
    uint32_t total = 0;
    total += fleece_state_manager_list_fields(n->m, n->id, names, 128);
    uint32_t n_owners = fleece_state_manager_list_nodes(n->m, owners, MAX_NODES);
    for (uint32_t k = 0; k < n_owners; k++) {
        if (owners[k] == n->id) continue;  // own fields already counted above
        total += fleece_state_manager_list_fields(n->m, owners[k], names, 128);
    }
    total += fleece_state_manager_list_fields(n->m, FLEECE_SHARED_OWNER_ID, names, 128);
    return total;
}

// Run one (N, loss) configuration: fresh swarm, gossip until convergence (or
// the tick cap), then a 0-loss steady-state throughput phase.
static void run_config(uint32_t count, double loss, uint32_t nfields, uint32_t shared_count,
                       GossipMode mode,
                       uint32_t* conv_ticks, bool* converged, uint32_t* fields_stored,
                       uint64_t* air_to_converge, uint64_t* rx_to_converge,
                       uint64_t* air_bytes_per_tick, uint64_t* rx_bytes_per_tick,
                       uint32_t* max_frame, double* wall_us_per_tick, uint64_t* allocs_per_tick) {
    SimNode nodes[MAX_NODES];
    rng_seed(0x9E3779B97F4A7C15ULL);  // same stream for every config: comparable runs

    for (uint32_t i = 0; i < count; i++) {
        memset(&nodes[i], 0, sizeof(SimNode));
        nodes[i].id = (uint64_t)i + 1;
        nodes[i].m = fleece_state_manager_create_with_node_id(nodes[i].id);
        node_write_init(&nodes[i], nfields, shared_count, (uint64_t)(i * 31 + 7));
    }

    // Phase A: convergence (anti-entropy under loss). Total radio bytes spent
    // during recovery is tracked separately from steady-state cost - under
    // loss this is where periodic's full-resync broadcasts really cost airtime,
    // while on-demand spends only tiny requests plus snapshots to stragglers.
    uint32_t t = 0;
    uint64_t air_conv = 0;
    uint64_t rx_conv = 0;
    while (t < CONVERGE_TICK_CAP) {
        uint64_t a = 0, r = 0;
        sim_tick(nodes, count, loss, t, nfields, shared_count, mode, &a, &r, NULL);
        air_conv += a;
        rx_conv += r;
        t++;
        if (swarm_converged(nodes, count)) break;
    }
    *conv_ticks = t;
    *converged = (t < CONVERGE_TICK_CAP);
    *air_to_converge = air_conv;
    *rx_to_converge = rx_conv;
    *fields_stored = count_stored_fields(&nodes[0]);

    // Phase B: steady-state throughput/churn, always at 0% loss and measured
    // on the just-converged swarm (sensors bump every tick, world churn every
    // 10) - this is what one runtime loop tick costs at scale.
    if (loss == 0.0) {
        alloc_calls = 0;
        uint64_t air_acc = 0;
        uint64_t rx_acc = 0;
        uint32_t mf = 0;
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (uint32_t s = 0; s < STEADY_TICKS; s++) {
            for (uint32_t i = 0; i < count; i++) {
                node_write_tick(&nodes[i], nfields, s);
            }
            uint64_t a = 0, r = 0;
            uint32_t m = 0;
            sim_tick(nodes, count, 0.0, s, nfields, shared_count, mode, &a, &r, &m);
            air_acc += a;
            rx_acc += r;
            if (m > mf) mf = m;
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        uint64_t us = (uint64_t)((t1.tv_sec - t0.tv_sec) * 1000000ull
                                 + (t1.tv_nsec - t0.tv_nsec) / 1000);
        *air_bytes_per_tick = air_acc / STEADY_TICKS;
        *rx_bytes_per_tick = rx_acc / STEADY_TICKS;
        *max_frame = mf;
        *wall_us_per_tick = (double)us / STEADY_TICKS;
        *allocs_per_tick = alloc_calls / STEADY_TICKS;
    }

    for (uint32_t i = 0; i < count; i++) {
        fleece_state_manager_destroy(nodes[i].m);
    }
}

// --- Reporting -------------------------------------------------------------

static void print_usage(const char* argv0) {
    printf("usage: %s [--csv] [--ondemand] [--compare] [N loss [nfields [shared]]]\n", argv0);
    printf("  default: sweep N in {4,8,16,24,32,40,48} x loss in {0,0.1,0.3,0.5,0.8}\n");
    printf("  default workload: nfields=3 per node + shared=2 world fields\n");
    printf("  --ondemand: pull-repair anti-entropy (resync only when a node asks)\n");
    printf("  --compare : run periodic vs on-demand side by side (sweep mode)\n");
}

int main(int argc, char** argv) {
    bool csv = false;
    bool ondemand = false;
    bool compare = false;
    int a = 1;
    for (; a < argc; a++) {
        if (strcmp(argv[a], "--csv") == 0) csv = true;
        else if (strcmp(argv[a], "--ondemand") == 0) ondemand = true;
        else if (strcmp(argv[a], "--compare") == 0) compare = true;
        else break;
    }
    GossipMode mode = ondemand ? GOSSIP_ONDEMAND : GOSSIP_PERIODIC;

    uint32_t nfields = 3;
    uint32_t shared_count = 2;
    bool single_mode = false;
    uint32_t single_n = 0;
    double single_loss = 0.0;

    if (argc - a >= 2) {
        single_mode = true;
        single_n = (uint32_t)atol(argv[a]);
        single_loss = atof(argv[a + 1]);
        if (argc - a >= 4) {
            nfields = (uint32_t)atol(argv[a + 2]);
            shared_count = (uint32_t)atol(argv[a + 3]);
        }
    } else if (argc > a) {
        print_usage(argv[0]);
        return 1;
    }

    static const uint32_t N_SWEEP[] = {4, 8, 16, 24, 32, 40, 48};
    static const double LOSS_SWEEP[] = {0.0, 0.1, 0.3, 0.5, 0.8};
    const uint32_t* ns;
    const double* losses;
    uint32_t n_sizes, n_losses;
    if (single_mode) {
        ns = &single_n;
        losses = &single_loss;
        n_sizes = 1;
        n_losses = 1;
    } else {
        ns = N_SWEEP;
        losses = LOSS_SWEEP;
        n_sizes = sizeof(N_SWEEP) / sizeof(N_SWEEP[0]);
        n_losses = sizeof(LOSS_SWEEP) / sizeof(LOSS_SWEEP[0]);
    }

    // Install the counting allocator before creating any fleece object.
    fleece_malloc_fn = count_malloc;
    fleece_free_fn = count_free;
    fleece_calloc_fn = count_calloc;
    fleece_realloc_fn = count_realloc;

    // Theoretical swarm capacity of the fixed store: every node's fields share
    // one 128-slot pool, so N*nfields + shared_count must fit.
    uint32_t capacity_units = (STORE_CAPACITY - shared_count) / nfields;

    if (!csv) {
        printf("Fleece headless swarm benchmark\n");
        printf("================================\n");
        printf("workload : %u self fields/node + %u shared world fields\n", nfields, shared_count);
        printf("store    : %d-slot pool shared by ALL nodes (capacity ~ %u units at this workload)\n",
               STORE_CAPACITY, capacity_units);
        if (compare) {
            printf("gossip   : comparing PERIODIC (full resync every %d ticks) vs ONDEMAND (pull repair)\n",
                   FULL_RESYNC_TICKS);
        } else {
            printf("gossip   : %s\n", mode == GOSSIP_ONDEMAND
                       ? "delta per tick, pull-repair resync only when a node asks"
                       : "delta per tick, full resync every %d ticks (mirrors fleece_runtime)");
        }
        printf("steady   : %d ticks, sensors written every tick, world every 10\n", STEADY_TICKS);
        printf("RNG      : deterministic seed, runs reproducible\n\n");
    }

    struct rusage before, after;
    getrusage(RUSAGE_SELF, &before);

    if (csv) {
        printf("N,loss,mode,fields_stored,cap_units,conv_ticks,converged,"
               "air_to_converge,rx_to_converge,"
               "air_bytes_per_tick,rx_bytes_per_tick,max_frame,wall_us_per_tick,allocs_per_tick\n");
    }

    for (uint32_t si = 0; si < n_sizes; si++) {
        uint32_t N = ns[si];

        if (!csv && !single_mode && !compare) {
            printf("=== N=%u  (need %u fields, store holds %d) ===\n", N,
                   N * nfields + shared_count, STORE_CAPACITY);
        }

        for (uint32_t li = 0; li < n_losses; li++) {
            double loss = losses[li];

            // Periodic results (needed for --compare; otherwise the selected mode).
            uint32_t p_conv = 0, p_fields = 0, p_max = 0;
            bool p_ok = false;
            uint64_t p_airc = 0, p_rxc = 0, p_air = 0, p_rx = 0, p_allocs = 0;
            double p_us = 0;
            if (compare || mode == GOSSIP_PERIODIC) {
                run_config(N, loss, nfields, shared_count, GOSSIP_PERIODIC,
                           &p_conv, &p_ok, &p_fields,
                           &p_airc, &p_rxc, &p_air, &p_rx, &p_max, &p_us, &p_allocs);
            }

            // On-demand results (needed for --compare; otherwise only if selected).
            uint32_t o_conv = 0, o_fields = 0, o_max = 0;
            bool o_ok = false;
            uint64_t o_airc = 0, o_rxc = 0, o_air = 0, o_rx = 0, o_allocs = 0;
            double o_us = 0;
            if (compare || mode == GOSSIP_ONDEMAND) {
                run_config(N, loss, nfields, shared_count, GOSSIP_ONDEMAND,
                           &o_conv, &o_ok, &o_fields,
                           &o_airc, &o_rxc, &o_air, &o_rx, &o_max, &o_us, &o_allocs);
            }

            if (csv) {
                if (compare) {
                    printf("%u,%.2f,periodic,%u,%u,%u,%s,%llu,%llu,%llu,%llu,%u,%.1f,%llu\n",
                           N, loss, p_fields, capacity_units, p_conv, p_ok ? "yes" : "no",
                           (unsigned long long)p_airc, (unsigned long long)p_rxc,
                           (unsigned long long)p_air, (unsigned long long)p_rx,
                           p_max, p_us, (unsigned long long)p_allocs);
                    printf("%u,%.2f,ondemand,%u,%u,%u,%s,%llu,%llu,%llu,%llu,%u,%.1f,%llu\n",
                           N, loss, o_fields, capacity_units, o_conv, o_ok ? "yes" : "no",
                           (unsigned long long)o_airc, (unsigned long long)o_rxc,
                           (unsigned long long)o_air, (unsigned long long)o_rx,
                           o_max, o_us, (unsigned long long)o_allocs);
                } else {
                    uint32_t fields = p_fields ? p_fields : o_fields;
                    uint32_t conv = p_conv ? p_conv : o_conv;
                    bool ok = p_ok || o_ok;
                    printf("%u,%.2f,%s,%u,%u,%u,%s,%llu,%llu,%llu,%llu,%u,%.1f,%llu\n",
                           N, loss, mode == GOSSIP_ONDEMAND ? "ondemand" : "periodic",
                           fields, capacity_units, conv, ok ? "yes" : "no",
                           (unsigned long long)(p_airc ? p_airc : o_airc),
                           (unsigned long long)(p_rxc ? p_rxc : o_rxc),
                           (unsigned long long)(p_air ? p_air : o_air),
                           (unsigned long long)(p_rx ? p_rx : o_rx),
                           p_max ? p_max : o_max, p_us ? p_us : o_us,
                           (unsigned long long)(p_allocs ? p_allocs : o_allocs));
                }
            } else if (compare) {
                // Side-by-side: periodic -> on-demand.
                printf("N=%u loss=%3.0f%%: conv %u->%u ticks (%s%s)",
                       N, loss * 100.0, p_conv, o_conv, p_ok ? "ok" : "FAIL", o_ok ? "" : "->FAIL");
                if (loss == 0.0) {
                    printf(", air %llu->%llu B/t (steady), max frame %u->%u B",
                           (unsigned long long)p_air, (unsigned long long)o_air,
                           p_max, o_max);
                } else {
                    printf(", air-to-converge %llu->%llu B",
                           (unsigned long long)p_airc, (unsigned long long)o_airc);
                }
                printf("\n");
            } else if (single_mode) {
                uint32_t fields = p_fields ? p_fields : o_fields;
                uint32_t conv = p_conv ? p_conv : o_conv;
                bool ok = p_ok || o_ok;
                uint64_t airc = p_airc ? p_airc : o_airc;
                uint64_t air = p_air ? p_air : o_air;
                uint64_t rx = p_rx ? p_rx : o_rx;
                uint32_t mf = p_max ? p_max : o_max;
                uint64_t allocs = p_allocs ? p_allocs : o_allocs;
                double us = p_us ? p_us : o_us;
                printf("N=%u loss=%.0f%% (%s): fields=%u/%d, conv=%u ticks (%s), air-to-converge %llu B\n",
                       N, loss * 100.0, mode == GOSSIP_ONDEMAND ? "ondemand" : "periodic",
                       fields, STORE_CAPACITY, conv, ok ? "converged" : "FAIL",
                       (unsigned long long)airc);
                if (loss == 0.0) {
                    printf("  steady: air %llu B/tick (O(N)), ingest %llu B/tick, max frame %u B, %.1f us/tick, %llu allocs/tick\n",
                           (unsigned long long)air, (unsigned long long)rx,
                           mf, us, (unsigned long long)allocs);
                } else {
                    printf("  steady: (measured at 0%% loss only)\n");
                }
            } else {
                uint32_t conv = p_conv ? p_conv : o_conv;
                bool ok = p_ok || o_ok;
                printf("  [%s] loss=%3.0f%%: conv=%u ticks (%s), air-to-converge %llu B\n",
                       mode == GOSSIP_ONDEMAND ? "ondemand" : "periodic",
                       loss * 100.0, conv, ok ? "ok" : "FAIL",
                       (unsigned long long)(p_airc ? p_airc : o_airc));
                if (loss == 0.0) {
                    printf("  steady : air %llu B/tick (O(N)), ingest %llu B/tick, max frame %u B, %.1f us/tick, %llu allocs/tick, fields %u/%d\n",
                           (unsigned long long)(p_air ? p_air : o_air),
                           (unsigned long long)(p_rx ? p_rx : o_rx),
                           p_max ? p_max : o_max, p_us ? p_us : o_us,
                           (unsigned long long)(p_allocs ? p_allocs : o_allocs),
                           p_fields ? p_fields : o_fields, STORE_CAPACITY);
                }
            }
        }
        if (!csv && !single_mode && !compare) printf("\n");
    }

    getrusage(RUSAGE_SELF, &after);
    long peak_kb = after.ru_maxrss > before.ru_maxrss ? after.ru_maxrss : before.ru_maxrss;
    if (!csv) {
        printf("peak RSS: %ld KB\n", peak_kb);
    }

    return 0;
}
