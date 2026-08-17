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

static int copy_json_literal(const char* text, uint8_t** result_json, uint32_t* result_size) {
    uint32_t len = (uint32_t)strlen(text);
    uint8_t* copy = (uint8_t*)malloc(len);
    if (!copy) return -1;
    memcpy(copy, text, len);
    *result_json = copy;
    *result_size = len;
    return 0;
}

// Always returns "A", regardless of args (proves independent dispatch from marker_b_fn/echo_fn).
static int marker_a_fn(const uint8_t* args_json, uint32_t args_size, uint8_t** result_json, uint32_t* result_size, void* user_data) {
    (void)args_json; (void)args_size; (void)user_data;
    return copy_json_literal("\"A\"", result_json, result_size);
}

// Always returns "B", regardless of args.
static int marker_b_fn(const uint8_t* args_json, uint32_t args_size, uint8_t** result_json, uint32_t* result_size, void* user_data) {
    (void)args_json; (void)args_size; (void)user_data;
    return copy_json_literal("\"B\"", result_json, result_size);
}

// Returns bytes that are deliberately not valid JSON, to exercise graceful
// degradation (should surface as JS `undefined`, not a crash or a thrown parse error).
static int garbage_result_fn(const uint8_t* args_json, uint32_t args_size, uint8_t** result_json, uint32_t* result_size, void* user_data) {
    (void)args_json; (void)args_size; (void)user_data;
    return copy_json_literal("not valid json {{{", result_json, result_size);
}

