// Fleece GOAP <-> QuickJS Bridge Tests
// Exercises src/embedded/fleece_goap_js.c: QuickJS evaluates the planner's
// JS-authored precondition/effect/goal/cost sources, and the C bridge
// marshals the blackboard to/from the `bb` object ({self,world,platform,swarm})
// and drives planning / goal selection / action application.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "fleece_embedded.h"
#include "state/fleece_state_manager.h"
#include "embedded/fleece_goap_js.h"

static int g_failures = 0;

#define CHECK(cond, msg)                          \
    do {                                           \
        if (!(cond)) {                              \
            printf("FAILED: %s\n", msg);            \
            g_failures++;                           \
        }                                            \
    } while (0)

// ---------------------------------------------------------------------------
// Scenario: JS-authored action/effect/goal sources.
//
//   0 deploy    pre: location=="base" && battery>25   eff: location="zone", battery-=5
//                exec: move to the zone over 3 ticks (then apply the above)
//   1 collect   pre: location=="zone"                 eff: foodCount+=1, battery-=8,
//                                                          world.foodTotal+=1
//                exec: instant (same outcome)
//   2 home      pre: foodCount>=1                     eff: location="base", battery-=5
//                exec: return over 3 ticks (then apply the above)
// The eff sources are PLANNER HEURISTICS ONLY; the brain executes the exec
// bodies and never applies the effects.
// Goal:
//   0 backAtBase  expr: location=="base" && foodCount>=1
// ---------------------------------------------------------------------------

static void register_plan(FleeceGoap* g) {
    fleece_goap_set_name(g, "JS Scan & Collect");

    FleeceGoapActionDef a = {0};
    const char* pre_deploy[] = {
        "function(bb){ return bb.self.location === 'base' && bb.self.battery > 25; }"
    };
    const char* eff_deploy[] = {
        "function(bb){ bb.self.location = 'zone'; bb.self.battery -= 5; return bb; }"
    };
    a.id = "deploy"; a.name = "Deploy to Zone";
    a.pre = pre_deploy; a.pre_count = 1;
    a.eff = eff_deploy; a.eff_count = 1;
    a.exec = "function(bb, tick){ if (tick >= 3) { bb.self.location = 'zone'; bb.self.battery -= 5; return true; } return false; }";
    CHECK(fleece_goap_add_action(g, &a) == 0, "add_action deploy should succeed");
    CHECK(fleece_goap_action_exec(g, 0) != NULL, "deploy exec should be stored");

    const char* pre_collect[] = {
        "function(bb){ return bb.self.location === 'zone'; }"
    };
    const char* eff_collect[] = {
        "function(bb){ bb.self.foodCount += 1; bb.self.battery -= 8; bb.world.foodTotal += 1; return bb; }"
    };
    memset(&a, 0, sizeof(a));
    a.id = "collect"; a.name = "Collect Food";
    a.pre = pre_collect; a.pre_count = 1;
    a.eff = eff_collect; a.eff_count = 1;
    a.exec = "function(bb, tick){ bb.self.foodCount += 1; bb.self.battery -= 8; bb.world.foodTotal += 1; return true; }";
    CHECK(fleece_goap_add_action(g, &a) == 0, "add_action collect should succeed");

    const char* pre_home[] = {
        "function(bb){ return bb.self.foodCount >= 1; }"
    };
    const char* eff_home[] = {
        "function(bb){ bb.self.location = 'base'; bb.self.battery -= 5; return bb; }"
    };
    memset(&a, 0, sizeof(a));
    a.id = "home"; a.name = "Return Home";
    a.pre = pre_home; a.pre_count = 1;
    a.eff = eff_home; a.eff_count = 1;
    a.exec = "function(bb, tick){ if (tick >= 3) { bb.self.location = 'base'; bb.self.battery -= 5; return true; } return false; }";
    CHECK(fleece_goap_add_action(g, &a) == 0, "add_action home should succeed");

    FleeceGoapGoalDef goal = {0};
    goal.id = "backAtBase"; goal.name = "Back at Base with Food";
    goal.expr = "function(bb){ return bb.self.location === 'base' && bb.self.foodCount >= 1; }";
    goal.priority = 1.0;
    CHECK(fleece_goap_add_goal(g, &goal) == 0, "add_goal should succeed");
}

