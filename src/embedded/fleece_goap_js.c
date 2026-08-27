#define _POSIX_C_SOURCE 200809L

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "quickjs.h"

#include "fleece_embedded.h"
#include "fleece_alloc.h"
#include "state/fleece_state_manager.h"
#include "fleece_goap_js.h"

// QuickJS evaluation bridge for the GOAP planner (see fleece_goap_js.h).
// The planner's pre/eff/goal/cost tables hold JS FUNCTION SOURCES; this module
// compiles them (on demand, cached per eval) and evaluates them against a
// `bb` object: { self, world, platform, swarm }.

#define GOAP_FN_PRE   0
#define GOAP_FN_EFF   1
#define GOAP_FN_GOAL  2
#define GOAP_FN_COST  3
#define GOAP_FN_EXEC  4

typedef struct GoapFnEntry {
    bool used;
    uint32_t kind;   // GOAP_FN_*
    uint32_t idx;    // action or goal index
    uint32_t sub;    // pre/eff position (0 for goal/cost)
    char* src;       // source this entry was compiled from (self-invalidating)
    JSValue fn;
} GoapFnEntry;

struct FleeceGoapJsEval {
    FleeceEmbedded* embedded;
    FleeceGoap* goap;
    FleeceGoapEval eval;
    GoapFnEntry* entries;
    uint32_t entry_cap;
    uint32_t entry_next;   // FIFO position for eviction
};

static void goap_js_dump_exception(JSContext* ctx, const char* where) {
    JSValue exc = JS_GetException(ctx);
    const char* msg = JS_ToCString(ctx, exc);
    fprintf(stderr, "fleece: goap JS exception [%s]: %s\n", where, msg ? msg : "(unknown)");
    if (msg) JS_FreeCString(ctx, msg);

    // Include the JS stack when available - authored sources fail with
    // exceptions that are much easier to diagnose with a traceback.
    JSValue stack = JS_GetPropertyStr(ctx, exc, "stack");
    if (JS_IsString(stack)) {
        const char* st = JS_ToCString(ctx, stack);
        if (st) {
            fprintf(stderr, "fleece: goap JS stack:\n%s\n", st);
            JS_FreeCString(ctx, st);
        }
    }
    JS_FreeValue(ctx, stack);
    JS_FreeValue(ctx, exc);
}

// Compile one function source to a JSValue. Sources are written as bare
// function expressions ("function(bb){ ... }"); wrapping in parens makes JS_Eval
// treat them as expressions, so the result is the function itself rather than
// undefined (a FunctionDeclaration at statement position).
static JSValue compile_goap_fn(JSContext* ctx, const char* src) {
    size_t slen = strlen(src);
    size_t alloc = slen + 2;  // "(" + src + ")" - no NUL in the parse length
    char* buf = (char*)fleece_malloc(alloc + 1);
    if (!buf) return JS_EXCEPTION;
    buf[0] = '(';
    memcpy(buf + 1, src, slen);
    buf[slen + 1] = ')';
    buf[slen + 2] = '\0';

    JSValue result = JS_Eval(ctx, buf, alloc, "<goap fn>", JS_EVAL_TYPE_GLOBAL);
    fleece_free(buf);
    return result;
}

static void goap_fn_entry_free(FleeceGoapJsEval* je, GoapFnEntry* e) {
    JS_FreeValue((JSContext*)fleece_embedded_get_context(je->embedded), e->fn);
    fleece_free(e->src);
    e->used = false;
    e->src = NULL;
}

static bool goap_fn_entry_match(const GoapFnEntry* e, uint32_t kind, uint32_t idx, uint32_t sub, const char* src) {
    return e->used && e->kind == kind && e->idx == idx && e->sub == sub && strcmp(e->src, src) == 0;
}

// Returns a borrowed-and-duplicated JSValue; caller frees. JS_UNDEFINED means
// "no source" or "could not compile".
static JSValue goap_get_js_fn(FleeceGoapJsEval* je, uint32_t kind, uint32_t idx, uint32_t sub, const char* src) {
    JSContext* ctx = (JSContext*)fleece_embedded_get_context(je->embedded);
    if (!src || src[0] == '\0') return JS_UNDEFINED;

    for (uint32_t i = 0; i < je->entry_cap; i++) {
        GoapFnEntry* e = &je->entries[i];
        if (goap_fn_entry_match(e, kind, idx, sub, src)) return JS_DupValue(ctx, e->fn);
        if (e->used && e->kind == kind && e->idx == idx && e->sub == sub) {
            // Source changed (tables were rebuilt); recompile in place.
            goap_fn_entry_free(je, e);
            e->used = true;
            e->kind = kind;
            e->idx = idx;
            e->sub = sub;
            e->src = strdup(src);
            e->fn = compile_goap_fn(ctx, src);
            if (JS_IsException(e->fn)) {
                goap_js_dump_exception(ctx, "compile");
                e->fn = JS_UNDEFINED;
            }
            return JS_DupValue(ctx, e->fn);
        }
    }

    GoapFnEntry* slot = &je->entries[je->entry_next];
    je->entry_next = (je->entry_next + 1) % (je->entry_cap ? je->entry_cap : 1);
    if (slot->used) goap_fn_entry_free(je, slot);
    slot->used = true;
    slot->kind = kind;
    slot->idx = idx;
    slot->sub = sub;
    slot->src = strdup(src);
    slot->fn = compile_goap_fn(ctx, src);
    if (JS_IsException(slot->fn)) {
        goap_js_dump_exception(ctx, "compile");
        slot->fn = JS_UNDEFINED;
    }
    return JS_DupValue(ctx, slot->fn);
}

// --- Blackboard <-> JS bb object marshalling -----------------------------

// Build the `bb` object passed to authored functions: { self, world,
// platform, swarm }. self/world are detached copies (functions may mutate
// them); platform/swarm are the live read-only script globals.
static JSValue goap_bb_to_js(FleeceGoapJsEval* je, const FleeceGoapBlackboard* bb) {
    JSContext* ctx = (JSContext*)fleece_embedded_get_context(je->embedded);
    JSValue obj = JS_NewObject(ctx);
    JSValue self = JS_NewObject(ctx);
    JSValue world = JS_NewObject(ctx);

    for (uint32_t i = 0; i < bb->count; i++) {
        const FleeceGoapField* f = &bb->fields[i];
        char* scratch = (char*)fleece_malloc((size_t)f->size + 1);
        if (!scratch) continue;
        memcpy(scratch, f->data, f->size);
        scratch[f->size] = '\0';
        JSValue v = JS_ParseJSON(ctx, scratch, f->size, "<bb>");
        fleece_free(scratch);
        if (JS_IsException(v)) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            continue;
        }
        if (JS_IsUndefined(v)) {
            JS_FreeValue(ctx, v);
            continue;
        }
        JS_SetPropertyStr(ctx, f->is_shared ? world : self, f->name, v);
    }

    JS_SetPropertyStr(ctx, obj, "self", self);
    JS_SetPropertyStr(ctx, obj, "world", world);

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue platform = JS_GetPropertyStr(ctx, global, "platform");
    JSValue swarm = JS_GetPropertyStr(ctx, global, "swarm");
    JS_FreeValue(ctx, global);
    if (JS_IsUndefined(platform)) {
        JS_FreeValue(ctx, platform);
        platform = JS_NewObject(ctx);
    }
    if (JS_IsUndefined(swarm)) {
        JS_FreeValue(ctx, swarm);
        swarm = JS_NewObject(ctx);
    }
    JS_SetPropertyStr(ctx, obj, "platform", platform);
    JS_SetPropertyStr(ctx, obj, "swarm", swarm);
    return obj;
}

