// Swarm Convergence Test
// Proves the LWW gossip protocol converges - including the shared/"world"
// stream - for an arbitrary swarm size N, not just a pair of nodes. A full-mesh
// 0%-loss gossip exchange must leave every node holding every peer's self
// fields, every node agreeing on the single shared-field winner (highest
// origin_node_id on a timestamp tie), and no node flagged "behind" on either
// stream (import_ex reports current) once synchronized.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "state/fleece_state_manager.h"

static int g_failures = 0;

#define CHECK(cond, msg)                          \
    do {                                           \
        if (!(cond)) {                              \
            printf("  FAILED (N=%d): %s\n", g_n, msg); \
            g_failures++;                           \
        }                                            \
    } while (0)

static int g_n = 0;

// Runs the convergence check for a swarm of `n` nodes (ids 1..n). Each node
// publishes two self fields ("probe", "sensor") and one shared "world" field;
// all nodes write "world" at the same internal tick, forcing the highest-id
// tie-break. Returns true if all assertions hold.
static void run_swarm(int n) {
    g_n = n;

    FleeceStateManager** nodes = calloc((size_t)n, sizeof(FleeceStateManager*));
    if (!nodes) {
        printf("  FAILED (N=%d): calloc\n", n);
        g_failures++;
        return;
    }

    for (int i = 0; i < n; i++) {
        uint64_t id = (uint64_t)(i + 1);
        nodes[i] = fleece_state_manager_create_with_node_id(id);
        CHECK(nodes[i] != NULL, "manager creation");
        if (!nodes[i]) {
            for (int k = 0; k < i; k++) fleece_state_manager_destroy(nodes[k]);
            free(nodes);
            return;
        }

        // Distinct self fields per node (same names, owners differ) + a shared
        // field every node races on - same tick -> exact timestamp tie.
        char sensor[16];
        snprintf(sensor, sizeof(sensor), "%d", i + 1);
        fleece_state_manager_set_named(nodes[i], "probe", (const uint8_t*)"1", 1);
        fleece_state_manager_set_named(nodes[i], "sensor", (const uint8_t*)sensor, (uint32_t)strlen(sensor));
        fleece_state_manager_set_shared(nodes[i], "world", (const uint8_t*)sensor, (uint32_t)strlen(sensor));
    }

    // Full-mesh gossip: every node broadcasts its self and shared streams; every
    // other node imports them (0% loss). One round propagates everything in a
    // full mesh; a second round must find everyone already current (behind=false).
    for (int round = 0; round < 2; round++) {
        for (int i = 0; i < n; i++) {
            uint8_t* self_frame = NULL;
            uint32_t self_frame_size = 0;
            uint8_t* shared_frame = NULL;
            uint32_t shared_frame_size = 0;
            CHECK(fleece_state_manager_export(nodes[i], &self_frame, &self_frame_size) == 0, "self export");
            CHECK(fleece_state_manager_export_shared(nodes[i], &shared_frame, &shared_frame_size) == 0, "shared export");

            for (int j = 0; j < n; j++) {
                if (i == j) continue;
                bool behind_self = true, behind_shared = true;
                CHECK(fleece_state_manager_import_ex(nodes[j], self_frame, self_frame_size, &behind_self, &behind_shared) == 0, "import of peer self stream");
                CHECK(fleece_state_manager_import_ex(nodes[j], shared_frame, shared_frame_size, &behind_self, &behind_shared) == 0, "import of peer shared stream");
            }
            free(self_frame);
            free(shared_frame);
        }
    }

    // Convergence assertions.
    char expected_world[16];
    snprintf(expected_world, sizeof(expected_world), "%d", n);  // highest id wins the tie

    for (int j = 0; j < n; j++) {
        uint64_t my_id = (uint64_t)(j + 1);
        uint64_t shared_hw = fleece_state_manager_get_shared_hw(nodes[j]);

        for (int i = 0; i < n; i++) {
            if (i == j) continue;
            uint64_t peer_id = (uint64_t)(i + 1);
            CHECK(fleece_state_manager_exists_named(nodes[j], peer_id, "probe"), "every node should see every peer's probe field");

            char expected_sensor[16];
            snprintf(expected_sensor, sizeof(expected_sensor), "%d", i + 1);
            uint8_t* data = NULL;
            uint32_t size = 0;
            CHECK(fleece_state_manager_get_named(nodes[j], peer_id, "sensor", &data, &size) == 0, "every node should read every peer's sensor field");
            CHECK(data != NULL && size == (uint32_t)strlen(expected_sensor) && memcmp(data, expected_sensor, size) == 0, "peer sensor value should match the author");
            free(data);

            CHECK(!fleece_state_manager_exists_named(nodes[j], my_id == peer_id ? 0 : peer_id, "world"), "world must be shared, not a peer-owned self field");
        }

        // The single shared "world" winner must be identical on every node.
        uint8_t* data = NULL;
        uint32_t size = 0;
        CHECK(fleece_state_manager_get_named(nodes[j], FLEECE_SHARED_OWNER_ID, "world", &data, &size) == 0, "shared world field readable");
        CHECK(data != NULL && size == (uint32_t)strlen(expected_world) && memcmp(data, expected_world, size) == 0, "shared world must converge to the highest-origin winner on every node");
        free(data);

        // After synchronization nobody is behind on either stream.
        for (int i = 0; i < n; i++) {
            if (i == j) continue;
            uint64_t peer_id = (uint64_t)(i + 1);
            uint8_t* frame = NULL;
            uint32_t frame_size = 0;
            fleece_state_manager_export(nodes[i], &frame, &frame_size);
            bool behind_self = false, behind_shared = false;
            fleece_state_manager_import_ex(nodes[j], frame, frame_size, &behind_self, &behind_shared);
            CHECK(!behind_self, "no node should be behind on a peer's self stream after convergence");
            free(frame);

            frame = NULL;
            frame_size = 0;
            fleece_state_manager_export_shared(nodes[i], &frame, &frame_size);
            behind_self = behind_shared = false;
            fleece_state_manager_import_ex(nodes[j], frame, frame_size, &behind_self, &behind_shared);
            CHECK(!behind_shared, "no node should be behind on the shared stream after convergence");
            free(frame);
        }

        // Sanity: local hw reflects a full mesh at every node.
        CHECK(shared_hw == fleece_state_manager_get_shared_hw(nodes[j]), "shared hw should be stable at every node");
        for (int i = 0; i < n; i++) {
            if (i == j) continue;
            uint64_t peer_id = (uint64_t)(i + 1);
            CHECK(fleece_state_manager_get_peer_self_hw(nodes[j], peer_id) == fleece_state_manager_get_self_hw(nodes[i]), "each node should hold each peer's full self-stream hw");
        }
    }

    for (int i = 0; i < n; i++) fleece_state_manager_destroy(nodes[i]);
    free(nodes);
}

static void test_swarm_convergence(void) {
    printf("Running swarm convergence test (shared-convergence at any N)...\n");

    int sizes[] = {2, 4, 8, 16, 32, 63};  // 2 self + 1 shared field per node -> 2N+1 <= 128
    size_t n_sizes = sizeof(sizes) / sizeof(sizes[0]);
    for (size_t k = 0; k < n_sizes; k++) {
        run_swarm(sizes[k]);
    }

    printf("Done: swarm convergence test\n");
}

int main(void) {
    printf("Fleece Swarm Convergence Tests\n");
    printf("================================\n\n");

    test_swarm_convergence();
    printf("\n");

    if (g_failures > 0) {
        printf("%d check(s) FAILED\n", g_failures);
        return 1;
    }
    printf("All tests passed!\n");
    return 0;
}