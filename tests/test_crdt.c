// World-Object CRDT Tests
//
// The shared/"world" collection is THE swarm-replicated object: a flat JSON
// object whose keys are LWW-registers ordered by (logical_timestamp,
// origin_node_id) - a deterministic total order, so concurrent conflicting
// writes resolve to the same winner on every node regardless of arrival
// order. These tests pin down the three properties that make that guarantee
// real over a lossy mesh:
//
//   1. Convergence: any set of concurrent writes converges identically
//      everywhere (tie-break included).
//   2. Gap detection: a receiver missing ANY update - not just the stream's
//      newest record, which was the documented v3 scalar-hw blind spot -
//      is flagged behind via the advertised view digest and heals via a
//      full resync.
//   3. Delete durability: a removal survives compaction and cannot be
//      resurrected by a stale replica's older copy.
//
// Plus a seeded randomized multi-node fuzz (lossy links, partitions, heal)
// asserting pairwise semantic equality of every node's world view.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "state/fleece_state_manager.h"

static int g_failures = 0;

#define CHECK(cond, msg)                          \
    do {                                           \
        if (!(cond)) {                             \
            printf("FAILED: %s\n", msg);           \
            g_failures++;                          \
        }                                          \
    } while (0)

#define N_FUZZ_NODES 6

// Exports `from`'s full shared stream and imports it into `to`.
static void full_sync_shared(FleeceStateManager* from, FleeceStateManager* to) {
    uint8_t* frame = NULL;
    uint32_t size = 0;
    if (fleece_state_manager_export_shared(from, &frame, &size) == 0 && frame) {
        fleece_state_manager_import(to, frame, size);
        free(frame);
    }
}

// Exports `from`'s delta since `since`, simulating lossy delivery into `to`.
// Pass keep_frame=false to simulate a packet dropped in transit.
static void delta_sync_shared(FleeceStateManager* from, FleeceStateManager* to, uint64_t since,
                              bool* behind_shared, bool deliver) {
    uint8_t* frame = NULL;
    uint32_t size = 0;
    if (fleece_state_manager_export_shared_delta(from, since, &frame, &size) == 0 && frame) {
        if (deliver) {
            fleece_state_manager_import_ex(to, frame, size, NULL, behind_shared);
        }
        free(frame);
    }
}

// Reads a world field into a freshly allocated buffer (caller frees). Returns
// false if absent.
static bool read_world(FleeceStateManager* m, const char* name, uint8_t** data, uint32_t* size) {
    *data = NULL;
    *size = 0;
    return fleece_state_manager_get_named(m, FLEECE_SHARED_OWNER_ID, name, data, size) == 0;
}

// --- 1. Concurrent writes converge on the same winner everywhere ----------

