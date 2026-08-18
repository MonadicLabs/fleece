// Fleece Example 4: GOAP-driven unit - plan -> select action -> execute action
// -> when done, re-execute planning.
//
// ALL behavior is done through GOAP - there is no script. The behavior-loop
// "brain" (see embedded/fleece_goap_js.h) owns every decision:
//
//   each tick: snapshot self/world -> pick highest-utility unsatisfied goal ->
//   plan for it -> run the plan's first action's exec() body (committing its
//   work each tick so the world sees progress) -> when it reports done, replan.
//
// The planner's action/goal/cost tables hold JS FUNCTION SOURCES (pre, eff,
// goal expr, cost); QuickJS evaluates them via the bridge (still JS - but only
// to author the functions). An action's exec() is its real runtime body - the
// eff is only the PLANNER's heuristic for what the action is expected to do and
// is never applied by the executor. A tiny C tick callback supplies the
// environment (passive battery drain, and consuming the food once the brain has
// delivered it home recharged, which re-opens the forage goal and keeps the
// cycle going).
//
// Scenario: a forager robot at its base with a battery.
//   - "forage"   (foodCount >= 1):        deploy to the zone, collect food.
//   - "recharge" (battery >= 95):         return home, rest to top up.
// The shared world.foodTotal counter accumulates across cycles (visible to
// peers via the usual world gossip).
//
// Single agent, no transport needed (the default comms backend is a
// simulation). Runs for a fixed number of ticks then exits:
//   ./example4_goap
//
// Keep an eye on the [brain] lines: they show the full
// plan -> execute -> replan loop in action.

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>

#include "runtime/fleece_runtime.h"
#include "comms/fleece_comms.h"
#include "state/fleece_state_manager.h"
#include "planner/fleece_planner.h"
#include "embedded/fleece_goap_js.h"

#define RUN_TICKS 70  // ~100ms each; enough for several full forage/recharge cycles

#define BATTERY_DRAIN 0.12   // passive drain per tick
#define RECHARGE_GOAL 95.0   // battery target at which the recharge goal reads satisfied
#define LOG_EVERY 1         // log the local view every N ticks

// --- Environment simulation (was the script's step(), now plain C) --------

typedef struct ForagerEnv {
    uint64_t tick;
    uint64_t self_id;
} ForagerEnv;

static bool env_get_string(FleeceStateManager* sm, uint64_t owner, const char* name, char* out, size_t cap) {
    uint8_t* data = NULL;
    uint32_t size = 0;
    if (fleece_state_manager_get_named(sm, owner, name, &data, &size) != 0 || !data || size >= cap) {
        free(data);
        return false;
    }
    memcpy(out, data, size);
    out[size] = '\0';
    free(data);
    return true;
}

static bool env_get_double(FleeceStateManager* sm, uint64_t owner, const char* name, double* out) {
    char buf[48];
    if (!env_get_string(sm, owner, name, buf, sizeof(buf))) return false;
    char* end = NULL;
    *out = strtod(buf, &end);
    return end != buf && *end == '\0';
}

static void env_set_double(FleeceStateManager* sm, const char* name, double v) {
    char buf[48];
    int n = snprintf(buf, sizeof(buf), "%.17g", v);
    fleece_state_manager_set_named(sm, name, (const uint8_t*)buf, (uint32_t)n);
}

// Strip surrounding double quotes from a JSON string value (state manager
// stores JSON-encoded bytes, e.g. `"base"` for the string base).
static void env_unquote(char* s) {
    size_t n = strlen(s);
    if (n >= 2 && s[0] == '"' && s[n - 1] == '"') {
        memmove(s, s + 1, n - 2);
        s[n - 2] = '\0';
    }
}

// Read the shared world.foodTotal counter as a string.
static void env_world_food(FleeceStateManager* sm, char* out, size_t cap) {
    uint8_t* ft = NULL;
    uint32_t ft_sz = 0;
    if (fleece_state_manager_get_named(sm, FLEECE_SHARED_OWNER_ID, "foodTotal", &ft, &ft_sz) == 0 && ft && ft_sz < cap) {
        memcpy(out, ft, ft_sz);
        out[ft_sz] = '\0';
    } else {
        snprintf(out, cap, "0");
    }
    free(ft);
}

