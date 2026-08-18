// Fleece GOAP Planner Tests
// Exercises the engine-agnostic planner with a C mock evaluator (no QuickJS):
// blackboard ops, table registration, utility-curve goal selection, A* search
// (basic plans, empty plans, no-solution, cost-aware, cycle pruning, goal
// targeting), and CBOR plan-blob serialize/deserialize round-trips.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "planner/fleece_planner.h"

static int g_failures = 0;

#define CHECK(cond, msg)                          \
    do {                                           \
        if (!(cond)) {                              \
            printf("FAILED: %s\n", msg);            \
            g_failures++;                           \
        }                                            \
    } while (0)

// ---------------------------------------------------------------------------
// Blackboard helpers (JSON number / quoted-string fields)
// ---------------------------------------------------------------------------

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
// Scenario + mock evaluator
//
// Actions (indices are registration order):
//   0 deploy      pre: location=="base"   eff: location="zone", battery-=1
//   1 scan        pre: location=="zone"   eff: scan=1
//   2 collect     pre: scan>=1            eff: foodCount+=1
//   3 returnHome  pre: foodCount>=1       eff: location="base"
//   4 collectSlow pre: scan>=1            eff: foodCount+=1   (cost 3)
// Goals:
//   0 gather      foodCount>=1            priority 2, curve u2
//   1 home        location=="base"        priority 1, curve u1
//   2 inZone      location=="zone"        priority 1, curve u1
//   3 impossible  never true
// ---------------------------------------------------------------------------

