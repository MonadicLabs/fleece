#define _POSIX_C_SOURCE 200809L

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "quickjs.h"

#include "fleece_embedded.h"
#include "state/fleece_state_manager.h"

// Generous defaults for a desktop demo build; a real microcontroller target
// would tune these down (or size them to the target's actual RAM budget).
#define FLEECE_JS_MEMORY_LIMIT (8u * 1024u * 1024u)
#define FLEECE_JS_STACK_SIZE   (256u * 1024u)

#define FLEECE_JS_LIST_MAX 128

struct FleeceEmbedded {
    JSRuntime* rt;
    JSContext* ctx;
    FleeceStateManager* manager;
};

// --- self/swarm Proxy support (evaluated once, wires natives into globalThis.self/swarm) ---
// JSON marshalling happens in C (JS_ParseJSON/JS_JSONStringify); the prelude only
// forwards Proxy trap calls to the native helpers below.
static const char* SELF_SWARM_PRELUDE =
    "(function() {\n"
    "  var self_get = __self_get, self_set = __self_set, self_delete = __self_delete,\n"
    "      self_keys = __self_keys, self_id = __self_id,\n"
    "      swarm_nodes = __swarm_nodes, swarm_get = __swarm_get, swarm_keys = __swarm_keys;\n"
    "\n"
    "  function makeNodeView(id) {\n"
    "    return new Proxy({}, {\n"
    "      get: function(t, p, r) { return typeof p === 'string' ? swarm_get(id, p) : Reflect.get(t, p, r); },\n"
    "      has: function(t, p) { return typeof p === 'string' ? swarm_keys(id).indexOf(p) !== -1 : Reflect.has(t, p); },\n"
    "      ownKeys: function() { return swarm_keys(id); },\n"
    "      getOwnPropertyDescriptor: function(t, p) {\n"
    "        if (typeof p !== 'string' || swarm_keys(id).indexOf(p) === -1) return undefined;\n"
    "        return { value: swarm_get(id, p), writable: false, enumerable: true, configurable: true };\n"
    "      },\n"
    "      set: function() { return false; },\n"
    "      deleteProperty: function() { return false; }\n"
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
    "  globalThis.swarm = new Proxy({}, {\n"
    "    get: function(t, p, r) {\n"
    "      if (typeof p !== 'string') return Reflect.get(t, p, r);\n"
    "      return swarm_nodes().indexOf(p) === -1 ? undefined : makeNodeView(p);\n"
    "    },\n"
    "    has: function(t, p) { return typeof p === 'string' ? swarm_nodes().indexOf(p) !== -1 : Reflect.has(t, p); },\n"
    "    ownKeys: function() { return swarm_nodes(); },\n"
    "    getOwnPropertyDescriptor: function(t, p) {\n"
    "      if (typeof p !== 'string' || swarm_nodes().indexOf(p) === -1) return undefined;\n"
    "      return { value: makeNodeView(p), writable: false, enumerable: true, configurable: true };\n"
    "    },\n"
    "    set: function() { return false; },\n"
    "    deleteProperty: function() { return false; }\n"
    "  });\n"
    "\n"
    "  delete globalThis.__self_get; delete globalThis.__self_set; delete globalThis.__self_delete;\n"
    "  delete globalThis.__self_keys; delete globalThis.__self_id;\n"
    "  delete globalThis.__swarm_nodes; delete globalThis.__swarm_get; delete globalThis.__swarm_keys;\n"
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

    char* scratch = (char*)malloc((size_t)size + 1);
    if (!scratch) {
        free(data);
        return JS_ThrowOutOfMemory(ctx);
    }
    memcpy(scratch, data, size);
    scratch[size] = '\0';
    free(data);

    JSValue result = JS_ParseJSON(ctx, scratch, size, "<state>");
    free(scratch);
    if (JS_IsException(result)) {
        // Stored bytes aren't valid JSON (shouldn't happen via our own setters) -
        // surface as undefined on read rather than throwing.
        JS_FreeValue(ctx, JS_GetException(ctx));
        return JS_UNDEFINED;
    }
    return result;
}

