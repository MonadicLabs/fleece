// Fleece GOAP <-> QuickJS evaluation bridge.
//
// The GOAP planner (see planner/fleece_planner.h) is engine-agnostic: action
// preconditions, effects, goal expressions, and costs are stored as opaque JS
// FUNCTION SOURCES and evaluated by host-supplied callbacks. In fleece the
// host is QuickJS, and those callbacks live here.
//
// Script JS is used ONLY to author the functions themselves (pre/eff/goal/
// cost). Planning, goal selection, and execution orchestration stay in C and
// are driven through this module's C API.
//
// The `bb` object handed to authored functions mirrors the fleece script
// globals - bb.self (local node fields), bb.world (shared fields),
// bb.platform (hardware), bb.swarm (peers). self/world are detached copies the
// functions may mutate; an effect function mutates its copy and RETURNS it
// (the effect contract in fleece_planner.h). platform/swarm are the live
// read-only script globals.

#ifndef FLEECE_GOAP_JS_H
#define FLEECE_GOAP_JS_H

#include <stdint.h>
#include <stdbool.h>

#include "planner/fleece_planner.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FleeceEmbedded FleeceEmbedded;
typedef struct FleeceGoapJsEval FleeceGoapJsEval;
typedef struct FleeceGoapBrain FleeceGoapBrain;

// --- Evaluation ----------------------------------------------------------

// Create an evaluation context that compiles the goap tables' JS function
// sources on demand (with a small per-eval cache) and runs them against
// `embedded`'s QuickJS context. Call before any plan/select/apply call; the
// returned struct stays valid until fleece_goap_js_eval_destroy.
FleeceGoapJsEval* fleece_goap_js_eval_create(FleeceEmbedded* embedded, FleeceGoap* goap);
void fleece_goap_js_eval_destroy(FleeceGoapJsEval* je);

// The FleeceGoapEval wired to this evaluation context, for use with
// fleece_goap_plan / fleece_goap_select_goal directly.
const FleeceGoapEval* fleece_goap_js_eval_get(const FleeceGoapJsEval* je);

// --- Convenience helpers -------------------------------------------------

// Snapshot the live state manager into a blackboard (self fields + world
// fields). Caller releases the blackboard. Used to build the planner's start
// state from the current self/world.
int fleece_goap_js_bb_from_state(FleeceEmbedded* embedded, FleeceGoapBlackboard* bb);

// Commit a blackboard's fields to the live state manager: self-namespace
// fields are written as local fields, world-namespace fields as shared
// fields. Used by the executor after applying an action for real.
int fleece_goap_js_bb_commit(FleeceEmbedded* embedded, const FleeceGoapBlackboard* bb);

// Run A* with QuickJS-evaluated preconditions/effects/goals/costs. goal_ids
// may be NULL or empty to target all registered goals. Returns a plan (freed
// with fleece_goap_plan_destroy) or NULL.
FleeceGoapPlan* fleece_goap_js_plan(FleeceEmbedded* embedded, FleeceGoap* goap,
                                    const FleeceGoapBlackboard* start,
                                    const char** goal_ids, uint32_t n_goals);

// Highest-utility unsatisfied goal (QuickJS-evaluated goal expressions), or -1.
int fleece_goap_js_select_goal(FleeceEmbedded* embedded, FleeceGoap* goap,
                               const FleeceGoapBlackboard* bb, double threshold);

// Apply action_idx's effects to a copy of src, producing dst (caller releases
// dst). Mirrors the eval's action_apply callback - the executor uses this to
// apply a plan step to a live blackboard.
int fleece_goap_js_apply_action(FleeceEmbedded* embedded, FleeceGoap* goap,
                                uint32_t action_idx, const FleeceGoapBlackboard* src,
                                FleeceGoapBlackboard* dst);

// --- Behavior-loop driver ("brain") --------------------------------------
//
// A C-side GOAP executor that drives the unit through the full loop:
//
//   plan -> select action -> execute action -> when done, re-execute planning
//   (select goal, replan, repeat).
//
// Each tick it snapshots the live state manager (fleece_goap_js_bb_from_state),
// picks the highest-utility unsatisfied goal, plans for it, and executes the
// plan's FIRST action. Execution means running the action's exec() body
// (fleece_goap_action_exec) once per tick - the body performs the real work,
// mutating its `bb` copy, which the brain commits each tick so the world sees
// progress as it happens - until the body returns true. An action with no exec
// has no runtime body: it completes at once and changes nothing (its effect is
// only a planner heuristic and is never applied). After each action the brain
// replans. This matches the "re-execute planning when an action is done"
// pattern: the plan is a one-step-ahead guide, so the brain adapts to world
// changes between steps. Script step() (or a C tick callback) still runs each
// tick (sensors/hardware); the brain owns decisions and applies state.

