// Fleece Embedded (QuickJS) Unit Tests
// Exercises the real self/swarm Proxy bindings and lifecycle functions,
// independent of the runtime loop.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "state/fleece_state_manager.h"
#include "embedded/fleece_embedded.h"
#include "fleece_alloc.h"

static int g_failures = 0;

#define CHECK(cond, msg)                          \
    do {                                           \
        if (!(cond)) {                              \
            printf("FAILED: %s\n", msg);            \
            g_failures++;                           \
        }                                            \
    } while (0)

static FleeceEmbedded* make_embedded(FleeceStateManager* manager) {
    FleeceEmbedded* embedded = fleece_embedded_create();
    fleece_embedded_set_state_manager(embedded, manager);
    fleece_embedded_register_c_functions(embedded);
    return embedded;
}

static void test_self_read_write(void) {
    printf("Running self read/write test...\n");

    FleeceStateManager* manager = fleece_state_manager_create_with_node_id(0x1010101010101010ULL);
    FleeceEmbedded* embedded = make_embedded(manager);
    uint64_t node_id = fleece_state_manager_get_node_id(manager);

    CHECK(fleece_embedded_execute(embedded,
        "self.n = 1;"
        "self.o = { a: [1, 2] };"
    ) == 0, "assigning to self should not throw");

    uint8_t* data = NULL;
    uint32_t size = 0;
    CHECK(fleece_state_manager_get_named(manager, node_id, "n", &data, &size) == 0, "self.n should be stored in the state manager");
    CHECK(data != NULL && size == 1 && data[0] == '1', "self.n should be stored as the JSON text '1'");
    free(data);

    CHECK(fleece_embedded_execute(embedded,
        "if (self.n !== 1) throw new Error('self.n readback mismatch: ' + self.n);"
        "if (JSON.stringify(self.o) !== '{\"a\":[1,2]}') throw new Error('self.o readback mismatch: ' + JSON.stringify(self.o));"
        "if (!('n' in self)) throw new Error('n should be in self');"
        "var keys = Object.keys(self).sort();"
        "if (keys.length !== 2 || keys[0] !== 'n' || keys[1] !== 'o') throw new Error('Object.keys(self) mismatch: ' + keys);"
    ) == 0, "reading self back through JS should match what was written");

    CHECK(fleece_embedded_execute(embedded,
        "delete self.n;"
        "if ('n' in self) throw new Error('n should be gone after delete');"
    ) == 0, "delete self.n should remove the field");
    CHECK(!fleece_state_manager_exists_named(manager, node_id, "n"), "n should be tombstoned in the state manager after delete");

    fleece_embedded_destroy(embedded);
    fleece_state_manager_destroy(manager);
    printf("Done: self read/write test\n");
}

static void test_self_id(void) {
    printf("Running self.id test...\n");

    FleeceStateManager* manager = fleece_state_manager_create_with_node_id(0x1234567890ABCDEFULL);
    FleeceEmbedded* embedded = make_embedded(manager);

    CHECK(fleece_embedded_execute(embedded,
        "if (self.id !== '1234567890abcdef') throw new Error('self.id mismatch: ' + self.id);"
    ) == 0, "self.id should be the local node's hex id");

    fleece_embedded_destroy(embedded);
    fleece_state_manager_destroy(manager);
    printf("Done: self.id test\n");
}

static void test_swarm_view(void) {
    printf("Running swarm view test...\n");

    FleeceStateManager* manager = fleece_state_manager_create_with_node_id(0x2020202020202020ULL);
    FleeceEmbedded* embedded = make_embedded(manager);
    uint64_t peer_id = 0x3030303030303030ULL;

    CHECK(fleece_state_manager_merge_named(manager, peer_id, "battery", (const uint8_t*)"87", 2, 1, false) == 0,
          "seeding a fake peer field via merge_named should succeed");

    CHECK(fleece_embedded_execute(embedded,
        "var ids = Object.keys(swarm);"
        "if (ids.length !== 1 || ids[0] !== '3030303030303030') throw new Error('swarm node id mismatch: ' + ids);"
        "var peer = swarm['3030303030303030'];"
        "if (peer.battery !== 87) throw new Error('swarm field mismatch: ' + peer.battery);"
        "if (!('battery' in peer)) throw new Error('battery should be in the peer view');"
        "peer.battery = 1;"
        "if (peer.battery !== 87) throw new Error('swarm should be read-only, write was not ignored');"
    ) == 0, "swarm should reflect the merged peer field and stay read-only");

    fleece_embedded_destroy(embedded);
    fleece_state_manager_destroy(manager);
    printf("Done: swarm view test\n");
}