// Extract a JS object's string-keyed enumerable properties, JSON-stringify each
// value, and store into `dst` (is_shared selects the namespace).
static int goap_js_object_to_bb(JSContext* ctx, JSValue obj, bool is_shared, FleeceGoapBlackboard* dst) {
    if (!JS_IsObject(obj)) return 0;

    JSPropertyEnum* props = NULL;
    uint32_t nprops = 0;
    if (JS_GetOwnPropertyNames(ctx, &props, &nprops, obj, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) != 0) {
        return -1;
    }
    for (uint32_t i = 0; i < nprops; i++) {
        JSAtom atom = props[i].atom;
        const char* name = JS_AtomToCString(ctx, atom);
        if (!name) continue;
        JSValue val = JS_GetProperty(ctx, obj, atom);
        JSValue json = JS_JSONStringify(ctx, val, JS_UNDEFINED, JS_UNDEFINED);
        JS_FreeValue(ctx, val);
        if (JS_IsException(json)) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            JS_FreeCString(ctx, name);
            continue;
        }
        if (JS_IsUndefined(json)) {  // value not JSON-serializable (undefined/function/...)
            JS_FreeValue(ctx, json);
            JS_FreeCString(ctx, name);
            continue;
        }
        size_t len = 0;
        const char* text = JS_ToCStringLen(ctx, &len, json);
        JS_FreeValue(ctx, json);
        if (text) {
            fleece_goap_bb_set(dst, name, (const uint8_t*)text, (uint32_t)len, is_shared);
            JS_FreeCString(ctx, text);
        }
        JS_FreeCString(ctx, name);
        JS_FreeAtom(ctx, atom);
    }
    js_free(ctx, props);
    return 0;
}

// Convert a returned `bb` object into a C blackboard (self + world namespaces).
static int goap_js_to_bb(FleeceGoapJsEval* je, JSValue bbjs, FleeceGoapBlackboard* dst) {
    JSContext* ctx = (JSContext*)fleece_embedded_get_context(je->embedded);
    if (!JS_IsObject(bbjs)) return -1;
    JSValue self = JS_GetPropertyStr(ctx, bbjs, "self");
    JSValue world = JS_GetPropertyStr(ctx, bbjs, "world");
    int rc = 0;
    if (goap_js_object_to_bb(ctx, self, false, dst) != 0) rc = -1;
    if (goap_js_object_to_bb(ctx, world, true, dst) != 0) rc = -1;
    JS_FreeValue(ctx, self);
    JS_FreeValue(ctx, world);
    return rc;
}

// --- Eval callbacks ------------------------------------------------------

static int js_action_pre(void* user_data, uint32_t action_idx, const FleeceGoapBlackboard* bb, bool* out) {
    FleeceGoapJsEval* je = (FleeceGoapJsEval*)user_data;
    JSContext* ctx = (JSContext*)fleece_embedded_get_context(je->embedded);
    *out = true;

    JSValue bbjs = goap_bb_to_js(je, bb);
    uint32_t n = fleece_goap_action_pre_count(je->goap, action_idx);
    for (uint32_t i = 0; i < n; i++) {
        const char* src = fleece_goap_action_pre(je->goap, action_idx, i);
        JSValue fn = goap_get_js_fn(je, GOAP_FN_PRE, action_idx, i, src);
        if (JS_IsUndefined(fn)) {  // no source: precondition trivially true
            JS_FreeValue(ctx, fn);
            continue;
        }
        if (!JS_IsFunction(ctx, fn)) {
            JS_FreeValue(ctx, fn);
            JS_FreeValue(ctx, bbjs);
            *out = false;
            return 0;
        }
        JSValue result = JS_Call(ctx, fn, JS_UNDEFINED, 1, (JSValueConst*)&bbjs);
        JS_FreeValue(ctx, fn);
        if (JS_IsException(result)) {
            goap_js_dump_exception(ctx, "pre");
            JS_FreeValue(ctx, bbjs);
            *out = false;
            return 0;
        }
        bool holds = JS_ToBool(ctx, result) > 0;
        JS_FreeValue(ctx, result);
        *out = holds;
        if (!holds) break;
    }
    JS_FreeValue(ctx, bbjs);
    return 0;
}

static int js_goal_satisfied(void* user_data, uint32_t goal_idx, const FleeceGoapBlackboard* bb, bool* out) {
    FleeceGoapJsEval* je = (FleeceGoapJsEval*)user_data;
    JSContext* ctx = (JSContext*)fleece_embedded_get_context(je->embedded);
    *out = false;

    const char* src = fleece_goap_goal_expr(je->goap, goal_idx);
    if (!src || src[0] == '\0') return 0;
    JSValue fn = goap_get_js_fn(je, GOAP_FN_GOAL, goal_idx, 0, src);
    if (JS_IsUndefined(fn)) {
        JS_FreeValue(ctx, fn);
        return 0;
    }
    if (!JS_IsFunction(ctx, fn)) {
        JS_FreeValue(ctx, fn);
        return 0;
    }
    JSValue bbjs = goap_bb_to_js(je, bb);
    JSValue result = JS_Call(ctx, fn, JS_UNDEFINED, 1, (JSValueConst*)&bbjs);
    JS_FreeValue(ctx, bbjs);
    JS_FreeValue(ctx, fn);
    if (JS_IsException(result)) {
        goap_js_dump_exception(ctx, "goal");
        return 0;
    }
    *out = JS_ToBool(ctx, result) > 0;
    JS_FreeValue(ctx, result);
    return 0;
}

static int js_action_cost(void* user_data, uint32_t action_idx, const FleeceGoapBlackboard* bb, double* out) {
    FleeceGoapJsEval* je = (FleeceGoapJsEval*)user_data;
    JSContext* ctx = (JSContext*)fleece_embedded_get_context(je->embedded);
    *out = 1.0;

    const char* src = fleece_goap_action_cost_source(je->goap, action_idx);
    if (!src || src[0] == '\0') return 0;
    JSValue v = goap_get_js_fn(je, GOAP_FN_COST, action_idx, 0, src);
    if (JS_IsUndefined(v)) {
        JS_FreeValue(ctx, v);
        return 0;
    }
    if (JS_IsFunction(ctx, v)) {
        JSValue bbjs = goap_bb_to_js(je, bb);
        JSValue result = JS_Call(ctx, v, JS_UNDEFINED, 1, (JSValueConst*)&bbjs);
        JS_FreeValue(ctx, bbjs);
        JS_FreeValue(ctx, v);
        if (JS_IsException(result)) {
            goap_js_dump_exception(ctx, "compile");
            return 0;
        }
        double c;
        if (JS_ToFloat64(ctx, &c, result) != 0 || c < 0.0) c = 1.0;
        JS_FreeValue(ctx, result);
        *out = c;
        return 0;
    }
    // Non-function cost source: a numeric literal ("1", "2.5").
    double c;
    if (JS_ToFloat64(ctx, &c, v) != 0 || c < 0.0) c = 1.0;
    JS_FreeValue(ctx, v);
    *out = c;
    return 0;
}

static int js_action_apply(void* user_data, uint32_t action_idx, const FleeceGoapBlackboard* src, FleeceGoapBlackboard* dst) {
    FleeceGoapJsEval* je = (FleeceGoapJsEval*)user_data;
    JSContext* ctx = (JSContext*)fleece_embedded_get_context(je->embedded);

    JSValue bbjs = goap_bb_to_js(je, src);
    uint32_t n = fleece_goap_action_eff_count(je->goap, action_idx);
    if (n == 0) {  // no effects: the action changes nothing
        JS_FreeValue(ctx, bbjs);
        return 0;
    }
    for (uint32_t i = 0; i < n; i++) {
        const char* esrc = fleece_goap_action_eff(je->goap, action_idx, i);
        JSValue fn = goap_get_js_fn(je, GOAP_FN_EFF, action_idx, i, esrc);
        if (!JS_IsFunction(ctx, fn)) {
            JS_FreeValue(ctx, fn);
            JS_FreeValue(ctx, bbjs);
            return -1;  // effect contract requires a function
        }
        JSValue result = JS_Call(ctx, fn, JS_UNDEFINED, 1, (JSValueConst*)&bbjs);
        JS_FreeValue(ctx, fn);
        if (JS_IsException(result)) {
            goap_js_dump_exception(ctx, "pre");
            JS_FreeValue(ctx, bbjs);
            return -1;
        }
        JS_FreeValue(ctx, bbjs);
        if (!JS_IsObject(result)) {  // effect must return the blackboard
            JS_FreeValue(ctx, result);
            return -1;
        }
        bbjs = JS_DupValue(ctx, result);
        JS_FreeValue(ctx, result);
    }

    int rc = goap_js_to_bb(je, bbjs, dst);
    JS_FreeValue(ctx, bbjs);
    return rc;
}

