// Fleece Platform Registry Tests
// fleece_platform is a pure name -> native function registry: fleece defines
// no functions of its own. These tests register throwaway test functions
// (never anything drone/robot-specific) to exercise the registry and, in
// test_embedded_binding(), the platform.<name>(...) JS bridge.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "platform/fleece_platform.h"
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

// Echoes its JSON args array straight back as the result.
static int echo_fn(const uint8_t* args_json, uint32_t args_size, uint8_t** result_json, uint32_t* result_size, void* user_data) {
    (void)user_data;
    uint8_t* copy = (uint8_t*)malloc(args_size);
    if (!copy) return -1;
    memcpy(copy, args_json, args_size);
    *result_json = copy;
    *result_size = args_size;
    return 0;
}

// Takes no args, returns nothing (exercises the "no return value" path).
static int noop_fn(const uint8_t* args_json, uint32_t args_size, uint8_t** result_json, uint32_t* result_size, void* user_data) {
    (void)args_json; (void)args_size; (void)result_json; (void)result_size; (void)user_data;
    return 0;
}

// Always fails (exercises the error path).
static int failing_fn(const uint8_t* args_json, uint32_t args_size, uint8_t** result_json, uint32_t* result_size, void* user_data) {
    (void)args_json; (void)args_size; (void)result_json; (void)result_size; (void)user_data;
    return -1;
}

static void test_registry_basics(void) {
    printf("Running registry basics test...\n");

    FleecePlatform* platform = fleece_platform_create();
    CHECK(platform != NULL, "create should succeed");

    CHECK(!fleece_platform_has_function(platform, "echo"), "unregistered function should not be reported as present");

    CHECK(fleece_platform_register(platform, "echo", echo_fn, NULL) == 0, "registering a function should succeed");
    CHECK(fleece_platform_has_function(platform, "echo"), "registered function should be reported as present");

    char names[8][FLEECE_PLATFORM_FUNCTION_NAME_MAX];
    uint32_t count = fleece_platform_list_functions(platform, names, 8);
    CHECK(count == 1 && strcmp(names[0], "echo") == 0, "list_functions should report exactly the registered name");

    CHECK(fleece_platform_register(platform, "noop", noop_fn, NULL) == 0, "registering a second function should succeed");
    count = fleece_platform_list_functions(platform, names, 8);
    CHECK(count == 2, "list_functions should report both registered names");

    CHECK(fleece_platform_unregister(platform, "noop") == 0, "unregistering a registered function should succeed");
    CHECK(!fleece_platform_has_function(platform, "noop"), "unregistered function should no longer be present");
    CHECK(fleece_platform_unregister(platform, "noop") != 0, "unregistering an already-gone function should fail");

    fleece_platform_destroy(platform);
    printf("Done: registry basics test\n");
}

static void test_call_roundtrip(void) {
    printf("Running call roundtrip test...\n");

    FleecePlatform* platform = fleece_platform_create();
    fleece_platform_register(platform, "echo", echo_fn, NULL);
    fleece_platform_register(platform, "noop", noop_fn, NULL);
    fleece_platform_register(platform, "fail", failing_fn, NULL);

    const char* args = "[1,2,3]";
    uint8_t* result = NULL;
    uint32_t result_size = 0;
    CHECK(fleece_platform_call(platform, "echo", (const uint8_t*)args, (uint32_t)strlen(args), &result, &result_size) == 0, "calling a registered function should succeed");
    CHECK(result != NULL && result_size == strlen(args) && memcmp(result, args, result_size) == 0, "echo should return exactly what it was given");
    free(result);

    result = NULL;
    result_size = 0;
    CHECK(fleece_platform_call(platform, "noop", NULL, 0, &result, &result_size) == 0, "calling noop should succeed");
    CHECK(result == NULL && result_size == 0, "a function that sets no result should leave result_json NULL");

    CHECK(fleece_platform_call(platform, "fail", NULL, 0, &result, &result_size) != 0, "a function returning failure should propagate as an error");

    CHECK(fleece_platform_call(platform, "does_not_exist", NULL, 0, &result, &result_size) != 0, "calling an unregistered name should fail");

    fleece_platform_destroy(platform);
    printf("Done: call roundtrip test\n");
}

static void test_embedded_binding(void) {
    printf("Running platform.<name>() JS binding test...\n");

    FleeceStateManager* manager = fleece_state_manager_create_with_node_id(0xD0D0D0D0D0D0D0D0ULL);
    FleecePlatform* platform = fleece_platform_create();
    fleece_platform_register(platform, "echo", echo_fn, NULL);
    fleece_platform_register(platform, "fail", failing_fn, NULL);

    FleeceEmbedded* embedded = fleece_embedded_create();
    fleece_embedded_set_state_manager(embedded, manager);
    fleece_embedded_set_platform(embedded, platform);
    fleece_embedded_register_c_functions(embedded);

    CHECK(fleece_embedded_execute(embedded,
        "var names = Object.keys(platform).sort();"
        "if (names.length !== 2 || names[0] !== 'echo' || names[1] !== 'fail') throw new Error('Object.keys(platform) mismatch: ' + names);"
        "if (!('echo' in platform)) throw new Error('echo should be in platform');"
        "if ('not_registered' in platform) throw new Error('unregistered name should not be in platform');"
        "if (platform.not_registered !== undefined) throw new Error('unregistered platform.x should be undefined');"
        "var result = platform.echo(1, 'two', [3]);"
        "if (JSON.stringify(result) !== '[1,\"two\",[3]]') throw new Error('platform.echo roundtrip mismatch: ' + JSON.stringify(result));"
    ) == 0, "platform Proxy should reflect the registry and round-trip arguments through echo");

    CHECK(fleece_embedded_execute(embedded,
        "var threw = false;"
        "try { platform.fail(); } catch (e) { threw = true; }"
        "if (!threw) throw new Error('platform.fail() should have thrown');"
    ) == 0, "a platform function returning failure should surface as a thrown JS exception");

    fleece_embedded_destroy(embedded);
    fleece_platform_destroy(platform);
    fleece_state_manager_destroy(manager);
    printf("Done: platform.<name>() JS binding test\n");
}

static void test_empty_platform_is_harmless(void) {
    printf("Running empty platform test...\n");

    FleeceStateManager* manager = fleece_state_manager_create_with_node_id(0xE0E0E0E0E0E0E0E0ULL);
    FleeceEmbedded* embedded = fleece_embedded_create();
    fleece_embedded_set_state_manager(embedded, manager);
    // Deliberately never call fleece_embedded_set_platform() - platform.xxx should stay empty/undefined.
    fleece_embedded_register_c_functions(embedded);

    CHECK(fleece_embedded_execute(embedded,
        "if (Object.keys(platform).length !== 0) throw new Error('platform should be empty when unset: ' + Object.keys(platform));"
        "if (platform.anything !== undefined) throw new Error('platform.anything should be undefined when unset');"
    ) == 0, "an embedded instance with no platform bound should expose an empty, harmless platform object");

    fleece_embedded_destroy(embedded);
    fleece_state_manager_destroy(manager);
    printf("Done: empty platform test\n");
}

int main(void) {
    printf("Fleece Platform Registry Tests\n");
    printf("===============================\n\n");

    test_registry_basics();
    printf("\n");
    test_call_roundtrip();
    printf("\n");
    test_embedded_binding();
    printf("\n");
    test_empty_platform_is_harmless();
    printf("\n");

    if (g_failures > 0) {
        printf("%d check(s) FAILED\n", g_failures);
        return 1;
    }
    printf("All tests passed!\n");
    return 0;
}
