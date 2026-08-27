#define _POSIX_C_SOURCE 200809L

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "quickjs.h"

#include "fleece_embedded.h"
#include "fleece_alloc.h"
#include "state/fleece_state_manager.h"
#include "platform/fleece_platform.h"

// Generous defaults for a desktop demo build; a real microcontroller target
// would tune these down (or size them to the target's actual RAM budget).
#define FLEECE_JS_MEMORY_LIMIT (8u * 1024u * 1024u)
#define FLEECE_JS_STACK_SIZE   (256u * 1024u)

#define FLEECE_JS_LIST_MAX 128

// Default peer liveness TTL, in runtime loop ticks (see fleece_embedded_set_peer_ttl_ticks).
#define FLEECE_DEFAULT_PEER_TTL_TICKS 50

// Pluggable allocator defaults (see include/fleece_alloc.h). These are the
// standard libc implementations; fleece_embedded_set_allocator() replaces
// them to point at a static pool/arena on memory-constrained targets.
void* (*fleece_malloc_fn)(size_t size) = malloc;
void (*fleece_free_fn)(void* ptr) = free;
void* (*fleece_calloc_fn)(size_t count, size_t size) = calloc;
void* (*fleece_realloc_fn)(void* ptr, size_t size) = realloc;

void fleece_embedded_set_allocator(void* (*malloc_fn)(size_t),
                                   void (*free_fn)(void*),
                                   void* (*calloc_fn)(size_t, size_t),
                                   void* (*realloc_fn)(void*, size_t)) {
    if (!malloc_fn && !free_fn && !calloc_fn && !realloc_fn) {
        fleece_malloc_fn = malloc;
        fleece_free_fn = free;
        fleece_calloc_fn = calloc;
        fleece_realloc_fn = realloc;
        return;
    }
    if (malloc_fn) fleece_malloc_fn = malloc_fn;
    if (free_fn) fleece_free_fn = free_fn;
    if (calloc_fn) fleece_calloc_fn = calloc_fn;
    if (realloc_fn) fleece_realloc_fn = realloc_fn;
}

// The QuickJS engine allocates through a JSMallocFunctions pair (functions
// that take a `void* opaque`). The fleece-side allocator (see fleece_alloc.h)
// has no opaque, so when the caller doesn't supply an explicit engine
// allocator we wrap the fleece functions into a JSMallocFunctions so a single
// fleece_embedded_set_allocator() call covers the whole library, JS engine
// included. The wrapper closes over no state - it forwards straight to the
// (pluggable) fleece_*_fn pointers, which are read at call time, so a later
// fleece_embedded_set_allocator() still takes effect on the next create.

static void* fleece_js_malloc(void* opaque, size_t size) { (void)opaque; return fleece_malloc_fn(size); }
static void* fleece_js_calloc(void* opaque, size_t count, size_t size) { (void)opaque; return fleece_calloc_fn(count, size); }
static void* fleece_js_realloc(void* opaque, void* ptr, size_t size) { (void)opaque; return fleece_realloc_fn(ptr, size); }
static void fleece_js_free(void* opaque, void* ptr) { (void)opaque; fleece_free_fn(ptr); }

// Build the auto-wrap JSMallocFunctions from the current fleece allocator.
static JSMallocFunctions fleece_js_alloc_adapter(void) {
    JSMallocFunctions mf;
    mf.js_calloc = fleece_js_calloc;
    mf.js_malloc = fleece_js_malloc;
    mf.js_free = fleece_js_free;
    mf.js_realloc = fleece_js_realloc;
    mf.js_malloc_usable_size = NULL;  // QuickJS substitutes a "unknown" fallback
    return mf;
}

struct FleeceEmbedded {
    JSRuntime* rt;
    JSContext* ctx;
    FleeceStateManager* manager;
    FleecePlatform* platform;
    uint64_t peer_ttl_ticks;
};

// --- self/swarm/platform/world Proxy support (evaluated once, wires natives
// into globalThis.self/swarm/platform/world) ---
// JSON marshalling happens in C (JS_ParseJSON/JS_JSONStringify); the prelude only
// forwards Proxy trap calls to the native helpers below.
//
// swarm's per-node view, swarm itself, platform, and world are all built from
// one factory, makeProxyStore(keysFn, getFn, opts): a Proxy that lists its
// own keys via keysFn(), resolves a valid key via getFn(key), and rejects
// writes/deletes unless opts.set/opts.del are given (world is read/write;
// the other three are read-only). getFn is only ever called for a key
// keysFn() actually reported, so it doesn't need its own "not found" handling.
//
// self is deliberately hand-written rather than run through that same
// factory: 'id' is readable (self.id) and visible to `in` (deliberately -
// see the has trap below), but NOT a real stored field, so it must NOT show
// up in Object.keys(self)/getOwnPropertyDescriptor - an asymmetry between the
// has-list and the ownKeys-list that makeProxyStore's single keysFn can't
// express, and that tests/test_embedded.c locks in explicitly.
static const char* FLEECE_JS_PRELUDE =
    "(function() {\n"
    "  var self_get = __self_get, self_set = __self_set, self_delete = __self_delete,\n"
    "      self_keys = __self_keys, self_id = __self_id,\n"
    "      swarm_nodes = __swarm_nodes, swarm_get = __swarm_get, swarm_keys = __swarm_keys,\n"
    "      platform_names = __platform_names, platform_call = __platform_call,\n"
    "      world_get = __world_get, world_set = __world_set,\n"
    "      world_delete = __world_delete, world_keys = __world_keys;\n"
    "\n"
    "  function makeProxyStore(keysFn, getFn, opts) {\n"
    "    opts = opts || {};\n"
    "    return new Proxy({}, {\n"
    "      get: function(t, p, r) {\n"
    "        if (typeof p !== 'string') return Reflect.get(t, p, r);\n"
    "        return keysFn().indexOf(p) === -1 ? undefined : getFn(p);\n"
    "      },\n"
    "      set: function(t, p, v) {\n"
    "        if (typeof p !== 'string') return Reflect.set(t, p, v);\n"
    "        if (!opts.set) return false;\n"
    "        opts.set(p, v);\n"
    "        return true;\n"
    "      },\n"
    "      has: function(t, p) {\n"
    "        return typeof p === 'string' ? keysFn().indexOf(p) !== -1 : Reflect.has(t, p);\n"
    "      },\n"
    "      ownKeys: function() { return keysFn(); },\n"
    "      getOwnPropertyDescriptor: function(t, p) {\n"
    "        if (typeof p !== 'string' || keysFn().indexOf(p) === -1) return undefined;\n"
    "        return { value: getFn(p), writable: !!opts.set, enumerable: true, configurable: true };\n"
    "      },\n"
    "      deleteProperty: function(t, p) {\n"
    "        if (typeof p !== 'string') return Reflect.deleteProperty(t, p);\n"
    "        if (!opts.del) return false;\n"
    "        opts.del(p);\n"
    "        return true;\n"
    "      }\n"
    "    });\n"
    "  }\n"
    "\n"
    "  globalThis.self = new Proxy({}, {\n"
    "    get: function(t, p, r) {\n"
    "      if (typeof p !== 'string') return Reflect.get(t, p, r);\n"
    "      return p === 'id' ? self_id() : self_get(p);\n"
    "    },\n"
    "    set: function(t, p, v) {\n"
    "      if (typeof p !== 'string') return Reflect.set(t, p, v);\n"
    "      self_set(p, v);\n"
    "      return true;\n"
    "    },\n"
    "    has: function(t, p) {\n"
    "      return typeof p === 'string' ? (p === 'id' || self_keys().indexOf(p) !== -1) : Reflect.has(t, p);\n"
    "    },\n"
    "    ownKeys: function() { return self_keys(); },\n"
    "    getOwnPropertyDescriptor: function(t, p) {\n"
    "      if (typeof p !== 'string' || self_keys().indexOf(p) === -1) return undefined;\n"
    "      return { value: self_get(p), writable: true, enumerable: true, configurable: true };\n"
    "    },\n"
    "    deleteProperty: function(t, p) {\n"
    "      if (typeof p !== 'string') return Reflect.deleteProperty(t, p);\n"
    "      self_delete(p);\n"
    "      return true;\n"
    "    }\n"
    "  });\n"
    "\n"
    "  function makeNodeView(id) {\n"
    "    return makeProxyStore(function() { return swarm_keys(id); }, function(p) { return swarm_get(id, p); });\n"
    "  }\n"
    "  globalThis.swarm = makeProxyStore(swarm_nodes, makeNodeView);\n"
    "\n"
    "  function makePlatformFunction(name) {\n"
    "    return function() { return platform_call(name, Array.prototype.slice.call(arguments)); };\n"
    "  }\n"
    "  globalThis.platform = makeProxyStore(platform_names, makePlatformFunction);\n"
    "\n"
    "  globalThis.world = makeProxyStore(world_keys, world_get, { set: world_set, del: world_delete });\n"
    "\n"
    "  delete globalThis.__self_get; delete globalThis.__self_set; delete globalThis.__self_delete;\n"
    "  delete globalThis.__self_keys; delete globalThis.__self_id;\n"
    "  delete globalThis.__swarm_nodes; delete globalThis.__swarm_get; delete globalThis.__swarm_keys;\n"
    "  delete globalThis.__platform_names; delete globalThis.__platform_call;\n"
    "  delete globalThis.__world_get; delete globalThis.__world_set;\n"
    "  delete globalThis.__world_delete; delete globalThis.__world_keys;\n"
    "})();\n";

