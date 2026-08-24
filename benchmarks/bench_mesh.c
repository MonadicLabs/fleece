// Fleece mesh integration benchmark - the REAL stack, lossy radio.
//
// Unlike bench_swarm/bench_gossip (which drive FleeceStateManager instances
// directly against a synthetic link model), this benchmark runs the actual
// shipped transport path end to end:
//
//   script-free runtime loop -> FleeceComms -> fleece_reticulum (microReticulum,
//   announces, destinations, Resource/Link ARQ for oversized payloads)
//   -> host radio callbacks -> UDP multicast "radio" in this file.
//
// The harness acts as the radio, which is where truth lives:
//   - every byte counted at the wire boundary (tx/rx, per node),
//   - deterministic per-packet drop injection BEFORE the packet leaves,
//     so Reticulum's own recovery mechanisms (PROVE, Resource retries) have
//     to fight it exactly as they would on a real link,
//   - baud pacing: each node is throttled to an equal 1/N share of the
//     channel rate, modelling fair shared-medium access.
//
// One process per node (microReticulum has global state), forked from here;
// each writes a result file the parent aggregates into the final report.
//
// Usage:
//   bench_mesh [N] [loss_pct] [ticks] [mtu] [k] [vlen]
//
// Defaults: N=4, loss=20%, ticks=300 (~30 s of runtime loop), MTU=500 (the
// Reticulum default; LoRa-class radios cap lower), k=3 world keys/node with
// 24-byte values.

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <fcntl.h>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>

#include "fleece_runtime.h"
#include "fleece_comms.h"
#include "reticulum/fleece_reticulum.h"

#define CHANNEL_BPS 115200        // the radio's raw rate (115200 baud, 8N1)
#define MULTICAST_GROUP "239.255.0.42"
#define TICK_SECONDS 0.1          // matches fleece_runtime's internal pacing
#define STORE_REPORT_MAX 128      // FIELD_CAPACITY; caps the result-file listing

static volatile sig_atomic_t g_run = 1;
static void on_signal(int sig) { (void)sig; g_run = 0; }

// --- Deterministic per-node RNG for drop injection --------------------------

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

// --- Radio context (one per node process) -----------------------------------

typedef struct {
    uint32_t idx;
    int fd;
    struct sockaddr_in dest;
    uint32_t n_nodes;      // channel-share divisor for pacing
    double drop_p;
    uint64_t seq;
    uint64_t tx_packets, tx_bytes, rx_packets, rx_bytes, dropped, fx_tx, fx_rx;
} RadioCtx;

static bool radio_send(const uint8_t* data, uint32_t size, void* user_data) {
    RadioCtx* r = (RadioCtx*)user_data;
    r->tx_packets++;
    r->tx_bytes += size;
    if (size >= 2 && data[0] == 'F' && data[1] == 'X') r->fx_tx++;

    rng_seed((r->idx + 1) * 0x9E3779B97F4A7C15ULL ^ (r->seq++ * 0xFF51AFD7));
    if (rng_drop(r->drop_p)) {
        r->dropped++;
        return true;  // silently eaten by the "channel"
    }

    // Fair shared-medium pacing: this node owns 1/N of the channel.
    useconds_t us = (useconds_t)((uint64_t)size * 8ULL * 1000000ULL * r->n_nodes / CHANNEL_BPS);
    if (us > 0) usleep(us);

    if (sendto(r->fd, data, size, 0, (struct sockaddr*)&r->dest, sizeof(r->dest)) < 0) {
        return false;
    }
    return true;
}

static uint32_t radio_receive(uint8_t* out, uint32_t max_size, void* user_data) {
    RadioCtx* r = (RadioCtx*)user_data;
    ssize_t n = recvfrom(r->fd, out, max_size, 0, NULL, NULL);
    if (n <= 0) return 0;
    if (n >= 2 && out[0] == 'F' && out[1] == 'X') r->fx_rx++;
    r->rx_packets++;
    r->rx_bytes += (uint64_t)n;
    return (uint32_t)n;
}

