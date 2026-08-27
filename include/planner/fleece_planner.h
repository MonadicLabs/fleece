// Fleece GOAP Planner
// A* planner over a "blackboard" of named fields holding JSON values - the same
// value space as the fleece state manager. The engine is deliberately
// engine-agnostic: preconditions, effects, goals, and costs are evaluated by
// host-supplied callbacks (in fleece, QuickJS functions), so the planner itself
// knows nothing about JavaScript. It handles the search (visited-state pruning,
// cost, heuristic), utility-curve goal selection, and serializing the
// action/goal/utility tables to/from a compact CBOR "plan blob" for offline
// authoring -> on-device deployment.
//
// Blackboard contract: the object passed to precondition/effect/goal functions
// bundles the fleece script globals - bb.self (local node fields), bb.world
// (shared fields), bb.platform (hardware), bb.swarm (peers) - mirroring the
// self/world/platform/swarm paradigm fleece scripts already use. The planner's
// C-side blackboard is a flat field store where each field records which
// namespace it belongs to (is_shared), so the executor can commit `self` and
// `world` writes to the right store.
//
// Effect contract: an action's effect function receives a COPY of the current
// blackboard, mutates it (typically bb.self/bb.world), and RETURNS the
// resulting blackboard - so the planner knows exactly what the action produces
// without the host touching live state. See fleece_goap_plan()'s
// FleeceGoapEval.action_apply.

#ifndef FLEECE_PLANNER_H
#define FLEECE_PLANNER_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// 64, matching state/fleece_state_manager.h's FLEECE_FIELD_NAME_MAX -- a
// FleeceGoapBlackboard field's own name (FleeceGoapField.name, below) is
// always derived from a state-manager field name (fleece_goap_bb_set is fed
// straight from fleece_state_manager_list_fields()'s own output, both by
// fleece_goap_js_bb_from_state's per-tick snapshot and by every commit back
// afterward), so capping it any tighter than the state manager's own budget
// just reintroduces the exact same silent-overflow trap at one layer up.
// Found live: main.c's own per-peer telemetry mirroring
// (publish_world_telemetry, "<16-hex-node-id>.<field>") produces
// "<id>.battery_percent" at exactly 32 bytes -- one over the old 31-byte
// cap -- which made fleece_goap_js_bb_from_state fail outright for the
// WHOLE blackboard snapshot the instant a second peer's telemetry entered
// world state, silently freezing all goal-pool dispatch (claim/blocked/
// done, precondition repair) even though claiming itself kept working
// (it never goes through this blackboard at all). Also used for action/
// goal/unit/curve ids/names/dims in the real (non-goal-pool) GOAP planner
// -- same reasoning applies there, and the memory cost is the same
// deliberately-accepted order of magnitude as the state-manager bump.
#define FLEECE_GOAP_NAME_MAX 64      // ids / names / dims / curve refs
#define FLEECE_GOAP_SOURCE_MAX 2048   // pre/eff/goal/cost/exec JS function sources
#define FLEECE_GOAP_NOTE_MAX 128     // mission notes

// Search-budget defaults (see fleece_goap_set_max_*). Bounding these is what
// keeps planning from ever starving a real-time MCU loop, however pathological
// the action library gets.
#define FLEECE_GOAP_DEFAULT_MAX_ITERS 2048     // A* pop-and-goal-test iterations
#define FLEECE_GOAP_DEFAULT_MAX_NODES 512      // frontier/visited state nodes
#define FLEECE_GOAP_DEFAULT_MAX_PLAN_DEPTH 8   // max actions in a synthesized plan

typedef struct FleeceGoap FleeceGoap;
typedef struct FleeceGoapPlan FleeceGoapPlan;

// --- Blackboard ----------------------------------------------------------

// One named field. `data` holds JSON-encoded bytes (e.g. "3", "true",
// "\"base\"", "[1,2]") and is owned by the blackboard. `is_shared` records the
// field's namespace: false = bb.self (local node), true = bb.world (shared).
typedef struct FleeceGoapField {
    char name[FLEECE_GOAP_NAME_MAX];
    uint8_t* data;
    uint32_t size;
    bool is_shared;
} FleeceGoapField;

// An ordered array of named fields. Owned by the planner during a search and
// by the host otherwise (build one from self/world, let the planner clone it).
typedef struct FleeceGoapBlackboard {
    FleeceGoapField* fields;
    uint32_t count;
    uint32_t cap;
} FleeceGoapBlackboard;