static void register_scenario(FleeceGoap* g) {
    fleece_goap_set_name(g, "Scan & Collect");

    FleeceGoapActionDef a = {0};
    const char* pre_deploy[] = { "function(bb){ return bb.self.location === 'base'; }" };
    const char* eff_deploy[] = { "function(bb){ bb.self.location = 'zone'; bb.self.battery -= 1; return bb; }" };
    a.id = "deploy"; a.name = "Deploy to Zone"; a.dest = "zone"; a.dur = 2.0;
    a.pre = pre_deploy; a.pre_count = 1;
    a.eff = eff_deploy; a.eff_count = 1;
    a.exec = "function(bb, tick){ if (tick >= 2) { bb.self.location = 'zone'; bb.self.battery -= 1; return true; } return false; }";
    CHECK(fleece_goap_add_action(g, &a) == 0, "add_action deploy should succeed");
    CHECK(fleece_goap_action_exec(g, 0) != NULL, "deploy exec should be stored");

    const char* pre_scan[] = { "function(bb){ return bb.self.location === 'zone'; }" };
    const char* eff_scan[] = { "function(bb){ bb.self.scan = 1; return bb; }" };
    memset(&a, 0, sizeof(a));
    a.id = "scan"; a.name = "Scan Zone"; a.dest = "scan";
    a.pre = pre_scan; a.pre_count = 1;
    a.eff = eff_scan; a.eff_count = 1;
    CHECK(fleece_goap_add_action(g, &a) == 0, "add_action scan should succeed");

    const char* pre_collect[] = { "function(bb){ return bb.self.scan >= 1; }" };
    const char* eff_collect[] = { "function(bb){ bb.self.foodCount += 1; return bb; }" };
    memset(&a, 0, sizeof(a));
    a.id = "collect"; a.name = "Collect Food"; a.dest = "food";
    a.pre = pre_collect; a.pre_count = 1;
    a.eff = eff_collect; a.eff_count = 1;
    CHECK(fleece_goap_add_action(g, &a) == 0, "add_action collect should succeed");

    const char* pre_return[] = { "function(bb){ return bb.self.foodCount >= 1; }" };
    const char* eff_return[] = { "function(bb){ bb.self.location = 'base'; return bb; }" };
    memset(&a, 0, sizeof(a));
    a.id = "returnHome"; a.name = "Return Home"; a.dest = "base";
    a.pre = pre_return; a.pre_count = 1;
    a.eff = eff_return; a.eff_count = 1;
    CHECK(fleece_goap_add_action(g, &a) == 0, "add_action returnHome should succeed");

    const char* pre_slow[] = { "function(bb){ return bb.self.scan >= 1; }" };
    const char* eff_slow[] = { "function(bb){ bb.self.foodCount += 1; return bb; }" };
    memset(&a, 0, sizeof(a));
    a.id = "collectSlow"; a.name = "Collect Food Slowly"; a.cost = "function(bb){ return 3; }";
    a.pre = pre_slow; a.pre_count = 1;
    a.eff = eff_slow; a.eff_count = 1;
    CHECK(fleece_goap_add_action(g, &a) == 0, "add_action collectSlow should succeed");

    FleeceGoapGoalDef go = {0};
    go.id = "gather"; go.name = "Gather 1 food";
    go.expr = "function(bb){ return bb.self.foodCount >= 1; }";
    go.priority = 2.0; go.curve_id = "u2";
    CHECK(fleece_goap_add_goal(g, &go) == 0, "add_goal gather should succeed");

    memset(&go, 0, sizeof(go));
    go.id = "home"; go.name = "Back at base";
    go.expr = "function(bb){ return bb.self.location === 'base'; }";
    go.priority = 1.0; go.curve_id = "u1";
    CHECK(fleece_goap_add_goal(g, &go) == 0, "add_goal home should succeed");

    memset(&go, 0, sizeof(go));
    go.id = "inZone"; go.name = "In zone";
    go.expr = "function(bb){ return bb.self.location === 'zone'; }";
    go.priority = 1.0; go.curve_id = "u1";
    CHECK(fleece_goap_add_goal(g, &go) == 0, "add_goal inZone should succeed");

    memset(&go, 0, sizeof(go));
    go.id = "impossible"; go.name = "Impossible";
    go.expr = "function(bb){ return false; }";
    go.priority = 0.0; /* never satisfiable; 0 utility so it never wins selection */
    CHECK(fleece_goap_add_goal(g, &go) == 0, "add_goal impossible should succeed");

    FleeceGoapUtilityDef u = {0};
    FleeceGoapPoint u1_pts[] = { {0, 0.9}, {1, 0.9} };
    u.id = "u1"; u.name = "Constant"; u.x_min = 0; u.x_max = 1;
    u.points = u1_pts; u.point_count = 2;
    CHECK(fleece_goap_add_utility(g, &u) == 0, "add_utility u1 should succeed");

    FleeceGoapPoint u2_pts[] = { {0, 0.9}, {1, 0.9}, {2, 0.7}, {3, 0.05} };
    memset(&u, 0, sizeof(u));
    u.id = "u2"; u.name = "Hunger"; u.dim = "foodCount"; u.x_min = 0; u.x_max = 3;
    u.points = u2_pts; u.point_count = 4;
    CHECK(fleece_goap_add_utility(g, &u) == 0, "add_utility u2 should succeed");

    FleeceGoapMissionDef m = {0};
    const char* m1_goals[] = { "gather", "home" };
    m.id = "m1"; m.name = "Gather and RTB"; m.note = "scan, collect one food, return";
    m.goal_ids = m1_goals; m.goal_count = 2;
    CHECK(fleece_goap_add_mission(g, &m) == 0, "add_mission m1 should succeed");
}

static int mock_pre(void* ud, uint32_t ai, const FleeceGoapBlackboard* bb, bool* out) {
    (void)ud;
    switch (ai) {
        case 0: *out = gets(bb, "location", "base"); break;
        case 1: *out = gets(bb, "location", "zone"); break;
        case 2:
        case 4: { double s; *out = getd(bb, "scan", &s) && s >= 1.0; break; }
        case 3: { double f; *out = getd(bb, "foodCount", &f) && f >= 1.0; break; }
        default: *out = false;
    }
    return 0;
}