static FleeceEmbedded* get_embedded(JSContext* ctx) {
    return (FleeceEmbedded*)JS_GetContextOpaque(ctx);
}

static void format_node_id(uint64_t node_id, char out[17]) {
    snprintf(out, 17, "%016llx", (unsigned long long)node_id);
}

static bool parse_node_id(const char* text, uint64_t* out) {
    if (!text || !text[0]) return false;
    char* end = NULL;
    unsigned long long v = strtoull(text, &end, 16);
    if (end == text || *end != '\0') return false;
    *out = (uint64_t)v;
    return true;
}

static void dump_exception(JSContext* ctx) {
    JSValue exc = JS_GetException(ctx);
    const char* msg = JS_ToCString(ctx, exc);
    fprintf(stderr, "fleece: JS exception: %s\n", msg ? msg : "(unknown)");
    if (msg) JS_FreeCString(ctx, msg);

    JSValue stack = JS_GetPropertyStr(ctx, exc, "stack");
    if (!JS_IsUndefined(stack)) {
        const char* stack_str = JS_ToCString(ctx, stack);
        if (stack_str) {
            fprintf(stderr, "%s\n", stack_str);
            JS_FreeCString(ctx, stack_str);
        }
    }
    JS_FreeValue(ctx, stack);
    JS_FreeValue(ctx, exc);
}

static void print_js_value(JSContext* ctx, JSValueConst val) {
    if (JS_IsString(val)) {
        const char* s = JS_ToCString(ctx, val);
        if (s) {
            fputs(s, stdout);
            JS_FreeCString(ctx, s);
        }
        return;
    }

    JSValue json = JS_JSONStringify(ctx, val, JS_UNDEFINED, JS_UNDEFINED);
    if (JS_IsException(json)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        fputs("undefined", stdout);
        JS_FreeValue(ctx, json);
        return;
    }
    if (JS_IsUndefined(json)) {
        fputs("undefined", stdout);  // functions/symbols/undefined stringify to nothing
    } else {
        const char* s = JS_ToCString(ctx, json);
        if (s) {
            fputs(s, stdout);
            JS_FreeCString(ctx, s);
        }
    }
    JS_FreeValue(ctx, json);
}

static JSValue js_console_log(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    (void)this_val;
    for (int i = 0; i < argc; i++) {
        if (i > 0) fputc(' ', stdout);
        print_js_value(ctx, argv[i]);
    }
    fputc('\n', stdout);
    return JS_UNDEFINED;
}

// Reads a named field's JSON bytes and parses them back into a JSValue.
// Field bytes aren't NUL-terminated; JS_ParseJSON requires that they are.
static JSValue get_named_value(JSContext* ctx, FleeceStateManager* manager, uint64_t owner_node_id, const char* name) {
    uint8_t* data = NULL;
    uint32_t size = 0;
    if (fleece_state_manager_get_named(manager, owner_node_id, name, &data, &size) != 0) {
        return JS_UNDEFINED;
    }

    char* scratch = (char*)fleece_malloc((size_t)size + 1);
    if (!scratch) {
        fleece_free(data);
        return JS_ThrowOutOfMemory(ctx);
    }
    memcpy(scratch, data, size);
    scratch[size] = '\0';
    fleece_free(data);

    JSValue result = JS_ParseJSON(ctx, scratch, size, "<state>");
    fleece_free(scratch);
    if (JS_IsException(result)) {
        // Stored bytes aren't valid JSON (shouldn't happen via our own setters) -
        // surface as undefined on read rather than throwing.
        JS_FreeValue(ctx, JS_GetException(ctx));
        return JS_UNDEFINED;
    }
    return result;
}

// `names` is a flat 2D char array (names[count][stride]) from ANY fixed-width
// name buffer -- callers pass different ones (FLEECE_FIELD_NAME_MAX for
// state-manager field names, FLEECE_PLATFORM_FUNCTION_NAME_MAX for platform
// function names), which happened to compile as the same type only by
// coincidence while both constants were numerically 32. Generic over
// `stride` rather than hardcoded to one of them so that coincidence isn't
// load-bearing.
static JSValue names_to_js_array(JSContext* ctx, const char* names, size_t stride, uint32_t count) {
    JSValue arr = JS_NewArray(ctx);
    for (uint32_t i = 0; i < count; i++) {
        JS_DefinePropertyValueUint32(ctx, arr, i, JS_NewString(ctx, names + (size_t)i * stride), JS_PROP_C_W_E);
    }
    return arr;
}

