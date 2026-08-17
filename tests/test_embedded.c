// Fleece Embedded (QuickJS) Unit Tests
// Exercises the real self/swarm Proxy bindings and lifecycle functions,
// independent of the runtime loop.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "state/fleece_state_manager.h"
#include "embedded/fleece_embedded.h"

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

int main(void) {
    printf("Fleece Embedded (QuickJS self/swarm) Unit Tests\n");
    printf("=================================================\n\n");

    test_self_read_write();
    printf("\n");
    test_self_id();
    printf("\n");
    test_swarm_view();
    printf("\n");
    test_console_log_no_crash();
    printf("\n");
    test_lifecycle_functions();
    printf("\n");

    if (g_failures > 0) {
        printf("%d check(s) FAILED\n", g_failures);
        return 1;
    }
    printf("All tests passed!\n");
    return 0;
}