static void test_register_convergence(void) {
    printf("Running register convergence test...\n");

    // Three nodes write the same world key concurrently (all at their own
    // logical time 1 - an exact three-way tie). The deterministic tie-break
    // (highest origin_node_id) must pick the SAME winner on every node, in
    // every arrival order.
    FleeceStateManager* a = fleece_state_manager_create_with_node_id(0xA000000000000001ULL);
    FleeceStateManager* b = fleece_state_manager_create_with_node_id(0xB000000000000002ULL);
    FleeceStateManager* c = fleece_state_manager_create_with_node_id(0xC000000000000003ULL);

    CHECK(fleece_state_manager_set_shared(a, "claim", (const uint8_t*)"\"A\"", 3) == 0, "A writes claim");
    CHECK(fleece_state_manager_set_shared(b, "claim", (const uint8_t*)"\"B\"", 3) == 0, "B writes claim");
    CHECK(fleece_state_manager_set_shared(c, "claim", (const uint8_t*)"\"C\"", 3) == 0, "C writes claim");

    // Relay everything through everyone in an arbitrary order (two full rounds).
    FleeceStateManager* all[3] = {a, b, c};
    for (int round = 0; round < 2; round++) {
        for (int i = 0; i < 3; i++) {
            for (int j = 0; j < 3; j++) {
                if (i != j) full_sync_shared(all[i], all[j]);
            }
        }
    }

    FleeceStateManager* nodes[3] = {a, b, c};
    for (int i = 0; i < 3; i++) {
        uint8_t* data = NULL;
        uint32_t size = 0;
        CHECK(read_world(nodes[i], "claim", &data, &size), "every node can read the contested claim");
        CHECK(data != NULL && size == 3 && memcmp(data, "\"C\"", 3) == 0, "the highest-origin write must win on every node");
        free(data);

        uint64_t d = 0;
        uint64_t e = 0;
        CHECK(fleece_state_manager_stream_digest(nodes[i], FLEECE_SHARED_OWNER_ID, &d) == 0, "digest computable");
        CHECK(fleece_state_manager_stream_digest(nodes[(i + 1) % 3], FLEECE_SHARED_OWNER_ID, &e) == 0, "digest computable (peer)");
        CHECK(d == e, "converged nodes must hold identical view digests");
    }

    // A strictly newer write wins outright regardless of origin id.
    CHECK(fleece_state_manager_set_shared(a, "claim", (const uint8_t*)"\"A2\"", 4) == 0, "A writes a newer claim");
    full_sync_shared(a, b);
    full_sync_shared(b, c);
    uint8_t* data = NULL;
    uint32_t size = 0;
    CHECK(read_world(c, "claim", &data, &size), "c reads after newer write relays through b");
    CHECK(data != NULL && size == 4 && memcmp(data, "\"A2\"", 4) == 0, "a causally-newer write must beat the old tie-break winner");
    free(data);

    fleece_state_manager_destroy(a);
    fleece_state_manager_destroy(b);
    fleece_state_manager_destroy(c);
    printf("Done: register convergence test\n");
}

// --- 2. Digest catches a non-newest gap (the v3 scalar-hw blind spot) ------

static void test_non_newest_gap_detection(void) {
    printf("Running non-newest gap detection test...\n");

    FleeceStateManager* a = fleece_state_manager_create_with_node_id(0x1100000000000011ULL);
    FleeceStateManager* r = fleece_state_manager_create_with_node_id(0x2200000000000022ULL);

    // Two fields, fully synced.
    fleece_state_manager_set_shared(a, "w1", (const uint8_t*)"\"v1\"", 4);
    fleece_state_manager_set_shared(a, "w2", (const uint8_t*)"\"v1\"", 4);
    uint64_t watermark = fleece_state_manager_get_local_timestamp(a);
    full_sync_shared(a, r);

    bool behind_self = false, behind_shared = false;
    delta_sync_shared(a, r, watermark, &behind_shared, true);
    CHECK(!behind_shared, "fully synced receiver is not behind");

    // A updates w1 - that delta is DROPPED. Then A updates w2 and THAT delta
    // gets through. Afterwards R holds the stream's newest record (w2), so the
    // v3 high-water-mark check saw "current"; the view digest must still flag
    // the stale w1.
    fleece_state_manager_set_shared(a, "w1", (const uint8_t*)"\"v2\"", 4);
    delta_sync_shared(a, r, watermark, NULL, false);  // dropped in transit
    watermark = fleece_state_manager_get_local_timestamp(a);

    fleece_state_manager_set_shared(a, "w2", (const uint8_t*)"\"v2\"", 4);
    delta_sync_shared(a, r, watermark, &behind_shared, true);
    CHECK(behind_shared, "a dropped non-newest update MUST be flagged via the view digest");

    // Full resync heals the gap.
    full_sync_shared(a, r);
    delta_sync_shared(a, r, 0, &behind_shared, true);
    CHECK(!behind_shared, "after a full resync the receiver is current again");

    uint8_t* data = NULL;
    uint32_t size = 0;
    CHECK(read_world(r, "w1", &data, &size) && size == 4 && memcmp(data, "\"v2\"", 4) == 0, "the healed snapshot carries the missed w1 update");
    free(data);

    fleece_state_manager_destroy(a);
    fleece_state_manager_destroy(r);
    printf("Done: non-newest gap detection test\n");
}