static JSValue names_to_js_array(JSContext* ctx, char names[][FLEECE_FIELD_NAME_MAX], uint32_t count) {
    JSValue arr = JS_NewArray(ctx);
    for (uint32_t i = 0; i < count; i++) {
        JS_DefinePropertyValueUint32(ctx, arr, i, JS_NewString(ctx, names[i]), JS_PROP_C_W_E);
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
    return names_to_js_array(ctx, names, count);
}

static JSValue js_self_id(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    (void)this_val; (void)argc; (void)argv;
    FleeceEmbedded* emb = get_embedded(ctx);
    if (!emb || !emb->manager) return JS_UNDEFINED;

    char hex[17];
    format_node_id(fleece_state_manager_get_node_id(emb->manager), hex);
    return JS_NewString(ctx, hex);
}

static JSValue js_swarm_nodes(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    (void)this_val; (void)argc; (void)argv;
    FleeceEmbedded* emb = get_embedded(ctx);
    if (!emb || !emb->manager) return JS_NewArray(ctx);

    uint64_t local_id = fleece_state_manager_get_node_id(emb->manager);
    uint64_t ids[FLEECE_JS_LIST_MAX];
    uint32_t count = fleece_state_manager_list_nodes(emb->manager, ids, FLEECE_JS_LIST_MAX);

    JSValue arr = JS_NewArray(ctx);
    uint32_t out_idx = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (ids[i] == local_id) continue;  // swarm excludes the local node - that's what self is for
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
    if (!ok) return JS_UNDEFINED;

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
    if (!ok) return JS_NewArray(ctx);

    char names[FLEECE_JS_LIST_MAX][FLEECE_FIELD_NAME_MAX];
    uint32_t count = fleece_state_manager_list_fields(emb->manager, node_id, names, FLEECE_JS_LIST_MAX);
    return names_to_js_array(ctx, names, count);
}

FleeceEmbedded* fleece_embedded_create(void) {
    FleeceEmbedded* embedded = (FleeceEmbedded*)calloc(1, sizeof(FleeceEmbedded));
    if (!embedded) {
        return NULL;
    }

    embedded->rt = JS_NewRuntime();
    if (!embedded->rt) {
        free(embedded);
        return NULL;
    }
    JS_SetMemoryLimit(embedded->rt, FLEECE_JS_MEMORY_LIMIT);
    JS_SetMaxStackSize(embedded->rt, FLEECE_JS_STACK_SIZE);

    embedded->ctx = JS_NewContext(embedded->rt);
    if (!embedded->ctx) {
        JS_FreeRuntime(embedded->rt);
        free(embedded);
        return NULL;
    }
    JS_SetContextOpaque(embedded->ctx, embedded);

    return embedded;
}

void fleece_embedded_destroy(FleeceEmbedded* embedded) {
    if (!embedded) return;

    if (embedded->ctx) JS_FreeContext(embedded->ctx);
    if (embedded->rt) JS_FreeRuntime(embedded->rt);
    free(embedded);
}

int fleece_embedded_set_state_manager(FleeceEmbedded* embedded, FleeceStateManager* manager) {
    if (!embedded || !manager) return -1;

    embedded->manager = manager;
    return 0;
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

    JS_FreeValue(ctx, global);

    JSValue prelude_result = JS_Eval(ctx, SELF_SWARM_PRELUDE, strlen(SELF_SWARM_PRELUDE), "<prelude>", JS_EVAL_TYPE_GLOBAL);
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

int fleece_embedded_call_init(FleeceEmbedded* embedded) { return call_lifecycle_fn(embedded, "init"); }
int fleece_embedded_call_step(FleeceEmbedded* embedded) { return call_lifecycle_fn(embedded, "step"); }
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

    char* scratch = (char*)malloc((size_t)size + 1);
    if (!scratch) return -1;
    memcpy(scratch, data, size);
    scratch[size] = '\0';

    JSValue val = JS_ParseJSON(ctx, scratch, size, "<set_value>");
    free(scratch);
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

    uint8_t* buf = (uint8_t*)malloc(len);
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