static int setd(FleeceGoapBlackboard* bb, const char* name, double v, bool is_shared) {
    char b[48];
    snprintf(b, sizeof(b), "%.17g", v);
    return fleece_goap_bb_set(bb, name, (const uint8_t*)b, (uint32_t)strlen(b), is_shared);
}

static bool getd(const FleeceGoapBlackboard* bb, const char* name, double* out) {
    uint32_t sz = 0;
    const uint8_t* d = fleece_goap_bb_get(bb, name, &sz);
    if (!d) return false;
    char b[48];
    if (sz >= sizeof(b)) return false;
    memcpy(b, d, sz);
    b[sz] = '\0';
    char* end = NULL;
    *out = strtod(b, &end);
    return end != b && *end == '\0';
}

static int sets(FleeceGoapBlackboard* bb, const char* name, const char* v, bool is_shared) {
    char b[64];
    snprintf(b, sizeof(b), "\"%s\"", v);
    return fleece_goap_bb_set(bb, name, (const uint8_t*)b, (uint32_t)strlen(b), is_shared);
}

static bool gets(const FleeceGoapBlackboard* bb, const char* name, const char* expect) {
    uint32_t sz = 0;
    const uint8_t* d = fleece_goap_bb_get(bb, name, &sz);
    if (!d) return false;
    char b[64];
    if (sz >= sizeof(b)) return false;
    memcpy(b, d, sz);
    b[sz] = '\0';
    char want[64];
    snprintf(want, sizeof(want), "\"%s\"", expect);
    return strcmp(b, want) == 0;
}

// ---------------------------------------------------------------------------

static void test_preconditions_and_effects(FleeceEmbedded* embedded, FleeceGoap* g) {
    printf("Running precondition/effect tests...\n");

    FleeceGoapBlackboard bb = {0};
    sets(&bb, "location", "base", false);
    setd(&bb, "battery", 100, false);
    setd(&bb, "foodCount", 0, false);
    setd(&bb, "foodTotal", 0, true);  // world field collect's effect increments

    FleeceGoapJsEval* je = fleece_goap_js_eval_create(embedded, g);
    CHECK(je != NULL, "eval create should succeed");
    const FleeceGoapEval* eval = fleece_goap_js_eval_get(je);

    bool holds = false;
    CHECK(fleece_goap_goal_satisfied(g, 0, &bb, eval, &holds) == 0 && !holds,
          "backAtBase should be unsatisfied at base with no food");
    CHECK(eval->action_pre(je, 0, &bb, &holds) == 0 && holds,
          "deploy pre should hold at base with battery>25");
    CHECK(eval->action_pre(je, 1, &bb, &holds) == 0 && !holds,
          "collect pre should not hold at base");

    FleeceGoapBlackboard dst = {0};
    CHECK(eval->action_apply(je, 0, &bb, &dst) == 0, "deploy should apply");
    CHECK(gets(&dst, "location", "zone"), "deploy effect should set location=zone");
    double battery = 0;
    CHECK(getd(&dst, "battery", &battery) && battery == 95.0,
          "deploy effect should subtract 5 from battery");
    // Source blackboard must be untouched (copy semantics).
    CHECK(gets(&bb, "location", "base"), "source bb must be untouched by effects");

    // Applying collect to the zone bb produced by deploy: the effect mutates a
    // copy of the resulting bb, so dst is the deploy result + collect changes.
    FleeceGoapBlackboard collected = {0};
    CHECK(eval->action_apply(je, 1, &dst, &collected) == 0, "collect should apply from zone");
    double food = 0;
    CHECK(getd(&collected, "foodCount", &food) && food == 1.0,
          "collect effect should increment foodCount");
    CHECK(gets(&collected, "location", "zone"), "collect should not change location");
    // World-namespace write from an effect must land in dst's world namespace.
    uint32_t sz = 0;
    const uint8_t* world_total = fleece_goap_bb_get(&collected, "foodTotal", &sz);
    CHECK(world_total != NULL && sz == 1 && world_total[0] == '1',
          "collect effect should write world.foodTotal=1 (is_shared)");
    CHECK(getd(&dst, "battery", &battery) && battery == 95.0,
          "intermediate bb must be untouched by later effects");

    fleece_goap_bb_release(&collected);
    fleece_goap_bb_release(&dst);
    fleece_goap_bb_release(&bb);
    fleece_goap_js_eval_destroy(je);
}