static JSValue js_self_get(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    (void)this_val;
    FleeceEmbedded* emb = get_embedded(ctx);
    if (!emb || !emb->manager || argc < 1) return JS_UNDEFINED;

    const char* name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_EXCEPTION;
    JSValue result = get_named_value(ctx, emb->manager, fleece_state_manager_get_node_id(emb->manager), name);
    JS_FreeCString(ctx, name);
    return result;
}

static JSValue js_self_set(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    (void)this_val;
    FleeceEmbedded* emb = get_embedded(ctx);
    if (!emb || !emb->manager) return JS_UNDEFINED;
    if (argc < 2) return JS_ThrowTypeError(ctx, "self: set requires a name and a value");

    const char* name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_EXCEPTION;

    JSValue json = JS_JSONStringify(ctx, argv[1], JS_UNDEFINED, JS_UNDEFINED);
    if (JS_IsException(json)) {
        JS_FreeCString(ctx, name);
        return JS_EXCEPTION;
    }
    if (JS_IsUndefined(json)) {
        JS_FreeValue(ctx, json);
        JSValue err = JS_ThrowTypeError(ctx, "self.%s: value is not JSON-serializable", name);
        JS_FreeCString(ctx, name);
        return err;
    }

    size_t len = 0;
    const char* text = JS_ToCStringLen(ctx, &len, json);
    JSValue result = JS_UNDEFINED;
    if (!text) {
        result = JS_EXCEPTION;
    } else {
        if (fleece_state_manager_set_named(emb->manager, name, (const uint8_t*)text, (uint32_t)len) != 0) {
            result = JS_ThrowTypeError(ctx, "self.%s: state store is full", name);
        }
        JS_FreeCString(ctx, text);
    }
    JS_FreeValue(ctx, json);
    JS_FreeCString(ctx, name);
    return result;
}

static JSValue js_self_delete(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    (void)this_val;
    FleeceEmbedded* emb = get_embedded(ctx);
    if (!emb || !emb->manager || argc < 1) return JS_UNDEFINED;

    const char* name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_EXCEPTION;
    fleece_state_manager_remove_named(emb->manager, name);
    JS_FreeCString(ctx, name);
    return JS_UNDEFINED;
}

static JSValue js_self_keys(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    (void)this_val; (void)argc; (void)argv;
    FleeceEmbedded* emb = get_embedded(ctx);
    if (!emb || !emb->manager) return JS_NewArray(ctx);

    char names[FLEECE_JS_LIST_MAX][FLEECE_FIELD_NAME_MAX];
    uint32_t count = fleece_state_manager_list_fields(emb->manager, fleece_state_manager_get_node_id(emb->manager), names, FLEECE_JS_LIST_MAX);
    return names_to_js_array(ctx, &names[0][0], FLEECE_FIELD_NAME_MAX, count);
}

static JSValue js_self_id(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    (void)this_val; (void)argc; (void)argv;
    FleeceEmbedded* emb = get_embedded(ctx);
    if (!emb || !emb->manager) return JS_UNDEFINED;

    char hex[17];
    format_node_id(fleece_state_manager_get_node_id(emb->manager), hex);
    return JS_NewString(ctx, hex);
}

// A peer counts as live if we've heard from it (see fleece_state_manager_import)
// within emb->peer_ttl_ticks. Checked on every swarm access, not just
// enumeration, so a script holding a stale Proxy reference across ticks also
// stops seeing data once the peer's TTL expires.
static bool js_peer_is_live(FleeceEmbedded* emb, uint64_t node_id) {
    if (!emb || !emb->manager) return false;
    return fleece_state_manager_ticks_since_seen(emb->manager, node_id) <= emb->peer_ttl_ticks;
}

static JSValue js_swarm_nodes(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    (void)this_val; (void)argc; (void)argv;
    FleeceEmbedded* emb = get_embedded(ctx);
    if (!emb || !emb->manager) return JS_NewArray(ctx);

    uint64_t local_id = fleece_state_manager_get_node_id(emb->manager);
    /* Enumerate HEARD peers, not field owners: gossip only carries the
     * shared/world stream (self streams are node-local), so a remote peer's
     * fields are all owned by FLEECE_SHARED_OWNER_ID locally and field
     * ownership can never identify a peer. The liveness table is the only
     * record that remote peers exist at all; the TTL check below still
     * hides silent ones. */
    uint64_t ids[FLEECE_JS_LIST_MAX];
    uint32_t count = fleece_state_manager_list_heard_peers(emb->manager, ids, FLEECE_JS_LIST_MAX);

    JSValue arr = JS_NewArray(ctx);
    uint32_t out_idx = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (ids[i] == local_id) continue;  // swarm excludes the local node - that's what self is for
        if (!js_peer_is_live(emb, ids[i])) continue;  // hide dead/silent peers
        char hex[17];
        format_node_id(ids[i], hex);
        JS_DefinePropertyValueUint32(ctx, arr, out_idx++, JS_NewString(ctx, hex), JS_PROP_C_W_E);
    }
    return arr;
}

static JSValue js_swarm_get(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    (void)this_val;
    FleeceEmbedded* emb = get_embedded(ctx);
    if (!emb || !emb->manager || argc < 2) return JS_UNDEFINED;

    const char* id_str = JS_ToCString(ctx, argv[0]);
    if (!id_str) return JS_EXCEPTION;
    uint64_t node_id;
    bool ok = parse_node_id(id_str, &node_id);
    JS_FreeCString(ctx, id_str);
    if (!ok || !js_peer_is_live(emb, node_id)) return JS_UNDEFINED;

    const char* name = JS_ToCString(ctx, argv[1]);
    if (!name) return JS_EXCEPTION;
    JSValue result = get_named_value(ctx, emb->manager, node_id, name);
    JS_FreeCString(ctx, name);
    return result;
}

static JSValue js_swarm_keys(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    (void)this_val;
    FleeceEmbedded* emb = get_embedded(ctx);
    if (!emb || !emb->manager || argc < 1) return JS_NewArray(ctx);

    const char* id_str = JS_ToCString(ctx, argv[0]);
    if (!id_str) return JS_EXCEPTION;
    uint64_t node_id;
    bool ok = parse_node_id(id_str, &node_id);
    JS_FreeCString(ctx, id_str);
    if (!ok || !js_peer_is_live(emb, node_id)) return JS_NewArray(ctx);

    char names[FLEECE_JS_LIST_MAX][FLEECE_FIELD_NAME_MAX];
    uint32_t count = fleece_state_manager_list_fields(emb->manager, node_id, names, FLEECE_JS_LIST_MAX);
    return names_to_js_array(ctx, &names[0][0], FLEECE_FIELD_NAME_MAX, count);
}

