// Fleece pluggable allocator tests.
// Exercises the two allocator hooks:
//   1. fleece_embedded_set_allocator() - redirects the fleece library's own
//      malloc/calloc/realloc/free (state-manager field values, GOAP planner
//      tables, embedded/comms/runtime structs) through a supplied allocator.
//   2. fleece_embedded_create_with_allocator() - routes the QuickJS engine's
//      GC heap through a JSMallocFunctions pair (a static pool on an MCU).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "state/fleece_state_manager.h"
#include "embedded/fleece_embedded.h"
#include "runtime/fleece_runtime.h"

static int g_failures = 0;

#define CHECK(cond, msg)                          \
    do {                                           \
        if (!(cond)) {                              \
            printf("FAILED: %s\n", msg);            \
            g_failures++;                           \
        }                                            \
    } while (0)

// --- Counting allocator (fleece lib side) --------------------------------

static size_t g_lib_alloc_calls = 0;
static size_t g_lib_free_calls = 0;
static int g_lib_active = 0;

static void* counting_malloc(size_t size) {
    if (g_lib_active) g_lib_alloc_calls++;
    return malloc(size);
}
static void counting_free(void* ptr) {
    if (g_lib_active) g_lib_free_calls++;
    free(ptr);
}
static void* counting_calloc(size_t count, size_t size) {
    if (g_lib_active) g_lib_alloc_calls++;
    return calloc(count, size);
}
static void* counting_realloc(void* ptr, size_t size) {
    if (g_lib_active) g_lib_alloc_calls++;
    return realloc(ptr, size);
}

static void test_fleece_lib_allocator(void) {
    printf("Running unified allocator test...\n");

    // Default: a state manager allocates through the standard allocator.
    FleeceStateManager* manager = fleece_state_manager_create();
    CHECK(manager != NULL, "state manager should create with default allocator");
    fleece_state_manager_destroy(manager);

    // Install the counting allocator, then create objects that allocate.
    g_lib_alloc_calls = 0;
    g_lib_free_calls = 0;
    g_lib_active = 1;
    fleece_embedded_set_allocator(counting_malloc, counting_free, counting_calloc, counting_realloc);

    // fleece_embedded_create() must route BOTH the library's own allocations
    // (structs, scratch buffers) AND the QuickJS engine's GC heap through the
    // configured allocator - a single set_allocator() call covers the stack.
    FleeceStateManager* m = fleece_state_manager_create_with_node_id(0x3030303030303030ULL);
    FleeceEmbedded* embedded = fleece_embedded_create();
    CHECK(embedded != NULL, "embedded should create with custom allocator");
    CHECK(g_lib_alloc_calls > 0, "custom allocator should be used during embedded_create");

    size_t lib_before = g_lib_alloc_calls;
    fleece_embedded_set_state_manager(embedded, m);
    fleece_embedded_register_c_functions(embedded);
    CHECK(fleece_embedded_execute(embedded, "self.foo = { a: [1, 2, 3] };") == 0,
          "executing a script should succeed");
    CHECK(g_lib_alloc_calls > lib_before,
          "JS engine allocation should route through the custom allocator too");

    g_lib_active = 0;
    fleece_embedded_destroy(embedded);
    fleece_state_manager_destroy(m);

    // Reset to defaults so the rest of the suite is unaffected.
    fleece_embedded_set_allocator(NULL, NULL, NULL, NULL);
}

// --- Counting allocator (QuickJS engine side) ----------------------------

static size_t g_js_alloc_calls = 0;
static size_t g_js_free_calls = 0;

static void* js_count_malloc(void* opaque, size_t size) {
    (void)opaque;
    g_js_alloc_calls++;
    return malloc(size);
}
static void* js_count_calloc(void* opaque, size_t count, size_t size) {
    (void)opaque;
    g_js_alloc_calls++;
    return calloc(count, size);
}
static void* js_count_realloc(void* opaque, void* ptr, size_t size) {
    (void)opaque;
    g_js_alloc_calls++;
    return realloc(ptr, size);
}
static void js_count_free(void* opaque, void* ptr) {
    (void)opaque;
    g_js_free_calls++;
    free(ptr);
}
static size_t js_count_usable(const void* ptr) {
    (void)ptr;
    return 0;  // unknown - QuickJS falls back to tracking internally
}

static void test_quickjs_allocator(void) {
    printf("Running dedicated JS-arena override test...\n");

    JSMallocFunctions mf;
    mf.js_calloc = js_count_calloc;
    mf.js_malloc = js_count_malloc;
    mf.js_free = js_count_free;
    mf.js_realloc = js_count_realloc;
    mf.js_malloc_usable_size = js_count_usable;

    g_js_alloc_calls = 0;
    g_js_free_calls = 0;

    FleeceStateManager* manager = fleece_state_manager_create_with_node_id(0x2020202020202020ULL);
    FleeceEmbedded* embedded = fleece_embedded_create_with_allocator(&mf, NULL);
    CHECK(embedded != NULL, "embedded should create with JS allocator");
    CHECK(g_js_alloc_calls > 0, "QuickJS runtime creation should allocate through the custom allocator");

    fleece_embedded_set_state_manager(embedded, manager);
    fleece_embedded_register_c_functions(embedded);

    // Executing a script should allocate JS heap through the allocator.
    size_t before = g_js_alloc_calls;
    CHECK(fleece_embedded_execute(embedded, "self.foo = { a: [1, 2, 3], s: 'hello world' };") == 0,
          "executing a script should succeed");
    CHECK(g_js_alloc_calls > before, "executing a script should allocate through the custom allocator");

    fleece_embedded_destroy(embedded);
    fleece_state_manager_destroy(manager);
}

int main(void) {
    printf("Fleece allocator tests\n");
    printf("======================\n\n");

    test_fleece_lib_allocator();
    test_quickjs_allocator();

    if (g_failures == 0) {
        printf("\nAll allocator tests passed.\n");
        return 0;
    }
    printf("\n%d allocator test(s) FAILED.\n", g_failures);
    return 1;
}