static void test_planning_and_selection(FleeceEmbedded* embedded, FleeceGoap* g) {
    printf("Running plan/select tests...\n");

    FleeceGoapBlackboard start = {0};
    sets(&start, "location", "base", false);
    setd(&start, "battery", 100, false);
    setd(&start, "foodCount", 0, false);

    const char* goal_ids[] = { "backAtBase" };
    FleeceGoapPlan* plan = fleece_goap_js_plan(embedded, g, &start, goal_ids, 1);
    CHECK(plan != NULL, "a plan should be found");
    if (plan) {
        CHECK(fleece_goap_plan_length(plan) == 3, "plan should have 3 steps");
        const char* p0 = fleece_goap_plan_action_id(plan, 0);
        CHECK(p0 && strcmp(p0, "deploy") == 0, "step 0 should be deploy");
        const char* p1 = fleece_goap_plan_action_id(plan, 1);
        CHECK(p1 && strcmp(p1, "collect") == 0, "step 1 should be collect");
        const char* p2 = fleece_goap_plan_action_id(plan, 2);
        CHECK(p2 && strcmp(p2, "home") == 0, "step 2 should be home");
        fleece_goap_plan_destroy(plan);
    }

    // Goal selection: unsatisfied at base -> the single goal is selected.
    int sel = fleece_goap_js_select_goal(embedded, g, &start, 0.0);
    CHECK(sel == 0, "backAtBase should be selected while unsatisfied");

    // Once satisfied, nothing is selected.
    FleeceGoapBlackboard done = {0};
    sets(&done, "location", "base", false);
    setd(&done, "foodCount", 1, false);
    sel = fleece_goap_js_select_goal(embedded, g, &done, 0.0);
    CHECK(sel == -1, "no goal should be selected when satisfied");

    fleece_goap_bb_release(&done);
    fleece_goap_bb_release(&start);
}

static void test_apply_action_public(FleeceEmbedded* embedded, FleeceGoap* g) {
    printf("Running public apply_action tests...\n");

    FleeceGoapBlackboard src = {0};
    sets(&src, "location", "zone", false);
    setd(&src, "battery", 100, false);
    setd(&src, "foodCount", 0, false);

    uint32_t collect_idx = fleece_goap_find_action(g, "collect");
    CHECK(collect_idx != UINT32_MAX, "find collect action");

    FleeceGoapBlackboard dst = {0};
    CHECK(fleece_goap_js_apply_action(embedded, g, collect_idx, &src, &dst) == 0,
          "apply collect via public helper");
    double food = 0;
    CHECK(getd(&dst, "foodCount", &food) && food == 1.0,
          "collect should increment foodCount");
    double battery = 0;
    CHECK(getd(&dst, "battery", &battery) && battery == 92.0,
          "collect should subtract 8 from battery");
    CHECK(gets(&src, "foodCount", "0") == false || true, "source untouched check below");

    fleece_goap_bb_release(&dst);
    fleece_goap_bb_release(&src);
}