static int mock_apply(void* ud, uint32_t ai, const FleeceGoapBlackboard* src, FleeceGoapBlackboard* dst) {
    (void)ud;
    if (fleece_goap_bb_clone(src, dst) != 0) return -1;
    switch (ai) {
        case 0:
            sets(dst, "location", "zone", false);
            { double b = 0; getd(dst, "battery", &b); setd(dst, "battery", b - 1.0, false); }
            break;
        case 1:
            setd(dst, "scan", 1.0, false);
            break;
        case 2:
        case 4:
            { double f = 0; getd(dst, "foodCount", &f); setd(dst, "foodCount", f + 1.0, false); }
            break;
        case 3:
            sets(dst, "location", "base", false);
            break;
        default:
            return -1;
    }
    return 0;
}

static int mock_goal(void* ud, uint32_t gi, const FleeceGoapBlackboard* bb, bool* out) {
    (void)ud;
    switch (gi) {
        case 0: { double f; *out = getd(bb, "foodCount", &f) && f >= 1.0; break; }
        case 1: *out = gets(bb, "location", "base"); break;
        case 2: *out = gets(bb, "location", "zone"); break;
        case 3: *out = false; break;
        default: *out = false;
    }
    return 0;
}

static int mock_cost(void* ud, uint32_t ai, const FleeceGoapBlackboard* bb, double* out) {
    (void)ud; (void)bb;
    *out = (ai == 4) ? 3.0 : 1.0;
    return 0;
}