// Run an action's exec() body for one tick. The body receives (bb, tick) where
// tick is the 1-based tick within this action; it mutates its `bb` copy to
// perform the real work. *dst receives the resulting blackboard (self + world)
// for the executor to commit, and *done is set when the body reports the action
// finished (its return value). A missing exec is not an error: *done stays
// false and *dst is left untouched.
static int js_action_exec(FleeceGoapJsEval* je, uint32_t action_idx, const FleeceGoapBlackboard* bb,
                          uint32_t tick, FleeceGoapBlackboard* dst, bool* done) {
    JSContext* ctx = (JSContext*)fleece_embedded_get_context(je->embedded);
    *done = false;
    if (!dst) return -1;

    const char* src = fleece_goap_action_exec(je->goap, action_idx);
    if (!src || src[0] == '\0') return 0;  // no exec body

    JSValue fn = goap_get_js_fn(je, GOAP_FN_EXEC, action_idx, 0, src);
    if (JS_IsUndefined(fn)) {
        JS_FreeValue(ctx, fn);
        return 0;
    }
    if (!JS_IsFunction(ctx, fn)) {
        JS_FreeValue(ctx, fn);
        return 0;
    }

    JSValue bbjs = goap_bb_to_js(je, bb);
    JSValue tickv = JS_NewInt32(ctx, (int32_t)tick);
    JSValue argv[2] = { bbjs, tickv };
    JSValue result = JS_Call(ctx, fn, JS_UNDEFINED, 2, (JSValueConst*)argv);
    JS_FreeValue(ctx, tickv);
    JS_FreeValue(ctx, fn);
    if (JS_IsException(result)) {
        goap_js_dump_exception(ctx, "exec");
        JS_FreeValue(ctx, bbjs);
        return -1;
    }
    *done = JS_ToBool(ctx, result) > 0;

    // The body either returns the (mutated) bb object or just a done flag - in
    // the latter case read the result from the passed-in object, which the body
    // mutated in place.
    JSValue extract = JS_IsObject(result) ? result : bbjs;
    int rc = goap_js_to_bb(je, extract, dst);
    JS_FreeValue(ctx, result);
    JS_FreeValue(ctx, bbjs);
    return rc;
}

// --- Lifecycle -----------------------------------------------------------

FleeceGoapJsEval* fleece_goap_js_eval_create(FleeceEmbedded* embedded, FleeceGoap* goap) {
    if (!embedded || !goap) return NULL;
    FleeceGoapJsEval* je = (FleeceGoapJsEval*)fleece_calloc(1, sizeof(FleeceGoapJsEval));
    if (!je) return NULL;
    je->embedded = embedded;
    je->goap = goap;

    // Size the cache to the total number of stored function sources.
    uint32_t cap = fleece_goap_action_count(goap);  // cost sources
    for (uint32_t a = 0; a < fleece_goap_action_count(goap); a++) {
        cap += fleece_goap_action_pre_count(goap, a) + fleece_goap_action_eff_count(goap, a);
    }
    cap += fleece_goap_action_count(goap);  // exec sources
    cap += fleece_goap_goal_count(goap);
    if (cap == 0) cap = 1;
    je->entries = (GoapFnEntry*)fleece_calloc(cap, sizeof(GoapFnEntry));
    if (!je->entries) {
        fleece_free(je);
        return NULL;
    }
    je->entry_cap = cap;
    je->eval.user_data = je;
    je->eval.action_pre = js_action_pre;
    je->eval.action_apply = js_action_apply;
    je->eval.goal_satisfied = js_goal_satisfied;
    je->eval.action_cost = js_action_cost;
    return je;
}

void fleece_goap_js_eval_destroy(FleeceGoapJsEval* je) {
    if (!je) return;
    for (uint32_t i = 0; i < je->entry_cap; i++) {
        if (je->entries[i].used) {
            JS_FreeValue((JSContext*)fleece_embedded_get_context(je->embedded), je->entries[i].fn);
            fleece_free(je->entries[i].src);
        }
    }
    fleece_free(je->entries);
    fleece_free(je);
}

const FleeceGoapEval* fleece_goap_js_eval_get(const FleeceGoapJsEval* je) {
    return je ? &je->eval : NULL;
}

// --- Convenience helpers -------------------------------------------------

int fleece_goap_js_bb_from_state(FleeceEmbedded* embedded, FleeceGoapBlackboard* bb) {
    if (!embedded || !bb) return -1;
    FleeceStateManager* sm = (FleeceStateManager*)fleece_embedded_get_state_manager(embedded);
    if (!sm) return -1;

    uint64_t self_id = fleece_state_manager_get_node_id(sm);
    char names[64][FLEECE_FIELD_NAME_MAX];

    // self namespace (local node fields)
    uint32_t n = fleece_state_manager_list_fields(sm, self_id, names, 64);
    for (uint32_t i = 0; i < n; i++) {
        uint8_t* data = NULL;
        uint32_t size = 0;
        if (fleece_state_manager_get_named(sm, self_id, names[i], &data, &size) == 0 && data) {
            if (fleece_goap_bb_set(bb, names[i], data, size, false) != 0) {
                fleece_free(data);
                return -1;
            }
            fleece_free(data);
        }
    }

    // world namespace (shared fields)
    n = fleece_state_manager_list_fields(sm, FLEECE_SHARED_OWNER_ID, names, 64);
    for (uint32_t i = 0; i < n; i++) {
        uint8_t* data = NULL;
        uint32_t size = 0;
        if (fleece_state_manager_get_named(sm, FLEECE_SHARED_OWNER_ID, names[i], &data, &size) == 0 && data) {
            if (fleece_goap_bb_set(bb, names[i], data, size, true) != 0) {
                fleece_free(data);
                return -1;
            }
            fleece_free(data);
        }
    }
    return 0;
}

int fleece_goap_js_bb_commit(FleeceEmbedded* embedded, const FleeceGoapBlackboard* bb) {
    if (!embedded || !bb) return -1;
    FleeceStateManager* sm = (FleeceStateManager*)fleece_embedded_get_state_manager(embedded);
    if (!sm) return -1;
    uint64_t self_id = fleece_state_manager_get_node_id(sm);

    for (uint32_t i = 0; i < bb->count; i++) {
        const FleeceGoapField* f = &bb->fields[i];
        // Skip the write entirely when the field's value is byte-identical to
        // what is already stored: every action exec runs every tick and its
        // returned blackboard mirrors the FULL loaded snapshot (self AND
        // world/shared fields), not just what the JS body actually touched.
        // Committing all of it unconditionally re-stamps EVERY shared field
        // with a fresh LWW timestamp on EVERY tick, forever - a genuinely
        // conflicting peer's write (correct, older) can then never win a
        // comparison against this node's own perpetually-refreshed copy, no
        // matter how the caller throttles its OWN reassertion logic. Found
        // live via the goal-pool auction: contested claim/goal records never
        // converged because bb_commit was re-touching all of them every
        // single tick regardless of goal_pool_tick()'s own reassert cadence.
        uint64_t owner = f->is_shared ? FLEECE_SHARED_OWNER_ID : self_id;
        uint8_t* existing = NULL;
        uint32_t existing_size = 0;
        bool unchanged = fleece_state_manager_get_named(sm, owner, f->name, &existing, &existing_size) == 0 &&
                          existing_size == f->size &&
                          (f->size == 0 || memcmp(existing, f->data, f->size) == 0);
        if (existing) fleece_free(existing);
        if (unchanged) continue;

        int rc = f->is_shared
            ? fleece_state_manager_set_shared(sm, f->name, f->data, f->size)
            : fleece_state_manager_set_named(sm, f->name, f->data, f->size);
        if (rc != 0) return -1;
    }
    return 0;
}