// Returns its user_data (a C string) as a quoted JSON string - proves each
// registration's user_data stays isolated even when the same function pointer
// is registered under multiple names (e.g. a generic "set_gpio" bound to
// different pin numbers).
static int read_user_data_fn(const uint8_t* args_json, uint32_t args_size, uint8_t** result_json, uint32_t* result_size, void* user_data) {
    (void)args_json; (void)args_size;
    const char* text = (const char*)user_data;
    size_t len = strlen(text);
    uint8_t* copy = (uint8_t*)malloc(len + 2);
    if (!copy) return -1;
    copy[0] = '"';
    memcpy(copy + 1, text, len);
    copy[len + 1] = '"';
    *result_json = copy;
    *result_size = (uint32_t)(len + 2);
    return 0;
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

static void test_multiple_functions_independent_dispatch(void) {
    printf("Running multiple functions independent dispatch test...\n");

    FleecePlatform* platform = fleece_platform_create();
    fleece_platform_register(platform, "fnA", marker_a_fn, NULL);
    fleece_platform_register(platform, "fnB", marker_b_fn, NULL);
    fleece_platform_register(platform, "echo", echo_fn, NULL);

    uint8_t* result = NULL;
    uint32_t result_size = 0;

    fleece_platform_call(platform, "fnA", (const uint8_t*)"[1,2,3]", 7, &result, &result_size);
    CHECK(result != NULL && result_size == 3 && memcmp(result, "\"A\"", 3) == 0, "fnA should always return \"A\", ignoring its args");
    free(result);
    result = NULL;
    result_size = 0;

    fleece_platform_call(platform, "fnB", (const uint8_t*)"[9,9,9]", 7, &result, &result_size);
    CHECK(result != NULL && result_size == 3 && memcmp(result, "\"B\"", 3) == 0, "fnB should always return \"B\", independent of fnA and its own args");
    free(result);
    result = NULL;
    result_size = 0;

    const char* args = "[\"hello\"]";
    fleece_platform_call(platform, "echo", (const uint8_t*)args, (uint32_t)strlen(args), &result, &result_size);
    CHECK(result != NULL && result_size == strlen(args) && memcmp(result, args, result_size) == 0, "echo should still behave correctly alongside fnA/fnB in the same registry");
    free(result);

    fleece_platform_destroy(platform);
    printf("Done: multiple functions independent dispatch test\n");
}

static void test_reregister_replaces_function(void) {
    printf("Running re-register replaces function test...\n");

    FleecePlatform* platform = fleece_platform_create();
    fleece_platform_register(platform, "foo", marker_a_fn, NULL);

    uint8_t* result = NULL;
    uint32_t result_size = 0;
    fleece_platform_call(platform, "foo", NULL, 0, &result, &result_size);
    CHECK(result != NULL && result_size == 3 && memcmp(result, "\"A\"", 3) == 0, "foo should initially resolve to marker_a_fn");
    free(result);
    result = NULL;
    result_size = 0;

    CHECK(fleece_platform_register(platform, "foo", marker_b_fn, NULL) == 0, "re-registering an existing name should succeed (replace, not duplicate)");

    char names[8][FLEECE_PLATFORM_FUNCTION_NAME_MAX];
    uint32_t count = fleece_platform_list_functions(platform, names, 8);
    CHECK(count == 1, "re-registering the same name should not create a second registry entry");

    fleece_platform_call(platform, "foo", NULL, 0, &result, &result_size);
    CHECK(result != NULL && result_size == 3 && memcmp(result, "\"B\"", 3) == 0, "foo should resolve to marker_b_fn after being replaced");
    free(result);

    fleece_platform_destroy(platform);
    printf("Done: re-register replaces function test\n");
}

static void test_registry_capacity(void) {
    printf("Running registry capacity test...\n");

    FleecePlatform* platform = fleece_platform_create();

    int registered = 0;
    for (int i = 0; i < 100; i++) {
        char name[16];
        snprintf(name, sizeof(name), "f%d", i);
        if (fleece_platform_register(platform, name, marker_a_fn, NULL) == 0) {
            registered++;
        }
    }
    CHECK(registered > 0 && registered < 100, "capacity should cap the number of registered functions, not crash or accept unbounded growth");

    char names[128][FLEECE_PLATFORM_FUNCTION_NAME_MAX];
    uint32_t listed = fleece_platform_list_functions(platform, names, 128);
    CHECK((int)listed == registered, "listed function count should match the number actually registered");

    uint8_t* result = NULL;
    uint32_t result_size = 0;
    CHECK(fleece_platform_call(platform, "f0", NULL, 0, &result, &result_size) == 0, "an early-registered function should still be callable once the registry is full");
    free(result);

    fleece_platform_destroy(platform);
    printf("Done: registry capacity test\n");
}

static void test_long_function_name_truncation(void) {
    printf("Running long function name truncation test...\n");

    FleecePlatform* platform = fleece_platform_create();

    // Longer than FLEECE_PLATFORM_FUNCTION_NAME_MAX - 1 (31 chars) - gets silently
    // truncated, same as named state-manager fields. Documenting the actual
    // behavior here rather than asserting it's ideal: callers should keep names
    // to 31 chars or fewer to avoid this.
    const char* long_name = "this_is_a_very_long_function_name_that_exceeds_the_limit";
    CHECK(fleece_platform_register(platform, long_name, marker_a_fn, NULL) == 0, "registering a too-long name should still succeed (truncated)");

    char names[8][FLEECE_PLATFORM_FUNCTION_NAME_MAX];
    uint32_t count = fleece_platform_list_functions(platform, names, 8);
    CHECK(count == 1, "the truncated registration should show up exactly once");
    CHECK(strlen(names[0]) == FLEECE_PLATFORM_FUNCTION_NAME_MAX - 1, "the stored name should be truncated to the max length");
    CHECK(strncmp(names[0], long_name, FLEECE_PLATFORM_FUNCTION_NAME_MAX - 1) == 0, "the truncated name should be a prefix of what was registered");

    CHECK(!fleece_platform_has_function(platform, long_name), "querying with the full untruncated name should NOT find it - it was stored truncated");
    CHECK(fleece_platform_has_function(platform, names[0]), "querying with the truncated name should find it");

    uint8_t* result = NULL;
    uint32_t result_size = 0;
    CHECK(fleece_platform_call(platform, names[0], NULL, 0, &result, &result_size) == 0, "calling by the truncated name should succeed");
    free(result);

    fleece_platform_destroy(platform);
    printf("Done: long function name truncation test\n");
}

static void test_user_data_isolation(void) {
    printf("Running user_data isolation test...\n");

    FleecePlatform* platform = fleece_platform_create();
    fleece_platform_register(platform, "who1", read_user_data_fn, (void*)"first");
    fleece_platform_register(platform, "who2", read_user_data_fn, (void*)"second");

    uint8_t* result = NULL;
    uint32_t result_size = 0;
    fleece_platform_call(platform, "who1", NULL, 0, &result, &result_size);
    CHECK(result != NULL && result_size == 7 && memcmp(result, "\"first\"", 7) == 0, "who1 should see its own user_data");
    free(result);
    result = NULL;
    result_size = 0;

    fleece_platform_call(platform, "who2", NULL, 0, &result, &result_size);
    CHECK(result != NULL && result_size == 8 && memcmp(result, "\"second\"", 8) == 0, "who2 should see its own user_data, independent of who1 despite sharing the same function pointer");
    free(result);

    fleece_platform_destroy(platform);
    printf("Done: user_data isolation test\n");
}

static void test_multi_type_args_roundtrip(void) {
    printf("Running multi-type args roundtrip (embedded-level) test...\n");

    FleeceStateManager* manager = fleece_state_manager_create_with_node_id(0xD1D1D1D1D1D1D1D1ULL);
    FleecePlatform* platform = fleece_platform_create();
    fleece_platform_register(platform, "echo", echo_fn, NULL);

    FleeceEmbedded* embedded = fleece_embedded_create();
    fleece_embedded_set_state_manager(embedded, manager);
    fleece_embedded_set_platform(embedded, platform);
    fleece_embedded_register_c_functions(embedded);

    CHECK(fleece_embedded_execute(embedded,
        "var result = platform.echo(1, 'two', true, false, null, 3.5, { a: 1, b: [1, 2, 3] }, [1, [2, 3], { c: 'd' }]);"
        "var expected = '[1,\"two\",true,false,null,3.5,{\"a\":1,\"b\":[1,2,3]},[1,[2,3],{\"c\":\"d\"}]]';"
        "if (JSON.stringify(result) !== expected) throw new Error('multi-type args roundtrip mismatch: ' + JSON.stringify(result));"
    ) == 0, "numbers, strings, booleans, null, floats, and nested objects/arrays should all round-trip in the given order");

    fleece_embedded_destroy(embedded);
    fleece_platform_destroy(platform);
    fleece_state_manager_destroy(manager);
    printf("Done: multi-type args roundtrip test\n");
}

static void test_zero_and_many_args(void) {
    printf("Running zero and many args (embedded-level) test...\n");

    FleeceStateManager* manager = fleece_state_manager_create_with_node_id(0xD2D2D2D2D2D2D2D2ULL);
    FleecePlatform* platform = fleece_platform_create();
    fleece_platform_register(platform, "echo", echo_fn, NULL);

    FleeceEmbedded* embedded = fleece_embedded_create();
    fleece_embedded_set_state_manager(embedded, manager);
    fleece_embedded_set_platform(embedded, platform);
    fleece_embedded_register_c_functions(embedded);

    CHECK(fleece_embedded_execute(embedded,
        "if (JSON.stringify(platform.echo()) !== '[]') throw new Error('a zero-argument call should round-trip as an empty array');"
    ) == 0, "calling with zero arguments should work");

    CHECK(fleece_embedded_execute(embedded,
        "var args = [];"
        "for (var i = 0; i < 20; i++) args.push(i);"
        "var result = platform.echo.apply(null, args);"
        "if (JSON.stringify(result) !== JSON.stringify(args)) throw new Error('20-argument call mismatch: ' + JSON.stringify(result));"
    ) == 0, "calling with a large number of arguments should not be artificially capped or reordered");

    fleece_embedded_destroy(embedded);
    fleece_platform_destroy(platform);
    fleece_state_manager_destroy(manager);
    printf("Done: zero and many args test\n");
}

static void test_malformed_native_result_no_crash(void) {
    printf("Running malformed native result (embedded-level) test...\n");

    FleeceStateManager* manager = fleece_state_manager_create_with_node_id(0xD3D3D3D3D3D3D3D3ULL);
    FleecePlatform* platform = fleece_platform_create();
    fleece_platform_register(platform, "garbage", garbage_result_fn, NULL);

    FleeceEmbedded* embedded = fleece_embedded_create();
    fleece_embedded_set_state_manager(embedded, manager);
    fleece_embedded_set_platform(embedded, platform);
    fleece_embedded_register_c_functions(embedded);

    CHECK(fleece_embedded_execute(embedded,
        "var result = platform.garbage();"
        "if (result !== undefined) throw new Error('a function returning invalid JSON should degrade to undefined, not throw or crash: ' + JSON.stringify(result));"
    ) == 0, "a native function returning malformed JSON should not crash or throw through the JS bridge");

    fleece_embedded_destroy(embedded);
    fleece_platform_destroy(platform);
    fleece_state_manager_destroy(manager);
    printf("Done: malformed native result test\n");
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
    test_multiple_functions_independent_dispatch();
    printf("\n");
    test_reregister_replaces_function();
    printf("\n");
    test_registry_capacity();
    printf("\n");
    test_long_function_name_truncation();
    printf("\n");
    test_user_data_isolation();
    printf("\n");
    test_multi_type_args_roundtrip();
    printf("\n");
    test_zero_and_many_args();
    printf("\n");
    test_malformed_native_result_no_crash();
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