typedef enum {
    FLEECE_GOAP_BRAIN_EVENT_REPLAN,        // new goal/action selected (see getters)
    FLEECE_GOAP_BRAIN_EVENT_ACTION_DONE,   // an action's effect was applied+committed
    FLEECE_GOAP_BRAIN_EVENT_ABORTED,       // an action exceeded max_action_ticks and was dropped
    FLEECE_GOAP_BRAIN_EVENT_IDLE           // no unsatisfied goal, or none currently plannable
} FleeceGoapBrainEvent;

// Called after a tick's decisions. brain is read-only during the callback;
// strings returned by the getters are valid only until the next tick.
typedef void (*FleeceGoapBrainEventFn)(FleeceGoapBrain* brain, FleeceGoapBrainEvent event, void* user_data);

// Host-supplied world-model hook: called by the brain once per tick, right
// after the live state manager is snapshotted into `bb` (in both the exec path
// and the plan/select path), so every authorized pre/goal/eff/exec sees fresh
// telemetry. Inject live sensor state into the `self` namespace with
// fleece_goap_bb_set(bb, name, data, size, false) - it commits and gossips like
// any other self field. Typical use: a PX4/MAVLink (or mock) telemetry thread
// writes a small ring buffer; this callback copies the latest sample in.
typedef void (*FleeceGoapWorldModelFn)(FleeceGoapBrain* brain, FleeceGoapBlackboard* bb, uint32_t tick, void* user_data);

// --- Divergence diagnostics ------------------------------------------------
//
// An action's `eff` is a planner heuristic - A* uses it to simulate the future,
// the executor never applies it - while `exec` is the real body. When the two
// disagree (an eff claiming a goal becomes reachable though the body never
// delivers, a field the body sets that eff forgot, ...), planning can chase a
// phantom state forever. These diagnostics surface exactly that, per field.

// One field where the declared effect diverges from what the body produced.
// Byte pointers are borrowed from the blackboards and valid only during the
// callback. A field predicted but absent from the actual output has
// in_actual=false; one produced without being predicted has in_eff=false.
typedef struct FleeceGoapDivergence {
    const char* name;
    bool in_eff;
    bool in_actual;
    const uint8_t* eff_value;
    uint32_t eff_size;
    const uint8_t* actual_value;
    uint32_t actual_size;
} FleeceGoapDivergence;

typedef void (*FleeceGoapDivergenceFn)(FleeceGoapBrain* brain, uint32_t action_idx,
                                       const FleeceGoapDivergence* diffs, uint32_t n_diffs,
                                       void* user_data);

// Create a brain driving `goap` against `embedded`'s state manager. Caller
// keeps ownership of `goap` (must outlive the brain).
FleeceGoapBrain* fleece_goap_brain_create(FleeceEmbedded* embedded, FleeceGoap* goap);
void fleece_goap_brain_destroy(FleeceGoapBrain* brain);

// Drive one tick of the behavior loop. Returns 0 on success, -1 on error.
int fleece_goap_brain_tick(FleeceGoapBrain* brain);

// Register an event callback (e.g. for logging). May be NULL to clear.
void fleece_goap_brain_set_event_callback(FleeceGoapBrain* brain, FleeceGoapBrainEventFn cb, void* user_data);

// Register the world-model hook (see FleeceGoapWorldModelFn). May be NULL to clear.
void fleece_goap_brain_set_world_model(FleeceGoapBrain* brain, FleeceGoapWorldModelFn cb, void* user_data);

// Register the divergence diagnostics (see FleeceGoapDivergenceFn). Enabling
// them costs one extra effect-apply per exec tick, so leave NULL (default) on
// resource-constrained targets and enable only when debugging the plan model.
// May be NULL to clear.
void fleece_goap_brain_set_divergence_cb(FleeceGoapBrain* brain, FleeceGoapDivergenceFn cb, void* user_data);

// Cap how many ticks a single action may execute before the brain gives up on
// it and replans (FLEECE_GOAP_BRAIN_EVENT_ABORTED). A wedged action - hardware
// that never responds, a target that vanished mid-hunt - would otherwise pin
// the agent forever. 0 (default) disables the watchdog.
void fleece_goap_brain_set_max_action_ticks(FleeceGoapBrain* brain, uint32_t max_ticks);

// Skip a goal for N ticks after it fails to produce a plan, instead of
// re-running a full A* search against it every tick while unplannable
// (e.g. dead battery blocking "recharge" forever). 0 (default) disables
// this and always retries every ranked goal every tick.
void fleece_goap_brain_set_goal_cooldown_ticks(FleeceGoapBrain* brain, uint32_t ticks);

// Current brain state, for introspection/logging. NULL when none.
const char* fleece_goap_brain_goal_id(const FleeceGoapBrain* brain);
const char* fleece_goap_brain_action_id(const FleeceGoapBrain* brain);
uint32_t fleece_goap_brain_action_progress(const FleeceGoapBrain* brain);  // ticks the current action has been executing
uint32_t fleece_goap_brain_plan_length(const FleeceGoapBrain* brain);
uint32_t fleece_goap_brain_tick_count(const FleeceGoapBrain* brain);

#ifdef __cplusplus
}
#endif

#endif // FLEECE_GOAP_JS_H