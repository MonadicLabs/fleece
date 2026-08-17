// Fleece Gossip Wire Format Tests
// Exercises fleece_state_manager_export/import/merge_named across separate
// managers (simulating distinct swarm nodes) without needing a real transport.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "state/fleece_state_manager.h"

static int g_failures = 0;

#define CHECK(cond, msg)                          \
    do {                                           \
        if (!(cond)) {                              \
            printf("FAILED: %s\n", msg);            \
            g_failures++;                           \
        }                                            \
    } while (0)

static void test_export_import_basic(void) {
    printf("Running export/import basic test...\n");

    FleeceStateManager* a = fleece_state_manager_create_with_node_id(0x1111111111111111ULL);
    FleeceStateManager* b = fleece_state_manager_create_with_node_id(0x2222222222222222ULL);

    fleece_state_manager_set_named(a, "temperature", (const uint8_t*)"21.5", 4);
    fleece_state_manager_set_named(a, "status", (const uint8_t*)"\"ok\"", 4);

    uint8_t* frame = NULL;
    uint32_t frame_size = 0;
    CHECK(fleece_state_manager_export(a, &frame, &frame_size) == 0, "export should succeed");
    CHECK(frame != NULL && frame_size > 0, "export should produce a non-empty frame");

    CHECK(fleece_state_manager_import(b, frame, frame_size) == 0, "import should succeed");

    uint8_t* data = NULL;
    uint32_t size = 0;
    uint64_t a_id = fleece_state_manager_get_node_id(a);
    CHECK(fleece_state_manager_get_named(b, a_id, "temperature", &data, &size) == 0, "peer field should be readable after import");
    CHECK(data != NULL && size == 4 && memcmp(data, "21.5", 4) == 0, "peer field value should match what was exported");
    free(data);

    uint64_t nodes[8];
    uint32_t node_count = fleece_state_manager_list_nodes(b, nodes, 8);
    CHECK(node_count == 1 && nodes[0] == a_id, "b should know about exactly one peer after import");

    free(frame);
    fleece_state_manager_destroy(a);
    fleece_state_manager_destroy(b);
    printf("Done: export/import basic test\n");
}

static void test_lww_merge(void) {
    printf("Running LWW merge test...\n");

    uint64_t peer_id = 0x3333333333333333ULL;
    FleeceStateManager* local = fleece_state_manager_create_with_node_id(0x4444444444444444ULL);

    CHECK(fleece_state_manager_merge_named(local, peer_id, "x", (const uint8_t*)"1", 1, 100, false) == 0, "first merge should apply");

    uint8_t* data = NULL;
    uint32_t size = 0;
    fleece_state_manager_merge_named(local, peer_id, "x", (const uint8_t*)"2", 1, 50, false);
    fleece_state_manager_get_named(local, peer_id, "x", &data, &size);
    CHECK(data != NULL && size == 1 && data[0] == '1', "an older remote write must not overwrite a newer one");
    free(data);
    data = NULL;

    fleece_state_manager_merge_named(local, peer_id, "x", (const uint8_t*)"3", 1, 200, false);
    fleece_state_manager_get_named(local, peer_id, "x", &data, &size);
    CHECK(data != NULL && size == 1 && data[0] == '3', "a newer remote write should win");
    free(data);

    fleece_state_manager_destroy(local);
    printf("Done: LWW merge test\n");
}

static void test_tombstone_propagation(void) {
    printf("Running tombstone propagation test...\n");

    FleeceStateManager* a = fleece_state_manager_create_with_node_id(0x5555555555555555ULL);
    FleeceStateManager* b = fleece_state_manager_create_with_node_id(0x6666666666666666ULL);
    uint64_t a_id = fleece_state_manager_get_node_id(a);

    fleece_state_manager_set_named(a, "battery", (const uint8_t*)"90", 2);
    uint8_t* frame = NULL;
    uint32_t frame_size = 0;
    fleece_state_manager_export(a, &frame, &frame_size);
    fleece_state_manager_import(b, frame, frame_size);
    free(frame);
    CHECK(fleece_state_manager_exists_named(b, a_id, "battery"), "field should exist on b after first sync");

    fleece_state_manager_remove_named(a, "battery");
    frame = NULL;
    frame_size = 0;
    fleece_state_manager_export(a, &frame, &frame_size);
    fleece_state_manager_import(b, frame, frame_size);
    free(frame);
    CHECK(!fleece_state_manager_exists_named(b, a_id, "battery"), "deletion on a should propagate to b via gossip");

    fleece_state_manager_destroy(a);
    fleece_state_manager_destroy(b);
    printf("Done: tombstone propagation test\n");
}