// Once per tick: drain the battery, and if the brain has collected food AND
// brought us home recharged, "store" it (deliver/spend it) - resetting
// foodCount makes the forage goal unsatisfied again so the brain starts a new
// cycle. The shared world.foodTotal counter keeps growing across cycles.
static void env_tick(FleeceRuntime* runtime, void* user_data) {
    ForagerEnv* env = (ForagerEnv*)user_data;
    FleeceStateManager* sm = (FleeceStateManager*)fleece_runtime_get_state_manager(runtime);
    env->tick++;

    double battery = 100.0;
    env_get_double(sm, env->self_id, "battery", &battery);
    battery -= BATTERY_DRAIN;
    if (battery < 0.0) battery = 0.0;
    env_set_double(sm, "battery", battery);

    char location[64] = "base";
    env_get_string(sm, env->self_id, "location", location, sizeof(location));
    env_unquote(location);
    double food = 0.0;
    env_get_double(sm, env->self_id, "foodCount", &food);

    char world[16] = "0";
    env_world_food(sm, world, sizeof(world));

    if (strcmp(location, "base") == 0 && food >= 1.0 && battery >= RECHARGE_GOAL) {
        printf("[env]  tick %-3llu stored food (world.foodTotal = %s) - foraging again\n",
               (unsigned long long)env->tick, world);
        env_set_double(sm, "foodCount", 0.0);
    }

    if (env->tick % LOG_EVERY == 0) {
        printf("[env]  tick %-3llu loc %s bat %.1f food %.0f worldFood %s\n",
               (unsigned long long)env->tick, location, battery, food, world);
    }
}

// --- GOAP scenario: JS-authored function sources -------------------------

static void register_scenario(FleeceGoap* g) {
    fleece_goap_set_name(g, "Forager");

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
    a.dest = "zone";
    a.exec = "function(bb, tick){ if (tick >= 3) { bb.self.location = 'zone'; bb.self.battery -= 5; return true; } return false; }";
    fleece_goap_add_action(g, &a);

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
    fleece_goap_add_action(g, &a);

    const char* pre_home[] = {
        "function(bb){ return bb.self.foodCount >= 1 || bb.self.battery < 30; }"
    };
    const char* eff_home[] = {
        "function(bb){ bb.self.location = 'base'; bb.self.battery -= 5; return bb; }"
    };
    memset(&a, 0, sizeof(a));
    a.id = "home"; a.name = "Return Home";
    a.pre = pre_home; a.pre_count = 1;
    a.eff = eff_home; a.eff_count = 1;
    a.dest = "base";
    a.exec = "function(bb, tick){ if (tick >= 3) { bb.self.location = 'base'; bb.self.battery -= 5; return true; } return false; }";
    fleece_goap_add_action(g, &a);

    const char* pre_rest[] = {
        "function(bb){ return bb.self.location === 'base'; }"
    };
    const char* eff_rest[] = {
        "function(bb){ bb.self.battery = 100; return bb; }"
    };
    memset(&a, 0, sizeof(a));
    a.id = "rest"; a.name = "Rest at Base";
    a.pre = pre_rest; a.pre_count = 1;
    a.eff = eff_rest; a.eff_count = 1;
    a.exec = "function(bb, tick){ if (tick >= 2) { bb.self.battery = 100; return true; } return false; }";
    fleece_goap_add_action(g, &a);

    // Goals with utility curves so selection shifts with state.
    FleeceGoapGoalDef goal = {0};
    goal.id = "forage";
    goal.name = "Forage 1 food";
    goal.expr = "function(bb){ return bb.self.foodCount >= 1; }";
    goal.priority = 2.0;
    goal.curve_id = "uFood";
    fleece_goap_add_goal(g, &goal);

    goal.id = "recharge";
    goal.name = "Battery topped up";
    goal.expr = "function(bb){ return bb.self.battery >= 95; }";
    goal.priority = 2.0;
    goal.curve_id = "uBat";
    fleece_goap_add_goal(g, &goal);

    FleeceGoapUtilityDef u = {0};
    FleeceGoapPoint u_food[] = { {0, 1.0}, {1, 0.0} };
    u.id = "uFood"; u.name = "Hunger";
    u.dim = "foodCount"; u.x_min = 0; u.x_max = 1;
    u.points = u_food; u.point_count = 2;
    fleece_goap_add_utility(g, &u);

    FleeceGoapPoint u_bat[] = { {0, 1.0}, {100, 0.0} };
    u.id = "uBat"; u.name = "Battery urgency";
    u.dim = "battery"; u.x_min = 0; u.x_max = 100;
    u.points = u_bat; u.point_count = 2;
    fleece_goap_add_utility(g, &u);
}

