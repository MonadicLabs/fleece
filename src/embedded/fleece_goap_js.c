#define _POSIX_C_SOURCE 200809L

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "quickjs.h"

#include "fleece_embedded.h"
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
    char* buf = (char*)malloc(alloc + 1);
    if (!buf) return JS_EXCEPTION;
    buf[0] = '(';
    memcpy(buf + 1, src, slen);
    buf[slen + 1] = ')';
    buf[slen + 2] = '\0';

    JSValue result = JS_Eval(ctx, buf, alloc, "<goap fn>", JS_EVAL_TYPE_GLOBAL);
    free(buf);
    return result;
}

static void goap_fn_entry_free(FleeceGoapJsEval* je, GoapFnEntry* e) {
    JS_FreeValue((JSContext*)fleece_embedded_get_context(je->embedded), e->fn);
    free(e->src);
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
        char* scratch = (char*)malloc((size_t)f->size + 1);
        if (!scratch) continue;
        memcpy(scratch, f->data, f->size);
        scratch[f->size] = '\0';
        JSValue v = JS_ParseJSON(ctx, scratch, f->size, "<bb>");
        free(scratch);
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
    FleeceGoapJsEval* je = (FleeceGoapJsEval*)calloc(1, sizeof(FleeceGoapJsEval));
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
    je->entries = (GoapFnEntry*)calloc(cap, sizeof(GoapFnEntry));
    if (!je->entries) {
        free(je);
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
            free(je->entries[i].src);
        }
    }
    free(je->entries);
    free(je);
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
                free(data);
                return -1;
            }
            free(data);
        }
    }

    // world namespace (shared fields)
    n = fleece_state_manager_list_fields(sm, FLEECE_SHARED_OWNER_ID, names, 64);
    for (uint32_t i = 0; i < n; i++) {
        uint8_t* data = NULL;
        uint32_t size = 0;
        if (fleece_state_manager_get_named(sm, FLEECE_SHARED_OWNER_ID, names[i], &data, &size) == 0 && data) {
            if (fleece_goap_bb_set(bb, names[i], data, size, true) != 0) {
                free(data);
                return -1;
            }
            free(data);
        }
    }
    return 0;
}

int fleece_goap_js_bb_commit(FleeceEmbedded* embedded, const FleeceGoapBlackboard* bb) {
    if (!embedded || !bb) return -1;
    FleeceStateManager* sm = (FleeceStateManager*)fleece_embedded_get_state_manager(embedded);
    if (!sm) return -1;

    for (uint32_t i = 0; i < bb->count; i++) {
        const FleeceGoapField* f = &bb->fields[i];
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
    uint32_t tick_count;
    FleeceGoapBrainEventFn event_cb;
    void* event_ud;
    FleeceGoapWorldModelFn world_model_cb;  // host telemetry injection (see header)
    void* world_model_ud;
};

FleeceGoapBrain* fleece_goap_brain_create(FleeceEmbedded* embedded, FleeceGoap* goap) {
    if (!embedded || !goap) return NULL;
    FleeceGoapBrain* b = (FleeceGoapBrain*)calloc(1, sizeof(FleeceGoapBrain));
    if (!b) return NULL;
    b->eval = fleece_goap_js_eval_create(embedded, goap);
    if (!b->eval) {
        free(b);
        return NULL;
    }
    b->embedded = embedded;
    b->goap = goap;
    b->goal_idx = -1;
    b->action_idx = -1;
    return b;
}

void fleece_goap_brain_destroy(FleeceGoapBrain* b) {
    if (!b) return;
    if (b->plan) fleece_goap_plan_destroy(b->plan);
    fleece_goap_js_eval_destroy(b->eval);
    free(b);
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

    uint32_t* ranked = (uint32_t*)malloc(cap * sizeof(uint32_t));
    if (!ranked) {
        if (b->event_cb) b->event_cb(b, FLEECE_GOAP_BRAIN_EVENT_IDLE, b->event_ud);
        return;
    }
    uint32_t n = fleece_goap_rank_goals(b->goap, bb, &b->eval->eval, 0.0, ranked, cap);

    const char* goal_ids[1];
    for (uint32_t i = 0; i < n; i++) {
        uint32_t goal = ranked[i];
        goal_ids[0] = fleece_goap_goal_id(b->goap, goal);
        FleeceGoapPlan* plan = fleece_goap_plan(b->goap, bb, goal_ids, 1, &b->eval->eval);
        if (!plan || fleece_goap_plan_length(plan) == 0) {
            if (plan) fleece_goap_plan_destroy(plan);
            continue;  // can't plan this goal right now - try the next best
        }

        b->goal_idx = (int)goal;
        const char* aid = fleece_goap_plan_action_id(plan, 0);
        b->action_idx = (int)fleece_goap_find_action(b->goap, aid);
        b->exec_progress = 0;
        b->plan = plan;
        if (b->event_cb) b->event_cb(b, FLEECE_GOAP_BRAIN_EVENT_REPLAN, b->event_ud);
        free(ranked);
        return;
    }

    free(ranked);
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

int fleece_goap_brain_tick(FleeceGoapBrain* b) {
    if (!b) return -1;
    b->tick_count++;

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
                fleece_goap_bb_release(&bb);
                if (rc != 0) {
                    fleece_goap_bb_release(&dst);
                    b->action_idx = -1;
                    return 0;  // exec failed - next tick replans
                }
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

void fleece_goap_brain_set_max_action_ticks(FleeceGoapBrain* b, uint32_t max_ticks) {
    if (!b) return;
    b->max_action_ticks = max_ticks;
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