static JSValue js_world_get(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    (void)this_val;
    FleeceEmbedded* emb = get_embedded(ctx);
    if (!emb || !emb->manager || argc < 1) return JS_UNDEFINED;

    const char* name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_EXCEPTION;
    JSValue result = get_named_value(ctx, emb->manager, FLEECE_SHARED_OWNER_ID, name);
    JS_FreeCString(ctx, name);
    return result;
}

static JSValue js_world_set(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    (void)this_val;
    FleeceEmbedded* emb = get_embedded(ctx);
    if (!emb || !emb->manager) return JS_UNDEFINED;
    if (argc < 2) return JS_ThrowTypeError(ctx, "world: set requires a name and a value");

    const char* name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_EXCEPTION;

    JSValue json = JS_JSONStringify(ctx, argv[1], JS_UNDEFINED, JS_UNDEFINED);
    if (JS_IsException(json)) {
        JS_FreeCString(ctx, name);
        return JS_EXCEPTION;
    }
    if (JS_IsUndefined(json)) {
        JS_FreeValue(ctx, json);
        JSValue err = JS_ThrowTypeError(ctx, "world.%s: value is not JSON-serializable", name);
        JS_FreeCString(ctx, name);
        return err;
    }

    size_t len = 0;
    const char* text = JS_ToCStringLen(ctx, &len, json);
    JSValue result = JS_UNDEFINED;
    if (!text) {
        result = JS_EXCEPTION;
    } else {
        if (fleece_state_manager_set_shared(emb->manager, name, (const uint8_t*)text, (uint32_t)len) != 0) {
            result = JS_ThrowTypeError(ctx, "world.%s: state store is full", name);
        }
        JS_FreeCString(ctx, text);
    }
    JS_FreeValue(ctx, json);
    JS_FreeCString(ctx, name);
    return result;
}

static JSValue js_world_delete(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    (void)this_val;
    FleeceEmbedded* emb = get_embedded(ctx);
    if (!emb || !emb->manager || argc < 1) return JS_UNDEFINED;

    const char* name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_EXCEPTION;
    fleece_state_manager_remove_shared(emb->manager, name);
    JS_FreeCString(ctx, name);
    return JS_UNDEFINED;
}

static JSValue js_world_keys(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    (void)this_val; (void)argc; (void)argv;
    FleeceEmbedded* emb = get_embedded(ctx);
    if (!emb || !emb->manager) return JS_NewArray(ctx);

    char names[FLEECE_JS_LIST_MAX][FLEECE_FIELD_NAME_MAX];
    uint32_t count = fleece_state_manager_list_fields(emb->manager, FLEECE_SHARED_OWNER_ID, names, FLEECE_JS_LIST_MAX);
    return names_to_js_array(ctx, &names[0][0], FLEECE_FIELD_NAME_MAX, count);
}

// worldCompareAndSet(name, expectedValueOrUndefined, newValue) -> bool.
// A permanent global function (unlike the underscore-prefixed __world_*
// plumbing, which the prelude deletes after wiring up the Proxy) - not part
// of the `world` Proxy itself, since Proxy traps only see property
// get/set/delete, not a 3-argument atomic operation. expectedValueOrUndefined
// === undefined means "the field must not currently exist" (claim-if-absent);
// see fleece_state_manager_set_shared_cas for the exact semantics and the
// claim-then-recheck pattern this is meant to support.
static JSValue js_world_compare_and_set(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    (void)this_val;
    FleeceEmbedded* emb = get_embedded(ctx);
    if (!emb || !emb->manager) return JS_UNDEFINED;
    if (argc < 3) return JS_ThrowTypeError(ctx, "worldCompareAndSet requires a name, an expected value (or undefined), and a new value");

    const char* name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_EXCEPTION;

    bool expect_absent = JS_IsUndefined(argv[1]);
    JSValue expected_json = JS_UNDEFINED;
    const char* expected_text = NULL;
    size_t expected_len = 0;
    if (!expect_absent) {
        expected_json = JS_JSONStringify(ctx, argv[1], JS_UNDEFINED, JS_UNDEFINED);
        if (JS_IsException(expected_json)) {
            JS_FreeCString(ctx, name);
            return JS_EXCEPTION;
        }
        if (JS_IsUndefined(expected_json)) {
            JS_FreeValue(ctx, expected_json);
            JSValue err = JS_ThrowTypeError(ctx, "worldCompareAndSet: expected value is not JSON-serializable");
            JS_FreeCString(ctx, name);
            return err;
        }
        expected_text = JS_ToCStringLen(ctx, &expected_len, expected_json);
        if (!expected_text) {
            JS_FreeValue(ctx, expected_json);
            JS_FreeCString(ctx, name);
            return JS_EXCEPTION;
        }
    }

    JSValue new_json = JS_JSONStringify(ctx, argv[2], JS_UNDEFINED, JS_UNDEFINED);
    if (JS_IsException(new_json)) {
        if (expected_text) JS_FreeCString(ctx, expected_text);
        if (!expect_absent) JS_FreeValue(ctx, expected_json);
        JS_FreeCString(ctx, name);
        return JS_EXCEPTION;
    }
    if (JS_IsUndefined(new_json)) {
        JS_FreeValue(ctx, new_json);
        if (expected_text) JS_FreeCString(ctx, expected_text);
        if (!expect_absent) JS_FreeValue(ctx, expected_json);
        JSValue err = JS_ThrowTypeError(ctx, "worldCompareAndSet: new value is not JSON-serializable");
        JS_FreeCString(ctx, name);
        return err;
    }
    size_t new_len = 0;
    const char* new_text = JS_ToCStringLen(ctx, &new_len, new_json);
    if (!new_text) {
        JS_FreeValue(ctx, new_json);
        if (expected_text) JS_FreeCString(ctx, expected_text);
        if (!expect_absent) JS_FreeValue(ctx, expected_json);
        JS_FreeCString(ctx, name);
        return JS_EXCEPTION;
    }

    int rc = fleece_state_manager_set_shared_cas(emb->manager, name,
        expect_absent ? NULL : (const uint8_t*)expected_text, (uint32_t)expected_len,
        (const uint8_t*)new_text, (uint32_t)new_len);

    JS_FreeCString(ctx, new_text);
    JS_FreeValue(ctx, new_json);
    if (expected_text) JS_FreeCString(ctx, expected_text);
    if (!expect_absent) JS_FreeValue(ctx, expected_json);
    JS_FreeCString(ctx, name);

    if (rc < 0) return JS_ThrowTypeError(ctx, "worldCompareAndSet: failed (bad arguments or state store full)");
    return JS_NewBool(ctx, rc == 0);
}