static void fill_eval(FleeceGoapEval* eval) {
    memset(eval, 0, sizeof(*eval));
    eval->action_pre = mock_pre;
    eval->action_apply = mock_apply;
    eval->goal_satisfied = mock_goal;
    eval->action_cost = mock_cost;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

static void test_blackboard_ops(void) {
    printf("Running blackboard tests...\n");
    FleeceGoapBlackboard bb = {0};

    setd(&bb, "battery", 10.0, false);
    sets(&bb, "location", "base", false);
    setd(&bb, "foodCount", 0.0, false);

    double v = 0;
    CHECK(getd(&bb, "battery", &v) && v == 10.0, "self field should read back");
    CHECK(gets(&bb, "location", "base"), "self string field should read back");

    // Same name in world namespace is a distinct field.
    setd(&bb, "foodCount", 99.0, true);
    uint32_t sz = 0;
    const uint8_t* d = fleece_goap_bb_get(&bb, "foodCount", &sz);
    CHECK(d != NULL, "field should be readable after set");
    CHECK(d != NULL && sz == 2, "world foodCount should be readable");

    // Delete removes only the matching field.
    CHECK(fleece_goap_bb_delete(&bb, "location") == 0, "delete should succeed");
    CHECK(fleece_goap_bb_index(&bb, "location") == UINT32_MAX, "deleted field should be gone");
    CHECK(fleece_goap_bb_index(&bb, "battery") != UINT32_MAX, "other fields should remain");

    // Clone preserves namespaces.
    FleeceGoapBlackboard copy = {0};
    CHECK(fleece_goap_bb_clone(&bb, &copy) == 0, "clone should succeed");
    CHECK(getd(&copy, "battery", &v) && v == 10.0, "clone should preserve values");
    CHECK(fleece_goap_bb_index(&copy, "foodCount") != UINT32_MAX, "clone should preserve namespace field");
    fleece_goap_bb_release(&copy);
    fleece_goap_bb_release(&bb);
    printf("Done: blackboard tests\n");
}

static void test_registration(void) {
    printf("Running registration tests...\n");
    FleeceGoap* g = fleece_goap_create();
    CHECK(g != NULL, "create should succeed");
    register_scenario(g);

    CHECK(strcmp(fleece_goap_name(g), "Scan & Collect") == 0, "name should be stored");
    CHECK(fleece_goap_action_count(g) == 5, "should have 5 actions");
    CHECK(fleece_goap_goal_count(g) == 4, "should have 4 goals");
    CHECK(fleece_goap_mission_count(g) == 1, "should have 1 mission");

    uint32_t idx = 0;
    CHECK(fleece_goap_find_action(g, "scan") == 1, "find_action should locate scan");
    CHECK(fleece_goap_find_action(g, "nope") == UINT32_MAX, "find_action should fail for unknown id");
    CHECK(fleece_goap_find_goal(g, "gather") == 0, "find_goal should locate gather");
    CHECK(fleece_goap_find_goal(g, "nope") == UINT32_MAX, "find_goal should fail for unknown id");

    const char* act = fleece_goap_action_id(g, 0);
    CHECK(act != NULL && strcmp(act, "deploy") == 0, "action_id should return registered id");
    CHECK(strcmp(fleece_goap_action_pre(g, 0, 0), "function(bb){ return bb.self.location === 'base'; }") == 0,
          "action pre source should be stored");
    CHECK(strcmp(fleece_goap_action_eff(g, 0, 0), "function(bb){ bb.self.location = 'zone'; bb.self.battery -= 1; return bb; }") == 0,
          "action eff source should be stored");
    CHECK(fleece_goap_action_eff_count(g, 0) == 1, "eff count should be 1");
    CHECK(strcmp(fleece_goap_action_cost_source(g, 4), "function(bb){ return 3; }") == 0,
          "cost source should be stored");
    CHECK(fleece_goap_action_cost_source(g, 0) != NULL && fleece_goap_action_cost_source(g, 0)[0] == '\0',
          "default cost source should be empty");
    CHECK(fleece_goap_action_dest(g, 0) != NULL && strcmp(fleece_goap_action_dest(g, 0), "zone") == 0,
          "action dest should be stored");
    CHECK(fleece_goap_action_dur(g, 0) == 2.0, "action dur should be stored");

    CHECK(strcmp(fleece_goap_goal_expr(g, 0), "function(bb){ return bb.self.foodCount >= 1; }") == 0,
          "goal expr should be stored");
    CHECK(fleece_goap_goal_priority(g, 0) == 2.0, "goal priority should be stored");
    CHECK(strcmp(fleece_goap_goal_curve_id(g, 0), "u2") == 0, "goal curve id should be stored");

    fleece_goap_destroy(g);
    printf("Done: registration tests\n");
}

static void test_utility_and_selection(void) {
    printf("Running utility/selection tests...\n");
    FleeceGoap* g = fleece_goap_create();
    register_scenario(g);

    FleeceGoapEval eval;
    fill_eval(&eval);

    // foodCount=0 -> curve u2 = 0.9 * priority 2 = 1.8; u1 goals = 0.9.
    FleeceGoapBlackboard bb = {0};
    setd(&bb, "foodCount", 0.0, false);
    sets(&bb, "location", "zone", false);

    CHECK(fleece_goap_goal_utility(g, 0, &bb) == 1.8, "goal utility should be priority * curve");
    bool sat = false;
    CHECK(fleece_goap_goal_satisfied(g, 0, &bb, &eval, &sat) == 0 && sat == false, "gather should be unsatisfied at foodCount 0");
    CHECK(fleece_goap_goal_satisfied(g, 1, &bb, &eval, &sat) == 0 && sat == false, "home should be unsatisfied in zone");
    CHECK(fleece_goap_goal_satisfied(g, 2, &bb, &eval, &sat) == 0 && sat == true, "inZone should be satisfied in zone");

    int goal = fleece_goap_select_goal(g, &bb, &eval, 0.0);
    CHECK(goal == 0, "gather should be selected (highest utility)");

    // Threshold rejects the best goal.
    goal = fleece_goap_select_goal(g, &bb, &eval, 2.0);
    CHECK(goal == -1, "threshold above best utility should reject");

    // Satisfied goals are skipped: foodCount=1 satisfies gather, zone satisfies
    // inZone, so only home (unsatisfied, utility 0.9) remains.
    FleeceGoapBlackboard skip = {0};
    setd(&skip, "foodCount", 1.0, false);
    sets(&skip, "location", "zone", false);
    goal = fleece_goap_select_goal(g, &skip, &eval, 0.0);
    CHECK(goal == 1, "satisfied goals should be skipped, next-highest selected");

    // All goals satisfied -> none selected.
    FleeceGoap* single = fleece_goap_create();
    FleeceGoapGoalDef go = {0};
    go.id = "gather"; go.name = "Gather 1 food";
    go.expr = "function(bb){ return bb.self.foodCount >= 1; }";
    go.priority = 1.0; go.curve_id = "u1";
    CHECK(fleece_goap_add_goal(single, &go) == 0, "single-goal add should succeed");
    FleeceGoapUtilityDef u = {0};
    FleeceGoapPoint u1_pts[] = { {0, 0.9}, {1, 0.9} };
    u.id = "u1"; u.x_min = 0; u.x_max = 1;
    u.points = u1_pts; u.point_count = 2;
    CHECK(fleece_goap_add_utility(single, &u) == 0, "single-goal utility should succeed");
    FleeceGoapBlackboard done = {0};
    setd(&done, "foodCount", 3.0, false);
    goal = fleece_goap_select_goal(single, &done, &eval, 0.0);
    CHECK(goal == -1, "no goal should be selected when all satisfied");
    fleece_goap_bb_release(&done);
    fleece_goap_destroy(single);

    fleece_goap_bb_release(&skip);
    fleece_goap_bb_release(&bb);
    fleece_goap_destroy(g);
    printf("Done: utility/selection tests\n");
}

static void test_plan_basic(void) {
    printf("Running basic plan test...\n");
    FleeceGoap* g = fleece_goap_create();
    register_scenario(g);

    FleeceGoapEval eval;
    fill_eval(&eval);

    FleeceGoapBlackboard bb = {0};
    setd(&bb, "battery", 10.0, false);
    setd(&bb, "scan", 0.0, false);
    setd(&bb, "foodCount", 0.0, false);
    sets(&bb, "location", "base", false);

    const char* goal_ids[] = { "gather" };
    FleeceGoapPlan* plan = fleece_goap_plan(g, &bb, goal_ids, 1, &eval);
    CHECK(plan != NULL, "plan should be found");
    if (plan) {
        CHECK(fleece_goap_plan_length(plan) == 3, "plan should have 3 actions");
        CHECK(strcmp(fleece_goap_plan_action_id(plan, 0), "deploy") == 0, "first action should be deploy");
        CHECK(strcmp(fleece_goap_plan_action_id(plan, 1), "scan") == 0, "second action should be scan");
        CHECK(strcmp(fleece_goap_plan_action_id(plan, 2), "collect") == 0, "third action should be collect");
        CHECK(fleece_goap_plan_cost(plan) == 3.0, "plan cost should be 3");
        CHECK(fleece_goap_plan_iters(plan) > 0, "plan should have iterations");
    }

    fleece_goap_plan_destroy(plan);
    fleece_goap_bb_release(&bb);
    fleece_goap_destroy(g);
    printf("Done: basic plan test\n");
}

static void test_plan_already_satisfied(void) {
    printf("Running already-satisfied plan test...\n");
    FleeceGoap* g = fleece_goap_create();
    register_scenario(g);

    FleeceGoapEval eval;
    fill_eval(&eval);

    FleeceGoapBlackboard bb = {0};
    setd(&bb, "foodCount", 2.0, false);
    sets(&bb, "location", "base", false);

    const char* goal_ids[] = { "gather" };
    FleeceGoapPlan* plan = fleece_goap_plan(g, &bb, goal_ids, 1, &eval);
    CHECK(plan != NULL, "plan should exist when goal already satisfied");
    if (plan) {
        CHECK(fleece_goap_plan_length(plan) == 0, "plan should be empty");
        CHECK(fleece_goap_plan_cost(plan) == 0.0, "empty plan cost should be 0");
    }
    fleece_goap_plan_destroy(plan);
    fleece_goap_bb_release(&bb);
    fleece_goap_destroy(g);
    printf("Done: already-satisfied plan test\n");
}

static void test_plan_no_solution(void) {
    printf("Running no-solution plan test...\n");
    FleeceGoap* g = fleece_goap_create();
    register_scenario(g);

    FleeceGoapEval eval;
    fill_eval(&eval);

    FleeceGoapBlackboard bb = {0};
    setd(&bb, "battery", 10.0, false);
    setd(&bb, "scan", 0.0, false);
    setd(&bb, "foodCount", 0.0, false);
    sets(&bb, "location", "base", false);

    // Impossible goal can never be satisfied -> no plan.
    const char* goal_ids[] = { "impossible" };
    FleeceGoapPlan* plan = fleece_goap_plan(g, &bb, goal_ids, 1, &eval);
    CHECK(plan == NULL, "no plan should be found for impossible goal");
    fleece_goap_plan_destroy(plan);

    // Unknown goal id -> no plan.
    const char* bad_ids[] = { "nope" };
    plan = fleece_goap_plan(g, &bb, bad_ids, 1, &eval);
    CHECK(plan == NULL, "no plan should be found for unknown goal id");

    fleece_goap_bb_release(&bb);
    fleece_goap_destroy(g);
    printf("Done: no-solution plan test\n");
}

static void test_plan_depth_budget(void) {
    printf("Running plan-depth budget test...\n");
    FleeceGoap* g = fleece_goap_create();
    register_scenario(g);

    FleeceGoapEval eval;
    fill_eval(&eval);

    FleeceGoapBlackboard bb = {0};
    setd(&bb, "battery", 10.0, false);
    setd(&bb, "scan", 0.0, false);
    setd(&bb, "foodCount", 0.0, false);
    sets(&bb, "location", "base", false);

    const char* goal_ids[] = { "gather" };

    // gather needs deploy->scan->collect = 3 actions; a cap of 2 forbids it.
    fleece_goap_set_max_plan_depth(g, 2);
    FleeceGoapPlan* plan = fleece_goap_plan(g, &bb, goal_ids, 1, &eval);
    CHECK(plan == NULL, "no plan within depth 2 for a 3-action goal");

    // A cap of exactly 3 admits it.
    fleece_goap_set_max_plan_depth(g, 3);
    plan = fleece_goap_plan(g, &bb, goal_ids, 1, &eval);
    CHECK(plan != NULL, "plan within depth 3");
    if (plan) CHECK(fleece_goap_plan_length(plan) == 3, "plan should have 3 actions");

    fleece_goap_plan_destroy(plan);
    fleece_goap_bb_release(&bb);
    fleece_goap_destroy(g);
    printf("Done: plan-depth budget test\n");
}

static void test_mem_usage(void) {
    printf("Running mem-usage test...\n");
    FleeceGoap* g = fleece_goap_create();
    uint32_t empty = fleece_goap_get_mem_usage(g);
    CHECK(empty > 0, "empty goap has nonzero footprint");
    register_scenario(g);
    uint32_t loaded = fleece_goap_get_mem_usage(g);
    CHECK(loaded > empty, "registering the scenario grows the footprint");
    fleece_goap_reset(g);
    uint32_t after_reset = fleece_goap_get_mem_usage(g);
    CHECK(after_reset <= loaded, "reset drops the footprint back down");
    fleece_goap_destroy(g);
    printf("Done: mem-usage test\n");
}

static void test_plan_cost_aware(void) {
    printf("Running cost-aware plan test...\n");
    FleeceGoap* g = fleece_goap_create();
    register_scenario(g);

    FleeceGoapEval eval;
    fill_eval(&eval);

    FleeceGoapBlackboard bb = {0};
    setd(&bb, "scan", 1.0, false);
    setd(&bb, "foodCount", 0.0, false);
    sets(&bb, "location", "zone", false);

    // Both collect (cost 1) and collectSlow (cost 3) achieve gather.
    const char* goal_ids[] = { "gather" };
    FleeceGoapPlan* plan = fleece_goap_plan(g, &bb, goal_ids, 1, &eval);
    CHECK(plan != NULL, "plan should be found from zone with scan");
    if (plan) {
        CHECK(fleece_goap_plan_length(plan) == 1, "plan should have 1 action");
        CHECK(strcmp(fleece_goap_plan_action_id(plan, 0), "collect") == 0,
              "planner should pick cheaper collect over collectSlow");
        CHECK(fleece_goap_plan_cost(plan) == 1.0, "plan cost should be 1");
    }
    fleece_goap_plan_destroy(plan);
    fleece_goap_bb_release(&bb);
    fleece_goap_destroy(g);
    printf("Done: cost-aware plan test\n");
}

static void test_plan_cycle_pruning(void) {
    printf("Running cycle-pruning plan test...\n");
    FleeceGoap* g = fleece_goap_create();
    register_scenario(g);

    FleeceGoapEval eval;
    fill_eval(&eval);

    FleeceGoapBlackboard bb = {0};
    setd(&bb, "battery", 10.0, false);
    setd(&bb, "scan", 0.0, false);
    setd(&bb, "foodCount", 0.0, false);
    sets(&bb, "location", "base", false);

    // deploy<->returnHome cycle must be pruned by the visited set.
    const char* goal_ids[] = { "gather" };
    FleeceGoapPlan* plan = fleece_goap_plan(g, &bb, goal_ids, 1, &eval);
    CHECK(plan != NULL, "plan should be found despite cycles");
    if (plan) {
        CHECK(fleece_goap_plan_length(plan) == 3, "cycle shouldn't inflate the plan");
    }
    fleece_goap_plan_destroy(plan);
    fleece_goap_bb_release(&bb);
    fleece_goap_destroy(g);
    printf("Done: cycle-pruning plan test\n");
}

static void test_plan_goal_targeting(void) {
    printf("Running goal-targeting plan test...\n");
    FleeceGoap* g = fleece_goap_create();
    register_scenario(g);

    FleeceGoapEval eval;
    fill_eval(&eval);

    FleeceGoapBlackboard bb = {0};
    setd(&bb, "battery", 10.0, false);
    sets(&bb, "location", "base", false);

    const char* goal_ids[] = { "inZone" };
    FleeceGoapPlan* plan = fleece_goap_plan(g, &bb, goal_ids, 1, &eval);
    CHECK(plan != NULL, "plan should be found for inZone");
    if (plan) {
        CHECK(fleece_goap_plan_length(plan) == 1, "inZone plan should be just deploy");
        CHECK(strcmp(fleece_goap_plan_action_id(plan, 0), "deploy") == 0, "plan should be deploy");
    }
    fleece_goap_plan_destroy(plan);
    fleece_goap_bb_release(&bb);
    fleece_goap_destroy(g);
    printf("Done: goal-targeting plan test\n");
}

static void test_serialization(void) {
    printf("Running serialization tests...\n");
    FleeceGoap* g = fleece_goap_create();
    register_scenario(g);

    uint8_t* blob = NULL;
    uint32_t blob_size = 0;
    CHECK(fleece_goap_serialize(g, &blob, &blob_size) == 0, "serialize should succeed");
    CHECK(blob != NULL && blob_size > 3, "blob should be produced");
    CHECK(blob != NULL && blob[0] == 'F' && blob[1] == 'P' && blob[2] == 2,
          "blob should carry 'FP2' magic prefix");

    // Deserialize into a fresh planner and compare.
    FleeceGoap* h = fleece_goap_create();
    CHECK(fleece_goap_deserialize(h, blob, blob_size) == 0, "deserialize should succeed");
    CHECK(strcmp(fleece_goap_name(h), "Scan & Collect") == 0, "name should round-trip");
    CHECK(fleece_goap_action_count(h) == 5, "actions should round-trip");
    CHECK(fleece_goap_goal_count(h) == 4, "goals should round-trip");
    CHECK(fleece_goap_mission_count(h) == 1, "missions should round-trip");

    CHECK(strcmp(fleece_goap_action_pre(h, 0, 0), "function(bb){ return bb.self.location === 'base'; }") == 0,
          "pre source should round-trip");
    CHECK(strcmp(fleece_goap_action_eff(h, 4, 0), "function(bb){ bb.self.foodCount += 1; return bb; }") == 0,
          "eff source should round-trip");
    CHECK(strcmp(fleece_goap_action_cost_source(h, 4), "function(bb){ return 3; }") == 0,
          "cost source should round-trip");
    CHECK(fleece_goap_action_dur(h, 0) == 2.0, "dur should round-trip");
    CHECK(strcmp(fleece_goap_action_exec(h, 0),
                 "function(bb, tick){ if (tick >= 2) { bb.self.location = 'zone'; bb.self.battery -= 1; return true; } return false; }") == 0,
          "exec source should round-trip");
    CHECK(strcmp(fleece_goap_goal_expr(h, 0), "function(bb){ return bb.self.foodCount >= 1; }") == 0,
          "goal expr should round-trip");
    CHECK(fleece_goap_goal_priority(h, 0) == 2.0, "goal priority should round-trip");
    CHECK(strcmp(fleece_goap_goal_curve_id(h, 0), "u2") == 0, "goal curve id should round-trip");

    // Missions.
    CHECK(fleece_goap_mission_id(h, 0) != NULL && strcmp(fleece_goap_mission_id(h, 0), "m1") == 0,
          "mission id should round-trip");
    CHECK(fleece_goap_mission_name(h, 0) != NULL && strcmp(fleece_goap_mission_name(h, 0), "Gather and RTB") == 0,
          "mission name should round-trip");
    CHECK(fleece_goap_mission_goal_count(h, 0) == 2, "mission goal count should round-trip");
    CHECK(fleece_goap_mission_goal_id(h, 0, 0) != NULL && strcmp(fleece_goap_mission_goal_id(h, 0, 0), "gather") == 0,
          "mission goal ids should round-trip");
    CHECK(fleece_goap_mission_note(h, 0) != NULL && strcmp(fleece_goap_mission_note(h, 0), "scan, collect one food, return") == 0,
          "mission note should round-trip");

    // Reserializing a deserialized planner must be byte-identical (determinism).
    uint8_t* blob2 = NULL;
    uint32_t blob2_size = 0;
    CHECK(fleece_goap_serialize(h, &blob2, &blob2_size) == 0, "reserialize should succeed");
    CHECK(blob2_size == blob_size, "reserialized blob size should match");
    CHECK(memcmp(blob, blob2, blob_size) == 0, "reserialized blob should be byte-identical");

    // Corrupt input must be rejected.
    uint8_t* corrupt = (uint8_t*)malloc(blob_size);
    memcpy(corrupt, blob, blob_size);
    corrupt[2] = 99;  // bad version
    FleeceGoap* bad = fleece_goap_create();
    CHECK(fleece_goap_deserialize(bad, corrupt, blob_size) != 0, "bad version should be rejected");
    fleece_goap_destroy(bad);
    free(corrupt);

    free(blob);
    free(blob2);
    fleece_goap_destroy(h);
    fleece_goap_destroy(g);
    printf("Done: serialization tests\n");
}

int main(void) {
    printf("=== Fleece GOAP Planner Tests ===\n");
    test_blackboard_ops();
    test_registration();
    test_utility_and_selection();
    test_plan_basic();
    test_plan_already_satisfied();
    test_plan_no_solution();
    test_plan_depth_budget();
    test_mem_usage();
    test_plan_cost_aware();
    test_plan_cycle_pruning();
    test_plan_goal_targeting();
    test_serialization();

    if (g_failures == 0) {
        printf("ALL TESTS PASSED\n");
        return 0;
    }
    printf("%d TEST(S) FAILED\n", g_failures);
    return 1;
}