FleeceGoapPlan* fleece_goap_js_plan(FleeceEmbedded* embedded, FleeceGoap* goap,
                                    const FleeceGoapBlackboard* start,
                                    const char** goal_ids, uint32_t n_goals) {
    FleeceGoapJsEval* je = fleece_goap_js_eval_create(embedded, goap);
    if (!je) return NULL;
    FleeceGoapPlan* plan = fleece_goap_plan(goap, start, goal_ids, n_goals, &je->eval);
    fleece_goap_js_eval_destroy(je);
    return plan;
}

int fleece_goap_js_select_goal(FleeceEmbedded* embedded, FleeceGoap* goap,
                               const FleeceGoapBlackboard* bb, double threshold) {
    FleeceGoapJsEval* je = fleece_goap_js_eval_create(embedded, goap);
    if (!je) return -1;
    int goal = fleece_goap_select_goal(goap, bb, &je->eval, threshold);
    fleece_goap_js_eval_destroy(je);
    return goal;
}

int fleece_goap_js_apply_action(FleeceEmbedded* embedded, FleeceGoap* goap,
                                uint32_t action_idx, const FleeceGoapBlackboard* src,
                                FleeceGoapBlackboard* dst) {
    FleeceGoapJsEval* je = fleece_goap_js_eval_create(embedded, goap);
    if (!je) return -1;
    int rc = js_action_apply(je, action_idx, src, dst);
    fleece_goap_js_eval_destroy(je);
    return rc;
}

// --- Behavior-loop driver (brain) ----------------------------------------

struct FleeceGoapBrain {
    FleeceEmbedded* embedded;
    FleeceGoap* goap;
    FleeceGoapJsEval* eval;
    FleeceGoapPlan* plan;       // current one-step-ahead plan (rebuilt each action)
    int goal_idx;               // selected goal, -1 = none
    int action_idx;             // action being executed, -1 = none
    uint32_t exec_progress;     // ticks the current action has been executing
    uint32_t max_action_ticks;  // watchdog: abort a wedged action after N ticks (0 = off)
    uint32_t* goal_cooldown_until;  // per-goal-index tick_count value before which brain_replan skips it
    uint32_t goal_cooldown_ticks;   // 0 (default) = cooldown off, always retry every ranked goal
    uint32_t tick_count;
    FleeceGoapBrainEventFn event_cb;
    void* event_ud;
    FleeceGoapWorldModelFn world_model_cb;  // host telemetry injection (see header)
    void* world_model_ud;
    FleeceGoapDivergenceFn divergence_cb;   // eff-predicted vs exec-produced (debug aid)
    void* divergence_ud;
    char goal_pool_prefix[FLEECE_FIELD_NAME_MAX];    // empty = goal-pool mode off
    char goal_pool_key_field[FLEECE_FIELD_NAME_MAX]; // self field the held key is published into
    double goal_pool_contest_margin;
    char goal_pool_last_logged[FLEECE_FIELD_NAME_MAX]; // last currentGoalKey a transition was fired for (see action_library_tick)

    // Action library (fleece_goap_brain_set_action_library) -- parallel
    // arrays, one entry per registered FleeceActionDef. Compiled once at
    // registration (action libraries are small and effectively static),
    // not per tick. No name array: actions are found by which `pre` holds
    // against the held goal's state, never by looking one up by name (see
    // fleece_goap_js.h's action-library doc comment for why).
    JSValue* action_pre_fns;   // JS_UNDEFINED = no precondition (trivially true)
    JSValue* action_eff_fns;   // JS_UNDEFINED = no effect (can't provide anything)
    JSValue* action_exec_fns;  // JS_UNDEFINED = no exec body
    uint32_t action_count;

    // Last goal-pool transition reported via FLEECE_GOAP_BRAIN_EVENT_GOAL_POOL
    // (see fleece_goap_brain_goal_pool_transition_kind/key()). Valid only
    // during that callback.
    FleeceGoalPoolTransitionKind goal_pool_transition_kind;
    char goal_pool_transition_key[FLEECE_FIELD_NAME_MAX];
};

FleeceGoapBrain* fleece_goap_brain_create(FleeceEmbedded* embedded, FleeceGoap* goap) {
    if (!embedded || !goap) return NULL;
    FleeceGoapBrain* b = (FleeceGoapBrain*)fleece_calloc(1, sizeof(FleeceGoapBrain));
    if (!b) return NULL;
    b->eval = fleece_goap_js_eval_create(embedded, goap);
    if (!b->eval) {
        fleece_free(b);
        return NULL;
    }
    uint32_t goal_count = fleece_goap_goal_count(goap);
    if (goal_count > 0) {
        b->goal_cooldown_until = (uint32_t*)fleece_calloc(goal_count, sizeof(uint32_t));
        if (!b->goal_cooldown_until) {
            fleece_goap_js_eval_destroy(b->eval);
            fleece_free(b);
            return NULL;
        }
    }
    b->embedded = embedded;
    b->goap = goap;
    b->goal_idx = -1;
    b->action_idx = -1;
    return b;
}

// Frees the brain's currently-registered action library (JSValues), if
// any. Shared by fleece_goap_brain_set_action_library (replacing a prior
// registration) and fleece_goap_brain_destroy.
static void action_library_free(FleeceGoapBrain* b) {
    if (!b->action_pre_fns) return;
    JSContext* ctx = (JSContext*)fleece_embedded_get_context(b->embedded);
    for (uint32_t i = 0; i < b->action_count; i++) {
        JS_FreeValue(ctx, b->action_pre_fns[i]);
        JS_FreeValue(ctx, b->action_eff_fns[i]);
        JS_FreeValue(ctx, b->action_exec_fns[i]);
    }
    fleece_free(b->action_pre_fns);
    fleece_free(b->action_eff_fns);
    fleece_free(b->action_exec_fns);
    b->action_pre_fns = NULL;
    b->action_eff_fns = NULL;
    b->action_exec_fns = NULL;
    b->action_count = 0;
}

void fleece_goap_brain_destroy(FleeceGoapBrain* b) {
    if (!b) return;
    if (b->plan) fleece_goap_plan_destroy(b->plan);
    action_library_free(b);
    fleece_goap_js_eval_destroy(b->eval);
    fleece_free(b->goal_cooldown_until);
    fleece_free(b);
}

// Plan for the highest-utility unsatisfied goal and select its first action.
// If the best goal cannot currently be planned (its actions' preconditions
// don't hold), fall back to the next-best goal rather than idling - a UAV that
// can't scan on an empty battery should still be able to pick "recharge".
static void brain_replan(FleeceGoapBrain* b, const FleeceGoapBlackboard* bb) {
    if (b->plan) {
        fleece_goap_plan_destroy(b->plan);
        b->plan = NULL;
    }
    b->goal_idx = -1;
    b->action_idx = -1;

    uint32_t cap = fleece_goap_goal_count(b->goap);
    if (cap == 0) {
        if (b->event_cb) b->event_cb(b, FLEECE_GOAP_BRAIN_EVENT_IDLE, b->event_ud);
        return;
    }

    uint32_t* ranked = (uint32_t*)fleece_malloc(cap * sizeof(uint32_t));
    if (!ranked) {
        if (b->event_cb) b->event_cb(b, FLEECE_GOAP_BRAIN_EVENT_IDLE, b->event_ud);
        return;
    }
    uint32_t n = fleece_goap_rank_goals(b->goap, bb, &b->eval->eval, 0.0, ranked, cap);

    const char* goal_ids[1];
    for (uint32_t i = 0; i < n; i++) {
        uint32_t goal = ranked[i];
        if (b->goal_cooldown_ticks > 0 && b->goal_cooldown_until[goal] > b->tick_count) {
            continue;  // failed to plan recently - don't re-search it every tick
        }
        goal_ids[0] = fleece_goap_goal_id(b->goap, goal);
        FleeceGoapPlan* plan = fleece_goap_plan(b->goap, bb, goal_ids, 1, &b->eval->eval);
        if (!plan || fleece_goap_plan_length(plan) == 0) {
            if (plan) fleece_goap_plan_destroy(plan);
            if (b->goal_cooldown_ticks > 0) b->goal_cooldown_until[goal] = b->tick_count + b->goal_cooldown_ticks;
            continue;  // can't plan this goal right now - try the next best
        }

        b->goal_idx = (int)goal;
        const char* aid = fleece_goap_plan_action_id(plan, 0);
        b->action_idx = (int)fleece_goap_find_action(b->goap, aid);
        b->exec_progress = 0;
        b->plan = plan;
        if (b->event_cb) b->event_cb(b, FLEECE_GOAP_BRAIN_EVENT_REPLAN, b->event_ud);
        fleece_free(ranked);
        return;
    }

    fleece_free(ranked);
    if (b->event_cb) b->event_cb(b, FLEECE_GOAP_BRAIN_EVENT_IDLE, b->event_ud);
}