// --- claimBestGoal: native CBBA-over-the-CRDT claiming --------------------
//
// A generic, mission-agnostic primitive: auctioning a shared pool of world
// records via scored compare-and-set, with dead-peer takeover and automatic
// re-assertion of a held claim. Originally hand-rolled per-mission in
// swarmpu's own JS (see docs/decisions/026-cbba-goal-pool-auction.md in that
// superproject) — moved here because the mechanics (CAS, contest margin,
// liveness, and especially periodic re-assertion so an unmoving claim keeps
// getting re-exported by the normal delta-gossip path) are exactly the kind
// of thing every fleece mission wanting this pattern would otherwise
// reimplement from scratch in JS, with the same subtle convergence trap
// swarmpu hit doing exactly that. Scoring stays mission-specific and
// JS-authored (distance, priority, capability match are all mission
// concerns fleece has no business knowing about) via one well-known global
// function the loaded script defines.

// Parses `data` (raw JSON bytes for one pool record) and calls the script's
// scoreGoal(key, parsedRecord) -> number. Returns false (ineligible) on a
// thrown exception or a non-finite result — the scorer's own way of
// rejecting a candidate (bad capability match, missing required field,
// required_agents excluding this node, etc.) without a separate eligibility
// callback.
static bool js_score_goal(JSContext* ctx, JSValueConst score_fn, const char* key,
                           const uint8_t* data, uint32_t size, double* out_score) {
    // JS_ParseJSON needs a NUL-terminated buffer despite taking an explicit
    // length -- fleece_state_manager_get_named()'s own buffer is exactly
    // `size` bytes, no trailing NUL (raw stored bytes, verbatim). Parsing it
    // directly one-byte-overreads into whatever heap garbage follows,
    // intermittently (depends on what's adjacent) producing bogus parse
    // errors -- found live, this exact call flaked between working and a
    // "SyntaxError: Unexpected token '\x90'"-style failure on THE SAME
    // stored bytes from one call to the next. Mirrors goap_bb_to_js()'s own
    // established fleece_malloc(size+1)+NUL-terminate pattern just above in
    // this file.
    char* scratch = (char*)fleece_malloc((size_t)size + 1);
    if (!scratch) return false;
    memcpy(scratch, data, size);
    scratch[size] = '\0';
    JSValue parsed = JS_ParseJSON(ctx, scratch, size, "<goal>");
    fleece_free(scratch);
    if (JS_IsException(parsed)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return false;
    }
    JSValue key_js = JS_NewString(ctx, key);
    JSValue argv[2] = { key_js, parsed };
    JSValue result = JS_Call(ctx, score_fn, JS_UNDEFINED, 2, (JSValueConst*)argv);
    JS_FreeValue(ctx, key_js);
    JS_FreeValue(ctx, parsed);
    if (JS_IsException(result)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        JS_FreeValue(ctx, result);
        return false;
    }
    double score;
    int rc = JS_ToFloat64(ctx, &score, result);
    JS_FreeValue(ctx, result);
    if (rc != 0) return false;
    if (score != score) return false;          // NaN
    if (score > 1e300 || score < -1e300) {
        // Treat as a finite "unbeatable"/"never" sentinel rather than
        // propagating +/-Infinity into stored JSON (not valid JSON) or into
        // arithmetic against CONTEST_MARGIN-style comparisons downstream.
        score = score > 0 ? 1e300 : -1e300;
    }
    *out_score = score;
    return true;
}

// Reads claim_<key> (if present) and extracts its owner (hex self.id string
// -> uint64) and score. Returns false if absent or malformed.
static bool read_claim(FleeceEmbedded* emb, JSContext* ctx, const char* key,
                        uint64_t* out_owner, double* out_score,
                        uint8_t** out_raw, uint32_t* out_raw_size) {
    char claim_key[FLEECE_FIELD_NAME_MAX + 8];
    snprintf(claim_key, sizeof(claim_key), "claim_%s", key);

    uint8_t* data = NULL;
    uint32_t size = 0;
    *out_raw = NULL;
    *out_raw_size = 0;
    if (fleece_state_manager_get_named(emb->manager, FLEECE_SHARED_OWNER_ID, claim_key, &data, &size) != 0 ||
        !data) {
        return false;
    }
    *out_raw = data;
    *out_raw_size = size;

    // NUL-terminate before parsing -- see js_score_goal's own comment above
    // for why (JS_ParseJSON needs it despite taking an explicit length).
    char* scratch = (char*)fleece_malloc((size_t)size + 1);
    if (!scratch) return false;
    memcpy(scratch, data, size);
    scratch[size] = '\0';
    JSValue parsed = JS_ParseJSON(ctx, scratch, size, "<claim>");
    fleece_free(scratch);
    if (JS_IsException(parsed)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return false;
    }
    JSValue owner_js = JS_GetPropertyStr(ctx, parsed, "o");
    JSValue score_js = JS_GetPropertyStr(ctx, parsed, "b");
    const char* owner_hex = JS_ToCString(ctx, owner_js);
    bool ok = false;
    if (owner_hex) {
        *out_owner = (uint64_t)strtoull(owner_hex, NULL, 16);
        double score = 0;
        if (JS_ToFloat64(ctx, &score, score_js) == 0) {
            *out_score = score;
            ok = true;
        }
        JS_FreeCString(ctx, owner_hex);
    }
    JS_FreeValue(ctx, owner_js);
    JS_FreeValue(ctx, score_js);
    JS_FreeValue(ctx, parsed);
    return ok;
}

// Writes claim_<key> = {"o":"<self hex id>","b":<score>} via CAS.
// expected_raw/expected_raw_size == NULL/0 means "claim only if absent".
static int cas_claim(FleeceEmbedded* emb, const char* key, uint64_t self_id, double score,
                      const uint8_t* expected_raw, uint32_t expected_raw_size) {
    char claim_key[FLEECE_FIELD_NAME_MAX + 8];
    snprintf(claim_key, sizeof(claim_key), "claim_%s", key);
    char new_value[96];
    int written = snprintf(new_value, sizeof(new_value), "{\"o\":\"%016llx\",\"b\":%.17g}",
                            (unsigned long long)self_id, score);
    if (written < 0 || (size_t)written >= sizeof(new_value)) return -1;
    return fleece_state_manager_set_shared_cas(emb->manager, claim_key,
                                                expected_raw_size > 0 ? expected_raw : NULL, expected_raw_size,
                                                (const uint8_t*)new_value, (uint32_t)written);
}