static void test_self_owned_rejection(void) {
    printf("Running self-owned-frame rejection test...\n");

    FleeceStateManager* a = fleece_state_manager_create_with_node_id(0x7777777777777777ULL);
    uint64_t a_id = fleece_state_manager_get_node_id(a);
    fleece_state_manager_set_named(a, "x", (const uint8_t*)"1", 1);

    uint8_t* frame = NULL;
    uint32_t frame_size = 0;
    fleece_state_manager_export(a, &frame, &frame_size);
    CHECK(fleece_state_manager_import(a, frame, frame_size) != 0, "importing a's own exported frame into a should be rejected");
    free(frame);

    CHECK(fleece_state_manager_merge_named(a, a_id, "y", (const uint8_t*)"2", 1, 1, false) != 0, "merge_named must reject owner_node_id == local node id");

    fleece_state_manager_destroy(a);
    printf("Done: self-owned-frame rejection test\n");
}

static void test_malformed_frames(void) {
    printf("Running malformed frame test...\n");

    FleeceStateManager* m = fleece_state_manager_create_with_node_id(0x8888888888888888ULL);

    CHECK(fleece_state_manager_import(m, NULL, 0) != 0, "NULL frame should be rejected");

    uint8_t tiny[1] = {0};
    CHECK(fleece_state_manager_import(m, tiny, 0) != 0, "zero-length frame should be rejected");
    CHECK(fleece_state_manager_import(m, tiny, 1) != 0, "too-short frame should be rejected");

    uint8_t bad_magic[15] = {0};
    bad_magic[0] = 'X';
    bad_magic[1] = 'X';
    bad_magic[2] = 1;
    CHECK(fleece_state_manager_import(m, bad_magic, sizeof(bad_magic)) != 0, "wrong magic should be rejected");

    // Valid header (owner id 0, distinct from m's id) claiming 5 fields with no room for any of them.
    uint8_t truncated[15 + 2] = {0};
    truncated[0] = 'F';
    truncated[1] = 'G';
    truncated[2] = 1;
    truncated[11] = 5;  // field_count (LE u32 at offset 11)
    CHECK(fleece_state_manager_import(m, truncated, sizeof(truncated)) != 0, "truncated field records should be rejected without crashing");

    fleece_state_manager_destroy(m);
    printf("Done: malformed frame test (no crash)\n");
}

static void test_delta_export(void) {
    printf("Running delta export test...\n");

    FleeceStateManager* a = fleece_state_manager_create_with_node_id(0xB0B0B0B0B0B0B0B0ULL);
    FleeceStateManager* b = fleece_state_manager_create_with_node_id(0xC0C0C0C0C0C0C0C0ULL);
    uint64_t a_id = fleece_state_manager_get_node_id(a);

    fleece_state_manager_set_named(a, "x", (const uint8_t*)"1", 1);
    uint64_t watermark = fleece_state_manager_get_local_timestamp(a);

    // Nothing changed since the watermark -> delta should carry zero fields.
    uint8_t* frame = NULL;
    uint32_t frame_size = 0;
    CHECK(fleece_state_manager_export_delta(a, watermark, &frame, &frame_size) == 0, "delta export with nothing new should still succeed");
    CHECK(fleece_state_manager_import(b, frame, frame_size) == 0, "importing an empty delta should succeed");
    CHECK(!fleece_state_manager_exists_named(b, a_id, "x"), "an empty delta should not (yet) reveal fields set before the watermark");
    free(frame);

    // Only the field changed after the watermark should appear in the next delta.
    fleece_state_manager_set_named(a, "y", (const uint8_t*)"2", 2);
    frame = NULL;
    frame_size = 0;
    CHECK(fleece_state_manager_export_delta(a, watermark, &frame, &frame_size) == 0, "delta export after a new write should succeed");
    CHECK(fleece_state_manager_import(b, frame, frame_size) == 0, "importing the delta should succeed");
    free(frame);

    CHECK(fleece_state_manager_exists_named(b, a_id, "y"), "the field written after the watermark should be visible via delta");
    CHECK(!fleece_state_manager_exists_named(b, a_id, "x"), "the field written before the watermark should NOT be visible via delta alone");

    // A full export (delta since 0) should carry everything, healing the gap.
    frame = NULL;
    frame_size = 0;
    fleece_state_manager_export(a, &frame, &frame_size);
    fleece_state_manager_import(b, frame, frame_size);
    free(frame);
    CHECK(fleece_state_manager_exists_named(b, a_id, "x"), "a full resync export should carry fields the earlier delta skipped");

    fleece_state_manager_destroy(a);
    fleece_state_manager_destroy(b);
    printf("Done: delta export test\n");
}