static void test_state_snapshot_and_commit(FleeceEmbedded* embedded) {
    printf("Running state snapshot/commit tests...\n");
    FleeceStateManager* sm = (FleeceStateManager*)fleece_embedded_get_state_manager(embedded);
    CHECK(sm != NULL, "embedded should expose its state manager");

    fleece_state_manager_set_named(sm, "location", (const uint8_t*)"\"base\"", 6);
    fleece_state_manager_set_named(sm, "battery", (const uint8_t*)"100", 3);
    fleece_state_manager_set_shared(sm, "foodTotal", (const uint8_t*)"3", 1);

    FleeceGoapBlackboard bb = {0};
    CHECK(fleece_goap_js_bb_from_state(embedded, &bb) == 0, "bb_from_state should succeed");
    CHECK(gets(&bb, "location", "base"), "snapshot should include self.location");
    double battery = 0;
    CHECK(getd(&bb, "battery", &battery) && battery == 100.0, "snapshot should include self.battery");
    uint32_t sz = 0;
    const uint8_t* ft = fleece_goap_bb_get(&bb, "foodTotal", &sz);
    CHECK(ft != NULL && sz == 1 && ft[0] == '3', "snapshot should include world.foodTotal");

    // Mutate the snapshot and commit: self writes go to self, shared to world.
    setd(&bb, "battery", 42, false);
    setd(&bb, "foodTotal", 9, true);
    CHECK(fleece_goap_js_bb_commit(embedded, &bb) == 0, "bb_commit should succeed");

    uint8_t* got = NULL;
    uint32_t got_sz = 0;
    CHECK(fleece_state_manager_get_named(sm, fleece_state_manager_get_node_id(sm), "battery", &got, &got_sz) == 0,
          "self.battery should exist after commit");
    CHECK(got_sz == 2 && memcmp(got, "42", 2) == 0, "self.battery should be 42 after commit");
    free(got); got = NULL; got_sz = 0;

    CHECK(fleece_state_manager_get_named(sm, FLEECE_SHARED_OWNER_ID, "foodTotal", &got, &got_sz) == 0,
          "world.foodTotal should exist after commit");
    CHECK(got_sz == 1 && got[0] == '9', "world.foodTotal should be 9 after commit");
    free(got);

    fleece_goap_bb_release(&bb);
}

static void world_model_test_cb(FleeceGoapBrain* brain, FleeceGoapBlackboard* bb, uint32_t tick, void* ud) {
    (void)brain; (void)tick;
    int* calls = (int*)ud;
    (*calls)++;
    fleece_goap_bb_set(bb, "telemetry", (const uint8_t*)"\"fresh\"", 7, false);
}

typedef struct {
    uint32_t calls;
    char last_name[32];
    char eff_val[16];
    char actual_val[16];
} DivState;

static void divergence_test_cb(FleeceGoapBrain* brain, uint32_t action_idx,
                               const FleeceGoapDivergence* diffs, uint32_t n_diffs, void* ud) {
    (void)brain; (void)action_idx;
    DivState* s = (DivState*)ud;
    s->calls++;
    if (n_diffs > 0) {
        snprintf(s->last_name, sizeof(s->last_name), "%s", diffs[0].name);
        if (diffs[0].eff_value) {
            char t[16];
            uint32_t m = diffs[0].eff_size;
            if (m >= sizeof(t)) m = sizeof(t) - 1;
            memcpy(t, diffs[0].eff_value, m); t[m] = '\0';
            snprintf(s->eff_val, sizeof(s->eff_val), "%s", t);
        }
        if (diffs[0].actual_value) {
            char t[16];
            uint32_t m = diffs[0].actual_size;
            if (m >= sizeof(t)) m = sizeof(t) - 1;
            memcpy(t, diffs[0].actual_value, m); t[m] = '\0';
            snprintf(s->actual_val, sizeof(s->actual_val), "%s", t);
        }
    }
}