// Log the brain's decisions as they happen.
static void brain_event(FleeceGoapBrain* brain, FleeceGoapBrainEvent event, void* user_data) {
    (void)user_data;
    const char* goal = fleece_goap_brain_goal_id(brain);
    const char* action = fleece_goap_brain_action_id(brain);
    switch (event) {
        case FLEECE_GOAP_BRAIN_EVENT_REPLAN:
            printf("[brain] tick %-3u goal '%s' -> execute '%s' (progress %u)\n",
                   fleece_goap_brain_tick_count(brain),
                   goal ? goal : "(none)",
                   action ? action : "(none)",
                   fleece_goap_brain_action_progress(brain));
            break;
        case FLEECE_GOAP_BRAIN_EVENT_ACTION_DONE:
            printf("[brain] tick %-3u action complete, replanning...\n",
                   fleece_goap_brain_tick_count(brain));
            break;
        case FLEECE_GOAP_BRAIN_EVENT_IDLE:
            printf("[brain] tick %-3u idle - all goals satisfied\n",
                   fleece_goap_brain_tick_count(brain));
            break;
    }
}

// Stop the runtime after RUN_TICKS so the demo self-terminates.
static void* watchdog(void* ud) {
    FleeceRuntime* runtime = (FleeceRuntime*)ud;
    struct timespec ts = {.tv_sec = 0, .tv_nsec = 100000000};  // 100ms
    for (int i = 0; i < RUN_TICKS; i++) {
        nanosleep(&ts, NULL);
        if (!fleece_runtime_is_running(runtime)) break;
    }
    fleece_runtime_stop(runtime);
    return NULL;
}

int main(int argc, char** argv) {
    printf("Fleece Example 4: GOAP-driven Unit\n");
    printf("==================================\n\n");

    FleeceRuntime* runtime = fleece_runtime_create();
    if (!runtime) {
        fprintf(stderr, "Failed to create runtime\n");
        return 1;
    }

    FleeceComms* comms = (FleeceComms*)fleece_runtime_get_comms(runtime);
    if (comms && fleece_comms_initialize(comms) != 0) {
        fprintf(stderr, "Failed to initialize comms\n");
        fleece_runtime_destroy(runtime);
        return 1;
    }

    // Seed the live state: unit at base, full battery, no food yet.
    FleeceStateManager* sm = (FleeceStateManager*)fleece_runtime_get_state_manager(runtime);
    uint64_t self_id = fleece_state_manager_get_node_id(sm);
    fleece_state_manager_set_named(sm, "location", (const uint8_t*)"\"base\"", 6);
    fleece_state_manager_set_named(sm, "battery", (const uint8_t*)"100", 3);
    fleece_state_manager_set_named(sm, "foodCount", (const uint8_t*)"0", 1);
    fleece_state_manager_set_shared(sm, "foodTotal", (const uint8_t*)"0", 1);

    // Environment simulation (sensors/consumption) - pure C, no script.
    ForagerEnv env = { .self_id = self_id };
    fleece_runtime_set_tick_callback(runtime, env_tick, &env);

    // Build the GOAP tables (JS-authored function sources) and attach the brain.
    FleeceGoap* goap = fleece_goap_create();
    if (!goap) {
        fprintf(stderr, "Failed to create goap planner\n");
        if (comms) fleece_comms_close(comms);
        fleece_runtime_destroy(runtime);
        return 1;
    }
    register_scenario(goap);

    if (fleece_runtime_set_goap(runtime, goap) != 0) {
        fprintf(stderr, "Failed to attach goap brain\n");
        fleece_goap_destroy(goap);
        if (comms) fleece_comms_close(comms);
        fleece_runtime_destroy(runtime);
        return 1;
    }
    FleeceGoapBrain* brain = (FleeceGoapBrain*)fleece_runtime_get_goap_brain(runtime);
    fleece_goap_brain_set_event_callback(brain, brain_event, NULL);

    pthread_t wd;
    if (pthread_create(&wd, NULL, watchdog, runtime) != 0) {
        fprintf(stderr, "Failed to start watchdog thread\n");
    }

    printf("Starting runtime (runs %d ticks then exits)...\n\n", RUN_TICKS);
    int result = fleece_runtime_start(runtime);

    if (pthread_join(wd, NULL) != 0) {
        pthread_detach(wd);
    }

    fleece_goap_destroy(goap);
    if (comms) fleece_comms_close(comms);
    fleece_runtime_destroy(runtime);

    return result;
}