static void test_capacity_exhaustion(void) {
    printf("Running capacity exhaustion test...\n");

    FleeceStateManager* local = fleece_state_manager_create_with_node_id(0x9999999999999999ULL);
    uint64_t peer_id = 0xAAAAAAAAAAAAAAAAULL;

    int applied = 0;
    for (int i = 0; i < 200; i++) {
        char name[16];
        snprintf(name, sizeof(name), "f%d", i);
        if (fleece_state_manager_merge_named(local, peer_id, name, (const uint8_t*)"1", 1, (uint64_t)(i + 1), false) == 0) {
            applied++;
        }
    }
    CHECK(applied > 0 && applied < 200, "capacity should cap the number of merged fields, not crash or accept unbounded growth");

    char names[128][FLEECE_FIELD_NAME_MAX];
    uint32_t listed = fleece_state_manager_list_fields(local, peer_id, names, 128);
    CHECK((int)listed == applied, "listed field count should match the number actually applied");

    fleece_state_manager_destroy(local);
    printf("Done: capacity exhaustion test\n");
}

static void test_peer_liveness_ticks(void) {
    printf("Running peer liveness tick test...\n");

    FleeceStateManager* a = fleece_state_manager_create_with_node_id(0xAAAA1111AAAA1111ULL);
    FleeceStateManager* b = fleece_state_manager_create_with_node_id(0xBBBB2222BBBB2222ULL);
    uint64_t a_id = fleece_state_manager_get_node_id(a);

    CHECK(fleece_state_manager_ticks_since_seen(b, a_id) == UINT64_MAX, "a never-heard-from peer should report UINT64_MAX");

    // a has zero fields, so this is an empty delta - it should still count as a heartbeat.
    uint8_t* frame = NULL;
    uint32_t frame_size = 0;
    fleece_state_manager_export(a, &frame, &frame_size);
    fleece_state_manager_import(b, frame, frame_size);
    free(frame);
    CHECK(fleece_state_manager_ticks_since_seen(b, a_id) == 0, "importing even an empty frame should register liveness");

    fleece_state_manager_tick(b);
    fleece_state_manager_tick(b);
    fleece_state_manager_tick(b);
    CHECK(fleece_state_manager_ticks_since_seen(b, a_id) == 3, "ticks_since_seen should track elapsed ticks since the last time heard from");

    frame = NULL;
    frame_size = 0;
    fleece_state_manager_export(a, &frame, &frame_size);
    fleece_state_manager_import(b, frame, frame_size);
    free(frame);
    CHECK(fleece_state_manager_ticks_since_seen(b, a_id) == 0, "hearing from the peer again should reset ticks_since_seen");

    fleece_state_manager_destroy(a);
    fleece_state_manager_destroy(b);
    printf("Done: peer liveness tick test\n");
}

static void test_shared_fields_basic(void) {
    printf("Running shared fields basic test...\n");

    CHECK(fleece_state_manager_create_with_node_id(FLEECE_SHARED_OWNER_ID) == NULL, "creating a manager with the reserved shared owner id should fail");

    FleeceStateManager* a = fleece_state_manager_create_with_node_id(0xCCCC3333CCCC3333ULL);

    CHECK(fleece_state_manager_set_shared(a, "T1", (const uint8_t*)"{\"lat\":1}", 9) == 0, "set_shared should succeed");
    CHECK(fleece_state_manager_exists_named(a, FLEECE_SHARED_OWNER_ID, "T1"), "shared field should be readable under FLEECE_SHARED_OWNER_ID");

    uint8_t* data = NULL;
    uint32_t size = 0;
    CHECK(fleece_state_manager_get_named(a, FLEECE_SHARED_OWNER_ID, "T1", &data, &size) == 0, "get_named with the shared owner should succeed");
    CHECK(data != NULL && size == 9 && memcmp(data, "{\"lat\":1}", 9) == 0, "shared field value should match what was set");
    free(data);

    CHECK(fleece_state_manager_remove_shared(a, "T1") == 0, "remove_shared should succeed");
    CHECK(!fleece_state_manager_exists_named(a, FLEECE_SHARED_OWNER_ID, "T1"), "removed shared field should no longer exist");

    fleece_state_manager_destroy(a);
    printf("Done: shared fields basic test\n");
}