// Snapshot the live state manager into a blackboard, then let the host's
// world-model hook inject live telemetry into the `self` namespace (e.g.
// freshly-streamed GPS/battery/mode). Authorized pre/goal/eff/exec functions
// see these fields every tick; they commit like any other self field.
static int brain_fresh_bb(FleeceGoapBrain* b, FleeceGoapBlackboard* bb) {
    if (fleece_goap_js_bb_from_state(b->embedded, bb) != 0) return -1;
    if (b->world_model_cb) b->world_model_cb(b, bb, b->tick_count, b->world_model_ud);
    return 0;
}

// Report where the action's declared effect (`eff`, used by the planner's A*)
// diverges from what its runtime body (`exec`) actually produced. This is a
// debug aid for catching model errors - an eff claiming a goal becomes
// reachable while the real body never delivers it. Small divergences are
// normal (eff is a heuristic); the callback just surfaces them.
static void report_divergences(FleeceGoapBrain* b, uint32_t action_idx,
                               const FleeceGoapBlackboard* pred,
                               const FleeceGoapBlackboard* actual) {
    enum { MAX_DIFFS = 16 };
    FleeceGoapDivergence diffs[MAX_DIFFS];
    uint32_t n = 0;

    for (uint32_t i = 0; i < pred->count && n < MAX_DIFFS; i++) {
        const FleeceGoapField* pf = &pred->fields[i];
        uint32_t asz = 0;
        const uint8_t* av = fleece_goap_bb_get(actual, pf->name, &asz);
        if (av && asz == pf->size && memcmp(av, pf->data, pf->size) == 0) continue;
        diffs[n].name = pf->name;
        diffs[n].in_eff = true;
        diffs[n].in_actual = (av != NULL);
        diffs[n].eff_value = pf->data;
        diffs[n].eff_size = pf->size;
        diffs[n].actual_value = av;
        diffs[n].actual_size = asz;
        n++;
    }
    for (uint32_t i = 0; i < actual->count && n < MAX_DIFFS; i++) {
        const FleeceGoapField* af = &actual->fields[i];
        if (fleece_goap_bb_index(pred, af->name) != UINT32_MAX) continue;
        diffs[n].name = af->name;
        diffs[n].in_eff = false;
        diffs[n].in_actual = true;
        diffs[n].eff_value = NULL;
        diffs[n].eff_size = 0;
        diffs[n].actual_value = af->data;
        diffs[n].actual_size = af->size;
        n++;
    }

    if (n > 0) b->divergence_cb(b, action_idx, diffs, n, b->divergence_ud);
}

static void goal_pool_tick(FleeceGoapBrain* b);

int fleece_goap_brain_tick(FleeceGoapBrain* b) {
    if (!b) return -1;
    b->tick_count++;

    // 0. Goal-pool auction (only when configured, see
    //    fleece_goap_brain_set_goal_pool) -- runs first so this SAME tick's
    //    exec body already sees an up-to-date claim in
    //    self[goal_pool_key_field].
    goal_pool_tick(b);

    // 1. Run the in-flight action's exec() body. The body does the real work
    //    every tick (mutating its bb copy, which we commit so the world sees
    //    progress as it happens) and reports done when the action is finished.
    //    An action with no exec has no runtime body: it completes at once and
    //    changes nothing. On completion we drop the one-step plan and replan
    //    this tick.
    if (b->action_idx >= 0) {
        const char* esrc = fleece_goap_action_exec(b->goap, (uint32_t)b->action_idx);
        bool aborted = false;
        if (esrc && esrc[0] != '\0') {
            b->exec_progress++;
            // Watchdog: a wedged action (hardware that never responds, a target
            // that vanished mid-hunt) must not pin the agent forever - give up
            // and replan. max_action_ticks == 0 disables this.
            if (b->max_action_ticks > 0 && b->exec_progress >= b->max_action_ticks) {
                aborted = true;
            } else {
                FleeceGoapBlackboard bb = {0};
                if (brain_fresh_bb(b, &bb) != 0) return -1;
                FleeceGoapBlackboard dst = {0};
                bool done = false;
                int rc = js_action_exec(b->eval, (uint32_t)b->action_idx, &bb, b->exec_progress, &dst, &done);
                if (rc != 0) {
                    fleece_goap_bb_release(&dst);
                    fleece_goap_bb_release(&bb);
                    b->action_idx = -1;
                    return 0;  // exec failed - next tick replans
                }
                // Diagnostics (off by default): compare what the action's eff
                // predicted (A* model) with what exec actually produced.
                if (b->divergence_cb) {
                    FleeceGoapBlackboard pred = {0};
                    if (b->eval->eval.action_apply(b->eval, (uint32_t)b->action_idx, &bb, &pred) == 0) {
                        report_divergences(b, (uint32_t)b->action_idx, &pred, &dst);
                        fleece_goap_bb_release(&pred);
                    }
                }
                fleece_goap_bb_release(&bb);
                if (fleece_goap_js_bb_commit(b->embedded, &dst) != 0) {
                    fleece_goap_bb_release(&dst);
                    return -1;
                }
                fleece_goap_bb_release(&dst);
                if (!done) return 0;  // keep executing next tick
            }
        }

        b->action_idx = -1;
        b->exec_progress = 0;
        if (b->plan) {
            fleece_goap_plan_destroy(b->plan);
            b->plan = NULL;
        }
        if (b->event_cb) b->event_cb(b, aborted ? FLEECE_GOAP_BRAIN_EVENT_ABORTED : FLEECE_GOAP_BRAIN_EVENT_ACTION_DONE, b->event_ud);
        // fall through: re-execute planning now that the world changed
    }

    // 2. (Re)plan: pick the best unsatisfied goal and its first action.
    FleeceGoapBlackboard bb = {0};
    if (brain_fresh_bb(b, &bb) != 0) return -1;
    brain_replan(b, &bb);
    fleece_goap_bb_release(&bb);
    return 0;
}

void fleece_goap_brain_set_event_callback(FleeceGoapBrain* b, FleeceGoapBrainEventFn cb, void* user_data) {
    if (!b) return;
    b->event_cb = cb;
    b->event_ud = user_data;
}

void fleece_goap_brain_set_world_model(FleeceGoapBrain* b, FleeceGoapWorldModelFn cb, void* user_data) {
    if (!b) return;
    b->world_model_cb = cb;
    b->world_model_ud = user_data;
}

void fleece_goap_brain_set_divergence_cb(FleeceGoapBrain* b, FleeceGoapDivergenceFn cb, void* user_data) {
    if (!b) return;
    b->divergence_cb = cb;
    b->divergence_ud = user_data;
}

void fleece_goap_brain_set_max_action_ticks(FleeceGoapBrain* b, uint32_t max_ticks) {
    if (!b) return;
    b->max_action_ticks = max_ticks;
}

void fleece_goap_brain_set_goal_cooldown_ticks(FleeceGoapBrain* b, uint32_t ticks) {
    if (!b) return;
    b->goal_cooldown_ticks = ticks;
}