static void test_swarm_shows_shared_only_peer(void) {
    /* Regression: gossip carries only the shared/world stream (self streams
     * are node-local), so a real mesh peer owns NO node-local fields on a
     * remote node - every field of theirs arrives under
     * FLEECE_SHARED_OWNER_ID. Swarm enumeration must therefore be driven by
     * the liveness table (who we heard from), not field ownership, or
     * gossiping peers stay invisible to scripts forever. */
    printf("Running swarm-shares-shared-only-peer test...\n");

    FleeceStateManager* b = fleece_state_manager_create_with_node_id(0x5151515151515151ULL);
    CHECK(fleece_state_manager_set_shared(b, "hb", (const uint8_t*)"7", 1) == 0,
          "peer publishing one shared heartbeat should succeed");
    uint8_t* frame = NULL;
    uint32_t frame_size = 0;
    CHECK(fleece_state_manager_export_shared(b, &frame, &frame_size) == 0 && frame != NULL,
          "exporting the shared stream should succeed");

    FleeceStateManager* a = fleece_state_manager_create_with_node_id(0x5252525252525252ULL);
    bool behind = false;
    uint64_t sender = 0;
    CHECK(fleece_state_manager_import_from(a, frame, frame_size, NULL, &behind, &sender) == 0,
          "importing the peer's shared frame should succeed");
    CHECK(sender == 0x5151515151515151ULL, "the frame sender should be the peer");

    FleeceEmbedded* embedded = make_embedded(a);
    CHECK(fleece_embedded_execute(embedded,
        "var ids = Object.keys(swarm);"
        "if (ids.length !== 1 || ids[0] !== '5151515151515151') "
        "throw new Error('a shared-only gossip peer must still appear in swarm: ' + ids);"
        "if (!('hb' in world)) throw new Error('the shared field should be in world');"
    ) == 0, "swarm must list a peer heard from only via the shared stream");

    fleece_embedded_destroy(embedded);
    fleece_state_manager_destroy(a);
    fleece_state_manager_destroy(b);
    fleece_free(frame);
    printf("Done: swarm-shares-shared-only-peer test\n");
}

static void test_console_log_no_crash(void) {
    printf("Running console.log robustness test...\n");

    FleeceStateManager* manager = fleece_state_manager_create_with_node_id(0x4040404040404040ULL);
    FleeceEmbedded* embedded = make_embedded(manager);

    CHECK(fleece_embedded_execute(embedded,
        "console.log('hi', 42, self, swarm, JSON.stringify(self));"
    ) == 0, "console.log should handle strings/numbers/proxies without throwing (Symbol.toPrimitive etc.)");

    fleece_embedded_destroy(embedded);
    fleece_state_manager_destroy(manager);
    printf("Done: console.log robustness test\n");
}

static void test_lifecycle_functions(void) {
    printf("Running lifecycle function test...\n");

    FleeceStateManager* manager = fleece_state_manager_create_with_node_id(0x5050505050505050ULL);
    FleeceEmbedded* embedded = make_embedded(manager);

    CHECK(fleece_embedded_load_script(embedded,
        "function init() { self.calls = 'init'; }"
        "function step() { self.calls = self.calls + ',step'; }"
        "function destroy() { self.calls = self.calls + ',destroy'; }",
        "<test>"
    ) == 0, "loading a script that defines lifecycle functions should succeed");

    CHECK(fleece_embedded_call_init(embedded) == 0, "call_init should succeed");
    CHECK(fleece_embedded_call_step(embedded) == 0, "call_step should succeed");
    CHECK(fleece_embedded_call_reset(embedded) == 0, "call_reset on an undefined reset() should be a silent no-op, not an error");
    CHECK(fleece_embedded_call_destroy(embedded) == 0, "call_destroy should succeed");

    uint8_t* data = NULL;
    uint32_t size = 0;
    fleece_state_manager_get_named(manager, fleece_state_manager_get_node_id(manager), "calls", &data, &size);
    CHECK(data != NULL && size == strlen("\"init,step,destroy\"") &&
          memcmp(data, "\"init,step,destroy\"", size) == 0,
          "lifecycle functions should have run in order (init, step, destroy; reset skipped since undefined)");
    free(data);

    fleece_embedded_destroy(embedded);
    fleece_state_manager_destroy(manager);
    printf("Done: lifecycle function test\n");
}