static void test_shared_fields_multi_writer(void) {
    printf("Running shared fields multi-writer test...\n");

    FleeceStateManager* a = fleece_state_manager_create_with_node_id(0xDDDD4444DDDD4444ULL);
    FleeceStateManager* b = fleece_state_manager_create_with_node_id(0xEEEE5555EEEE5555ULL);

    // a discovers and publishes a target
    fleece_state_manager_set_shared(a, "T1", (const uint8_t*)"\"discovered\"", 12);

    uint8_t* frame = NULL;
    uint32_t frame_size = 0;
    CHECK(fleece_state_manager_export_shared(a, &frame, &frame_size) == 0, "export_shared should succeed");
    CHECK(fleece_state_manager_import(b, frame, frame_size) == 0, "b should be able to import a's shared frame");
    free(frame);

    uint8_t* data = NULL;
    uint32_t size = 0;
    fleece_state_manager_get_named(b, FLEECE_SHARED_OWNER_ID, "T1", &data, &size);
    CHECK(data != NULL && size == 12 && memcmp(data, "\"discovered\"", 12) == 0, "b should see a's published target");
    free(data);
    data = NULL;

    // b claims/updates the target - a local write always applies immediately, regardless of clock skew
    CHECK(fleece_state_manager_set_shared(b, "T1", (const uint8_t*)"\"claimed\"", 9) == 0, "b claiming the target should succeed");

    // b relays its (now freshest) copy back to a
    frame = NULL;
    frame_size = 0;
    fleece_state_manager_export_shared(b, &frame, &frame_size);
    fleece_state_manager_import(a, frame, frame_size);
    free(frame);

    fleece_state_manager_get_named(a, FLEECE_SHARED_OWNER_ID, "T1", &data, &size);
    CHECK(data != NULL && size == 9 && memcmp(data, "\"claimed\"", 9) == 0, "a should see b's claim after b relays it back");
    free(data);

    uint64_t nodes[8];
    uint32_t node_count = fleece_state_manager_list_nodes(b, nodes, 8);
    for (uint32_t i = 0; i < node_count; i++) {
        CHECK(nodes[i] != FLEECE_SHARED_OWNER_ID, "FLEECE_SHARED_OWNER_ID must never be tracked as a peer node id");
    }

    fleece_state_manager_destroy(a);
    fleece_state_manager_destroy(b);
    printf("Done: shared fields multi-writer test\n");
}

static void test_shared_fields_survive_discoverer_death(void) {
    printf("Running shared fields survive discoverer death test...\n");

    FleeceStateManager* discoverer = fleece_state_manager_create_with_node_id(0xF0F0F0F0F0F0F0F0ULL);
    FleeceStateManager* other = fleece_state_manager_create_with_node_id(0x0F0F0F0F0F0F0F0FULL);

    fleece_state_manager_set_shared(discoverer, "T2", (const uint8_t*)"1", 1);
    fleece_state_manager_set_named(discoverer, "battery", (const uint8_t*)"90", 2);  // discoverer's own self data, for contrast

    uint8_t* frame = NULL;
    uint32_t frame_size = 0;
    fleece_state_manager_export_shared(discoverer, &frame, &frame_size);
    fleece_state_manager_import(other, frame, frame_size);
    free(frame);
    CHECK(fleece_state_manager_exists_named(other, FLEECE_SHARED_OWNER_ID, "T2"), "other should have the target after import");

    fleece_state_manager_destroy(discoverer);  // simulate the discoverer dying entirely

    CHECK(fleece_state_manager_exists_named(other, FLEECE_SHARED_OWNER_ID, "T2"), "the target should survive the discoverer going away entirely - it was never tied to the discoverer's liveness");

    fleece_state_manager_destroy(other);
    printf("Done: shared fields survive discoverer death test\n");
}

int main(void) {
    printf("Fleece Gossip Wire Format Tests\n");
    printf("================================\n\n");

    test_export_import_basic();
    printf("\n");
    test_lww_merge();
    printf("\n");
    test_tombstone_propagation();
    printf("\n");
    test_self_owned_rejection();
    printf("\n");
    test_delta_export();
    printf("\n");
    test_malformed_frames();
    printf("\n");
    test_capacity_exhaustion();
    printf("\n");
    test_peer_liveness_ticks();
    printf("\n");
    test_shared_fields_basic();
    printf("\n");
    test_shared_fields_multi_writer();
    printf("\n");
    test_shared_fields_survive_discoverer_death();
    printf("\n");

    if (g_failures > 0) {
        printf("%d check(s) FAILED\n", g_failures);
        return 1;
    }
    printf("All tests passed!\n");
    return 0;
}