// --- Identity plumbing (per-process files, deterministic divergence) --------

static char g_dir[256];
static uint32_t g_idx;

static bool identity_load(uint8_t* out, uint32_t size, void* user_data) {
    (void)user_data;
    if (size != FLEECE_RETICULUM_IDENTITY_KEY_SIZE) return false;
    char path[512];
    snprintf(path, sizeof path, "%s/node%u.id", g_dir, g_idx);
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    size_t n = fread(out, 1, size, f);
    fclose(f);
    return n == size;
}

static int identity_save(const uint8_t* key, uint32_t size, void* user_data) {
    (void)user_data;
    char path[512];
    snprintf(path, sizeof path, "%s/node%u.id", g_dir, g_idx);
    FILE* f = fopen(path, "wb");
    if (!f) return -1;
    fwrite(key, 1, size, f);
    fclose(f);
    return 0;
}

static uint32_t device_id(uint8_t* out, uint32_t size, void* user_data) {
    (void)user_data;
    if (size < 4) return 0;
    out[0] = (uint8_t)(g_idx);
    out[1] = (uint8_t)(g_idx >> 8);
    out[2] = (uint8_t)(g_idx >> 16);
    out[3] = (uint8_t)(g_idx >> 24);
    return 4;
}

static int entropy(uint8_t* out, uint32_t size, void* user_data) {
    (void)user_data;
    FILE* f = fopen("/dev/urandom", "rb");
    if (!f) return -1;
    size_t n = fread(out, 1, size, f);
    fclose(f);
    return n == size ? 0 : -1;
}

static FILE* g_logfile = NULL;

static void log_sink(const char* line, void* user_data) {
    (void)user_data;
    if (!g_logfile) return;
    fprintf(g_logfile, "%s", line);
}

// --- Host wiring: route traffic between FleeceComms and the RNS aspects ----

static FleeceRuntime* g_runtime = NULL;

// Repair-path visibility counters (reported in the result file).
static uint64_t g_ctrl_rx, g_gap_calls, g_gap_true, g_ctrl_tx, g_uni_ok, g_uni_fail;

// Outbound router. Gossip frames ('F','G') and fan-out control frames ride
// the gossip aspect; "node:<id>" destinations are UNICAST to that peer via
// fleece_reticulum_send_to_node() - the repair handshake's replies, so one
// node's resync no longer multiplies across the whole mesh.
static void comms_send_router(const char* destination, const uint8_t* data, uint32_t size, void* user_data) {
    if (size >= 2 && data[0] == 'F' && data[1] == 'X') g_ctrl_tx++;
    if (strncmp(destination, "node:", 5) == 0) {
        uint64_t id = strtoull(destination + 5, NULL, 16);
        if (fleece_reticulum_send_to_node(id, data, size)) { g_uni_ok++; return; }
        g_uni_fail++;  // peer unknown - fall through to fan-out rather than dropping
    }
    fleece_reticulum_send(destination, data, size, user_data);
}

static void on_control_packet(const uint8_t* data, uint32_t size, void* user_data) {
    (void)user_data;
    g_ctrl_rx++;
    fleece_runtime_on_control_frame(g_runtime, data, size);
}

static void on_gap(bool behind_shared, uint64_t sender_node_id, void* user_data) {
    g_gap_calls++;
    if (behind_shared) g_gap_true++;
    fleece_runtime_note_behind_from((FleeceRuntime*)user_data, behind_shared, sender_node_id);
}

// --- Workload driver ---------------------------------------------------------

typedef struct {
    uint32_t idx;
    uint32_t k;
    size_t vlen;
    uint32_t total_ticks;
    uint32_t tick;
    uint32_t steady_from;  // churn begins after this many warm-up ticks
    uint32_t cursor;
    RadioCtx* radio;
} NodeCtx;