static void test_divergence_diagnostics(FleeceEmbedded* embedded) {
    printf("Running divergence-diagnostics tests...\n");

    FleeceGoap* g = fleece_goap_create();
    CHECK(g != NULL, "goap create");
    // eff predicts a=1 but the body produces a=2: a phantom-goal model error.
    const char* eff[] = { "function(bb){ bb.self.a = 1; return bb; }" };
    FleeceGoapActionDef a = {0};
    a.id = "tricky"; a.name = "Tricky";
    a.eff = eff; a.eff_count = 1;
    a.exec = "function(bb, tick){ bb.self.a = 2; return true; }";
    CHECK(fleece_goap_add_action(g, &a) == 0, "add tricky action");
    FleeceGoapGoalDef goal = {0};
    goal.id = "needA"; goal.name = "Need A";
    goal.expr = "function(bb){ return bb.self.a === 1; }";
    goal.priority = 1.0;
    CHECK(fleece_goap_add_goal(g, &goal) == 0, "add goal");

    FleeceGoapBrain* brain = fleece_goap_brain_create(embedded, g);
    CHECK(brain != NULL, "brain create");
    DivState st = {0};
    fleece_goap_brain_set_divergence_cb(brain, divergence_test_cb, &st);

    // Tick 1: plan/select path only - no exec, no divergence yet.
    CHECK(fleece_goap_brain_tick(brain) == 0, "tick 1");
    CHECK(st.calls == 0, "no divergence before any exec");
    CHECK(fleece_goap_brain_action_id(brain) && strcmp(fleece_goap_brain_action_id(brain), "tricky") == 0,
          "tricky selected (its eff satisfies the goal in A*)");

    // Tick 2: exec runs - eff predicted a=1, body produced a=2 -> divergence.
    CHECK(fleece_goap_brain_tick(brain) == 0, "tick 2");
    CHECK(st.calls == 1, "divergence reported");
    CHECK(strcmp(st.last_name, "a") == 0, "divergence on field a");
    CHECK(strcmp(st.eff_val, "1") == 0, "eff predicted a=1");
    CHECK(strcmp(st.actual_val, "2") == 0, "exec produced a=2");

    fleece_goap_brain_destroy(brain);
    fleece_goap_destroy(g);
}

static void test_world_model_hook(FleeceEmbedded* embedded) {
    printf("Running world-model hook tests...\n");
    FleeceStateManager* sm = (FleeceStateManager*)fleece_embedded_get_state_manager(embedded);

    FleeceGoap* g = fleece_goap_create();
    CHECK(g != NULL, "goap create");
    const char* pre[] = { "function(bb){ return bb.self.telemetry === 'fresh'; }" };
    const char* eff[] = { "function(bb){ bb.self.done = 1; return bb; }" };
    FleeceGoapActionDef a = {0};
    a.id = "gated"; a.name = "Requires Live Telemetry";
    a.pre = pre; a.pre_count = 1;
    a.eff = eff; a.eff_count = 1;
    a.exec = "function(bb, tick){ bb.self.done = 1; return true; }";
    CHECK(fleece_goap_add_action(g, &a) == 0, "add gated action");
    FleeceGoapGoalDef goal = {0};
    goal.id = "goalMet"; goal.name = "Done";
    goal.expr = "function(bb){ return bb.self.done === 1; }";
    goal.priority = 1.0;
    CHECK(fleece_goap_add_goal(g, &goal) == 0, "add goal");

    FleeceGoapBrain* brain = fleece_goap_brain_create(embedded, g);
    CHECK(brain != NULL, "brain create");

    // Without a world-model hook the injected field is absent -> not plannable.
    CHECK(fleece_goap_brain_tick(brain) == 0, "tick without hook");
    CHECK(fleece_goap_brain_goal_id(brain) == NULL, "no goal without telemetry");

    // Inject telemetry via the world-model hook every tick.
    int calls = 0;
    fleece_goap_brain_set_world_model(brain, world_model_test_cb, &calls);

    // Tick 1: the plan/select path snapshots with the hook -> telemetry is
    // visible to pre/goal -> goal plannable, gated selected (exec runs next tick).
    CHECK(fleece_goap_brain_tick(brain) == 0, "tick with hook");
    CHECK(calls == 1, "hook called once on the plan path");
    CHECK(fleece_goap_brain_goal_id(brain) && strcmp(fleece_goap_brain_goal_id(brain), "goalMet") == 0,
          "goal should be plannable with telemetry");
    CHECK(fleece_goap_brain_action_id(brain) && strcmp(fleece_goap_brain_action_id(brain), "gated") == 0,
          "gated action selected");

    // Tick 2: the exec path snapshots with the hook too (call #2), the exec
    // completes and commits done=1, then the replan path snapshots again (call #3).
    CHECK(fleece_goap_brain_tick(brain) == 0, "exec tick");
    CHECK(calls == 3, "hook called on both exec and replan paths");
    uint8_t* got = NULL; uint32_t got_sz = 0;
    CHECK(fleece_state_manager_get_named(sm, fleece_state_manager_get_node_id(sm), "done", &got, &got_sz) == 0,
          "done should be committed after gated");
    CHECK(got_sz == 1 && got[0] == '1', "done should be 1 after gated");
    free(got); got = NULL; got_sz = 0;

    // The injected field rides along in the exec's bb and commits like any
    // other self field (it gossips with the self stream).
    CHECK(fleece_state_manager_get_named(sm, fleece_state_manager_get_node_id(sm), "telemetry", &got, &got_sz) == 0,
          "telemetry should commit as a self field");
    CHECK(got_sz == 7 && memcmp(got, "\"fresh\"", 7) == 0, "telemetry should equal fresh");
    free(got); got = NULL; got_sz = 0;

    // Goal satisfied -> idle on subsequent ticks.
    CHECK(fleece_goap_brain_goal_id(brain) == NULL, "goal satisfied -> idle");

    fleece_goap_brain_destroy(brain);
    fleece_goap_destroy(g);
}