// Read a field's JSON bytes (borrowed pointer - valid until the blackboard is
// mutated). Looks up by name across both namespaces, self first. Returns NULL
// (and 0 in *size) if the field is absent.
const uint8_t* fleece_goap_bb_get(const FleeceGoapBlackboard* bb, const char* name, uint32_t* size);

// Index of a field, or UINT32_MAX if absent.
uint32_t fleece_goap_bb_index(const FleeceGoapBlackboard* bb, const char* name);

// Set/replace a field. data must be non-NULL with size > 0 (JSON bytes).
// is_shared selects the bb.world namespace (true) vs bb.self (false).
int fleece_goap_bb_set(FleeceGoapBlackboard* bb, const char* name, const uint8_t* data, uint32_t size, bool is_shared);

// Remove a field (no-op if absent).
int fleece_goap_bb_delete(FleeceGoapBlackboard* bb, const char* name);

// Deep-clone src into dst (dst must be released with fleece_goap_bb_release).
int fleece_goap_bb_clone(const FleeceGoapBlackboard* src, FleeceGoapBlackboard* dst);

// Free a blackboard's owned storage and zero it.
void fleece_goap_bb_release(FleeceGoapBlackboard* bb);

// --- Plan definition tables ----------------------------------------------

typedef struct FleeceGoapPoint { double x, y; } FleeceGoapPoint;

// An action. pre/eff/cost are JS FUNCTION SOURCES treated as opaque strings by
// the planner; the host compiles them to callables. `cost` empty means the
// default cost of 1. `dest`/`dur` are executor hints (""/0 when unset).
//
// Contract between the planner and the executor:
//   - pre  : conditions the planner (and executor) check before the action.
//   - eff  : the action's DECLARED outcome - used ONLY as a heuristic so the
//            planner can simulate the future during A*. The executor never
//            applies it.
//   - exec : the action's actual body - the JS function the executor calls
//            every tick while the action runs. It performs the real work
//            (drive hardware, move, consume, ...), mutating its `bb` copy; it
//            must return true when the action is finished. Optional: an action
//            with no exec has no runtime body - the executor completes it at
//            once and changes nothing. `dur` is deprecated as a timing hint.
typedef struct FleeceGoapActionDef {
    const char* id;
    const char* name;
    const char* cost;
    const char* dest;
    double dur;
    const char* exec;              // action body source (may be NULL): exec(bb, tick) -> done
    const char** pre;              // array of precondition sources (may be NULL)
    uint32_t pre_count;
    const char** eff;              // array of effect function sources (typically one)
    uint32_t eff_count;
} FleeceGoapActionDef;

typedef struct FleeceGoapGoalDef {
    const char* id;
    const char* name;
    const char* expr;              // JS function source -> bool
    double priority;
    const char* curve_id;          // utility curve used for selection ("" = none)
} FleeceGoapGoalDef;

typedef struct FleeceGoapUtilityDef {
    const char* id;
    const char* name;
    const char* dim;               // blackboard field to score ("" = constant)
    double x_min;
    double x_max;
    const FleeceGoapPoint* points; // piecewise-linear curve, sorted by x
    uint32_t point_count;
} FleeceGoapUtilityDef;

typedef struct FleeceGoapMissionDef {
    const char* id;
    const char* name;
    const char** goal_ids;         // goal ids this mission pursues (may be NULL)
    uint32_t goal_count;
    const char* note;
} FleeceGoapMissionDef;

// --- Lifecycle / registration --------------------------------------------

FleeceGoap* fleece_goap_create(void);
void fleece_goap_destroy(FleeceGoap* goap);

// Remove all tables (actions/goals/utilities/missions/name).
void fleece_goap_reset(FleeceGoap* goap);

const char* fleece_goap_name(const FleeceGoap* goap);
int fleece_goap_set_name(FleeceGoap* goap, const char* name);

// The planner copies all provided strings. Returns 0 on success, -1 on
// allocation failure or invalid arguments.
int fleece_goap_add_action(FleeceGoap* goap, const FleeceGoapActionDef* def);
int fleece_goap_add_goal(FleeceGoap* goap, const FleeceGoapGoalDef* def);
int fleece_goap_add_utility(FleeceGoap* goap, const FleeceGoapUtilityDef* def);
int fleece_goap_add_mission(FleeceGoap* goap, const FleeceGoapMissionDef* def);