// See fleece_embedded.h's own extensive doc comment for the design and
// contract. This is the GOAP brain's core reasoning loop for goal-pool mode
// (fleece_goap_brain_set_goal_pool, fleece_goap_js.c) -- pure C, called
// once per brain tick, never JS-exposed. SCORING one candidate is the sole
// piece that calls back into JS (the mission-authored `scoreGoal` global) --
// deliberately: distance/priority/capability-match heuristics are mission
// concerns, the claiming mechanics around them are not. See
// docs/decisions/026-cbba-goal-pool-auction.md (swarmpu superproject) for
// the convergence incident that motivated moving the mechanics here.
int fleece_embedded_claim_best_goal(FleeceEmbedded* emb, const char* prefix,
                                     const char* current_key, double contest_margin,
                                     bool allow_reassert,
                                     char* out_key, uint32_t out_key_cap) {
    if (out_key && out_key_cap > 0) out_key[0] = '\0';
    if (!emb || !emb->manager || !prefix || !out_key || out_key_cap == 0) return -1;
    if (current_key && current_key[0] == '\0') current_key = NULL;

    JSContext* ctx = emb->ctx;
    size_t prefix_len = strlen(prefix);

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue score_fn = JS_GetPropertyStr(ctx, global, "scoreGoal");
    JS_FreeValue(ctx, global);
    if (!JS_IsFunction(ctx, score_fn)) {
        JS_FreeValue(ctx, score_fn);
        return -1;  // no scoreGoal(key, record) heuristic defined -- goal-pool mode can't score anything
    }

    uint64_t self_id = fleece_state_manager_get_node_id(emb->manager);

    // Is my own currently-held key still genuinely mine, and what does it
    // score right now? An owner mismatch (someone else's claim won) or a
    // vanished record drops it unconditionally, same as swarmpu's own prior
    // JS-side "lost" handling.
    bool have_current = false;
    double current_score = 0;
    if (current_key) {
        uint64_t owner = 0;
        double score = 0;
        uint8_t* raw = NULL;
        uint32_t raw_size = 0;
        bool claim_ok = read_claim(emb, ctx, current_key, &owner, &score, &raw, &raw_size);
        if (raw) fleece_free(raw);
        if (claim_ok && owner == self_id) {
            uint8_t* data = NULL;
            uint32_t size = 0;
            if (fleece_state_manager_get_named(emb->manager, FLEECE_SHARED_OWNER_ID, current_key, &data, &size) == 0 && data) {
                have_current = js_score_goal(ctx, score_fn, current_key, data, size, &current_score);
                fleece_free(data);
            }
        }
    }

    char names[FLEECE_JS_LIST_MAX][FLEECE_FIELD_NAME_MAX];
    uint32_t count = fleece_state_manager_list_fields(emb->manager, FLEECE_SHARED_OWNER_ID, names, FLEECE_JS_LIST_MAX);

    char best_key[FLEECE_FIELD_NAME_MAX];
    best_key[0] = '\0';
    double best_score = 0;
    uint8_t* best_expected_raw = NULL;
    uint32_t best_expected_raw_size = 0;

    for (uint32_t i = 0; i < count; i++) {
        const char* key = names[i];
        if (strncmp(key, prefix, prefix_len) != 0) continue;
        if (strncmp(key, "claim_", 6) == 0) continue;  // never bid on a claim record itself
        if (current_key && strcmp(key, current_key) == 0) continue;  // scored separately above

        uint8_t* data = NULL;
        uint32_t size = 0;
        if (fleece_state_manager_get_named(emb->manager, FLEECE_SHARED_OWNER_ID, key, &data, &size) != 0 || !data) {
            continue;
        }
        double score;
        bool eligible_score = js_score_goal(ctx, score_fn, key, data, size, &score);
        fleece_free(data);
        if (!eligible_score) continue;

        uint64_t holder = 0;
        double holder_score = 0;
        uint8_t* claim_raw = NULL;
        uint32_t claim_raw_size = 0;
        bool has_claim = read_claim(emb, ctx, key, &holder, &holder_score, &claim_raw, &claim_raw_size);

        bool eligible;
        if (!has_claim || holder == self_id) {
            eligible = true;  // unclaimed, or (stale) already ours
        } else if (fleece_state_manager_ticks_since_seen(emb->manager, holder) > emb->peer_ttl_ticks) {
            eligible = true;  // holder has gone silent -- takeover
        } else {
            eligible = score > holder_score + contest_margin;
        }

        if (eligible && score > best_score) {
            best_score = score;
            strncpy(best_key, key, sizeof(best_key) - 1);
            best_key[sizeof(best_key) - 1] = '\0';
            if (best_expected_raw) fleece_free(best_expected_raw);
            best_expected_raw = claim_raw;
            best_expected_raw_size = claim_raw_size;
            claim_raw = NULL;
        }
        if (claim_raw) fleece_free(claim_raw);
    }

    bool should_switch = best_key[0] != '\0' && (!have_current || best_score > current_score + contest_margin);

    const char* result_key = NULL;
    if (should_switch) {
        int rc = cas_claim(emb, best_key, self_id, best_score, best_expected_raw, best_expected_raw_size);
        if (rc == 0) {
            if (have_current) {
                char old_claim_key[FLEECE_FIELD_NAME_MAX + 8];
                snprintf(old_claim_key, sizeof(old_claim_key), "claim_%s", current_key);
                fleece_state_manager_remove_shared(emb->manager, old_claim_key);
            }
            result_key = best_key;
        } else if (have_current) {
            result_key = current_key;  // lost the race -- keep what I already hold
        }
    } else if (have_current) {
        // Re-assert: a same-value CAS touch bumps this claim's LWW
        // timestamp so the next delta-export round carries it again. See
        // this primitive's own block comment for why that matters, and why
        // the caller MUST gate allow_reassert well below its own call
        // frequency instead of passing true every tick.
        if (allow_reassert) {
            uint8_t* raw = NULL;
            uint32_t raw_size = 0;
            char claim_key[FLEECE_FIELD_NAME_MAX + 8];
            snprintf(claim_key, sizeof(claim_key), "claim_%s", current_key);
            if (fleece_state_manager_get_named(emb->manager, FLEECE_SHARED_OWNER_ID, claim_key, &raw, &raw_size) == 0 && raw) {
                cas_claim(emb, current_key, self_id, current_score, raw, raw_size);
                fleece_free(raw);
            }
        }
        result_key = current_key;
    }

    if (result_key) {
        strncpy(out_key, result_key, out_key_cap - 1);
        out_key[out_key_cap - 1] = '\0';
    }

    if (best_expected_raw) fleece_free(best_expected_raw);
    JS_FreeValue(ctx, score_fn);
    return 0;
}

static JSValue js_platform_names(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    (void)this_val; (void)argc; (void)argv;
    FleeceEmbedded* emb = get_embedded(ctx);
    if (!emb || !emb->platform) return JS_NewArray(ctx);

    char names[FLEECE_JS_LIST_MAX][FLEECE_PLATFORM_FUNCTION_NAME_MAX];
    uint32_t count = fleece_platform_list_functions(emb->platform, names, FLEECE_JS_LIST_MAX);
    return names_to_js_array(ctx, &names[0][0], FLEECE_PLATFORM_FUNCTION_NAME_MAX, count);
}