// --- 3. Deletes survive compaction; stale replicas cannot resurrect -------

static void test_delete_durability(void) {
    printf("Running delete durability test...\n");

    FleeceStateManager* a = fleece_state_manager_create_with_node_id(0x3300000000000033ULL);
    FleeceStateManager* b = fleece_state_manager_create_with_node_id(0x4400000000000044ULL);
    FleeceStateManager* d = fleece_state_manager_create_with_node_id(0x5500000000000055ULL);

    fleece_state_manager_set_shared(a, "T", (const uint8_t*)"\"v1\"", 4);
    full_sync_shared(a, b);  // B holds a live pre-delete copy

    // Delete and compact. compact() must RETAIN the named tombstone - it is
    // the delete-marker every not-yet-caught-up peer still needs.
    CHECK(fleece_state_manager_remove_shared(a, "T") == 0, "delete applies locally");
    CHECK(fleece_state_manager_compact(a) == 0, "compact succeeds");

    // Stale replica B pushes its older live copy back at A: the tombstone is
    // newer, so the delete must stand.
    full_sync_shared(b, a);
    CHECK(!fleece_state_manager_exists_named(a, FLEECE_SHARED_OWNER_ID, "T"), "a stale replica must not resurrect a deleted field");

    // A fresh late joiner learns the delete from A's full export...
    full_sync_shared(a, d);
    CHECK(!fleece_state_manager_exists_named(d, FLEECE_SHARED_OWNER_ID, "T"), "a late joiner inherits the delete");
    // ...and stays deleted even when another stale replica pushes at it.
    full_sync_shared(b, d);
    CHECK(!fleece_state_manager_exists_named(d, FLEECE_SHARED_OWNER_ID, "T"), "the delete must survive contact with any number of stale replicas");

    // B eventually hears about the delete too; then everyone's live view is
    // empty and identical.
    full_sync_shared(a, b);
    FleeceStateManager* nodes[3] = {a, b, d};
    for (int i = 0; i < 3; i++) {
        CHECK(!fleece_state_manager_exists_named(nodes[i], FLEECE_SHARED_OWNER_ID, "T"), "T absent everywhere after propagation");
        uint64_t di = 0, dj = 0;
        fleece_state_manager_stream_digest(nodes[i], FLEECE_SHARED_OWNER_ID, &di);
        fleece_state_manager_stream_digest(nodes[(i + 1) % 3], FLEECE_SHARED_OWNER_ID, &dj);
        CHECK(di == dj, "post-delete views identical everywhere");
    }

    fleece_state_manager_destroy(a);
    fleece_state_manager_destroy(b);
    fleece_state_manager_destroy(d);
    printf("Done: delete durability test\n");
}

// --- 3b. Delete/write race: the tombstone-origin regression ---------------
//
// Regression guard for a real deadlock: remove_shared() used to leave the
// tombstone stamped with the OLD value's author, so a concurrent new value
// from that same author produced two indistinguishable versions (same
// timestamp, same origin - one live, one deleted). Each version rejected its
// rival forever and the swarm split permanently. The tombstone must carry the
// DELETER's identity so the total order stays total.