// Search budget knobs (defaults: 2048 iterations, 512 nodes, 8 plan depth).
// Replanning is cheap for typical action libraries; these bound worst-case MCU
// CPU/RAM - and a depth cap stops the planner from synthesizing silly long
// detour plans (e.g. reaching 'recharge' via a whole forage cycle) when a
// shorter path exists.
void fleece_goap_set_max_iters(FleeceGoap* goap, uint32_t max_iters);
void fleece_goap_set_max_nodes(FleeceGoap* goap, uint32_t max_nodes);
void fleece_goap_set_max_plan_depth(FleeceGoap* goap, uint32_t max_depth);

// Approximate long-lived footprint of the action/goal/utility/mission tables in
// bytes (fixed records + allocated strings + curve points), for sizing static
// pools on MCU targets. Does not include the transient A* search buffers.
uint32_t fleece_goap_get_mem_usage(const FleeceGoap* goap);

// --- Evaluation callbacks ------------------------------------------------

// Host-implemented evaluation. All callbacks return 0 on success; a nonzero
// return from action_pre/goal_satisfied/action_cost treats the value as false
// /default; a nonzero return from action_apply skips the action.
typedef struct FleeceGoapEval {
    void* user_data;

    // Do action_idx's preconditions hold on bb? (no preconditions => *out = true)
    int (*action_pre)(void* user_data, uint32_t action_idx, const FleeceGoapBlackboard* bb, bool* out);

    // Apply action_idx's effects: dst is an empty blackboard the host fills
    // from the returned blackboard of the effect function (applied to a copy
    // of src). Returns 0 if the action can be applied.
    int (*action_apply)(void* user_data, uint32_t action_idx, const FleeceGoapBlackboard* src, FleeceGoapBlackboard* dst);

    // Is goal_idx satisfied on bb?
    int (*goal_satisfied)(void* user_data, uint32_t goal_idx, const FleeceGoapBlackboard* bb, bool* out);

    // Numeric cost of action_idx on bb. May be NULL; the planner then uses the
    // action's own cost source or the default 1.
    int (*action_cost)(void* user_data, uint32_t action_idx, const FleeceGoapBlackboard* bb, double* out);
} FleeceGoapEval;

// --- Planning ------------------------------------------------------------

// Runs A* from `start` until every goal in goal_ids is satisfied
// (goal_ids == NULL or n_goals == 0 targets all registered goals). Returns a
// plan (caller frees with fleece_goap_plan_destroy) or NULL if no plan exists
// within the search budget.
FleeceGoapPlan* fleece_goap_plan(FleeceGoap* goap, const FleeceGoapBlackboard* start,
                                 const char** goal_ids, uint32_t n_goals, const FleeceGoapEval* eval);

uint32_t fleece_goap_plan_length(const FleeceGoapPlan* plan);
const char* fleece_goap_plan_action_id(const FleeceGoapPlan* plan, uint32_t i);
double fleece_goap_plan_cost(const FleeceGoapPlan* plan);
uint32_t fleece_goap_plan_iters(const FleeceGoapPlan* plan);
void fleece_goap_plan_destroy(FleeceGoapPlan* plan);

// --- Goal selection / introspection ---------------------------------------

// Highest-utility goal that is not yet satisfied and scores above `threshold`,
// or -1 if none. Requires eval->goal_satisfied.
int fleece_goap_select_goal(FleeceGoap* goap, const FleeceGoapBlackboard* bb, const FleeceGoapEval* eval, double threshold);

// Rank unsatisfied goals by utility, highest first, into out[] (capped at
// cap), skipping goals at or below `threshold`. Returns the number written.
// Unlike select_goal this doesn't stop at the single best goal, so callers
// that need a fallback when a top goal can't currently be planned can walk
// the list in order.
uint32_t fleece_goap_rank_goals(FleeceGoap* goap, const FleeceGoapBlackboard* bb,
                                const FleeceGoapEval* eval, double threshold,
                                uint32_t* out, uint32_t cap);

// A goal's selection score on bb: utility curve over its `dim` field (or the
// curve's constant value) times the goal's priority. 0 if the goal/curve is
// unknown or the dim value is not a number.
double fleece_goap_goal_utility(const FleeceGoap* goap, uint32_t goal_idx, const FleeceGoapBlackboard* bb);