static void test_world_binding(void) {
    printf("Running world binding test...\n");

    FleeceStateManager* manager = fleece_state_manager_create_with_node_id(0x6060606060606060ULL);
    FleeceEmbedded* embedded = make_embedded(manager);

    CHECK(fleece_embedded_execute(embedded,
        "world.T1 = { lat: 42.1, lon: -71.05, type: 'debris' };"
        "if (!('T1' in world)) throw new Error('T1 should be in world after write');"
        "var keys = Object.keys(world);"
        "if (keys.length !== 1 || keys[0] !== 'T1') throw new Error('Object.keys(world) mismatch: ' + keys);"
        "if (JSON.stringify(world.T1) !== '{\"lat\":42.1,\"lon\":-71.05,\"type\":\"debris\"}') throw new Error('world.T1 readback mismatch: ' + JSON.stringify(world.T1));"
    ) == 0, "writing/reading world should work like self");

    CHECK(fleece_state_manager_exists_named(manager, FLEECE_SHARED_OWNER_ID, "T1"), "T1 should be stored under FLEECE_SHARED_OWNER_ID");
    CHECK(!fleece_state_manager_exists_named(manager, fleece_state_manager_get_node_id(manager), "T1"), "T1 should NOT be stored under the local node's own id");

    CHECK(fleece_embedded_execute(embedded,
        "delete world.T1;"
        "if ('T1' in world) throw new Error('T1 should be gone after delete');"
    ) == 0, "delete world.T1 should remove it");

    fleece_embedded_destroy(embedded);
    fleece_state_manager_destroy(manager);
    printf("Done: world binding test\n");
}

static void test_world_compare_and_set(void) {
    printf("Running worldCompareAndSet test...\n");

    FleeceStateManager* manager = fleece_state_manager_create_with_node_id(0x6161616161616161ULL);
    FleeceEmbedded* embedded = make_embedded(manager);

    CHECK(fleece_embedded_execute(embedded,
        "if (worldCompareAndSet('T1', undefined, { status: 'discovered' }) !== true) throw new Error('claim-if-absent should succeed when T1 does not exist');"
        "if (JSON.stringify(world.T1) !== '{\"status\":\"discovered\"}') throw new Error('T1 should reflect the claimed value: ' + JSON.stringify(world.T1));"
    ) == 0, "worldCompareAndSet(name, undefined, value) should claim an absent field");

    CHECK(fleece_embedded_execute(embedded,
        "if (worldCompareAndSet('T1', undefined, { status: 'stolen' }) !== false) throw new Error('a second claim-if-absent should fail now that T1 exists');"
        "if (JSON.stringify(world.T1) !== '{\"status\":\"discovered\"}') throw new Error('a failed CAS must not modify the existing value: ' + JSON.stringify(world.T1));"
    ) == 0, "worldCompareAndSet should return false (not throw) on a losing race, and leave the value untouched");

    CHECK(fleece_embedded_execute(embedded,
        "if (worldCompareAndSet('T1', { status: 'wrong' }, { status: 'claimed' }) !== false) throw new Error('CAS with a mismatched expected value should fail');"
        "if (worldCompareAndSet('T1', { status: 'discovered' }, { status: 'claimed' }) !== true) throw new Error('CAS with the correct expected value should succeed');"
        "if (JSON.stringify(world.T1) !== '{\"status\":\"claimed\"}') throw new Error('T1 should now reflect the claim: ' + JSON.stringify(world.T1));"
    ) == 0, "worldCompareAndSet should support compare-and-update, not just claim-if-absent");

    fleece_embedded_destroy(embedded);
    fleece_state_manager_destroy(manager);
    printf("Done: worldCompareAndSet test\n");
}

static void test_world_survive_and_propagate(void) {
    printf("Running world survive+propagate (embedded-level) test...\n");

    FleeceStateManager* discoverer = fleece_state_manager_create_with_node_id(0x7070707070707070ULL);
    FleeceEmbedded* discoverer_js = make_embedded(discoverer);
    fleece_embedded_execute(discoverer_js, "world.T3 = { status: 'detected' };");

    FleeceStateManager* other = fleece_state_manager_create_with_node_id(0x8080808080808080ULL);
    FleeceEmbedded* other_js = make_embedded(other);

    uint8_t* frame = NULL;
    uint32_t frame_size = 0;
    fleece_state_manager_export_shared(discoverer, &frame, &frame_size);
    fleece_state_manager_import(other, frame, frame_size);
    free(frame);

    CHECK(fleece_embedded_execute(other_js,
        "if (!('T3' in world)) throw new Error('T3 should have propagated to other');"
        "if (JSON.stringify(world.T3) !== '{\"status\":\"detected\"}') throw new Error('T3 mismatch: ' + JSON.stringify(world.T3));"
        "if ('T3' in swarm) throw new Error('the shared/world owner must never leak into swarm');"
    ) == 0, "a target discovered by one node should be visible to another via gossip");

    fleece_embedded_destroy(discoverer_js);
    fleece_state_manager_destroy(discoverer);  // simulate the discoverer dying entirely

    CHECK(fleece_embedded_execute(other_js,
        "if (!('T3' in world)) throw new Error('T3 should survive after the discoverer is gone');"
    ) == 0, "the target should persist after the discovering node is destroyed");

    fleece_embedded_destroy(other_js);
    fleece_state_manager_destroy(other);
    printf("Done: world survive+propagate test\n");
}