static void test_delete_write_race(void) {
    printf("Running delete/write race regression test...\n");

    // Origin order matters on the tie: make the DELETER the higher origin id,
    // so the correct outcome is "delete wins" and any origin-inheritance bug
    // flips it into a permanent live/deleted split instead of mere luck.
    FleeceStateManager* writer = fleece_state_manager_create_with_node_id(0x1111111111111111ULL);
    FleeceStateManager* deleter = fleece_state_manager_create_with_node_id(0x9999999999999999ULL);
    FleeceStateManager* observer = fleece_state_manager_create_with_node_id(0x5555555555555555ULL);

    // Seed k2 on all three, fully synced (clocks aligned at 1 everywhere).
    fleece_state_manager_set_shared(writer, "k2", (const uint8_t*)"\"v1\"", 4);
    full_sync_shared(writer, deleter);
    full_sync_shared(writer, observer);

    // RACE: both act at their own logical time 2 without hearing from the other.
    CHECK(fleece_state_manager_remove_shared(deleter, "k2") == 0, "deleter removes k2");
    CHECK(fleece_state_manager_set_shared(writer, "k2", (const uint8_t*)"\"v2\"", 4) == 0, "writer overwrites k2 concurrently");

    // Exchange until quiescent - must converge, not deadlock.
    for (int round = 0; round < 3; round++) {
        full_sync_shared(writer, deleter);
        full_sync_shared(deleter, writer);
        full_sync_shared(writer, observer);
        full_sync_shared(deleter, observer);
        full_sync_shared(observer, writer);
        full_sync_shared(observer, deleter);
    }

    FleeceStateManager* nodes[3] = {writer, deleter, observer};
    uint64_t d0 = 0;
    fleece_state_manager_stream_digest(nodes[0], FLEECE_SHARED_OWNER_ID, &d0);
    for (int i = 0; i < 3; i++) {
        CHECK(!fleece_state_manager_exists_named(nodes[i], FLEECE_SHARED_OWNER_ID, "k2"),
              "the higher-origin DELETE must win the race on every node");
        uint64_t di = 0;
        fleece_state_manager_stream_digest(nodes[i], FLEECE_SHARED_OWNER_ID, &di);
        CHECK(di == d0, "all nodes hold identical views after the race");
    }

    fleece_state_manager_destroy(writer);
    fleece_state_manager_destroy(deleter);
    fleece_state_manager_destroy(observer);
    printf("Done: delete/write race regression test\n");
}


//
// Deterministic LCG so failures are reproducible. Models the real runtime:
// delta broadcasts with per-node watermarks, lossy delivery, digest-flagged
// gaps answered by reliable unicast full resyncs, and a mid-run network
// partition that splits the swarm while both halves keep writing.

static uint32_t rng_next(uint64_t* state) {
    *state = *state * 6364136223846793005ULL + 1442695040888963407ULL;
    return (uint32_t)(*state >> 33);
}

// Semantic snapshot comparison: every LIVE world field (name + exact bytes)
// must match between two nodes.
static bool worlds_equal(FleeceStateManager* x, FleeceStateManager* y) {
    char nx[64][FLEECE_FIELD_NAME_MAX];
    char ny[64][FLEECE_FIELD_NAME_MAX];
    uint32_t cx = fleece_state_manager_list_fields(x, FLEECE_SHARED_OWNER_ID, nx, 64);
    uint32_t cy = fleece_state_manager_list_fields(y, FLEECE_SHARED_OWNER_ID, ny, 64);
    if (cx != cy) return false;

    // list_fields order follows storage order, which differs between nodes -
    // match names greedily instead of positionally.
    for (uint32_t i = 0; i < cx; i++) {
        uint8_t* vx = NULL;
        uint32_t sx = 0;
        bool found = false;
        if (!read_world(x, nx[i], &vx, &sx)) return false;
        for (uint32_t j = 0; j < cy && !found; j++) {
            if (strcmp(nx[i], ny[j]) != 0) continue;
            uint8_t* vy = NULL;
            uint32_t sy = 0;
            if (!read_world(y, ny[j], &vy, &sy)) return false;
            found = (sx == sy) && (sx == 0 || memcmp(vx, vy, sx) == 0);
            free(vy);
        }
        free(vx);
        if (!found) return false;

        uint64_t dx = 0, dy = 0;
        fleece_state_manager_stream_digest(x, FLEECE_SHARED_OWNER_ID, &dx);
        fleece_state_manager_stream_digest(y, FLEECE_SHARED_OWNER_ID, &dy);
        if (dx != dy) return false;
    }
    return true;
}