// Convenience: goal satisfaction via eval (returns 0 on success).
int fleece_goap_goal_satisfied(const FleeceGoap* goap, uint32_t goal_idx, const FleeceGoapBlackboard* bb, const FleeceGoapEval* eval, bool* out);

// Table introspection (used by the JS layer and tests).
uint32_t fleece_goap_action_count(const FleeceGoap* goap);
const char* fleece_goap_action_id(const FleeceGoap* goap, uint32_t idx);
const char* fleece_goap_action_name(const FleeceGoap* goap, uint32_t idx);
uint32_t fleece_goap_action_pre_count(const FleeceGoap* goap, uint32_t idx);
const char* fleece_goap_action_pre(const FleeceGoap* goap, uint32_t idx, uint32_t i);
uint32_t fleece_goap_action_eff_count(const FleeceGoap* goap, uint32_t idx);
const char* fleece_goap_action_eff(const FleeceGoap* goap, uint32_t idx, uint32_t i);
const char* fleece_goap_action_cost_source(const FleeceGoap* goap, uint32_t idx);
const char* fleece_goap_action_exec(const FleeceGoap* goap, uint32_t idx);
const char* fleece_goap_action_dest(const FleeceGoap* goap, uint32_t idx);
double fleece_goap_action_dur(const FleeceGoap* goap, uint32_t idx);
uint32_t fleece_goap_goal_count(const FleeceGoap* goap);
const char* fleece_goap_goal_id(const FleeceGoap* goap, uint32_t idx);
const char* fleece_goap_goal_name(const FleeceGoap* goap, uint32_t idx);
const char* fleece_goap_goal_expr(const FleeceGoap* goap, uint32_t idx);
double fleece_goap_goal_priority(const FleeceGoap* goap, uint32_t idx);
const char* fleece_goap_goal_curve_id(const FleeceGoap* goap, uint32_t idx);
uint32_t fleece_goap_find_action(const FleeceGoap* goap, const char* id);
uint32_t fleece_goap_find_goal(const FleeceGoap* goap, const char* id);

// Utility curves (piecewise-linear, referenced by goals via curve_id).
uint32_t fleece_goap_utility_count(const FleeceGoap* goap);
const char* fleece_goap_utility_id(const FleeceGoap* goap, uint32_t idx);
const char* fleece_goap_utility_name(const FleeceGoap* goap, uint32_t idx);
const char* fleece_goap_utility_dim(const FleeceGoap* goap, uint32_t idx);
double fleece_goap_utility_x_min(const FleeceGoap* goap, uint32_t idx);
double fleece_goap_utility_x_max(const FleeceGoap* goap, uint32_t idx);
uint32_t fleece_goap_utility_point_count(const FleeceGoap* goap, uint32_t idx);
// Out-of-range idx/i returns {0, 0}; there is no other sentinel value for a
// point, so callers must range-check via fleece_goap_utility_count/point_count.
FleeceGoapPoint fleece_goap_utility_point(const FleeceGoap* goap, uint32_t idx, uint32_t i);
uint32_t fleece_goap_find_utility(const FleeceGoap* goap, const char* id);

uint32_t fleece_goap_mission_count(const FleeceGoap* goap);
const char* fleece_goap_mission_id(const FleeceGoap* goap, uint32_t idx);
const char* fleece_goap_mission_name(const FleeceGoap* goap, uint32_t idx);
uint32_t fleece_goap_mission_goal_count(const FleeceGoap* goap, uint32_t idx);
const char* fleece_goap_mission_goal_id(const FleeceGoap* goap, uint32_t idx, uint32_t i);
const char* fleece_goap_mission_note(const FleeceGoap* goap, uint32_t idx);

// --- Serialization (plan blob) -------------------------------------------

// Compile the current tables into a CBOR plan blob (3-byte 'F','P',version
// prefix + CBOR body). Caller frees *blob. Returns 0 on success.
int fleece_goap_serialize(const FleeceGoap* goap, uint8_t** blob, uint32_t* blob_size);

// Replace the current tables by parsing a plan blob. On failure the tables may
// be partially populated - call fleece_goap_reset() and try again.
int fleece_goap_deserialize(FleeceGoap* goap, const uint8_t* blob, uint32_t blob_size);

#ifdef __cplusplus
}
#endif

#endif // FLEECE_PLANNER_H