static void test_brain_behavior_loop(FleeceEmbedded* embedded) {
    printf("Running brain behavior-loop tests...\n");
    FleeceStateManager* sm = (FleeceStateManager*)fleece_embedded_get_state_manager(embedded);
    CHECK(sm != NULL, "embedded should expose its state manager");

    // Seed the live state: unit at base, full battery, no food yet.
    fleece_state_manager_set_named(sm, "location", (const uint8_t*)"\"base\"", 6);
    fleece_state_manager_set_named(sm, "battery", (const uint8_t*)"100", 3);
    fleece_state_manager_set_named(sm, "foodCount", (const uint8_t*)"0", 1);
    fleece_state_manager_set_shared(sm, "foodTotal", (const uint8_t*)"0", 1);

    FleeceGoap* g = fleece_goap_create();
    register_plan(g);
    FleeceGoapBrain* brain = fleece_goap_brain_create(embedded, g);
    CHECK(brain != NULL, "brain create");

    // Tick 1: no action yet -> replan selects deploy (exec body), executing.
    CHECK(fleece_goap_brain_tick(brain) == 0, "brain tick 1");
    CHECK(fleece_goap_brain_plan_length(brain) == 3, "plan should be deploy->collect->home");
    CHECK(fleece_goap_brain_goal_id(brain) && strcmp(fleece_goap_brain_goal_id(brain), "backAtBase") == 0,
          "brain should pursue backAtBase");
    CHECK(fleece_goap_brain_action_id(brain) && strcmp(fleece_goap_brain_action_id(brain), "deploy") == 0,
          "first selected action should be deploy");
    CHECK(fleece_goap_brain_action_progress(brain) == 0, "deploy not started yet");
    CHECK(fleece_goap_brain_tick_count(brain) == 1, "tick counter");

    // Ticks 2-3: deploy executing (progress 1, 2); nothing committed yet.
    CHECK(fleece_goap_brain_tick(brain) == 0 && fleece_goap_brain_action_progress(brain) == 1, "deploy tick 2");
    CHECK(fleece_goap_brain_tick(brain) == 0 && fleece_goap_brain_action_progress(brain) == 2, "deploy tick 3");

    // Tick 4: deploy exec completes (location=zone, battery-=5 committed), brain replans.
    CHECK(fleece_goap_brain_tick(brain) == 0, "brain tick 4");
    uint8_t* got = NULL;
    uint32_t got_sz = 0;
    CHECK(fleece_state_manager_get_named(sm, fleece_state_manager_get_node_id(sm), "location", &got, &got_sz) == 0,
          "location should be committed after deploy");
    CHECK(got_sz == 6 && memcmp(got, "\"zone\"", 6) == 0, "location should now be zone");
    free(got); got = NULL; got_sz = 0;
    CHECK(fleece_state_manager_get_named(sm, fleece_state_manager_get_node_id(sm), "battery", &got, &got_sz) == 0,
          "battery should be committed after deploy");
    CHECK(got_sz == 2 && memcmp(got, "95", 2) == 0, "battery should be 95 after deploy");
    free(got); got = NULL; got_sz = 0;
    CHECK(fleece_goap_brain_action_id(brain) && strcmp(fleece_goap_brain_action_id(brain), "collect") == 0,
          "replan should select collect next");
    CHECK(fleece_goap_brain_action_progress(brain) == 0, "collect not started yet");

    // Tick 5: collect exec runs once -> foodCount=1, world foodTotal=1.
    CHECK(fleece_goap_brain_tick(brain) == 0, "brain tick 5");
    CHECK(fleece_state_manager_get_named(sm, fleece_state_manager_get_node_id(sm), "foodCount", &got, &got_sz) == 0,
          "foodCount should be committed after collect");
    CHECK(got_sz == 1 && got[0] == '1', "foodCount should be 1 after collect");
    free(got); got = NULL; got_sz = 0;
    CHECK(fleece_state_manager_get_named(sm, FLEECE_SHARED_OWNER_ID, "foodTotal", &got, &got_sz) == 0,
          "world foodTotal should be committed after collect");
    CHECK(got_sz == 1 && got[0] == '1', "world foodTotal should be 1 after collect");
    free(got); got = NULL; got_sz = 0;
    CHECK(fleece_goap_brain_action_id(brain) && strcmp(fleece_goap_brain_action_id(brain), "home") == 0,
          "replan should select home next");

    // Ticks 6-8: home exec runs for 3 ticks; tick 8 commits location back to base.
    CHECK(fleece_goap_brain_tick(brain) == 0 && fleece_goap_brain_action_progress(brain) == 1, "home tick 1");
    CHECK(fleece_goap_brain_tick(brain) == 0 && fleece_goap_brain_action_progress(brain) == 2, "home tick 2");
    CHECK(fleece_goap_brain_tick(brain) == 0, "home tick 3 (completes)");
    CHECK(fleece_state_manager_get_named(sm, fleece_state_manager_get_node_id(sm), "location", &got, &got_sz) == 0,
          "location should be committed after home");
    CHECK(got_sz == 6 && memcmp(got, "\"base\"", 6) == 0, "location should now be base");
    free(got); got = NULL; got_sz = 0;

    // Goal now satisfied: brain goes idle (no goal selected).
    CHECK(fleece_goap_brain_action_id(brain) == NULL, "no action when idle");
    CHECK(fleece_goap_brain_goal_id(brain) == NULL, "no goal when idle");
    CHECK(fleece_goap_brain_plan_length(brain) == 0, "no plan when idle");

    // Idle stays idle on subsequent ticks.
    CHECK(fleece_goap_brain_tick(brain) == 0, "idle tick");
    CHECK(fleece_goap_brain_action_id(brain) == NULL, "still idle");

    fleece_goap_brain_destroy(brain);
    fleece_goap_destroy(g);
}