void fleece_goap_brain_set_goal_pool(FleeceGoapBrain* b, const char* goal_prefix,
                                      const char* current_key_field, double contest_margin) {
    if (!b) return;
    if (!goal_prefix || !goal_prefix[0] || !current_key_field || !current_key_field[0]) {
        b->goal_pool_prefix[0] = '\0';
        return;
    }
    strncpy(b->goal_pool_prefix, goal_prefix, sizeof(b->goal_pool_prefix) - 1);
    b->goal_pool_prefix[sizeof(b->goal_pool_prefix) - 1] = '\0';
    strncpy(b->goal_pool_key_field, current_key_field, sizeof(b->goal_pool_key_field) - 1);
    b->goal_pool_key_field[sizeof(b->goal_pool_key_field) - 1] = '\0';
    b->goal_pool_contest_margin = contest_margin;
}

int fleece_goap_brain_set_action_library(FleeceGoapBrain* b, const FleeceActionDef* defs, uint32_t count) {
    if (!b) return -1;
    action_library_free(b);
    if (!defs || count == 0) return 0;

    JSContext* ctx = (JSContext*)fleece_embedded_get_context(b->embedded);
    JSValue* pre_fns = (JSValue*)fleece_calloc(count, sizeof(JSValue));
    JSValue* eff_fns = (JSValue*)fleece_calloc(count, sizeof(JSValue));
    JSValue* exec_fns = (JSValue*)fleece_calloc(count, sizeof(JSValue));
    if (!pre_fns || !eff_fns || !exec_fns) {
        fleece_free(pre_fns);
        fleece_free(eff_fns);
        fleece_free(exec_fns);
        return -1;
    }

    for (uint32_t i = 0; i < count; i++) {
        pre_fns[i] = (defs[i].pre && defs[i].pre[0]) ? compile_goap_fn(ctx, defs[i].pre) : JS_UNDEFINED;
        eff_fns[i] = (defs[i].eff && defs[i].eff[0]) ? compile_goap_fn(ctx, defs[i].eff) : JS_UNDEFINED;
        exec_fns[i] = (defs[i].exec && defs[i].exec[0]) ? compile_goap_fn(ctx, defs[i].exec) : JS_UNDEFINED;
        if (JS_IsException(pre_fns[i])) { goap_js_dump_exception(ctx, "action pre"); pre_fns[i] = JS_UNDEFINED; }
        if (JS_IsException(eff_fns[i])) { goap_js_dump_exception(ctx, "action eff"); eff_fns[i] = JS_UNDEFINED; }
        if (JS_IsException(exec_fns[i])) { goap_js_dump_exception(ctx, "action exec"); exec_fns[i] = JS_UNDEFINED; }
    }

    b->action_pre_fns = pre_fns;
    b->action_eff_fns = eff_fns;
    b->action_exec_fns = exec_fns;
    b->action_count = count;
    return 0;
}

FleeceGoalPoolTransitionKind fleece_goap_brain_goal_pool_transition_kind(const FleeceGoapBrain* b) {
    return b ? b->goal_pool_transition_kind : FLEECE_GOAL_POOL_LOST;
}

const char* fleece_goap_brain_goal_pool_transition_key(const FleeceGoapBrain* b) {
    return (b && b->goal_pool_transition_key[0]) ? b->goal_pool_transition_key : NULL;
}

// Reads b->goal_pool_key_field (a JSON string field, quotes included, same
// convention every fleece string field uses) and strips the quotes -- the
// currently-held pool key, or an empty string if none. A byte-level strip
// rather than a full JS_ParseJSON round trip: the field's shape is fully
// controlled by this same module (only goal_pool_tick() ever writes it), so
// there is nothing here that needs a general JSON parser.
static void goal_pool_read_current(FleeceStateManager* sm, uint64_t self_id,
                                    const char* field, char* out, uint32_t out_cap) {
    out[0] = '\0';
    uint8_t* data = NULL;
    uint32_t size = 0;
    if (fleece_state_manager_get_named(sm, self_id, field, &data, &size) != 0 || !data) return;
    const uint8_t* start = data;
    uint32_t len = size;
    if (len >= 2 && start[0] == '"' && start[len - 1] == '"') {
        start++;
        len -= 2;
    }
    if (len >= out_cap) len = out_cap - 1;
    memcpy(out, start, len);
    out[len] = '\0';
    fleece_free(data);
}

// Reassertion cadence for an already-held goal-pool claim, in brain ticks.
// goal_pool_tick() runs every brain tick, but fleece_embedded_claim_best_
// goal()'s own doc comment explains why blindly reasserting that often is a
// convergence bug, not a fix: reasserting faster than a gossip round can
// carry a genuinely conflicting peer's claim back and forth means this
// node's own copy is always the "newer" one by the time the peer's write
// arrives, so merge_shared() never gets a fair single comparison and two
// nodes that each locally won a simultaneous claim race disagree forever.
// This must stay comfortably above the runtime's gossip send cadence
// (FLEECE_GOSSIP_EVERY_TICKS in fleece_runtime.c, default 10 ticks) so a
// competing claim gets at least one real gossip round-trip to land and be
// merged before this node touches its own copy again.
#define FLEECE_GOAL_POOL_REASSERT_EVERY_TICKS 30

static void goal_pool_fire_event(FleeceGoapBrain* b, FleeceGoalPoolTransitionKind kind, const char* key) {
    b->goal_pool_transition_kind = kind;
    strncpy(b->goal_pool_transition_key, key ? key : "", sizeof(b->goal_pool_transition_key) - 1);
    b->goal_pool_transition_key[sizeof(b->goal_pool_transition_key) - 1] = '\0';
    if (b->event_cb) b->event_cb(b, FLEECE_GOAP_BRAIN_EVENT_GOAL_POOL, b->event_ud);
}

// Runs a compiled precondition (or effect, called the same way) against bb,
// returning its boolean result. JS_UNDEFINED (no source registered) is
// trivially true -- matches js_action_pre's "no source: trivially true".
static bool action_call_bool(FleeceGoapBrain* b, JSValue fn, const FleeceGoapBlackboard* bb) {
    JSContext* ctx = (JSContext*)fleece_embedded_get_context(b->embedded);
    if (JS_IsUndefined(fn)) return true;
    JSValue bbjs = goap_bb_to_js(b->eval, bb);
    JSValue result = JS_Call(ctx, fn, JS_UNDEFINED, 1, (JSValueConst*)&bbjs);
    JS_FreeValue(ctx, bbjs);
    if (JS_IsException(result)) {
        goap_js_dump_exception(ctx, "action pre/eff");
        JS_FreeValue(ctx, result);
        return false;
    }
    bool holds = JS_ToBool(ctx, result) > 0;
    JS_FreeValue(ctx, result);
    return holds;
}

// Applies a compiled effect to a COPY of bb, producing dst (heuristic
// simulation only -- see fleece_goap_js.h's FleeceActionDef doc comment).
// Returns false if fn is JS_UNDEFINED (nothing to apply) or on error.
static bool action_call_eff(FleeceGoapBrain* b, JSValue fn, const FleeceGoapBlackboard* bb,
                            FleeceGoapBlackboard* dst) {
    JSContext* ctx = (JSContext*)fleece_embedded_get_context(b->embedded);
    if (JS_IsUndefined(fn)) return false;
    JSValue bbjs = goap_bb_to_js(b->eval, bb);
    JSValue result = JS_Call(ctx, fn, JS_UNDEFINED, 1, (JSValueConst*)&bbjs);
    JS_FreeValue(ctx, bbjs);
    if (JS_IsException(result) || !JS_IsObject(result)) {
        if (JS_IsException(result)) goap_js_dump_exception(ctx, "action eff");
        JS_FreeValue(ctx, result);
        return false;
    }
    int rc = goap_js_to_bb(b->eval, result, dst);
    JS_FreeValue(ctx, result);
    return rc == 0;
}