static void tick_cb(FleeceRuntime* rt, void* user_data) {
    NodeCtx* c = (NodeCtx*)user_data;
    c->tick++;

    FleeceStateManager* sm = (FleeceStateManager*)fleece_runtime_get_state_manager(rt);

    if (c->tick == c->steady_from - 1) {
        // Publish our k world keys once gossip peers are being discovered.
        for (uint32_t f = 0; f < c->k; f++) {
            char name[FLEECE_FIELD_NAME_MAX];
            char val[128];
            snprintf(name, sizeof name, "n%u-%u", c->idx, f);
            snprintf(val, sizeof val, "{\"v\":%u,\"s\":%u}", f, c->idx);
            size_t len = strlen(val);
            while (len < c->vlen) val[len++] = 'x';
            fleece_state_manager_set_shared(sm, name, (const uint8_t*)val, (uint32_t)c->vlen);
        }
    } else if (c->tick > c->steady_from && c->tick < c->total_ticks * 6 / 10) {
        // Steady telemetry churn: one owned key rewritten per tick so deltas
        // keep carrying traffic through the mesh.
        char name[FLEECE_FIELD_NAME_MAX];
        char val[128];
        snprintf(name, sizeof name, "n%u-%u", c->idx, c->cursor % c->k);
        c->cursor++;
        snprintf(val, sizeof val, "{\"t\":%u}", c->tick);
        size_t len = strlen(val);
        while (len < 24) val[len++] = 'x';
        fleece_state_manager_set_shared(sm, name, (const uint8_t*)val, (uint32_t)len);
    }

    if (c->tick >= c->total_ticks) fleece_runtime_stop(rt);
}

// --- Child: one full node -----------------------------------------------------