// Calls a registered platform function: argv[0] is the name, argv[1] is a JS
// array of the caller's arguments. Marshals to/from JSON at this boundary so
// fleece_platform itself stays engine-agnostic (see fleece_platform.h).
static JSValue js_platform_call(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    (void)this_val;
    FleeceEmbedded* emb = get_embedded(ctx);
    if (!emb || !emb->platform || argc < 2) {
        return JS_ThrowTypeError(ctx, "platform: call requires a name and an argument list");
    }

    const char* name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_EXCEPTION;

    JSValue args_json_val = JS_JSONStringify(ctx, argv[1], JS_UNDEFINED, JS_UNDEFINED);
    if (JS_IsException(args_json_val)) {
        JS_FreeCString(ctx, name);
        return JS_EXCEPTION;
    }
    size_t args_len = 0;
    const char* args_text = JS_ToCStringLen(ctx, &args_len, args_json_val);
    JS_FreeValue(ctx, args_json_val);
    if (!args_text) {
        JS_FreeCString(ctx, name);
        return JS_EXCEPTION;
    }

    uint8_t* result_json = NULL;
    uint32_t result_size = 0;
    int rc = fleece_platform_call(emb->platform, name, (const uint8_t*)args_text, (uint32_t)args_len, &result_json, &result_size);
    JS_FreeCString(ctx, args_text);

    if (rc != 0) {
        JSValue err = JS_ThrowTypeError(ctx, "platform.%s: call failed", name);
        JS_FreeCString(ctx, name);
        return err;
    }
    JS_FreeCString(ctx, name);

    if (!result_json || result_size == 0) {
        fleece_free(result_json);
        return JS_UNDEFINED;
    }

    char* scratch = (char*)fleece_malloc((size_t)result_size + 1);
    if (!scratch) {
        fleece_free(result_json);
        return JS_ThrowOutOfMemory(ctx);
    }
    memcpy(scratch, result_json, result_size);
    scratch[result_size] = '\0';
    fleece_free(result_json);

    JSValue result = JS_ParseJSON(ctx, scratch, result_size, "<platform result>");
    fleece_free(scratch);
    if (JS_IsException(result)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return JS_UNDEFINED;
    }
    return result;
}

FleeceEmbedded* fleece_embedded_create(void) {
    return fleece_embedded_create_with_allocator(NULL, NULL);
}

FleeceEmbedded* fleece_embedded_create_with_allocator(const JSMallocFunctions* mf, void* opaque) {
    FleeceEmbedded* embedded = (FleeceEmbedded*)fleece_calloc(1, sizeof(FleeceEmbedded));
    if (!embedded) {
        return NULL;
    }

    if (mf && mf->js_malloc && mf->js_free) {
        // Explicit engine allocator (e.g. a dedicated JS arena on an MCU).
        embedded->rt = JS_NewRuntime2(mf, opaque);
    } else {
        // No explicit engine allocator: wrap the fleece allocator configured
        // by fleece_embedded_set_allocator() so one call covers everything.
        JSMallocFunctions auto_mf = fleece_js_alloc_adapter();
        embedded->rt = JS_NewRuntime2(&auto_mf, NULL);
    }
    if (!embedded->rt) {
        fleece_free(embedded);
        return NULL;
    }
    JS_SetMemoryLimit(embedded->rt, FLEECE_JS_MEMORY_LIMIT);
    JS_SetMaxStackSize(embedded->rt, FLEECE_JS_STACK_SIZE);

    embedded->ctx = JS_NewContext(embedded->rt);
    if (!embedded->ctx) {
        JS_FreeRuntime(embedded->rt);
        fleece_free(embedded);
        return NULL;
    }
    JS_SetContextOpaque(embedded->ctx, embedded);
    embedded->peer_ttl_ticks = FLEECE_DEFAULT_PEER_TTL_TICKS;

    return embedded;
}

void fleece_embedded_destroy(FleeceEmbedded* embedded) {
    if (!embedded) return;

    if (embedded->ctx) JS_FreeContext(embedded->ctx);
    if (embedded->rt) JS_FreeRuntime(embedded->rt);
    fleece_free(embedded);
}

int fleece_embedded_set_state_manager(FleeceEmbedded* embedded, FleeceStateManager* manager) {
    if (!embedded || !manager) return -1;

    embedded->manager = manager;
    return 0;
}

int fleece_embedded_set_platform(FleeceEmbedded* embedded, FleecePlatform* platform) {
    if (!embedded || !platform) return -1;

    embedded->platform = platform;
    return 0;
}

int fleece_embedded_set_peer_ttl_ticks(FleeceEmbedded* embedded, uint64_t ttl_ticks) {
    if (!embedded) return -1;

    embedded->peer_ttl_ticks = ttl_ticks;
    return 0;
}

uint64_t fleece_embedded_get_peer_ttl_ticks(FleeceEmbedded* embedded) {
    return embedded ? embedded->peer_ttl_ticks : FLEECE_DEFAULT_PEER_TTL_TICKS;
}

int fleece_embedded_register_c_functions(FleeceEmbedded* embedded) {
    if (!embedded || !embedded->ctx || !embedded->manager) {
        return -1;
    }
    JSContext* ctx = embedded->ctx;

    JSValue global = JS_GetGlobalObject(ctx);

    JSValue console = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, console, "log", JS_NewCFunction(ctx, js_console_log, "log", 0));
    JS_SetPropertyStr(ctx, global, "console", console);

    JS_SetPropertyStr(ctx, global, "__self_get", JS_NewCFunction(ctx, js_self_get, "__self_get", 1));
    JS_SetPropertyStr(ctx, global, "__self_set", JS_NewCFunction(ctx, js_self_set, "__self_set", 2));
    JS_SetPropertyStr(ctx, global, "__self_delete", JS_NewCFunction(ctx, js_self_delete, "__self_delete", 1));
    JS_SetPropertyStr(ctx, global, "__self_keys", JS_NewCFunction(ctx, js_self_keys, "__self_keys", 0));
    JS_SetPropertyStr(ctx, global, "__self_id", JS_NewCFunction(ctx, js_self_id, "__self_id", 0));
    JS_SetPropertyStr(ctx, global, "__swarm_nodes", JS_NewCFunction(ctx, js_swarm_nodes, "__swarm_nodes", 0));
    JS_SetPropertyStr(ctx, global, "__swarm_get", JS_NewCFunction(ctx, js_swarm_get, "__swarm_get", 2));
    JS_SetPropertyStr(ctx, global, "__swarm_keys", JS_NewCFunction(ctx, js_swarm_keys, "__swarm_keys", 1));
    JS_SetPropertyStr(ctx, global, "__platform_names", JS_NewCFunction(ctx, js_platform_names, "__platform_names", 0));
    JS_SetPropertyStr(ctx, global, "__platform_call", JS_NewCFunction(ctx, js_platform_call, "__platform_call", 2));
    JS_SetPropertyStr(ctx, global, "__world_get", JS_NewCFunction(ctx, js_world_get, "__world_get", 1));
    JS_SetPropertyStr(ctx, global, "__world_set", JS_NewCFunction(ctx, js_world_set, "__world_set", 2));
    JS_SetPropertyStr(ctx, global, "__world_delete", JS_NewCFunction(ctx, js_world_delete, "__world_delete", 1));
    JS_SetPropertyStr(ctx, global, "__world_keys", JS_NewCFunction(ctx, js_world_keys, "__world_keys", 0));
    // Permanent global (not deleted by the prelude below, unlike __world_* above) -
    // a 3-argument atomic operation doesn't fit the world Proxy's get/set/delete traps.
    JS_SetPropertyStr(ctx, global, "worldCompareAndSet", JS_NewCFunction(ctx, js_world_compare_and_set, "worldCompareAndSet", 3));
    // fleece_embedded_claim_best_goal() is deliberately NOT exposed here: it's
    // the GOAP brain's internal goal-pool reasoning loop (fleece_goap_js.c's
    // fleece_goap_brain_set_goal_pool), not a script-callable primitive --
    // see its own doc comment in fleece_embedded.h.

    JS_FreeValue(ctx, global);

    JSValue prelude_result = JS_Eval(ctx, FLEECE_JS_PRELUDE, strlen(FLEECE_JS_PRELUDE), "<prelude>", JS_EVAL_TYPE_GLOBAL);
    int rc = 0;
    if (JS_IsException(prelude_result)) {
        dump_exception(ctx);
        rc = -1;
    }
    JS_FreeValue(ctx, prelude_result);
    return rc;
}