// Round-trips bb through the SAME JS marshaling pipeline action_call_eff
// uses (goap_bb_to_js -> ... -> goap_js_to_bb) without calling any action --
// this is the "identity effect". Needed as the baseline for
// build_self_diff_json's comparison: bb's own raw stored bytes (written by
// native C -- e.g. a float serialized via snprintf) are not guaranteed
// byte-identical to how QuickJS re-serializes the SAME numeric value after
// a JSON.parse/JSON.stringify round trip (different precision/formatting).
// Diffing a genuinely-simulated `sim` (which DID go through that pipeline)
// against raw `bb` bytes therefore flags every such field as "changed" even
// though nothing about it was touched -- diffing against this round-tripped
// baseline instead means both sides went through identical formatting, so
// only fields an eff actually modified show up as different. Found live:
// an untouched `position` field (lat/lon as JS doubles vs the C side's
// float32 formatting) was polluting every prerequisite goal's params with
// itself, producing embedded floating-point literals immediately followed
// by other JSON tokens with no separating comma -- corrupting the whole
// goal record's JSON just enough that JS_ParseJSON silently failed to
// parse it, so bb.world[key] read back as undefined and goalSatisfied
// could never see the prerequisite's own params at all.
static bool action_call_identity(FleeceGoapBrain* b, const FleeceGoapBlackboard* bb, FleeceGoapBlackboard* dst) {
    JSContext* ctx = (JSContext*)fleece_embedded_get_context(b->embedded);
    JSValue bbjs = goap_bb_to_js(b->eval, bb);
    int rc = goap_js_to_bb(b->eval, bbjs, dst);
    JS_FreeValue(ctx, bbjs);
    return rc == 0;
}

// Calls the mission-authored global `goalSatisfied(bb) -> bool`, looked up
// fresh each call (same convention js_score_goal uses for scoreGoal in
// fleece_embedded.c: one well-known JS global the loaded script defines,
// not cached, so a script reload always takes effect). Undefined/non-
// function goalSatisfied means "never satisfied" -- unlike
// action_call_bool's "no source: trivially true" default for pre/eff,
// treating a missing goalSatisfied as trivially-true would silently
// auto-complete every held goal the instant it's claimed.
static bool js_call_goal_satisfied(FleeceGoapBrain* b, const FleeceGoapBlackboard* bb) {
    JSContext* ctx = (JSContext*)fleece_embedded_get_context(b->embedded);
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue fn = JS_GetPropertyStr(ctx, global, "goalSatisfied");
    JS_FreeValue(ctx, global);
    if (!JS_IsFunction(ctx, fn)) {
        JS_FreeValue(ctx, fn);
        return false;
    }
    JSValue bbjs = goap_bb_to_js(b->eval, bb);
    JSValue result = JS_Call(ctx, fn, JS_UNDEFINED, 1, (JSValueConst*)&bbjs);
    JS_FreeValue(ctx, bbjs);
    JS_FreeValue(ctx, fn);
    if (JS_IsException(result)) {
        goap_js_dump_exception(ctx, "goalSatisfied");
        JS_FreeValue(ctx, result);
        return false;
    }
    bool holds = JS_ToBool(ctx, result) > 0;
    JS_FreeValue(ctx, result);
    return holds;
}

// Returns the index of the first registered action whose `pre` currently
// holds against bb, or UINT32_MAX. There is no name/verb lookup -- each
// action decides its own relevance by inspecting the held goal's params
// directly in its own `pre` body (see fleece_goap_js.h's action-library
// doc comment); this is just a linear scan over that.
static uint32_t action_library_find_applicable(FleeceGoapBrain* b, const FleeceGoapBlackboard* bb) {
    for (uint32_t i = 0; i < b->action_count; i++) {
        if (action_call_bool(b, b->action_pre_fns[i], bb)) return i;
    }
    return UINT32_MAX;
}

// Builds a JSON object of the bb.self fields that differ between orig and
// sim (the state a candidate action's `eff` predicts) -- the diff becomes
// a prerequisite goal's own `params`, since a prerequisite has nothing to
// name (goal params are desired STATE, never a command -- see
// fleece_goap_js.h). Only self (non-shared) fields are considered: a
// prerequisite describes what the acting node itself needs to become
// true, not the world. Field values are copied verbatim as raw JSON
// (already committed-shape JSON from the bb round trip), not re-encoded.
// Fields that would overflow out_cap are skipped rather than truncating
// mid-object -- a dropped field just means a smaller (still valid) diff.
static int build_self_diff_json(const FleeceGoapBlackboard* orig, const FleeceGoapBlackboard* sim,
                                 char* out, size_t out_cap) {
    if (out_cap < 3) return -1;
    size_t len = 0;
    out[len++] = '{';
    bool first = true;
    for (uint32_t i = 0; i < sim->count; i++) {
        const FleeceGoapField* f = &sim->fields[i];
        if (f->is_shared) continue;
        uint32_t osz = 0;
        const uint8_t* ov = fleece_goap_bb_get(orig, f->name, &osz);
        if (ov && osz == f->size && (f->size == 0 || memcmp(ov, f->data, f->size) == 0)) continue;

        size_t name_len = strlen(f->name);
        size_t need = name_len + (size_t)f->size + 6;  // quotes + colon + comma headroom
        if (len + need >= out_cap) continue;

        if (!first) out[len++] = ',';
        first = false;
        out[len++] = '"';
        memcpy(out + len, f->name, name_len);
        len += name_len;
        out[len++] = '"';
        out[len++] = ':';
        memcpy(out + len, f->data, f->size);
        len += f->size;
    }
    if (len + 1 >= out_cap) return -1;
    out[len++] = '}';
    out[len] = '\0';
    return 0;
}

// Authors a self-targeted, interrupt-scored prerequisite goal whose params
// are the bb.self state diff between orig and sim (what a candidate
// action's `eff` predicts becoming true) -- exactly the override-goal
// shape fleece_embedded_claim_best_goal() already treats as an
// unconditional win (required_agents excludes every other node; interrupt
// bypasses CONTEST_MARGIN). Idempotent: does nothing if one is already in
// flight, so repeated blocked ticks before it's claimed/completed don't
// re-stamp it. Key is "goal_pre_" + 8 hex digits (the low 32 bits of
// self_id -- collision odds negligible at any realistic swarm size, and
// required_agents below still carries the FULL id for the actual
// eligibility check, which isn't length-limited) -- no verb suffix, since
// only one prerequisite is ever in flight per node at a time and there is
// nothing to name a state diff after (this scheme replaced an earlier
// "goal_pre_<verb>_<hex>" one that could overflow FLEECE_FIELD_NAME_MAX;
// that trap no longer applies since there's no verb string here at all).
static void goal_pool_spawn_prerequisite(FleeceStateManager* sm, uint64_t self_id,
                                          const FleeceGoapBlackboard* orig, const FleeceGoapBlackboard* sim) {
    char key[FLEECE_FIELD_NAME_MAX];
    int n = snprintf(key, sizeof(key), "goal_pre_%08llx", (unsigned long long)(self_id & 0xFFFFFFFFULL));
    if (n < 0 || (size_t)n >= sizeof(key)) return;
    if (fleece_state_manager_exists_named(sm, FLEECE_SHARED_OWNER_ID, key)) return;

    char diff[256];
    if (build_self_diff_json(orig, sim, diff, sizeof(diff)) != 0) return;

    char value[384];
    n = snprintf(value, sizeof(value),
                 "{\"params\":%s,\"kind\":\"prereq\",\"priority\":100,"
                 "\"required_agents\":[\"%016llx\"],\"interrupt\":true}",
                 diff, (unsigned long long)self_id);
    if (n < 0 || (size_t)n >= sizeof(value)) return;
    fleece_state_manager_set_shared(sm, key, (const uint8_t*)value, (uint32_t)n);
}