static int run_child(uint32_t idx, uint32_t n_nodes, double loss, uint32_t ticks,
                     uint32_t mtu, uint32_t k, size_t vlen, int port, const char* dir) {
    g_idx = idx;
    snprintf(g_dir, sizeof g_dir, "%s", dir);
    signal(SIGINT, SIG_IGN);
    signal(SIGTERM, SIG_IGN);

    static RadioCtx radio;
    memset(&radio, 0, sizeof(radio));
    radio.idx = idx;
    radio.n_nodes = n_nodes;
    radio.drop_p = loss;

    static NodeCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.idx = idx;
    ctx.k = k;
    ctx.vlen = vlen;
    ctx.total_ticks = ticks;
    ctx.steady_from = 20;  // ~2 s of announce/discovery warm-up before publishing
    ctx.radio = &radio;

    // The "radio": one UDP multicast socket shared by every node on the host.
    // Everyone sends to - and listens on - the SAME group:port; that is the
    // shared medium the drop model applies to.
    radio.fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (radio.fd < 0) return 1;
    int one = 1;
    setsockopt(radio.fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#ifdef SO_REUSEPORT
    setsockopt(radio.fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
#endif
    setsockopt(radio.fd, IPPROTO_IP, IP_MULTICAST_LOOP, &one, sizeof(one));
    unsigned char ttl = 1;
    setsockopt(radio.fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));

    radio.dest.sin_family = AF_INET;
    radio.dest.sin_port = htons((uint16_t)port);
    radio.dest.sin_addr.s_addr = inet_addr(MULTICAST_GROUP);

    struct sockaddr_in bind_addr;
    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr = radio.dest;  // same port; s_addr replaced below
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(radio.fd, (struct sockaddr*)&bind_addr, sizeof(bind_addr)) < 0) return 1;

    struct ip_mreq mreq;
    mreq.imr_multiaddr.s_addr = inet_addr(MULTICAST_GROUP);
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    setsockopt(radio.fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));

    int fl = fcntl(radio.fd, F_GETFL, 0);
    fcntl(radio.fd, F_SETFL, fl | O_NONBLOCK);

    FleeceReticulumConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.radio_send = radio_send;
    cfg.radio_receive = radio_receive;
    cfg.radio_mtu = mtu;
    cfg.radio_bitrate = CHANNEL_BPS;
    cfg.app_name = "benchmesh";
    cfg.identity_load = identity_load;
    cfg.identity_save = identity_save;
    cfg.device_id = device_id;
    cfg.entropy = entropy;
    cfg.log = log_sink;
    {
        char lp[512];
        snprintf(lp, sizeof lp, "%s/node%u.log", g_dir, g_idx);
        g_logfile = fopen(lp, "w");
        // Line-buffered: children exit via _exit() after fork(), which skips
        // stdio flush - unbuffered logs are the difference between a debuggable
        // run and a silent one.
        if (g_logfile) setvbuf(g_logfile, NULL, _IOLBF, 0);
    }
    cfg.user_data = &radio;

    fleece_reticulum_configure(&cfg);
    if (!fleece_reticulum_identity_init()) return 1;

    FleeceRuntime* rt = fleece_runtime_create_with_node_id(fleece_reticulum_node_id());
    if (!rt) return 1;

    FleeceStateManager* sm = (FleeceStateManager*)fleece_runtime_get_state_manager(rt);
    FleeceComms* comms = (FleeceComms*)fleece_runtime_get_comms(rt);
    g_runtime = rt;
    fleece_comms_set_send_callback(comms, comms_send_router, NULL);
    fleece_comms_set_poll_callback(comms, fleece_reticulum_poll, NULL);
    fleece_reticulum_control_set_receive_callback(on_control_packet, NULL);
    fleece_reticulum_set_gap_callback(on_gap, rt);
    fleece_comms_initialize(comms);

    if (!fleece_reticulum_start(sm)) return 1;

    fleece_runtime_set_tick_callback(rt, tick_cb, &ctx);
    fleece_runtime_start(rt);

    // Report what THIS node saw at the wire boundary plus its final view.
    uint64_t digest = 0;
    fleece_state_manager_stream_digest(sm, FLEECE_SHARED_OWNER_ID, &digest);
    char names[STORE_REPORT_MAX][FLEECE_FIELD_NAME_MAX];
    uint32_t fields = fleece_state_manager_list_fields(sm, FLEECE_SHARED_OWNER_ID, names, STORE_REPORT_MAX);

    char path[512];
    snprintf(path, sizeof path, "%s/result.%u", dir, idx);
    FILE* f = fopen(path, "w");
    if (f) {
        fprintf(f, "idx=%u\nnode_id=%llx\ndigest=%llx\nfields=%u\n"
                   "tx_bytes=%llu\nrx_bytes=%llu\n"
                   "dropped=%llu\nctrl_rx=%llu\ngap_calls=%llu\ngap_true=%llu\nctrl_tx=%llu\nuni_ok=%llu\nuni_fail=%llu\n",
                idx, (unsigned long long)fleece_reticulum_node_id(),
                (unsigned long long)digest, fields,
                (unsigned long long)radio.tx_bytes, (unsigned long long)radio.rx_bytes,
                (unsigned long long)radio.dropped,
                (unsigned long long)g_ctrl_rx,
                (unsigned long long)g_gap_calls,
                (unsigned long long)g_gap_true,
                (unsigned long long)g_ctrl_tx,
                (unsigned long long)g_uni_ok,
                (unsigned long long)g_uni_fail);
        // Per-key LWW state: lets the parent pinpoint WHICH entries disagree
        // when digests diverge.
        for (uint32_t i = 0; i < fields; i++) {
            uint64_t ts = 0, origin = 0;
            fleece_state_manager_get_meta_named(sm, FLEECE_SHARED_OWNER_ID, names[i], &ts, &origin);
            fprintf(f, "field=%s ts=%llu origin=%llx\n", names[i],
                    (unsigned long long)ts, (unsigned long long)origin);
        }
        fclose(f);
    }

    fleece_runtime_destroy(rt);
    close(radio.fd);
    return 0;
}