// Failure forensics: dump a node's live world view so a non-converging fuzz
// seed can be diffed by eye.
static void dump_node(int idx, FleeceStateManager* m) {
    char names[64][FLEECE_FIELD_NAME_MAX];
    uint32_t n = fleece_state_manager_list_fields(m, FLEECE_SHARED_OWNER_ID, names, 64);
    uint64_t d = 0;
    fleece_state_manager_stream_digest(m, FLEECE_SHARED_OWNER_ID, &d);
    printf("  node %d: hw=%llu digest=%016llx live=%u\n", idx,
           (unsigned long long)fleece_state_manager_get_shared_hw(m),
           (unsigned long long)d, n);
    for (uint32_t i = 0; i < n; i++) {
        uint8_t* v = NULL;
        uint32_t vs = 0;
        if (read_world(m, names[i], &v, &vs)) {
            printf("    %s = %.*s\n", names[i], (int)vs, (const char*)v);
            free(v);
        }
    }
}

static void run_fuzz(uint64_t seed) {
    FleeceStateManager** nodes = calloc(N_FUZZ_NODES, sizeof(FleeceStateManager*));
    if (!nodes) {
        printf("FAILED (seed %llu): alloc\n", (unsigned long long)seed);
        g_failures++;
        return;
    }
    for (int i = 0; i < N_FUZZ_NODES; i++) {
        nodes[i] = fleece_state_manager_create_with_node_id(0xF000000000000000ULL | (uint64_t)(i + 1));
    }

    uint64_t rng = seed | 1;
    uint64_t wm[N_FUZZ_NODES] = {0};                    // per-node delta watermark (runtime-style)
    bool pull[N_FUZZ_NODES][N_FUZZ_NODES] = {{false}};  // pull[j][i]: j owes a full fetch from i
    uint64_t counters[N_FUZZ_NODES] = {0};

    // Links: symmetric matrix; recomputed each round (flaky mesh topology).
    bool link[N_FUZZ_NODES][N_FUZZ_NODES] = {{false}};
    const int total_rounds = 140;
    const int partition_start = 50;
    const int partition_end = 80;

    for (int round = 0; round < total_rounds; round++) {
        bool partitioned = (round >= partition_start && round < partition_end);

        // Fresh topology: mostly-up random mesh. While partitioned, the two
        // halves (first half / second half of the node array) have no path
        // between them at all.
        for (int i = 0; i < N_FUZZ_NODES; i++) {
            for (int j = i + 1; j < N_FUZZ_NODES; j++) {
                bool up = (rng_next(&rng) % 100) < 75;
                if (partitioned && ((i < N_FUZZ_NODES / 2) != (j < N_FUZZ_NODES / 2))) up = false;
                link[i][j] = link[j][i] = up;
            }
        }

        // Phase 1: local writes (both halves keep writing during the split).
        for (int i = 0; i < N_FUZZ_NODES; i++) {
            uint32_t roll = rng_next(&rng) % 100;
            char name[16];
            snprintf(name, sizeof(name), "k%u", rng_next(&rng) % 5);
            if (roll < 35) {
                char val[32];
                counters[i]++;
                snprintf(val, sizeof(val), "\"n%u:%llu\"", i, (unsigned long long)counters[i]);
                fleece_state_manager_set_shared(nodes[i], name, (const uint8_t*)val, (uint32_t)strlen(val));
            } else if (roll < 45) {
                // Delete something this node currently sees (may already be gone).
                fleece_state_manager_remove_shared(nodes[i], name);
            }
        }

        // Phase 2: delta broadcast per node (one frame, delivered per-link
        // with loss), exactly like the runtime's Phase 3.
        for (int i = 0; i < N_FUZZ_NODES; i++) {
            uint8_t* frame = NULL;
            uint32_t size = 0;
            if (fleece_state_manager_export_shared_delta(nodes[i], wm[i], &frame, &size) != 0) continue;
            for (int j = 0; j < N_FUZZ_NODES; j++) {
                if (j == i || !link[i][j]) continue;
                if ((rng_next(&rng) % 100) < 15) continue;  // 15% packet loss
                bool behind = false;
                fleece_state_manager_import_ex(nodes[j], frame, size, NULL, &behind);
                if (behind) pull[j][i] = true;  // schedule a reliable unicast resync
            }
            free(frame);
            wm[i] = fleece_state_manager_get_shared_hw(nodes[i]);
        }

        // Phase 3: resync pulls - reliable by construction (unicast reply),
        // so a flagged gap always heals next round.
        for (int j = 0; j < N_FUZZ_NODES; j++) {
            for (int i = 0; i < N_FUZZ_NODES; i++) {
                if (j == i || !pull[j][i] || !link[i][j]) continue;
                pull[j][i] = false;
                full_sync_shared(nodes[i], nodes[j]);
            }
        }
    }

    // Heal phase: full mesh, no loss, no writes - gossip plus digest-driven
    // pulls must drive every pair to agreement within a bounded number of
    // rounds. If it doesn't, either convergence or gap detection is broken.
    for (int i = 0; i < N_FUZZ_NODES; i++)
        for (int j = 0; j < N_FUZZ_NODES; j++)
            link[i][j] = (i != j);

    bool converged = false;
    int dbg = getenv("CRDT_DEBUG") ? 1 : 0;
    for (int round = 0; round < 60 && !converged; round++) {
        for (int i = 0; i < N_FUZZ_NODES; i++) {
            uint8_t* frame = NULL;
            uint32_t size = 0;
            if (fleece_state_manager_export_shared_delta(nodes[i], wm[i], &frame, &size) == 0) {
                for (int j = 0; j < N_FUZZ_NODES; j++) {
                    if (j == i) continue;
                    bool behind = false;
                    fleece_state_manager_import_ex(nodes[j], frame, size, NULL, &behind);
                    if (behind) {
                        if (dbg) printf("    r%d: node %d behind node %d\n", round, j, i);
                        pull[j][i] = true;
                    }
                }
                free(frame);
                wm[i] = fleece_state_manager_get_shared_hw(nodes[i]);
            }
        }
        for (int j = 0; j < N_FUZZ_NODES; j++)
            for (int i = 0; i < N_FUZZ_NODES; i++) {
                if (j == i || !pull[j][i]) continue;
                pull[j][i] = false;
                full_sync_shared(nodes[i], nodes[j]);
            }

        converged = true;
        for (int i = 0; i < N_FUZZ_NODES && converged; i++)
            for (int j = i + 1; j < N_FUZZ_NODES; j++)
                if (!worlds_equal(nodes[i], nodes[j])) {
                    converged = false;
                    break;
                }
    }

    if (!converged) {
        printf("FAILED (seed %llu): swarm did not converge after partition heal\n", (unsigned long long)seed);
        for (int i = 0; i < N_FUZZ_NODES; i++) dump_node(i, nodes[i]);
        g_failures++;
    } else {
        // Belt and braces: after convergence, one more exchange must flag nobody.
        for (int i = 0; i < N_FUZZ_NODES; i++) {
            for (int j = 0; j < N_FUZZ_NODES; j++) {
                if (i == j) continue;
                bool behind = false;
                delta_sync_shared(nodes[i], nodes[j], wm[i], &behind, true);
                if (behind) {
                    printf("FAILED (seed %llu): converged nodes still report digest mismatch\n", (unsigned long long)seed);
                    g_failures++;
                    goto done;
                }
            }
        }
    }

done:
    for (int i = 0; i < N_FUZZ_NODES; i++) fleece_state_manager_destroy(nodes[i]);
    free(nodes);
}

static void test_fuzz_convergence(void) {
    printf("Running randomized convergence fuzz (seeds 1..20)...\n");
    for (uint64_t seed = 1; seed <= 20; seed++) {
        run_fuzz(seed);
        printf("  seed %llu done\n", (unsigned long long)seed);
    }
    printf("Done: randomized convergence fuzz\n");
}

int main(void) {
    printf("Fleece World-Object CRDT Tests\n");
    printf("==============================\n\n");

    test_register_convergence();
    printf("\n");
    test_non_newest_gap_detection();
    printf("\n");
    test_delete_durability();
    printf("\n");
    test_delete_write_race();
    printf("\n");
    test_fuzz_convergence();
    printf("\n");

    if (g_failures > 0) {
        printf("%d check(s) FAILED\n", g_failures);
        return 1;
    }
    printf("All tests passed!\n");
    return 0;
}