// Runs after claiming (goal_pool_tick, below): given the held pool key,
// checks whether it's already satisfied, else runs whichever registered
// action currently applies, else repairs a missing precondition -- see
// fleece_goap_js.h's own extensive doc comment on the action-library
// dispatch contract. No-op if no action library is registered or nothing
// is held. `current` is goal_pool_tick's own claim result for this tick;
// this function may overwrite goal_pool_key_field again (done/blocked
// clear it) - the caller does not need to re-read it afterward.
static void action_library_tick(FleeceGoapBrain* b, const char* current) {
    // Claim/lost transition logging: mirrors the pre-native-action-library
    // design's "mine !== lastLogged" check, now done here since nothing
    // else observes currentGoalKey changing.
    if (strcmp(current, b->goal_pool_last_logged) != 0) {
        if (b->goal_pool_last_logged[0]) goal_pool_fire_event(b, FLEECE_GOAL_POOL_LOST, b->goal_pool_last_logged);
        if (current[0]) goal_pool_fire_event(b, FLEECE_GOAL_POOL_CLAIM, current);
        strncpy(b->goal_pool_last_logged, current, sizeof(b->goal_pool_last_logged) - 1);
        b->goal_pool_last_logged[sizeof(b->goal_pool_last_logged) - 1] = '\0';
    }

    if (!current[0] || b->action_count == 0) return;

    FleeceStateManager* sm = (FleeceStateManager*)fleece_embedded_get_state_manager(b->embedded);
    JSContext* ctx = (JSContext*)fleece_embedded_get_context(b->embedded);
    uint64_t self_id = fleece_state_manager_get_node_id(sm);

    char claim_key[FLEECE_FIELD_NAME_MAX + 8];
    snprintf(claim_key, sizeof(claim_key), "claim_%s", current);

    FleeceGoapBlackboard bb = {0};
    if (fleece_goap_js_bb_from_state(b->embedded, &bb) != 0) return;

    if (js_call_goal_satisfied(b, &bb)) {
        fleece_state_manager_remove_shared(sm, current);
        fleece_state_manager_remove_shared(sm, claim_key);
        fleece_state_manager_set_named(sm, b->goal_pool_key_field, (const uint8_t*)"\"\"", 2);
        strncpy(b->goal_pool_last_logged, "", sizeof(b->goal_pool_last_logged));
        goal_pool_fire_event(b, FLEECE_GOAL_POOL_DONE, current);
        fleece_goap_bb_release(&bb);
        return;
    }

    uint32_t idx = action_library_find_applicable(b, &bb);
    if (idx != UINT32_MAX) {
        JSValue fn = b->action_exec_fns[idx];
        if (!JS_IsUndefined(fn)) {
            JSValue bbjs = goap_bb_to_js(b->eval, &bb);
            JSValue tickv = JS_NewInt32(ctx, (int32_t)b->tick_count);
            JSValue argv[2] = {bbjs, tickv};
            JSValue result = JS_Call(ctx, fn, JS_UNDEFINED, 2, (JSValueConst*)argv);
            JS_FreeValue(ctx, tickv);
            if (JS_IsException(result)) {
                goap_js_dump_exception(ctx, "action exec");
                JS_FreeValue(ctx, result);
                JS_FreeValue(ctx, bbjs);
            } else {
                // exec's own return value is a normal done/not-done report
                // for its own work, same shape a GOAP action's exec
                // reports -- but nothing here consumes it for cleanup.
                // Only goalSatisfied, checked at the top of the NEXT tick,
                // decides whether the held goal itself is done (see
                // fleece_goap_js.h's action-library doc comment).
                JSValue extract = JS_IsObject(result) ? result : bbjs;
                FleeceGoapBlackboard committed = {0};
                if (goap_js_to_bb(b->eval, extract, &committed) == 0) {
                    fleece_goap_js_bb_commit(b->embedded, &committed);
                    fleece_goap_bb_release(&committed);
                }
                JS_FreeValue(ctx, result);
                JS_FreeValue(ctx, bbjs);
            }
        }
        fleece_goap_bb_release(&bb);
        return;
    }

    // Nothing currently applicable: one-ply lookahead for a provider (see
    // fleece_goap_js.h's action-library doc comment) -- simulate each
    // OTHER action's `eff` and check whether goalSatisfied or some
    // action's `pre` would then hold. The diff is computed against
    // rt_orig (bb round-tripped through the identical JS marshaling
    // pipeline `sim` itself went through), not raw `bb` -- see
    // action_call_identity's own comment for why comparing against raw
    // bytes produces spurious diffs.
    bool spawned = false;
    FleeceGoapBlackboard rt_orig = {0};
    bool have_rt_orig = action_call_identity(b, &bb, &rt_orig);
    for (uint32_t c = 0; c < b->action_count && !spawned; c++) {
        if (JS_IsUndefined(b->action_eff_fns[c])) continue;
        FleeceGoapBlackboard sim = {0};
        if (!action_call_eff(b, b->action_eff_fns[c], &bb, &sim)) continue;
        bool now_ok = js_call_goal_satisfied(b, &sim) || action_library_find_applicable(b, &sim) != UINT32_MAX;
        if (now_ok && have_rt_orig) {
            goal_pool_spawn_prerequisite(sm, self_id, &rt_orig, &sim);
            spawned = true;
        }
        fleece_goap_bb_release(&sim);
    }
    if (have_rt_orig) fleece_goap_bb_release(&rt_orig);

    if (spawned) {
        fleece_state_manager_remove_shared(sm, claim_key);
        fleece_state_manager_set_named(sm, b->goal_pool_key_field, (const uint8_t*)"\"\"", 2);
        strncpy(b->goal_pool_last_logged, "", sizeof(b->goal_pool_last_logged));
        goal_pool_fire_event(b, FLEECE_GOAL_POOL_BLOCKED, current);
    }
    // No provider found: leave the claim held, retry next tick (matches a
    // GOAP action's own "precondition unmet, wait" behavior).
    fleece_goap_bb_release(&bb);
}

// Runs one goal-pool auction tick when configured (fleece_goap_brain_set_
// goal_pool): claims/holds the best-scoring eligible pool entry via
// fleece_embedded_claim_best_goal() and publishes the result into
// goal_pool_key_field as a JSON string self field, so an authored exec/pre/
// goal source can read it off bb.self exactly like any other field. Called
// at the very top of fleece_goap_brain_tick(), before exec/replan, so the
// SAME tick's exec body already sees an up-to-date claim.
static void goal_pool_tick(FleeceGoapBrain* b) {
    if (!b->goal_pool_prefix[0]) return;
    FleeceStateManager* sm = (FleeceStateManager*)fleece_embedded_get_state_manager(b->embedded);
    if (!sm) return;
    uint64_t self_id = fleece_state_manager_get_node_id(sm);

    char current[FLEECE_FIELD_NAME_MAX];
    goal_pool_read_current(sm, self_id, b->goal_pool_key_field, current, sizeof(current));

    bool allow_reassert = (b->tick_count % FLEECE_GOAL_POOL_REASSERT_EVERY_TICKS) == 0;

    char claimed[FLEECE_FIELD_NAME_MAX];
    claimed[0] = '\0';
    fleece_embedded_claim_best_goal(b->embedded, b->goal_pool_prefix,
                                     current[0] ? current : NULL, b->goal_pool_contest_margin,
                                     allow_reassert,
                                     claimed, sizeof(claimed));

    char json_val[FLEECE_FIELD_NAME_MAX + 4];
    int written = snprintf(json_val, sizeof(json_val), "\"%s\"", claimed);
    if (written > 0 && (size_t)written < sizeof(json_val)) {
        fleece_state_manager_set_named(sm, b->goal_pool_key_field, (const uint8_t*)json_val, (uint32_t)written);
    }

    // Action-library dispatch (no-op if none registered) may itself
    // overwrite goal_pool_key_field again (blocked/done both clear it) - it
    // re-reads nothing from what was just written above, `claimed` is
    // passed directly.
    action_library_tick(b, claimed);
}

const char* fleece_goap_brain_goal_id(const FleeceGoapBrain* b) {
    return (b && b->goal_idx >= 0) ? fleece_goap_goal_id(b->goap, (uint32_t)b->goal_idx) : NULL;
}

const char* fleece_goap_brain_action_id(const FleeceGoapBrain* b) {
    return (b && b->action_idx >= 0) ? fleece_goap_action_id(b->goap, (uint32_t)b->action_idx) : NULL;
}

uint32_t fleece_goap_brain_action_progress(const FleeceGoapBrain* b) {
    return b ? b->exec_progress : 0;
}

uint32_t fleece_goap_brain_plan_length(const FleeceGoapBrain* b) {
    return (b && b->plan) ? fleece_goap_plan_length(b->plan) : 0;
}

uint32_t fleece_goap_brain_tick_count(const FleeceGoapBrain* b) {
    return b ? b->tick_count : 0;
}