static void test_shared_owner_hidden_from_swarm(void) {
    printf("Running shared-owner-not-in-swarm test...\n");

    FleeceStateManager* manager = fleece_state_manager_create_with_node_id(0x9090909090909090ULL);
    FleeceEmbedded* embedded = make_embedded(manager);

    fleece_state_manager_set_shared(manager, "T4", (const uint8_t*)"1", 1);
    fleece_state_manager_merge_named(manager, 0xA1A1A1A1A1A1A1A1ULL, "battery", (const uint8_t*)"50", 2, 1, false);

    CHECK(fleece_embedded_execute(embedded,
        "var ids = Object.keys(swarm);"
        "if (ids.indexOf('0000000000000000') !== -1) throw new Error('the shared owner id must never appear in swarm: ' + ids);"
        "if (ids.length !== 1 || ids[0] !== 'a1a1a1a1a1a1a1a1') throw new Error('swarm should show exactly the real peer: ' + ids);"
    ) == 0, "the shared/world owner must never leak into swarm, even alongside a real peer");

    fleece_embedded_destroy(embedded);
    fleece_state_manager_destroy(manager);
    printf("Done: shared-owner-not-in-swarm test\n");
}

static void test_peer_ttl_expiry(void) {
    printf("Running peer TTL expiry (embedded-level) test...\n");

    FleeceStateManager* manager = fleece_state_manager_create_with_node_id(0xB1B1B1B1B1B1B1B1ULL);
    FleeceEmbedded* embedded = fleece_embedded_create();
    fleece_embedded_set_state_manager(embedded, manager);
    fleece_embedded_set_peer_ttl_ticks(embedded, 2);  // short TTL, just for the test
    fleece_embedded_register_c_functions(embedded);

    uint64_t peer_id = 0xB2B2B2B2B2B2B2B2ULL;
    fleece_state_manager_merge_named(manager, peer_id, "battery", (const uint8_t*)"70", 2, 1, false);

    CHECK(fleece_embedded_execute(embedded,
        "if (Object.keys(swarm).length !== 1) throw new Error('a freshly-heard peer should be visible in swarm');"
    ) == 0, "a freshly-heard peer should be in swarm");

    fleece_state_manager_tick(manager);
    fleece_state_manager_tick(manager);
    fleece_state_manager_tick(manager);  // 3 ticks elapsed, TTL is 2 -> stale

    CHECK(fleece_embedded_execute(embedded,
        "if (Object.keys(swarm).length !== 0) throw new Error('a stale peer should have disappeared from swarm');"
        "if (swarm['b2b2b2b2b2b2b2b2'] !== undefined) throw new Error('accessing a stale peer directly should also be undefined');"
    ) == 0, "a peer not heard from within the TTL should vanish from swarm");

    CHECK(fleece_state_manager_exists_named(manager, peer_id, "battery"), "underlying peer data should NOT be deleted, only hidden from swarm");

    fleece_state_manager_merge_named(manager, peer_id, "battery", (const uint8_t*)"71", 2, 2, false);
    CHECK(fleece_embedded_execute(embedded,
        "if (Object.keys(swarm).length !== 1) throw new Error('the peer should reappear immediately once heard from again');"
    ) == 0, "hearing from a stale peer again should immediately revive it in swarm");

    fleece_embedded_destroy(embedded);
    fleece_state_manager_destroy(manager);
    printf("Done: peer TTL expiry test\n");
}

int main(void) {
    printf("Fleece Embedded (QuickJS self/swarm) Unit Tests\n");
    printf("=================================================\n\n");

    test_self_read_write();
    printf("\n");
    test_self_id();
    printf("\n");
    test_swarm_view();
    test_swarm_shows_shared_only_peer();
    printf("\n");
    test_console_log_no_crash();
    printf("\n");
    test_lifecycle_functions();
    printf("\n");
    test_world_binding();
    printf("\n");
    test_world_compare_and_set();
    printf("\n");
    test_world_survive_and_propagate();
    printf("\n");
    test_shared_owner_hidden_from_swarm();
    printf("\n");
    test_peer_ttl_expiry();
    printf("\n");

    if (g_failures > 0) {
        printf("%d check(s) FAILED\n", g_failures);
        return 1;
    }
    printf("All tests passed!\n");
    return 0;
}
