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
// ['F']['G'][version] prefix: [owner_node_id, [records...]], where a
// self-stream record is [is_tombstone, name, timestamp, data]. These are
// small hand-built CBOR fragments (all values kept < 24 so each head is a
// single byte: (major << 5) | value) - see RFC 8949 for the encoding.
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

    // Valid magic/version/outer-array/owner/record-count (claims 5 records) but
    // truncated to zero bytes for the records themselves - the CBOR parser
    // must bounds-check every item read, not just the fixed-size header.
    uint8_t truncated_records[] = {'F', 'G', 2, CBOR_ARRAY(2), CBOR_UINT(5), CBOR_ARRAY(5)};
    CHECK(fleece_state_manager_import(m, truncated_records, sizeof(truncated_records)) != 0, "truncated field records should be rejected without crashing");

    // Outer array declares 3 elements instead of the required 2 (type/shape confusion).
    uint8_t wrong_outer_arity[] = {'F', 'G', 2, CBOR_ARRAY(3), CBOR_UINT(5), CBOR_ARRAY(0)};
    CHECK(fleece_state_manager_import(m, wrong_outer_arity, sizeof(wrong_outer_arity)) != 0, "wrong outer array arity should be rejected");

    // owner_node_id encoded as a text string instead of a uint (major-type confusion).
    uint8_t wrong_owner_type[] = {'F', 'G', 2, CBOR_ARRAY(2), CBOR_TEXT(0), CBOR_ARRAY(0)};
    CHECK(fleece_state_manager_import(m, wrong_owner_type, sizeof(wrong_owner_type)) != 0, "non-uint owner_node_id should be rejected");

    // A single well-formed record (owner=5, is_tombstone=false, name="x", ts=1,
    // data=[1 byte]) but the byte-string length claims 9 bytes when only 1 remains.
    uint8_t oversized_data_len[] = {
        'F', 'G', 2, CBOR_ARRAY(2), CBOR_UINT(5), CBOR_ARRAY(1),
        CBOR_ARRAY(4), CBOR_BOOL_FALSE, CBOR_TEXT(1), 'x', CBOR_UINT(1), CBOR_BYTES(9), 0xAB
    };
    CHECK(fleece_state_manager_import(m, oversized_data_len, sizeof(oversized_data_len)) != 0, "a data length exceeding the remaining buffer should be rejected");

    // Same shape, but the record's own inner array claims 3 elements instead of 4.
    uint8_t wrong_record_arity[] = {
        'F', 'G', 2, CBOR_ARRAY(2), CBOR_UINT(5), CBOR_ARRAY(1),
        CBOR_ARRAY(3), CBOR_BOOL_FALSE, CBOR_TEXT(1), 'x'
    };
    CHECK(fleece_state_manager_import(m, wrong_record_arity, sizeof(wrong_record_arity)) != 0, "wrong per-record array arity should be rejected");

    // A well-formed, fully in-bounds record should actually be accepted (positive control
    // proving the above rejections are about the corruption, not the hand-built encoding itself).
    uint8_t valid_record[] = {
        'F', 'G', 2, CBOR_ARRAY(2), CBOR_UINT(5), CBOR_ARRAY(1),
        CBOR_ARRAY(4), CBOR_BOOL_FALSE, CBOR_TEXT(1), 'x', CBOR_UINT(1), CBOR_BYTES(1), 0xAB
    };
    CHECK(fleece_state_manager_import(m, valid_record, sizeof(valid_record)) == 0, "a well-formed hand-built CBOR frame should be accepted");
    CHECK(fleece_state_manager_exists_named(m, 5, "x"), "the accepted record's field should actually be readable back");

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
    test_set_shared_cas();
    printf("\n");

    if (g_failures > 0) {
        printf("%d check(s) FAILED\n", g_failures);
        return 1;
    }
    printf("All tests passed!\n");
    return 0;
}