int main(int argc, char** argv) {
    uint32_t n_nodes = argc > 1 ? (uint32_t)atol(argv[1]) : 4;
    double loss = argc > 2 ? atof(argv[2]) : 20.0;
    uint32_t ticks = argc > 3 ? (uint32_t)atol(argv[3]) : 300;
    uint32_t mtu = argc > 4 ? (uint32_t)atol(argv[4]) : 500;
    uint32_t k = argc > 5 ? (uint32_t)atol(argv[5]) : 3;
    uint32_t vlen = argc > 6 ? (uint32_t)atol(argv[6]) : 24;

    printf("Fleece mesh integration benchmark (real microReticulum transport)\n");
    printf("===================================================================\n");
    printf("nodes=%u loss=%.0f%% ticks=%u mtu=%u workload=%ux%uB/channel=%d bps\n\n",
           n_nodes, loss, ticks, mtu, k, vlen, CHANNEL_BPS);

    char dir[256];
    snprintf(dir, sizeof dir, "/tmp/fleece_bench_mesh_%d", (int)getpid());
    mkdir(dir, 0755);

    pid_t pids[64];
    int port_base = 46800 + ((int)getpid() % 2000);

    fflush(stdout);
    for (uint32_t i = 0; i < n_nodes; i++) {
        pid_t pid = fork();
        if (pid == 0) {
            _exit(run_child(i, n_nodes, loss / 100.0, ticks, mtu, k, vlen,
                            port_base, dir));  // SAME port for every node: one shared medium
        }
        pids[i] = pid;
    }

    // Wait for every node's result file, or give up at the deadline.
    time_t deadline = time(NULL) + (time_t)(ticks * TICK_SECONDS) + 90;
    uint32_t done = 0;
    while (done < n_nodes && time(NULL) < deadline && g_run) {
        done = 0;
        for (uint32_t i = 0; i < n_nodes; i++) {
            char path[512];
            snprintf(path, sizeof path, "%s/result.%u", dir, i);
            struct stat st;
            if (stat(path, &st) == 0) done++;
        }
        if (done < n_nodes) usleep(200000);
    }

    // Aggregate.
    uint64_t digests[64];
    uint32_t fields[64];
    uint64_t tx[64], rx[64], drops[64], ctrl_rx_a[64], gaps[64], gaps_true[64], ctrl_tx_a[64], uni_ok[64], uni_fail[64];
    uint32_t got = 0;
    for (uint32_t i = 0; i < n_nodes; i++) {
        char path[512], line[256];
        snprintf(path, sizeof path, "%s/result.%u", dir, i);
        FILE* f = fopen(path, "r");
        if (!f) continue;
        while (fgets(line, sizeof line, f)) {
            unsigned long long v;
            if (sscanf(line, "digest=%llx", &v) == 1) digests[i] = v;
            else if (sscanf(line, "fields=%llu", &v) == 1) fields[i] = (uint32_t)v;
            else if (sscanf(line, "tx_bytes=%llu", &v) == 1) tx[i] = v;
            else if (sscanf(line, "rx_bytes=%llu", &v) == 1) rx[i] = v;
            else if (sscanf(line, "dropped=%llu", &v) == 1) drops[i] = v;
            else if (sscanf(line, "ctrl_rx=%llu", &v) == 1) ctrl_rx_a[i] = v;
            else if (sscanf(line, "gap_calls=%llu", &v) == 1) gaps[i] = v;
            else if (sscanf(line, "gap_true=%llu", &v) == 1) gaps_true[i] = v;
            else if (sscanf(line, "uni_ok=%llu", &v) == 1) uni_ok[i] = v;
            else if (sscanf(line, "uni_fail=%llu", &v) == 1) uni_fail[i] = v;
            else if (sscanf(line, "ctrl_tx=%llu", &v) == 1) ctrl_tx_a[i] = v;
        }
        fclose(f);
        got++;
    }

    bool converged = (got == n_nodes);
    uint64_t ref = digests[0];
    for (uint32_t i = 0; i < n_nodes && converged; i++) {
        if (digests[i] != ref || fields[i] != n_nodes * k) converged = false;
    }

    // When digests diverge, show exactly which keys disagree (vs node 0).
    if (!converged && got == n_nodes) {
        printf("\nper-key divergence vs node 0:\n");
        for (uint32_t i = 1; i < n_nodes; i++) {
            char path0[512], pathI[512], line[192];
            snprintf(path0, sizeof path0, "%s/result.0", dir);
            snprintf(pathI, sizeof pathI, "%s/result.%u", dir, i);
            FILE* f0 = fopen(path0, "r");
            FILE* fi = fopen(pathI, "r");
            if (!f0 || !fi) { if (f0) fclose(f0); if (fi) fclose(fi); continue; }
            // Skip the scalar header lines, collect field= maps.
            // Small N: linear scan per key is fine for a benchmark report.
            while (fgets(line, sizeof line, f0)) {
                if (strncmp(line, "field=", 6) != 0) continue;
                char name[FLEECE_FIELD_NAME_MAX];
                unsigned long long ts0 = 0, or0 = 0;
                sscanf(line + 6, "%63s ts=%llu origin=%llx", name, &ts0, &or0);
                // find same key in node i
                FILE* fj = fopen(pathI, "r");
                unsigned long long tsI = 0, orI = 0;
                char l2[192];
                bool found = false;
                if (fj) {
                    while (fgets(l2, sizeof l2, fj)) {
                        if (strncmp(l2, "field=", 6) != 0) continue;
                        char nm[FLEECE_FIELD_NAME_MAX];
                        unsigned long long t2 = 0, o2 = 0;
                        sscanf(l2 + 6, "%63s ts=%llu origin=%llx", nm, &t2, &o2);
                        if (strcmp(nm, name) == 0) { tsI = t2; orI = o2; found = true; break; }
                    }
                    fclose(fj);
                }
                if (!found || tsI != ts0 || orI != or0) {
                    printf("  node %u: %s %s ts=%llu origin=%llx (node0: ts=%llu origin=%llx)\n",
                           i, name, found ? "STALE/DIFFERS:" : "MISSING, has",
                           tsI, orI, ts0, or0);
                    break;  // one example per node pair is enough
                }
            }
            fclose(f0); fclose(fi);
        }
    }

    uint64_t total_tx = 0, total_rx = 0, total_drop = 0;
    for (uint32_t i = 0; i < got; i++) {
        printf("node %2u: fields=%u/%u digest=%016llx tx=%llukB rx=%llukB chan_dropped=%llu ctrl_tx=%llu ctrl_rx=%llu gap=%llu(gap+:%llu) uni=%llu/%llu\n",
               i, fields[i], n_nodes * k,
               (unsigned long long)digests[i],
               (unsigned long long)(tx[i] / 1024),
               (unsigned long long)(rx[i] / 1024),
               (unsigned long long)drops[i],
               (unsigned long long)ctrl_tx_a[i],
               (unsigned long long)ctrl_rx_a[i],
               (unsigned long long)gaps[i],
               (unsigned long long)gaps_true[i],
               (unsigned long long)uni_ok[i],
               (unsigned long long)uni_fail[i]);
        total_tx += tx[i];
        total_rx += rx[i];
        total_drop += drops[i];
    }

    double wall = (double)ticks * TICK_SECONDS;
    printf("\nresult : %s\n", converged ? "CONVERGED (all views identical)" : "NOT CONVERGED / incomplete");
    printf("channel: %.1f kB transmitted, %.1f kB received, %llu packets eaten by loss\n",
           (double)total_tx / 1024.0, (double)total_rx / 1024.0,
           (unsigned long long)total_drop);
    printf("load   : %.0f B/s average per node vs %.0f B/s fair share (%.0f%% utilization)\n",
           (double)total_tx / wall / (got ? got : 1),
           (double)CHANNEL_BPS / 8.0 / n_nodes,
           100.0 * ((double)total_tx / wall) / ((double)CHANNEL_BPS / 8.0));

    // Cleanup: results live under the run's own directory.
    char cmd[600];
    if (getenv("BENCH_MESH_KEEP")) printf("logs in %s\n", dir);
    else {
        char cmd[600];
        snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
        if (system(cmd) != 0) { /* best effort */ }
    }

    for (uint32_t i = 0; i < n_nodes; i++) {
        kill(pids[i], SIGKILL);  // no-op if already exited
        waitpid(pids[i], NULL, WNOHANG);
    }
    return converged ? 0 : 1;
}