int fleece_embedded_load_script(FleeceEmbedded* embedded, const char* source, const char* filename) {
    if (!embedded || !embedded->ctx || !source) {
        return -1;
    }

    JSValue result = JS_Eval(embedded->ctx, source, strlen(source), filename ? filename : "<script>", JS_EVAL_TYPE_GLOBAL);
    int rc = 0;
    if (JS_IsException(result)) {
        dump_exception(embedded->ctx);
        rc = -1;
    }
    JS_FreeValue(embedded->ctx, result);
    return rc;
}

static int call_lifecycle_fn(FleeceEmbedded* embedded, const char* fn_name) {
    if (!embedded || !embedded->ctx) {
        return -1;
    }
    JSContext* ctx = embedded->ctx;

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue fn = JS_GetPropertyStr(ctx, global, fn_name);
    JS_FreeValue(ctx, global);

    if (JS_IsException(fn)) {
        dump_exception(ctx);
        JS_FreeValue(ctx, fn);
        return -1;
    }
    if (!JS_IsFunction(ctx, fn)) {
        JS_FreeValue(ctx, fn);
        return 0;  // script doesn't define this lifecycle function - silent no-op
    }

    JSValue result = JS_Call(ctx, fn, JS_UNDEFINED, 0, NULL);
    JS_FreeValue(ctx, fn);
    int rc = 0;
    if (JS_IsException(result)) {
        dump_exception(ctx);
        rc = -1;
    }
    JS_FreeValue(ctx, result);
    return rc;
}

int fleece_embedded_call_init(FleeceEmbedded* embedded) {
    if (embedded && embedded->rt) JS_UpdateStackTop(embedded->rt);
    return call_lifecycle_fn(embedded, "init");
}
int fleece_embedded_call_step(FleeceEmbedded* embedded) {
    if (embedded && embedded->rt) JS_UpdateStackTop(embedded->rt);
    return call_lifecycle_fn(embedded, "step");
}
int fleece_embedded_call_reset(FleeceEmbedded* embedded) { return call_lifecycle_fn(embedded, "reset"); }
int fleece_embedded_call_destroy(FleeceEmbedded* embedded) { return call_lifecycle_fn(embedded, "destroy"); }

int fleece_embedded_execute(FleeceEmbedded* embedded, const char* script) {
    if (!embedded || !embedded->ctx || !script) {
        return -1;
    }

    JSValue result = JS_Eval(embedded->ctx, script, strlen(script), "<execute>", JS_EVAL_TYPE_GLOBAL);
    int rc = 0;
    if (JS_IsException(result)) {
        dump_exception(embedded->ctx);
        rc = -1;
    }
    JS_FreeValue(embedded->ctx, result);
    return rc;
}

int fleece_embedded_set_value(FleeceEmbedded* embedded, const char* name, const uint8_t* data, uint32_t size) {
    if (!embedded || !embedded->ctx || !name || !data) {
        return -1;
    }
    JSContext* ctx = embedded->ctx;

    char* scratch = (char*)fleece_malloc((size_t)size + 1);
    if (!scratch) return -1;
    memcpy(scratch, data, size);
    scratch[size] = '\0';

    JSValue val = JS_ParseJSON(ctx, scratch, size, "<set_value>");
    fleece_free(scratch);
    if (JS_IsException(val)) {
        dump_exception(ctx);
        JS_FreeValue(ctx, val);
        return -1;
    }

    JSValue global = JS_GetGlobalObject(ctx);
    int rc = JS_SetPropertyStr(ctx, global, name, val);  // consumes val
    JS_FreeValue(ctx, global);
    return rc >= 0 ? 0 : -1;
}

int fleece_embedded_get_value(FleeceEmbedded* embedded, const char* name, uint8_t** data, uint32_t* size) {
    if (!embedded || !embedded->ctx || !name || !data || !size) {
        return -1;
    }
    JSContext* ctx = embedded->ctx;

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue val = JS_GetPropertyStr(ctx, global, name);
    JS_FreeValue(ctx, global);
    if (JS_IsException(val)) {
        dump_exception(ctx);
        JS_FreeValue(ctx, val);
        return -1;
    }
    if (JS_IsUndefined(val)) {
        JS_FreeValue(ctx, val);
        return -1;
    }

    JSValue json = JS_JSONStringify(ctx, val, JS_UNDEFINED, JS_UNDEFINED);
    JS_FreeValue(ctx, val);
    if (JS_IsException(json)) {
        dump_exception(ctx);
        JS_FreeValue(ctx, json);
        return -1;
    }
    if (JS_IsUndefined(json)) {
        JS_FreeValue(ctx, json);
        return -1;  // value is not JSON-serializable (e.g. a function)
    }

    size_t len = 0;
    const char* text = JS_ToCStringLen(ctx, &len, json);
    JS_FreeValue(ctx, json);
    if (!text) return -1;

    uint8_t* buf = (uint8_t*)fleece_malloc(len);
    if (!buf && len > 0) {
        JS_FreeCString(ctx, text);
        return -1;
    }
    memcpy(buf, text, len);
    JS_FreeCString(ctx, text);

    *data = buf;
    *size = (uint32_t)len;
    return 0;
}

void* fleece_embedded_get_context(FleeceEmbedded* embedded) {
    return embedded ? (void*)embedded->ctx : NULL;
}

void* fleece_embedded_get_state_manager(FleeceEmbedded* embedded) {
    return embedded ? (void*)embedded->manager : NULL;
}