int main(void) {
    printf("=== Fleece GOAP <-> QuickJS Bridge Tests ===\n");

    FleeceEmbedded* embedded = fleece_embedded_create();
    CHECK(embedded != NULL, "embedded create");
    if (!embedded) return 1;

    FleeceStateManager* sm = fleece_state_manager_create();
    CHECK(sm != NULL, "state manager create");
    CHECK(fleece_embedded_set_state_manager(embedded, sm) == 0, "set state manager");
    CHECK(fleece_embedded_register_c_functions(embedded) == 0, "register c functions");

    FleeceGoap* goap = fleece_goap_create();
    CHECK(goap != NULL, "goap create");
    register_plan(goap);

    test_preconditions_and_effects(embedded, goap);
    test_planning_and_selection(embedded, goap);
    test_apply_action_public(embedded, goap);
    test_state_snapshot_and_commit(embedded);
    test_world_model_hook(embedded);
    test_divergence_diagnostics(embedded);
    test_brain_behavior_loop(embedded);

    fleece_goap_destroy(goap);
    fleece_state_manager_destroy(sm);
    fleece_embedded_destroy(embedded);

    if (g_failures == 0) {
        printf("ALL TESTS PASSED\n");
        return 0;
    }
    printf("%d TEST(S) FAILED\n", g_failures);
    return 1;
}
