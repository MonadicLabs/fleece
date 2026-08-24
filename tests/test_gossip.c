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

// Frames are real CBOR (see fleece_state_manager.c) behind a 3-byte
// ['F']['G'][version] prefix: [sender_node_id, owner_node_id, hw, digest,
// [records...]], where a self-stream record is [is_tombstone, name, timestamp,
// data], the sender id names the transmitting node (v5 - it makes divergence
// attributable per peer), `hw` is the sender's per-stream high-water mark
// (max record timestamp), and `digest` is an order-independent hash of the
// sender's live view - the value a receiver compares against to detect it is
// behind. These are small hand-built CBOR fragments (all values kept < 24 so
// each head is a single byte: (major << 5) | value) - see RFC 8949.
#define CBOR_ARRAY(n) (uint8_t)(0x80 | (n))
#define CBOR_UINT(n)  (uint8_t)(0x00 | (n))
#define CBOR_BOOL_FALSE 0xF4
#define CBOR_TEXT(n)  (uint8_t)(0x60 | (n))
#define CBOR_BYTES(n) (uint8_t)(0x40 | (n))

static void test_malformed_frames(void) {
    printf("Running malformed frame test...\n");

    FleeceStateManager* m = fleece_state_manager_create_with_node_id(0x8888888888888888ULL);

    CHECK(fleece_state_manager_import(m, NULL, 0) != 0, "NULL frame should be rejected");

    uint8_t tiny[1] = {0};
    CHECK(fleece_state_manager_import(m, tiny, 0) != 0, "zero-length frame should be rejected");
    CHECK(fleece_state_manager_import(m, tiny, 1) != 0, "too-short frame should be rejected");

    uint8_t bad_magic[8] = {'X', 'X', 2, 0, 0, 0, 0, 0};
    CHECK(fleece_state_manager_import(m, bad_magic, sizeof(bad_magic)) != 0, "wrong magic should be rejected");

    uint8_t bad_version[8] = {'F', 'G', 99, 0, 0, 0, 0, 0};
    CHECK(fleece_state_manager_import(m, bad_version, sizeof(bad_version)) != 0, "wrong version should be rejected");

    // Valid magic/version/outer-array/sender/owner/hw/digest/record-count
    // (claims 5 records) but truncated to zero bytes for the records
    // themselves - the CBOR parser must bounds-check every item read, not just
    // the header.
    uint8_t truncated_records[] = {'F', 'G', 5, CBOR_ARRAY(5), CBOR_UINT(6), CBOR_UINT(5), CBOR_UINT(0), CBOR_UINT(0), CBOR_ARRAY(5)};
    CHECK(fleece_state_manager_import(m, truncated_records, sizeof(truncated_records)) != 0, "truncated field records should be rejected without crashing");

    // Outer array declares 4 elements instead of the required 5 (type/shape confusion).
    uint8_t wrong_outer_arity[] = {'F', 'G', 5, CBOR_ARRAY(4), CBOR_UINT(6), CBOR_UINT(5), CBOR_UINT(0), CBOR_ARRAY(0)};
    CHECK(fleece_state_manager_import(m, wrong_outer_arity, sizeof(wrong_outer_arity)) != 0, "wrong outer array arity should be rejected");

    // sender_node_id encoded as a text string instead of a uint (major-type confusion).
    uint8_t wrong_sender_type[] = {'F', 'G', 5, CBOR_ARRAY(5), CBOR_TEXT(0), CBOR_UINT(5), CBOR_UINT(0), CBOR_UINT(0), CBOR_ARRAY(0)};
    CHECK(fleece_state_manager_import(m, wrong_sender_type, sizeof(wrong_sender_type)) != 0, "non-uint sender_node_id should be rejected");

    // A frame whose sender claims to BE the receiver must be rejected
    // (loopback); exercised end-to-end via export/import in
    // test_self_owned_rejection below.

    // The reserved shared-owner id (0) is not a valid sender either.
    uint8_t reserved_sender[] = {'F', 'G', 5, CBOR_ARRAY(5), CBOR_UINT(0), CBOR_UINT(5), CBOR_UINT(0), CBOR_UINT(0), CBOR_ARRAY(0)};
    CHECK(fleece_state_manager_import(m, reserved_sender, sizeof(reserved_sender)) != 0, "sender_node_id 0 (reserved) should be rejected");

    // owner_node_id encoded as a text string instead of a uint (major-type confusion).
    uint8_t wrong_owner_type[] = {'F', 'G', 5, CBOR_ARRAY(5), CBOR_UINT(6), CBOR_TEXT(0), CBOR_UINT(0), CBOR_UINT(0), CBOR_ARRAY(0)};
    CHECK(fleece_state_manager_import(m, wrong_owner_type, sizeof(wrong_owner_type)) != 0, "non-uint owner_node_id should be rejected");

    // hw (the high-water mark) encoded as a text string instead of a uint.
    uint8_t wrong_hw_type[] = {'F', 'G', 5, CBOR_ARRAY(5), CBOR_UINT(6), CBOR_UINT(5), CBOR_TEXT(0), CBOR_UINT(0), CBOR_ARRAY(0)};
    CHECK(fleece_state_manager_import(m, wrong_hw_type, sizeof(wrong_hw_type)) != 0, "non-uint high-water mark should be rejected");

    // digest encoded as a text string instead of a uint.
    uint8_t wrong_digest_type[] = {'F', 'G', 5, CBOR_ARRAY(5), CBOR_UINT(6), CBOR_UINT(5), CBOR_UINT(0), CBOR_TEXT(0), CBOR_ARRAY(0)};
    CHECK(fleece_state_manager_import(m, wrong_digest_type, sizeof(wrong_digest_type)) != 0, "non-uint view digest should be rejected");

    // A single well-formed record (sender=6, owner=5, hw=3, digest=0,
    // is_tombstone=false, name="x", ts=3, data=[1 byte]) but the byte-string
    // length claims 9 bytes when only 1 remains.
    uint8_t oversized_data_len[] = {
        'F', 'G', 5, CBOR_ARRAY(5), CBOR_UINT(6), CBOR_UINT(5), CBOR_UINT(0), CBOR_UINT(0), CBOR_ARRAY(1),
        CBOR_ARRAY(4), CBOR_BOOL_FALSE, CBOR_TEXT(1), 'x', CBOR_UINT(1), CBOR_BYTES(9), 0xAB
    };
    CHECK(fleece_state_manager_import(m, oversized_data_len, sizeof(oversized_data_len)) != 0, "a data length exceeding the remaining buffer should be rejected");

    // Same shape, but the record's own inner array claims 3 elements instead of 4.
    uint8_t wrong_record_arity[] = {
        'F', 'G', 5, CBOR_ARRAY(5), CBOR_UINT(6), CBOR_UINT(5), CBOR_UINT(0), CBOR_UINT(0), CBOR_ARRAY(1),
        CBOR_ARRAY(3), CBOR_BOOL_FALSE, CBOR_TEXT(1), 'x'
    };
    CHECK(fleece_state_manager_import(m, wrong_record_arity, sizeof(wrong_record_arity)) != 0, "wrong per-record array arity should be rejected");

    // A well-formed, fully in-bounds record should actually be accepted (positive control
    // proving the above rejections are about the corruption, not the hand-built encoding itself).
    uint8_t valid_record[] = {
        'F', 'G', 5, CBOR_ARRAY(5), CBOR_UINT(6), CBOR_UINT(5), CBOR_UINT(3), CBOR_UINT(0), CBOR_ARRAY(1),
        CBOR_ARRAY(4), CBOR_BOOL_FALSE, CBOR_TEXT(1), 'x', CBOR_UINT(3), CBOR_BYTES(1), 0xAB
    };
    bool behind_self = false, behind_shared = false;
    uint64_t sender = 0;
    CHECK(fleece_state_manager_import_from(m, valid_record, sizeof(valid_record), &behind_self, &behind_shared, &sender) == 0,
          "a well-formed hand-built CBOR frame should be accepted");
    CHECK(sender == 6, "import_from should report the v5 header's sender id back to the caller");
    CHECK(fleece_state_manager_exists_named(m, 5, "x"), "the accepted record's field should actually be readable back");
    CHECK(fleece_state_manager_get_peer_self_hw(m, 5) == 3, "peer hw should reflect the accepted record's timestamp");

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

// On an exact-timestamp tie for the same shared field, the higher
// origin_node_id must win, and every node must converge on that SAME winner
// regardless of the order the two competing writes arrive in - otherwise two
// nodes racing to claim the same target could each end up believing THEY won.
static void test_shared_field_tie_break_convergence(void) {
    printf("Running shared field tie-break convergence test...\n");

    uint64_t low_origin = 0x1111111111111111ULL;
    uint64_t high_origin = 0x9999999999999999ULL;

    FleeceStateManager* a = fleece_state_manager_create_with_node_id(0xAAAA111122223333ULL);
    CHECK(fleece_state_manager_merge_shared(a, low_origin, "T1", (const uint8_t*)"\"low\"", 5, 100, false) == 0, "first merge (low origin) should apply");
    CHECK(fleece_state_manager_merge_shared(a, high_origin, "T1", (const uint8_t*)"\"high\"", 6, 100, false) == 0, "merge should succeed even when it loses the tie-break (return code reflects the call, not who won)");

    uint8_t* data = NULL;
    uint32_t size = 0;
    fleece_state_manager_get_named(a, FLEECE_SHARED_OWNER_ID, "T1", &data, &size);
    CHECK(data != NULL && size == 6 && memcmp(data, "\"high\"", 6) == 0, "on a tie, the higher origin_node_id should win when it arrives second");
    free(data);
    fleece_state_manager_destroy(a);

    // Reverse arrival order on a second, independent manager - must converge
    // on the SAME winner (high_origin), proving this isn't a first-write-wins
    // race dressed up as a tie-break.
    FleeceStateManager* b = fleece_state_manager_create_with_node_id(0xBBBB111122223333ULL);
    CHECK(fleece_state_manager_merge_shared(b, high_origin, "T1", (const uint8_t*)"\"high\"", 6, 100, false) == 0, "first merge (high origin) should apply");
    CHECK(fleece_state_manager_merge_shared(b, low_origin, "T1", (const uint8_t*)"\"low\"", 5, 100, false) == 0, "merge should succeed even when it loses the tie-break");

    data = NULL;
    size = 0;
    fleece_state_manager_get_named(b, FLEECE_SHARED_OWNER_ID, "T1", &data, &size);
    CHECK(data != NULL && size == 6 && memcmp(data, "\"high\"", 6) == 0, "on a tie, the higher origin_node_id should win when it arrives first too - order must not matter");
    free(data);
    fleece_state_manager_destroy(b);

    // A strictly newer timestamp still wins outright, regardless of origin id -
    // the tie-break only kicks in on an exact match.
    FleeceStateManager* c = fleece_state_manager_create_with_node_id(0xCCCC111122223333ULL);
    CHECK(fleece_state_manager_merge_shared(c, high_origin, "T1", (const uint8_t*)"\"high\"", 6, 100, false) == 0, "seed with high origin at ts=100");
    CHECK(fleece_state_manager_merge_shared(c, low_origin, "T1", (const uint8_t*)"\"newer\"", 7, 200, false) == 0, "a strictly newer write should apply");
    data = NULL;
    size = 0;
    fleece_state_manager_get_named(c, FLEECE_SHARED_OWNER_ID, "T1", &data, &size);
    CHECK(data != NULL && size == 7 && memcmp(data, "\"newer\"", 7) == 0, "a strictly newer timestamp should win outright, even from the lower origin id");
    free(data);
    fleece_state_manager_destroy(c);

    printf("Done: shared field tie-break convergence test\n");
}

// The on-demand resync driver: a receiver that misses a delta must detect it is
// behind via the sender's advertised high-water mark and pull a full snapshot.
// Scenario: A writes x(ts1), y(ts2); R syncs (not behind). A then updates x(ts3)
// and broadcasts a delta that R drops. A's next (empty) delta still advertises
// hw=ts3 - importing it must report behind_self=true so the runtime pulls a full
// export, after which R is current again.
static void test_behind_detection(void) {
    printf("Running behind-detection test...\n");

    FleeceStateManager* a = fleece_state_manager_create_with_node_id(0xEEEE9999EEEE9999ULL);
    FleeceStateManager* r = fleece_state_manager_create_with_node_id(0xFFFF8888FFFF8888ULL);
    uint64_t a_id = fleece_state_manager_get_node_id(a);

    fleece_state_manager_set_named(a, "x", (const uint8_t*)"1", 1);
    fleece_state_manager_set_named(a, "y", (const uint8_t*)"2", 1);
    uint64_t watermark = fleece_state_manager_get_local_timestamp(a);

    // Initial full sync: R is fully caught up.
    uint8_t* frame = NULL;
    uint32_t frame_size = 0;
    fleece_state_manager_export(a, &frame, &frame_size);
    bool behind_self = true, behind_shared = true;
    CHECK(fleece_state_manager_import_ex(r, frame, frame_size, &behind_self, &behind_shared) == 0, "full export should import cleanly");
    free(frame);
    CHECK(!behind_self && !behind_shared, "a receiver holding everything should not be flagged behind");

    // A updates x -> the delta would carry it, but R "drops" the packet. A then
    // advances its watermark (as the runtime does after each send), so its next
    // delta is empty yet still advertises the higher hw.
    fleece_state_manager_set_named(a, "x", (const uint8_t*)"3", 1);
    frame = NULL;
    frame_size = 0;
    fleece_state_manager_export_delta(a, watermark, &frame, &frame_size);
    CHECK(frame_size > 0, "the delta carrying x@ts3 should be non-empty");
    free(frame);  // dropped - R never sees it
    watermark = fleece_state_manager_get_local_timestamp(a);

    frame = NULL;
    frame_size = 0;
    fleece_state_manager_export_delta(a, watermark, &frame, &frame_size);
    CHECK(fleece_state_manager_import_ex(r, frame, frame_size, &behind_self, &behind_shared) == 0, "the empty delta should import cleanly");
    free(frame);
    CHECK(behind_self, "a dropped delta must be detected via the advertised high-water mark");
    CHECK(!behind_shared, "an unrelated shared stream must not be flagged behind");
    CHECK(fleece_state_manager_get_peer_self_hw(r, a_id) < fleece_state_manager_get_self_hw(a), "R should still hold less than A's current hw");

    // The runtime pulls a full snapshot - behind is cleared.
    frame = NULL;
    frame_size = 0;
    fleece_state_manager_export(a, &frame, &frame_size);
    CHECK(fleece_state_manager_import_ex(r, frame, frame_size, &behind_self, &behind_shared) == 0, "the pulled full export should import cleanly");
    free(frame);
    CHECK(!behind_self, "after the full resync, R should no longer be behind");

    uint8_t* data = NULL;
    uint32_t size = 0;
    fleece_state_manager_get_named(r, a_id, "x", &data, &size);
    CHECK(data != NULL && size == 1 && data[0] == '3', "the pulled snapshot must carry the missed update");
    free(data);

    fleece_state_manager_destroy(a);
    fleece_state_manager_destroy(r);
    printf("Done: behind-detection test\n");
}

// Shared-stream analogue: same gap, but on the shared/"world" stream, and
// import_ex must flag behind_shared (not behind_self).
static void test_behind_detection_shared(void) {
    printf("Running behind-detection (shared) test...\n");

    FleeceStateManager* a = fleece_state_manager_create_with_node_id(0x7777AAAA7777AAAAULL);
    FleeceStateManager* r = fleece_state_manager_create_with_node_id(0x8888BBBB8888BBBBULL);

    fleece_state_manager_set_shared(a, "world", (const uint8_t*)"\"v1\"", 4);
    uint64_t watermark = fleece_state_manager_get_local_timestamp(a);

    uint8_t* frame = NULL;
    uint32_t frame_size = 0;
    fleece_state_manager_export_shared(a, &frame, &frame_size);
    bool behind_self = true, behind_shared = true;
    fleece_state_manager_import_ex(r, frame, frame_size, &behind_self, &behind_shared);
    free(frame);
    CHECK(!behind_self && !behind_shared, "a synchronized receiver should not be flagged behind on either stream");

    fleece_state_manager_set_shared(a, "world", (const uint8_t*)"\"v2\"", 4);  // delta dropped by R
    frame = NULL;
    frame_size = 0;
    fleece_state_manager_export_shared_delta(a, watermark, &frame, &frame_size);
    free(frame);
    watermark = fleece_state_manager_get_local_timestamp(a);

    frame = NULL;
    frame_size = 0;
    fleece_state_manager_export_shared_delta(a, watermark, &frame, &frame_size);  // empty, hw advanced
    fleece_state_manager_import_ex(r, frame, frame_size, &behind_self, &behind_shared);
    free(frame);
    CHECK(behind_shared, "the shared stream gap should be flagged via behind_shared");
    CHECK(!behind_self, "the self stream must not be flagged by a shared-frame gap");

    fleece_state_manager_destroy(a);
    fleece_state_manager_destroy(r);
    printf("Done: behind-detection (shared) test\n");
}

// v5: a shared/"world" frame's header names the RELAYING sender, so the
// receiver can attribute divergence to - and track liveness of - real peers
// even though every record's storage owner is FLEECE_SHARED_OWNER_ID.
static void test_shared_sender_attribution(void) {
    printf("Running shared-stream sender attribution test...\n");

    FleeceStateManager* a = fleece_state_manager_create_with_node_id(0x1A2B3C4D5E6F7788ULL);
    FleeceStateManager* b = fleece_state_manager_create_with_node_id(0x8877665544332211ULL);
    uint64_t a_id = fleece_state_manager_get_node_id(a);
    uint64_t b_id = fleece_state_manager_get_node_id(b);

    CHECK(fleece_state_manager_ticks_since_seen(b, a_id) == UINT64_MAX, "b has not heard from a yet");

    fleece_state_manager_set_shared(a, "pos", (const uint8_t*)"\"p1\"", 4);

    uint8_t* frame = NULL;
    uint32_t frame_size = 0;
    CHECK(fleece_state_manager_export_shared(a, &frame, &frame_size) == 0, "shared export should succeed");

    bool behind_self = true, behind_shared = true;
    uint64_t sender = 0;
    CHECK(fleece_state_manager_import_from(b, frame, frame_size, &behind_self, &behind_shared, &sender) == 0,
          "b should import a's shared frame");
    CHECK(sender == a_id, "import_from must report the transmitting node as the sender");
    free(frame);

    // The digest matches here: b merged everything a advertised.
    CHECK(!behind_shared, "a synchronized receiver must not be flagged behind");
    CHECK(!behind_self, "a shared frame must never flag behind_self");

    // And liveness is now attributed to the REAL relay (v4 could only ever
    // see FLEECE_SHARED_OWNER_ID on world frames, which touch_peer skips).
    CHECK(fleece_state_manager_ticks_since_seen(b, a_id) == 0,
          "importing a shared frame must register the sender as a live peer");

    fleece_state_manager_destroy(a);
    fleece_state_manager_destroy(b);
    printf("Done: shared-stream sender attribution test\n");
}

static void test_set_shared_cas(void) {
    printf("Running set_shared_cas test...\n");

    FleeceStateManager* m = fleece_state_manager_create_with_node_id(0xD0D0111122223333ULL);

    // Claim-if-absent: expected_data == NULL means "must not currently exist".
    CHECK(fleece_state_manager_set_shared_cas(m, "T1", NULL, 0, (const uint8_t*)"\"mine\"", 6) == 0, "CAS with expected=absent should succeed when the field doesn't exist yet");
    uint8_t* data = NULL;
    uint32_t size = 0;
    fleece_state_manager_get_named(m, FLEECE_SHARED_OWNER_ID, "T1", &data, &size);
    CHECK(data != NULL && size == 6 && memcmp(data, "\"mine\"", 6) == 0, "the CAS'd value should actually be stored");
    free(data);

    // A second claim-if-absent must now fail - it's no longer absent.
    CHECK(fleece_state_manager_set_shared_cas(m, "T1", NULL, 0, (const uint8_t*)"\"other\"", 7) == 1, "CAS with expected=absent should report a comparison failure (1) once the field exists");
    data = NULL;
    size = 0;
    fleece_state_manager_get_named(m, FLEECE_SHARED_OWNER_ID, "T1", &data, &size);
    CHECK(data != NULL && size == 6 && memcmp(data, "\"mine\"", 6) == 0, "a failed CAS must leave the existing value untouched");
    free(data);

    // Compare-and-update: succeeds only if the expected bytes match exactly.
    CHECK(fleece_state_manager_set_shared_cas(m, "T1", (const uint8_t*)"\"wrong\"", 7, (const uint8_t*)"\"updated\"", 9) == 1, "CAS with a mismatched expected value should fail (1), not error");
    CHECK(fleece_state_manager_set_shared_cas(m, "T1", (const uint8_t*)"\"mine\"", 6, (const uint8_t*)"\"updated\"", 9) == 0, "CAS with a matching expected value should succeed");
    data = NULL;
    size = 0;
    fleece_state_manager_get_named(m, FLEECE_SHARED_OWNER_ID, "T1", &data, &size);
    CHECK(data != NULL && size == 9 && memcmp(data, "\"updated\"", 9) == 0, "a successful compare-and-update should apply the new value");
    free(data);

    // Error cases (bad arguments) return -1, distinct from a comparison failure (1).
    CHECK(fleece_state_manager_set_shared_cas(NULL, "T1", NULL, 0, (const uint8_t*)"x", 1) == -1, "CAS on a NULL manager should be a real error, not a comparison failure");
    CHECK(fleece_state_manager_set_shared_cas(m, "T1", NULL, 0, NULL, 0) == -1, "CAS with a NULL new value should be a real error");
    CHECK(fleece_state_manager_set_shared_cas(m, NULL, NULL, 0, (const uint8_t*)"x", 1) == -1, "CAS with a NULL name should be a real error");

    fleece_state_manager_destroy(m);
    printf("Done: set_shared_cas test\n");
}

// The repair handshake's two control exports must round-trip through the
// ordinary import path in their CURRENT wire shape: an INDEX reply feeds the
// diff, and a VALUE reply (export_shared_by_hash) merges like any gossip
// frame - regression: the value frame lagged the v5 header change (stayed
// arity-4 without a sender id) and every repair payload was silently
// rejected on import.
static void test_repair_frame_roundtrip(void) {
    printf("Running repair frame roundtrip test...\n");

    FleeceStateManager* a = fleece_state_manager_create_with_node_id(0xA000000000000001ULL);
    FleeceStateManager* b = fleece_state_manager_create_with_node_id(0xB000000000000002ULL);
    uint64_t b_id = fleece_state_manager_get_node_id(b);

    fleece_state_manager_set_shared(a, "k1", (const uint8_t*)"\"v1\"", 4);
    fleece_state_manager_set_shared(a, "k2", (const uint8_t*)"\"v2\"", 4);
    uint64_t ts_k1 = 0, origin_k1 = 0;
    fleece_state_manager_get_meta_named(a, FLEECE_SHARED_OWNER_ID, "k1", &ts_k1, &origin_k1);

    // Index: parses as [hash, ts] pairs and reports k1's stored ts.
    uint8_t* idx = NULL;
    uint32_t idx_size = 0;
    CHECK(fleece_state_manager_export_shared_index(a, &idx, &idx_size) == 0, "index export should succeed");
    free(idx);

    // Value frame for k1 by hash: must import cleanly into b (v5 sender check
    // included), carrying the record and reporting a as the sender.
    uint32_t hash_k1 = 2166136261u;
    for (const unsigned char* p = (const unsigned char*)"k1"; *p; p++) {
        hash_k1 ^= *p;
        hash_k1 *= 16777619u;
    }
    uint8_t* val = NULL;
    uint32_t val_size = 0;
    CHECK(fleece_state_manager_export_shared_by_hash(a, &hash_k1, 1, &val, &val_size) == 0, "value export should succeed");

    bool behind_self = true, behind_shared = true;
    uint64_t sender = 0;
    CHECK(fleece_state_manager_import_from(b, val, val_size, &behind_self, &behind_shared, &sender) == 0,
          "a served VALUE frame must import (v5 wire shape)");
    uint64_t a_id = fleece_state_manager_get_node_id(a);
    CHECK(sender == a_id, "the VALUE frame's sender must be the serving node");
    // The frame advertises a's FULL view digest, and b does not yet hold k2,
    // so behind_shared must fire - which is exactly what drives the requester
    // to keep repairing until the next diff comes back empty.
    CHECK(behind_shared, "a VALUE frame advertising the server's full-view digest must flag the remaining gap");

    uint8_t* data = NULL;
    uint32_t size = 0;
    CHECK(fleece_state_manager_get_named(b, FLEECE_SHARED_OWNER_ID, "k1", &data, &size) == 0, "the repaired key should exist on b");
    CHECK(data != NULL && size == 4 && memcmp(data, "\"v1\"", 4) == 0, "the repaired value should match");
    free(data);

    uint64_t ts_b = 0, origin_b = 0;
    fleece_state_manager_get_meta_named(b, FLEECE_SHARED_OWNER_ID, "k1", &ts_b, &origin_b);
    CHECK(ts_b == ts_k1, "repaired record must carry the origin's LWW timestamp");
    CHECK(origin_b == origin_k1, "repaired record must carry the authoring origin");

    // And b must now consider itself current with that key at that ts.
    CHECK(fleece_state_manager_shared_at_least(b, hash_k1, ts_k1) == 1, "post-repair, shared_at_least should report the key is held at the fetched ts");

    free(val);
    fleece_state_manager_destroy(a);
    fleece_state_manager_destroy(b);
    printf("Done: repair frame roundtrip test\n");
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
    test_shared_field_tie_break_convergence();
    printf("\n");
    test_behind_detection();
    printf("\n");
    test_behind_detection_shared();
    printf("\n");
    test_shared_sender_attribution();
    printf("\n");
    test_set_shared_cas();
    printf("\n");
    test_repair_frame_roundtrip();
    printf("\n");

    if (g_failures > 0) {
        printf("%d check(s) FAILED\n", g_failures);
        return 1;
    }
    printf("All tests passed!\n");
    return 0;
}
