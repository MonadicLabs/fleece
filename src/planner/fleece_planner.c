#define _POSIX_C_SOURCE 200809L

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "planner/fleece_planner.h"
#include "fleece_cbor.h"

// Engine-agnostic GOAP planner. The planner owns its action/goal/utility
// tables (copied on registration) and the search's per-node blackboards; the
// host owns nothing except the source strings it registered and the `start`
// blackboard it passes to fleece_goap_plan().

#define FLEECE_GOAP_DEFAULT_MAX_ITERS 2048
#define FLEECE_GOAP_DEFAULT_MAX_NODES 512
#define FLEECE_GOAP_BLACKBOARD_INIT_CAP 8

#define FLEECE_PLAN_MAGIC0 'F'
#define FLEECE_PLAN_MAGIC1 'P'
#define FLEECE_PLAN_VERSION 2

// ---------------------------------------------------------------------------
// Table structs (private)
// ---------------------------------------------------------------------------

typedef struct GoapAction {
    char id[FLEECE_GOAP_NAME_MAX];
    char name[FLEECE_GOAP_NAME_MAX];
    char cost[FLEECE_GOAP_SOURCE_MAX];
    char exec[FLEECE_GOAP_SOURCE_MAX];
    char dest[FLEECE_GOAP_NAME_MAX];
    double dur;
    char** pre;
    uint32_t pre_count;
    char** eff;
    uint32_t eff_count;
} GoapAction;

typedef struct GoapGoal {
    char id[FLEECE_GOAP_NAME_MAX];
    char name[FLEECE_GOAP_NAME_MAX];
    char expr[FLEECE_GOAP_SOURCE_MAX];
    double priority;
    char curve_id[FLEECE_GOAP_NAME_MAX];
} GoapGoal;

typedef struct GoapUtility {
    char id[FLEECE_GOAP_NAME_MAX];
    char name[FLEECE_GOAP_NAME_MAX];
    char dim[FLEECE_GOAP_NAME_MAX];
    double x_min;
    double x_max;
    FleeceGoapPoint* points;  // sorted ascending by x
    uint32_t point_count;
} GoapUtility;

typedef struct GoapMission {
    char id[FLEECE_GOAP_NAME_MAX];
    char name[FLEECE_GOAP_NAME_MAX];
    char** goal_ids;
    uint32_t goal_count;
    char note[FLEECE_GOAP_NOTE_MAX];
} GoapMission;

struct FleeceGoap {
    char name[FLEECE_GOAP_NAME_MAX];
    GoapAction* actions;
    uint32_t action_count;
    uint32_t action_cap;
    GoapGoal* goals;
    uint32_t goal_count;
    uint32_t goal_cap;
    GoapUtility* utilities;
    uint32_t utility_count;
    uint32_t utility_cap;
    GoapMission* missions;
    uint32_t mission_count;
    uint32_t mission_cap;
    uint32_t max_iters;
    uint32_t max_nodes;
    uint32_t max_plan_depth;
};

struct FleeceGoapPlan {
    char** action_ids;
    uint32_t length;
    double total_cost;
    uint32_t iters;
};

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

static void copy_str(char* dst, size_t dst_sz, const char* src) {
    if (!src) {
        dst[0] = '\0';
        return;
    }
    size_t n = strlen(src);
    if (n >= dst_sz) {
        n = dst_sz - 1;
        fprintf(stderr, "fleece: source truncated to %zu bytes (was %zu); increase FLEECE_GOAP_SOURCE_MAX\n", n, strlen(src));
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static char* dup_str(const char* src) {
    if (!src) src = "";
    size_t n = strlen(src);
    char* out = (char*)malloc(n + 1);
    if (!out) return NULL;
    memcpy(out, src, n + 1);
    return out;
}

static int ensure_cap(void** arr, uint32_t* cap, uint32_t needed, size_t elem_size) {
    if (needed <= *cap) return 0;
    uint32_t new_cap = *cap ? *cap : 4;
    while (new_cap < needed) new_cap *= 2;
    void* grown = realloc(*arr, new_cap * elem_size);
    if (!grown) return -1;
    *arr = grown;
    *cap = new_cap;
    return 0;
}

static void free_strings(char** arr, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) free(arr[i]);
    free(arr);
}

// Best-effort parse of a JSON number (e.g. "3", "3.5", "-2e3").
static bool json_to_double(const uint8_t* json, uint32_t len, double* out) {
    if (!json || len == 0 || len >= 64) return false;
    char buf[64];
    memcpy(buf, json, len);
    buf[len] = '\0';
    char* end = NULL;
    double v = strtod(buf, &end);
    if (end == buf || *end != '\0') return false;
    *out = v;
    return true;
}

// ---------------------------------------------------------------------------
// Blackboard
// ---------------------------------------------------------------------------

uint32_t fleece_goap_bb_index(const FleeceGoapBlackboard* bb, const char* name) {
    if (!bb || !name) return UINT32_MAX;
    for (uint32_t i = 0; i < bb->count; i++) {
        if (strcmp(bb->fields[i].name, name) == 0) return i;
    }
    return UINT32_MAX;
}

const uint8_t* fleece_goap_bb_get(const FleeceGoapBlackboard* bb, const char* name, uint32_t* size) {
    if (size) *size = 0;
    uint32_t idx = fleece_goap_bb_index(bb, name);
    if (idx == UINT32_MAX) return NULL;
    if (size) *size = bb->fields[idx].size;
    return bb->fields[idx].data;
}

int fleece_goap_bb_set(FleeceGoapBlackboard* bb, const char* name, const uint8_t* data, uint32_t size, bool is_shared) {
    if (!bb || !name || !data || size == 0) return -1;

    uint32_t idx = fleece_goap_bb_index(bb, name);
    if (idx == UINT32_MAX) {
        if (ensure_cap((void**)&bb->fields, &bb->cap, bb->count + 1, sizeof(FleeceGoapField)) != 0) return -1;
        idx = bb->count++;
        copy_str(bb->fields[idx].name, sizeof(bb->fields[idx].name), name);
        bb->fields[idx].data = NULL;
        bb->fields[idx].size = 0;
        bb->fields[idx].is_shared = is_shared;
    }

    uint8_t* copy = (uint8_t*)malloc(size);
    if (!copy) return -1;
    memcpy(copy, data, size);
    free(bb->fields[idx].data);
    bb->fields[idx].data = copy;
    bb->fields[idx].size = size;
    bb->fields[idx].is_shared = is_shared;
    return 0;
}

int fleece_goap_bb_delete(FleeceGoapBlackboard* bb, const char* name) {
    if (!bb || !name) return -1;
    uint32_t idx = fleece_goap_bb_index(bb, name);
    if (idx == UINT32_MAX) return 0;
    free(bb->fields[idx].data);
    for (uint32_t i = idx; i + 1 < bb->count; i++) {
        bb->fields[i] = bb->fields[i + 1];
    }
    bb->count--;
    return 0;
}

int fleece_goap_bb_clone(const FleeceGoapBlackboard* src, FleeceGoapBlackboard* dst) {
    if (!src || !dst) return -1;
    dst->fields = NULL;
    dst->count = 0;
    dst->cap = 0;
    for (uint32_t i = 0; i < src->count; i++) {
        if (fleece_goap_bb_set(dst, src->fields[i].name, src->fields[i].data, src->fields[i].size, src->fields[i].is_shared) != 0) {
            fleece_goap_bb_release(dst);
            return -1;
        }
    }
    return 0;
}

void fleece_goap_bb_release(FleeceGoapBlackboard* bb) {
    if (!bb) return;
    for (uint32_t i = 0; i < bb->count; i++) free(bb->fields[i].data);
    free(bb->fields);
    bb->fields = NULL;
    bb->count = 0;
    bb->cap = 0;
}

// Deterministic state key: field name + 0x1F + JSON bytes, fields sorted by
// name. Caller frees. Returns NULL on OOM.
static char* make_state_key(const FleeceGoapBlackboard* bb, uint32_t* out_len) {
    *out_len = 0;
    const FleeceGoapField** sorted = (const FleeceGoapField**)malloc((bb->count ? bb->count : 1) * sizeof(const FleeceGoapField*));
    if (!sorted) return NULL;
    for (uint32_t i = 0; i < bb->count; i++) sorted[i] = &bb->fields[i];
    for (uint32_t i = 1; i < bb->count; i++) {
        const FleeceGoapField* key = sorted[i];
        int32_t j = (int32_t)i - 1;
        while (j >= 0 && strcmp(sorted[j]->name, key->name) > 0) {
            sorted[j + 1] = sorted[j];
            j--;
        }
        sorted[j + 1] = key;
    }

    size_t total = 0;
    for (uint32_t i = 0; i < bb->count; i++) total += 2 + strlen(sorted[i]->name) + 1 + sorted[i]->size;  // ns prefix + name + sep + json
    char* buf = (char*)malloc(total ? total : 1);
    if (!buf) {
        free(sorted);
        return NULL;
    }
    size_t p = 0;
    for (uint32_t i = 0; i < bb->count; i++) {
        buf[p++] = sorted[i]->is_shared ? 'w' : 's';  // world vs self namespace
        buf[p++] = ':';
        size_t nlen = strlen(sorted[i]->name);
        memcpy(&buf[p], sorted[i]->name, nlen);
        p += nlen;
        buf[p++] = 0x1F;
        if (sorted[i]->size > 0) {
            memcpy(&buf[p], sorted[i]->data, sorted[i]->size);
            p += sorted[i]->size;
        }
    }
    free(sorted);
    *out_len = (uint32_t)p;
    return buf;
}

// ---------------------------------------------------------------------------
// Visited-state set (open-addressing hash set of state keys)
// ---------------------------------------------------------------------------

typedef struct VisitedEntry {
    uint64_t hash;
    uint8_t* key;
    uint32_t key_len;
    bool used;
} VisitedEntry;

typedef struct VisitedSet {
    VisitedEntry* slots;
    uint32_t cap;
    uint32_t count;
} VisitedSet;

static uint64_t fnv1a64(const uint8_t* data, uint32_t len) {
    uint64_t h = 1469598103934665603ULL;
    for (uint32_t i = 0; i < len; i++) {
        h ^= data[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static int visited_init(VisitedSet* set, uint32_t max_nodes) {
    set->cap = max_nodes * 2 + 1;
    set->count = 0;
    set->slots = (VisitedEntry*)calloc(set->cap, sizeof(VisitedEntry));
    return set->slots ? 0 : -1;
}

static void visited_free(VisitedSet* set) {
    if (!set->slots) return;
    for (uint32_t i = 0; i < set->cap; i++) {
        if (set->slots[i].used) free(set->slots[i].key);
    }
    free(set->slots);
    set->slots = NULL;
}

// Returns true if `key` was newly inserted (set now owns it), false if it was
// already present (caller keeps ownership of `key`).
static bool visited_add(VisitedSet* set, uint8_t* key, uint32_t key_len) {
    uint64_t h = fnv1a64(key, key_len);
    uint32_t slot = (uint32_t)(h % set->cap);
    for (uint32_t probe = 0; probe < set->cap; probe++) {
        uint32_t i = (slot + probe) % set->cap;
        if (!set->slots[i].used) {
            set->slots[i].hash = h;
            set->slots[i].key = key;
            set->slots[i].key_len = key_len;
            set->slots[i].used = true;
            set->count++;
            return true;
        }
        if (set->slots[i].hash == h && set->slots[i].key_len == key_len &&
            memcmp(set->slots[i].key, key, key_len) == 0) {
            return false;
        }
    }
    return false;  // table full (shouldn't happen: cap = 2*max_nodes + 1)
}

// ---------------------------------------------------------------------------
// A* search
// ---------------------------------------------------------------------------

typedef struct SearchNode {
    FleeceGoapBlackboard bb;
    uint32_t parent;      // index into the node list, or UINT32_MAX for root
    uint32_t action_idx;  // action applied to reach this node, or UINT32_MAX
    uint32_t depth;       // actions taken to reach this node (root = 0)
    double g;
    double f;
} SearchNode;

typedef struct NodeList {
    SearchNode* items;
    uint32_t count;
    uint32_t cap;
} NodeList;

typedef struct HeapEntry {
    double f;
    uint32_t idx;
} HeapEntry;

typedef struct Heap {
    HeapEntry* items;
    uint32_t len;
    uint32_t cap;
} Heap;

static int nodes_push(NodeList* list, const SearchNode* node) {
    if (ensure_cap((void**)&list->items, &list->cap, list->count + 1, sizeof(SearchNode)) != 0) return -1;
    list->items[list->count++] = *node;
    return 0;
}

static int heap_push(Heap* heap, uint32_t idx, double f) {
    if (heap->len == heap->cap) {
        uint32_t new_cap = heap->cap ? heap->cap * 2 : 16;
        HeapEntry* grown = (HeapEntry*)realloc(heap->items, new_cap * sizeof(HeapEntry));
        if (!grown) return -1;
        heap->items = grown;
        heap->cap = new_cap;
    }
    uint32_t i = heap->len++;
    while (i > 0) {
        uint32_t parent = (i - 1) / 2;
        if (heap->items[parent].f <= f) break;
        heap->items[i] = heap->items[parent];
        i = parent;
    }
    heap->items[i].f = f;
    heap->items[i].idx = idx;
    return 0;
}

static bool heap_pop(Heap* heap, uint32_t* idx) {
    if (heap->len == 0) return false;
    *idx = heap->items[0].idx;
    HeapEntry last = heap->items[--heap->len];
    uint32_t i = 0;
    while (i * 2 + 1 < heap->len) {
        uint32_t l = i * 2 + 1;
        uint32_t r = l + 1;
        uint32_t m = (r < heap->len && heap->items[r].f < heap->items[l].f) ? r : l;
        if (heap->items[m].f >= last.f) break;
        heap->items[i] = heap->items[m];
        i = m;
    }
    heap->items[i] = last;
    return true;
}

static void release_nodes(NodeList* list) {
    for (uint32_t i = 0; i < list->count; i++) fleece_goap_bb_release(&list->items[i].bb);
    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->cap = 0;
}

// ---------------------------------------------------------------------------
// Utility / goal selection
// ---------------------------------------------------------------------------

static double curve_score(const GoapUtility* u, double v) {
    if (u->point_count == 0) return 0;
    const FleeceGoapPoint* pts = u->points;
    if (v <= pts[0].x) return pts[0].y;
    if (v >= pts[u->point_count - 1].x) return pts[u->point_count - 1].y;
    for (uint32_t i = 0; i + 1 < u->point_count; i++) {
        const FleeceGoapPoint* a = &pts[i];
        const FleeceGoapPoint* b = &pts[i + 1];
        if (v >= a->x && v <= b->x) {
            double k = (v - a->x) / ((b->x - a->x) != 0.0 ? (b->x - a->x) : 1.0);
            return a->y + (b->y - a->y) * k;
        }
    }
    return 0;
}

double fleece_goap_goal_utility(const FleeceGoap* goap, uint32_t goal_idx, const FleeceGoapBlackboard* bb) {
    if (!goap || goal_idx >= goap->goal_count) return 0;
    const GoapGoal* g = &goap->goals[goal_idx];

    const GoapUtility* u = NULL;
    if (g->curve_id[0] != '\0') {
        for (uint32_t i = 0; i < goap->utility_count; i++) {
            if (strcmp(goap->utilities[i].id, g->curve_id) == 0) {
                u = &goap->utilities[i];
                break;
            }
        }
    }

    double score;
    if (u && u->dim[0] != '\0') {
        uint32_t fsize = 0;
        const uint8_t* f = fleece_goap_bb_get(bb, u->dim, &fsize);
        if (f) {
            double v;
            score = json_to_double(f, fsize, &v) ? curve_score(u, v) : 0.0;
        } else {
            score = u->point_count ? u->points[0].y : 1.0;
        }
    } else if (u) {
        score = u->point_count ? u->points[0].y : 1.0;
    } else {
        score = 1.0;
    }
    return score * g->priority;
}

int fleece_goap_goal_satisfied(const FleeceGoap* goap, uint32_t goal_idx, const FleeceGoapBlackboard* bb,
                               const FleeceGoapEval* eval, bool* out) {
    *out = false;
    if (!goap || !eval || !eval->goal_satisfied || goal_idx >= goap->goal_count) return -1;
    return eval->goal_satisfied(eval->user_data, goal_idx, bb, out);
}

int fleece_goap_select_goal(FleeceGoap* goap, const FleeceGoapBlackboard* bb, const FleeceGoapEval* eval, double threshold) {
    if (!goap || !eval || !eval->goal_satisfied) return -1;
    int best = -1;
    double best_score = threshold;
    for (uint32_t g = 0; g < goap->goal_count; g++) {
        bool sat = false;
        if (eval->goal_satisfied(eval->user_data, g, bb, &sat) != 0 || sat) continue;
        double s = fleece_goap_goal_utility(goap, g, bb);
        if (s > best_score) {
            best_score = s;
            best = (int)g;
        }
    }
    return best;
}

// Rank unsatisfied goals by utility, highest first, into out[] (capped at
// cap). Goals with utility <= threshold (or satisfied) are skipped. Returns
// the number written. Unlike fleece_goap_select_goal this does not stop at the
// single best goal: callers that need a fallback when a top goal cannot be
// planned (e.g. a brain that should degrade to "recharge" when "forage" is
// currently unplannable) can walk the list in order.
uint32_t fleece_goap_rank_goals(FleeceGoap* goap, const FleeceGoapBlackboard* bb,
                                const FleeceGoapEval* eval, double threshold,
                                uint32_t* out, uint32_t cap) {
    if (!goap || !eval || !eval->goal_satisfied || !out || cap == 0) return 0;

    uint32_t n = 0;
    for (uint32_t g = 0; g < goap->goal_count && n < cap; g++) {
        bool sat = false;
        if (eval->goal_satisfied(eval->user_data, g, bb, &sat) != 0 || sat) continue;
        if (fleece_goap_goal_utility(goap, g, bb) <= threshold) continue;
        out[n++] = g;
    }

    // Insertion sort by utility descending (goal sets are tiny - N^2 is fine).
    for (uint32_t i = 1; i < n; i++) {
        uint32_t key = out[i];
        double kscore = fleece_goap_goal_utility(goap, key, bb);
        int32_t j = (int32_t)i - 1;
        while (j >= 0 && fleece_goap_goal_utility(goap, out[(uint32_t)j], bb) < kscore) {
            out[(uint32_t)j + 1] = out[(uint32_t)j];
            j--;
        }
        out[(uint32_t)j + 1] = key;
    }
    return n;
}

// ---------------------------------------------------------------------------
// Planning
// ---------------------------------------------------------------------------

static bool goals_met(const FleeceGoap* goap, const uint32_t* targets, uint32_t n_targets,
                      const FleeceGoapBlackboard* bb, const FleeceGoapEval* eval) {
    if (!eval || !eval->goal_satisfied) return false;
    for (uint32_t t = 0; t < n_targets; t++) {
        bool sat = false;
        if (eval->goal_satisfied(eval->user_data, targets[t], bb, &sat) != 0 || !sat) return false;
    }
    return true;
}

static uint32_t count_unsatisfied(const FleeceGoap* goap, const uint32_t* targets, uint32_t n_targets,
                                  const FleeceGoapBlackboard* bb, const FleeceGoapEval* eval) {
    uint32_t h = 0;
    if (!eval || !eval->goal_satisfied) return n_targets;
    for (uint32_t t = 0; t < n_targets; t++) {
        bool sat = false;
        if (eval->goal_satisfied(eval->user_data, targets[t], bb, &sat) != 0 || !sat) h++;
    }
    return h;
}

static double action_cost_for(const FleeceGoap* goap, uint32_t action_idx,
                              const FleeceGoapBlackboard* bb, const FleeceGoapEval* eval) {
    if (goap->actions[action_idx].cost[0] == '\0') return 1.0;
    if (eval && eval->action_cost) {
        double c = 1.0;
        if (eval->action_cost(eval->user_data, action_idx, bb, &c) == 0 && !(c < 0.0)) return c;
    }
    return 1.0;
}

FleeceGoapPlan* fleece_goap_plan(FleeceGoap* goap, const FleeceGoapBlackboard* start,
                                 const char** goal_ids, uint32_t n_goals, const FleeceGoapEval* eval) {
    if (!goap || !start || !eval || !eval->action_pre || !eval->action_apply || !eval->goal_satisfied) return NULL;

    // Resolve the goal set: explicit ids, or all goals.
    uint32_t* targets = NULL;
    uint32_t n_targets = 0;
    if (goal_ids && n_goals > 0) {
        targets = (uint32_t*)malloc(n_goals * sizeof(uint32_t));
        if (!targets) return NULL;
        for (uint32_t i = 0; i < n_goals; i++) {
            uint32_t idx = fleece_goap_find_goal(goap, goal_ids[i]);
            if (idx == UINT32_MAX) {
                free(targets);
                return NULL;
            }
            targets[i] = idx;
        }
        n_targets = n_goals;
    } else {
        n_targets = goap->goal_count;
        if (n_targets > 0) {
            targets = (uint32_t*)malloc(n_targets * sizeof(uint32_t));
            if (!targets) return NULL;
            for (uint32_t i = 0; i < n_targets; i++) targets[i] = i;
        }
    }

    FleeceGoapPlan* plan = NULL;
    NodeList nodes = {0};
    Heap heap = {0};
    VisitedSet visited = {0};
    uint32_t iters = 0;

    if (visited_init(&visited, goap->max_nodes) != 0) goto out;

    // Root node: clone of `start`.
    SearchNode root;
    root.parent = UINT32_MAX;
    root.action_idx = UINT32_MAX;
    root.depth = 0;
    root.g = 0.0;
    if (fleece_goap_bb_clone(start, &root.bb) != 0) goto out;
    root.f = (double)count_unsatisfied(goap, targets, n_targets, &root.bb, eval);
    if (nodes_push(&nodes, &root) != 0) {
        fleece_goap_bb_release(&root.bb);
        goto out;
    }
    uint32_t root_key_len = 0;
    char* root_key = make_state_key(&root.bb, &root_key_len);
    if (!root_key) goto out;
    visited_add(&visited, (uint8_t*)root_key, root_key_len);  // set owns it
    heap_push(&heap, 0, root.f);

    // A*: pop lowest f, goal-test on pop, expand applicable actions.
    while (nodes.count <= goap->max_nodes && iters < goap->max_iters) {
        uint32_t node_idx;
        if (!heap_pop(&heap, &node_idx)) break;
        iters++;
        SearchNode* node = &nodes.items[node_idx];

        if (goals_met(goap, targets, n_targets, &node->bb, eval)) {
            plan = (FleeceGoapPlan*)calloc(1, sizeof(FleeceGoapPlan));
            if (!plan) goto out;
            plan->iters = iters;
            plan->total_cost = node->g;

            // Walk parents goal -> root, collecting action indices in reverse.
            uint32_t depth = 0;
            uint32_t cur = node_idx;
            while (cur != UINT32_MAX) {
                depth++;
                cur = nodes.items[cur].parent;
            }
            uint32_t n_actions = depth - 1;
            uint32_t* rev = (uint32_t*)malloc(depth * sizeof(uint32_t));
            if (!rev) goto out;
            cur = node_idx;
            uint32_t k = 0;
            while (cur != UINT32_MAX) {
                rev[k++] = nodes.items[cur].action_idx;
                cur = nodes.items[cur].parent;
            }
            plan->action_ids = (char**)malloc((n_actions ? n_actions : 1) * sizeof(char*));
            if (!plan->action_ids) {
                free(rev);
                goto out;
            }
            for (uint32_t i = 0; i < n_actions; i++) {
                uint32_t ai = rev[k - 2 - i];
                plan->action_ids[i] = dup_str(goap->actions[ai].id);
                if (!plan->action_ids[i]) {
                    for (uint32_t j = 0; j < i; j++) free(plan->action_ids[j]);
                    free(plan->action_ids);
                    free(rev);
                    goto out;
                }
            }
            plan->length = n_actions;
            free(rev);
            goto out;
        }

        for (uint32_t a = 0; a < goap->action_count; a++) {
            bool pre_ok = true;
            if (eval->action_pre(eval->user_data, a, &node->bb, &pre_ok) != 0) pre_ok = false;
            if (!pre_ok) continue;

            double c = action_cost_for(goap, a, &node->bb, eval);

            FleeceGoapBlackboard dst = {0};
            if (eval->action_apply(eval->user_data, a, &node->bb, &dst) != 0) {
                fleece_goap_bb_release(&dst);
                continue;
            }

            uint32_t key_len = 0;
            char* key = make_state_key(&dst, &key_len);
            if (!key) {
                fleece_goap_bb_release(&dst);
                goto out;
            }
            if (!visited_add(&visited, (uint8_t*)key, key_len)) {
                free(key);
                fleece_goap_bb_release(&dst);
                continue;
            }

        SearchNode next;
        next.bb = dst;  // ownership transferred
        next.parent = node_idx;
        next.action_idx = a;
        next.depth = node->depth + 1;
        next.g = node->g + c;
        next.f = next.g + (double)count_unsatisfied(goap, targets, n_targets, &dst, eval);
        // Depth cap: a plan longer than max_plan_depth is never synthesized.
        // We still expand a full-depth node (it may be a valid goal of exactly
        // max_plan_depth actions), but its successors would exceed the cap.
        if (next.depth > goap->max_plan_depth) {
            fleece_goap_bb_release(&dst);
            continue;
        }
            if (nodes_push(&nodes, &next) != 0) {
                fleece_goap_bb_release(&dst);
                goto out;
            }
            node = &nodes.items[node_idx];  // nodes_push may have realloc'd items
            heap_push(&heap, nodes.count - 1, next.f);
        }
    }
    // No plan found within the search budget.

out:
    release_nodes(&nodes);
    free(heap.items);
    visited_free(&visited);
    free(targets);
    return plan;
}

uint32_t fleece_goap_plan_length(const FleeceGoapPlan* plan) {
    return plan ? plan->length : 0;
}

const char* fleece_goap_plan_action_id(const FleeceGoapPlan* plan, uint32_t i) {
    if (!plan || i >= plan->length) return NULL;
    return plan->action_ids[i];
}

double fleece_goap_plan_cost(const FleeceGoapPlan* plan) {
    return plan ? plan->total_cost : 0.0;
}

uint32_t fleece_goap_plan_iters(const FleeceGoapPlan* plan) {
    return plan ? plan->iters : 0;
}

void fleece_goap_plan_destroy(FleeceGoapPlan* plan) {
    if (!plan) return;
    for (uint32_t i = 0; i < plan->length; i++) free(plan->action_ids[i]);
    free(plan->action_ids);
    free(plan);
}

// ---------------------------------------------------------------------------
// Lifecycle / registration
// ---------------------------------------------------------------------------

FleeceGoap* fleece_goap_create(void) {
    FleeceGoap* goap = (FleeceGoap*)calloc(1, sizeof(FleeceGoap));
    if (!goap) return NULL;
    goap->max_iters = FLEECE_GOAP_DEFAULT_MAX_ITERS;
    goap->max_nodes = FLEECE_GOAP_DEFAULT_MAX_NODES;
    goap->max_plan_depth = FLEECE_GOAP_DEFAULT_MAX_PLAN_DEPTH;
    return goap;
}

void fleece_goap_reset(FleeceGoap* goap) {
    if (!goap) return;
    for (uint32_t i = 0; i < goap->action_count; i++) {
        free_strings(goap->actions[i].pre, goap->actions[i].pre_count);
        free_strings(goap->actions[i].eff, goap->actions[i].eff_count);
    }
    free(goap->actions);
    free(goap->goals);
    for (uint32_t i = 0; i < goap->utility_count; i++) free(goap->utilities[i].points);
    free(goap->utilities);
    for (uint32_t i = 0; i < goap->mission_count; i++) free_strings(goap->missions[i].goal_ids, goap->missions[i].goal_count);
    free(goap->missions);
    goap->actions = NULL;
    goap->action_count = 0;
    goap->action_cap = 0;
    goap->goals = NULL;
    goap->goal_count = 0;
    goap->goal_cap = 0;
    goap->utilities = NULL;
    goap->utility_count = 0;
    goap->utility_cap = 0;
    goap->missions = NULL;
    goap->mission_count = 0;
    goap->mission_cap = 0;
    goap->name[0] = '\0';
}

void fleece_goap_destroy(FleeceGoap* goap) {
    if (!goap) return;
    fleece_goap_reset(goap);
    free(goap);
}

const char* fleece_goap_name(const FleeceGoap* goap) {
    return goap ? goap->name : NULL;
}

int fleece_goap_set_name(FleeceGoap* goap, const char* name) {
    if (!goap) return -1;
    copy_str(goap->name, sizeof(goap->name), name);
    return 0;
}

void fleece_goap_set_max_iters(FleeceGoap* goap, uint32_t max_iters) {
    if (goap && max_iters > 0) goap->max_iters = max_iters;
}

void fleece_goap_set_max_nodes(FleeceGoap* goap, uint32_t max_nodes) {
    if (goap && max_nodes > 0) goap->max_nodes = max_nodes;
}

void fleece_goap_set_max_plan_depth(FleeceGoap* goap, uint32_t max_depth) {
    if (goap && max_depth > 0) goap->max_plan_depth = max_depth;
}

uint32_t fleece_goap_get_mem_usage(const FleeceGoap* goap) {
    if (!goap) return 0;
    uint32_t bytes = (uint32_t)sizeof(FleeceGoap);

    bytes += goap->action_cap * (uint32_t)sizeof(GoapAction);
    for (uint32_t i = 0; i < goap->action_count; i++) {
        bytes += goap->actions[i].pre_count * (uint32_t)sizeof(char*);
        for (uint32_t j = 0; j < goap->actions[i].pre_count; j++)
            if (goap->actions[i].pre[j]) bytes += (uint32_t)(strlen(goap->actions[i].pre[j]) + 1);
        bytes += goap->actions[i].eff_count * (uint32_t)sizeof(char*);
        for (uint32_t j = 0; j < goap->actions[i].eff_count; j++)
            if (goap->actions[i].eff[j]) bytes += (uint32_t)(strlen(goap->actions[i].eff[j]) + 1);
    }

    bytes += goap->goal_cap * (uint32_t)sizeof(GoapGoal);

    bytes += goap->utility_cap * (uint32_t)sizeof(GoapUtility);
    for (uint32_t i = 0; i < goap->utility_count; i++)
        bytes += goap->utilities[i].point_count * (uint32_t)sizeof(FleeceGoapPoint);

    bytes += goap->mission_cap * (uint32_t)sizeof(GoapMission);
    for (uint32_t i = 0; i < goap->mission_count; i++) {
        bytes += goap->missions[i].goal_count * (uint32_t)sizeof(char*);
        for (uint32_t j = 0; j < goap->missions[i].goal_count; j++)
            if (goap->missions[i].goal_ids[j]) bytes += (uint32_t)(strlen(goap->missions[i].goal_ids[j]) + 1);
    }
    return bytes;
}

int fleece_goap_add_action(FleeceGoap* goap, const FleeceGoapActionDef* def) {
    if (!goap || !def || !def->id) return -1;
    if (ensure_cap((void**)&goap->actions, &goap->action_cap, goap->action_count + 1, sizeof(GoapAction)) != 0) return -1;
    GoapAction* a = &goap->actions[goap->action_count];
    memset(a, 0, sizeof(*a));
    copy_str(a->id, sizeof(a->id), def->id);
    copy_str(a->name, sizeof(a->name), def->name);
    copy_str(a->cost, sizeof(a->cost), def->cost);
    copy_str(a->exec, sizeof(a->exec), def->exec);
    copy_str(a->dest, sizeof(a->dest), def->dest);
    a->dur = def->dur;

    if (def->pre_count > 0 && def->pre) {
        a->pre = (char**)calloc(def->pre_count, sizeof(char*));
        if (!a->pre) goto fail;
        for (uint32_t i = 0; i < def->pre_count; i++) {
            a->pre[i] = dup_str(def->pre[i]);
            if (!a->pre[i]) goto fail;
        }
        a->pre_count = def->pre_count;
    }
    if (def->eff_count > 0 && def->eff) {
        a->eff = (char**)calloc(def->eff_count, sizeof(char*));
        if (!a->eff) goto fail;
        for (uint32_t i = 0; i < def->eff_count; i++) {
            a->eff[i] = dup_str(def->eff[i]);
            if (!a->eff[i]) goto fail;
        }
        a->eff_count = def->eff_count;
    }
    goap->action_count++;
    return 0;

fail:
    free_strings(a->pre, a->pre_count);
    free_strings(a->eff, a->eff_count);
    return -1;
}

int fleece_goap_add_goal(FleeceGoap* goap, const FleeceGoapGoalDef* def) {
    if (!goap || !def || !def->id) return -1;
    if (ensure_cap((void**)&goap->goals, &goap->goal_cap, goap->goal_count + 1, sizeof(GoapGoal)) != 0) return -1;
    GoapGoal* g = &goap->goals[goap->goal_count++];
    memset(g, 0, sizeof(*g));
    copy_str(g->id, sizeof(g->id), def->id);
    copy_str(g->name, sizeof(g->name), def->name);
    copy_str(g->expr, sizeof(g->expr), def->expr);
    copy_str(g->curve_id, sizeof(g->curve_id), def->curve_id);
    g->priority = def->priority;
    return 0;
}

int fleece_goap_add_utility(FleeceGoap* goap, const FleeceGoapUtilityDef* def) {
    if (!goap || !def || !def->id) return -1;
    if (ensure_cap((void**)&goap->utilities, &goap->utility_cap, goap->utility_count + 1, sizeof(GoapUtility)) != 0) return -1;
    GoapUtility* u = &goap->utilities[goap->utility_count];
    memset(u, 0, sizeof(*u));
    copy_str(u->id, sizeof(u->id), def->id);
    copy_str(u->name, sizeof(u->name), def->name);
    copy_str(u->dim, sizeof(u->dim), def->dim);
    u->x_min = def->x_min;
    u->x_max = def->x_max;

    if (def->point_count > 0 && def->points) {
        u->points = (FleeceGoapPoint*)malloc(def->point_count * sizeof(FleeceGoapPoint));
        if (!u->points) return -1;
        memcpy(u->points, def->points, def->point_count * sizeof(FleeceGoapPoint));
        u->point_count = def->point_count;
        // Sort ascending by x (insertion sort; curves are tiny).
        for (uint32_t i = 1; i < u->point_count; i++) {
            FleeceGoapPoint key = u->points[i];
            int32_t j = (int32_t)i - 1;
            while (j >= 0 && u->points[j].x > key.x) {
                u->points[j + 1] = u->points[j];
                j--;
            }
            u->points[j + 1] = key;
        }
    }
    goap->utility_count++;
    return 0;
}

int fleece_goap_add_mission(FleeceGoap* goap, const FleeceGoapMissionDef* def) {
    if (!goap || !def || !def->id) return -1;
    if (ensure_cap((void**)&goap->missions, &goap->mission_cap, goap->mission_count + 1, sizeof(GoapMission)) != 0) return -1;
    GoapMission* m = &goap->missions[goap->mission_count];
    memset(m, 0, sizeof(*m));
    copy_str(m->id, sizeof(m->id), def->id);
    copy_str(m->name, sizeof(m->name), def->name);
    copy_str(m->note, sizeof(m->note), def->note);
    if (def->goal_count > 0 && def->goal_ids) {
        m->goal_ids = (char**)calloc(def->goal_count, sizeof(char*));
        if (!m->goal_ids) return -1;
        for (uint32_t i = 0; i < def->goal_count; i++) {
            m->goal_ids[i] = dup_str(def->goal_ids[i]);
            if (!m->goal_ids[i]) {
                free_strings(m->goal_ids, i);
                return -1;
            }
        }
        m->goal_count = def->goal_count;
    }
    goap->mission_count++;
    return 0;
}

// ---------------------------------------------------------------------------
// Introspection
// ---------------------------------------------------------------------------

uint32_t fleece_goap_action_count(const FleeceGoap* goap) { return goap ? goap->action_count : 0; }
const char* fleece_goap_action_id(const FleeceGoap* goap, uint32_t idx) {
    return (goap && idx < goap->action_count) ? goap->actions[idx].id : NULL;
}
uint32_t fleece_goap_action_pre_count(const FleeceGoap* goap, uint32_t idx) {
    return (goap && idx < goap->action_count) ? goap->actions[idx].pre_count : 0;
}
const char* fleece_goap_action_pre(const FleeceGoap* goap, uint32_t idx, uint32_t i) {
    return (goap && idx < goap->action_count && i < goap->actions[idx].pre_count) ? goap->actions[idx].pre[i] : NULL;
}
uint32_t fleece_goap_action_eff_count(const FleeceGoap* goap, uint32_t idx) {
    return (goap && idx < goap->action_count) ? goap->actions[idx].eff_count : 0;
}
const char* fleece_goap_action_eff(const FleeceGoap* goap, uint32_t idx, uint32_t i) {
    return (goap && idx < goap->action_count && i < goap->actions[idx].eff_count) ? goap->actions[idx].eff[i] : NULL;
}
const char* fleece_goap_action_cost_source(const FleeceGoap* goap, uint32_t idx) {
    return (goap && idx < goap->action_count) ? goap->actions[idx].cost : NULL;
}
const char* fleece_goap_action_exec(const FleeceGoap* goap, uint32_t idx) {
    return (goap && idx < goap->action_count) ? goap->actions[idx].exec : NULL;
}
const char* fleece_goap_action_dest(const FleeceGoap* goap, uint32_t idx) {
    return (goap && idx < goap->action_count) ? goap->actions[idx].dest : NULL;
}
double fleece_goap_action_dur(const FleeceGoap* goap, uint32_t idx) {
    return (goap && idx < goap->action_count) ? goap->actions[idx].dur : 0.0;
}
uint32_t fleece_goap_goal_count(const FleeceGoap* goap) { return goap ? goap->goal_count : 0; }
const char* fleece_goap_goal_id(const FleeceGoap* goap, uint32_t idx) {
    return (goap && idx < goap->goal_count) ? goap->goals[idx].id : NULL;
}
const char* fleece_goap_goal_expr(const FleeceGoap* goap, uint32_t idx) {
    return (goap && idx < goap->goal_count) ? goap->goals[idx].expr : NULL;
}
double fleece_goap_goal_priority(const FleeceGoap* goap, uint32_t idx) {
    return (goap && idx < goap->goal_count) ? goap->goals[idx].priority : 0.0;
}
const char* fleece_goap_goal_curve_id(const FleeceGoap* goap, uint32_t idx) {
    return (goap && idx < goap->goal_count) ? goap->goals[idx].curve_id : NULL;
}
uint32_t fleece_goap_find_action(const FleeceGoap* goap, const char* id) {
    if (!goap || !id) return UINT32_MAX;
    for (uint32_t i = 0; i < goap->action_count; i++) {
        if (strcmp(goap->actions[i].id, id) == 0) return i;
    }
    return UINT32_MAX;
}
uint32_t fleece_goap_find_goal(const FleeceGoap* goap, const char* id) {
    if (!goap || !id) return UINT32_MAX;
    for (uint32_t i = 0; i < goap->goal_count; i++) {
        if (strcmp(goap->goals[i].id, id) == 0) return i;
    }
    return UINT32_MAX;
}

uint32_t fleece_goap_mission_count(const FleeceGoap* goap) {
    return goap ? goap->mission_count : 0;
}

const char* fleece_goap_mission_id(const FleeceGoap* goap, uint32_t idx) {
    if (!goap || idx >= goap->mission_count) return NULL;
    return goap->missions[idx].id;
}

const char* fleece_goap_mission_name(const FleeceGoap* goap, uint32_t idx) {
    if (!goap || idx >= goap->mission_count) return NULL;
    return goap->missions[idx].name;
}

uint32_t fleece_goap_mission_goal_count(const FleeceGoap* goap, uint32_t idx) {
    if (!goap || idx >= goap->mission_count) return 0;
    return goap->missions[idx].goal_count;
}

const char* fleece_goap_mission_goal_id(const FleeceGoap* goap, uint32_t idx, uint32_t i) {
    if (!goap || idx >= goap->mission_count || i >= goap->missions[idx].goal_count) return NULL;
    return goap->missions[idx].goal_ids[i];
}

const char* fleece_goap_mission_note(const FleeceGoap* goap, uint32_t idx) {
    if (!goap || idx >= goap->mission_count) return NULL;
    return goap->missions[idx].note;
}

// ---------------------------------------------------------------------------
// Serialization (plan blob: 3-byte 'F','P',version prefix + CBOR body)
// ---------------------------------------------------------------------------

static size_t text_size(const char* s) { return fleece_cbor_text_size((uint32_t)strlen(s ? s : "")); }

static size_t serialize_body_size(const FleeceGoap* g) {
    size_t s = fleece_cbor_array_header_size(5);
    s += text_size(g->name);

    s += fleece_cbor_array_header_size(g->action_count);
    for (uint32_t i = 0; i < g->action_count; i++) {
        const GoapAction* a = &g->actions[i];
        s += fleece_cbor_array_header_size(8);
        s += text_size(a->id) + text_size(a->name) + text_size(a->cost) + text_size(a->dest);
        s += fleece_cbor_num_size(a->dur);
        s += text_size(a->exec);
        s += fleece_cbor_array_header_size(a->pre_count);
        for (uint32_t j = 0; j < a->pre_count; j++) s += text_size(a->pre[j]);
        s += fleece_cbor_array_header_size(a->eff_count);
        for (uint32_t j = 0; j < a->eff_count; j++) s += text_size(a->eff[j]);
    }

    s += fleece_cbor_array_header_size(g->goal_count);
    for (uint32_t i = 0; i < g->goal_count; i++) {
        const GoapGoal* gl = &g->goals[i];
        s += fleece_cbor_array_header_size(5);
        s += text_size(gl->id) + text_size(gl->name) + text_size(gl->expr);
        s += fleece_cbor_num_size(gl->priority);
        s += text_size(gl->curve_id);
    }

    s += fleece_cbor_array_header_size(g->utility_count);
    for (uint32_t i = 0; i < g->utility_count; i++) {
        const GoapUtility* u = &g->utilities[i];
        s += fleece_cbor_array_header_size(7);
        s += text_size(u->id) + text_size(u->name) + text_size(u->dim);
        s += fleece_cbor_num_size(u->x_min) + fleece_cbor_num_size(u->x_max);
        s += fleece_cbor_array_header_size(u->point_count);
        for (uint32_t j = 0; j < u->point_count; j++) {
            s += fleece_cbor_array_header_size(2);
            s += fleece_cbor_num_size(u->points[j].x) + fleece_cbor_num_size(u->points[j].y);
        }
    }

    s += fleece_cbor_array_header_size(g->mission_count);
    for (uint32_t i = 0; i < g->mission_count; i++) {
        const GoapMission* m = &g->missions[i];
        s += fleece_cbor_array_header_size(4);
        s += text_size(m->id) + text_size(m->name);
        s += fleece_cbor_array_header_size(m->goal_count);
        for (uint32_t j = 0; j < m->goal_count; j++) s += text_size(m->goal_ids[j]);
        s += text_size(m->note);
    }
    return s;
}

static void write_text(uint8_t* buf, size_t* pos, const char* s) {
    fleece_cbor_write_text(buf, pos, s ? s : "", (uint32_t)strlen(s ? s : ""));
}

static void serialize_body(const FleeceGoap* g, uint8_t* buf, size_t* pos) {
    fleece_cbor_write_array_header(buf, pos, 5);
    write_text(buf, pos, g->name);

    fleece_cbor_write_array_header(buf, pos, g->action_count);
    for (uint32_t i = 0; i < g->action_count; i++) {
        const GoapAction* a = &g->actions[i];
        fleece_cbor_write_array_header(buf, pos, 8);
        write_text(buf, pos, a->id);
        write_text(buf, pos, a->name);
        write_text(buf, pos, a->cost);
        write_text(buf, pos, a->dest);
        fleece_cbor_write_num(buf, pos, a->dur);
        write_text(buf, pos, a->exec);
        fleece_cbor_write_array_header(buf, pos, a->pre_count);
        for (uint32_t j = 0; j < a->pre_count; j++) write_text(buf, pos, a->pre[j]);
        fleece_cbor_write_array_header(buf, pos, a->eff_count);
        for (uint32_t j = 0; j < a->eff_count; j++) write_text(buf, pos, a->eff[j]);
    }

    fleece_cbor_write_array_header(buf, pos, g->goal_count);
    for (uint32_t i = 0; i < g->goal_count; i++) {
        const GoapGoal* gl = &g->goals[i];
        fleece_cbor_write_array_header(buf, pos, 5);
        write_text(buf, pos, gl->id);
        write_text(buf, pos, gl->name);
        write_text(buf, pos, gl->expr);
        fleece_cbor_write_num(buf, pos, gl->priority);
        write_text(buf, pos, gl->curve_id);
    }

    fleece_cbor_write_array_header(buf, pos, g->utility_count);
    for (uint32_t i = 0; i < g->utility_count; i++) {
        const GoapUtility* u = &g->utilities[i];
        fleece_cbor_write_array_header(buf, pos, 7);
        write_text(buf, pos, u->id);
        write_text(buf, pos, u->name);
        write_text(buf, pos, u->dim);
        fleece_cbor_write_num(buf, pos, u->x_min);
        fleece_cbor_write_num(buf, pos, u->x_max);
        fleece_cbor_write_array_header(buf, pos, u->point_count);
        for (uint32_t j = 0; j < u->point_count; j++) {
            fleece_cbor_write_array_header(buf, pos, 2);
            fleece_cbor_write_num(buf, pos, u->points[j].x);
            fleece_cbor_write_num(buf, pos, u->points[j].y);
        }
    }

    fleece_cbor_write_array_header(buf, pos, g->mission_count);
    for (uint32_t i = 0; i < g->mission_count; i++) {
        const GoapMission* m = &g->missions[i];
        fleece_cbor_write_array_header(buf, pos, 4);
        write_text(buf, pos, m->id);
        write_text(buf, pos, m->name);
        fleece_cbor_write_array_header(buf, pos, m->goal_count);
        for (uint32_t j = 0; j < m->goal_count; j++) write_text(buf, pos, m->goal_ids[j]);
        write_text(buf, pos, m->note);
    }
}

int fleece_goap_serialize(const FleeceGoap* goap, uint8_t** blob, uint32_t* blob_size) {
    if (!goap || !blob || !blob_size) return -1;
    size_t body = serialize_body_size(goap);
    uint8_t* buf = (uint8_t*)malloc(3 + body);
    if (!buf) return -1;
    size_t pos = 0;
    buf[pos++] = FLEECE_PLAN_MAGIC0;
    buf[pos++] = FLEECE_PLAN_MAGIC1;
    buf[pos++] = FLEECE_PLAN_VERSION;
    serialize_body(goap, buf, &pos);
    *blob = buf;
    *blob_size = (uint32_t)pos;
    return 0;
}

// --- deserialize helpers ---

static bool read_text(const uint8_t* buf, uint32_t size, size_t* pos, char* out, uint32_t out_max) {
    uint8_t major;
    uint64_t value;
    if (!fleece_cbor_read_head(buf, size, pos, &major, &value)) return false;
    if (major != 3) return false;
    if (!fleece_cbor_bounds_ok(*pos, value, size)) return false;
    uint32_t copy = value < (uint64_t)(out_max - 1) ? (uint32_t)value : out_max - 1;
    memcpy(out, &buf[*pos], copy);
    out[copy] = '\0';
    *pos += value;
    return true;
}

static char* read_text_dyn(const uint8_t* buf, uint32_t size, size_t* pos) {
    uint8_t major;
    uint64_t value;
    if (!fleece_cbor_read_head(buf, size, pos, &major, &value)) return NULL;
    if (major != 3) return NULL;
    if (!fleece_cbor_bounds_ok(*pos, value, size)) return NULL;
    char* out = (char*)malloc((size_t)value + 1);
    if (!out) return NULL;
    memcpy(out, &buf[*pos], value);
    out[value] = '\0';
    *pos += value;
    return out;
}

int fleece_goap_deserialize(FleeceGoap* goap, const uint8_t* blob, uint32_t blob_size) {
    if (!goap || !blob || blob_size < 3) return -1;
    if (blob[0] != FLEECE_PLAN_MAGIC0 || blob[1] != FLEECE_PLAN_MAGIC1 || blob[2] != FLEECE_PLAN_VERSION) return -1;

    size_t pos = 3;
    uint8_t major;
    uint64_t value;

    if (!fleece_cbor_read_head(blob, blob_size, &pos, &major, &value) || major != 4 || value != 5) return -1;

    fleece_goap_reset(goap);

    char* s1 = NULL;
    char* s2 = NULL;
    char* s3 = NULL;
    char* s4 = NULL;
    char* s5 = NULL;

    // name
    if (!read_text(blob, blob_size, &pos, goap->name, sizeof(goap->name))) return -1;

    // actions
    if (!fleece_cbor_read_head(blob, blob_size, &pos, &major, &value) || major != 4) return -1;
    uint64_t n_actions = value;
    for (uint64_t i = 0; i < n_actions; i++) {
        if (!fleece_cbor_read_head(blob, blob_size, &pos, &major, &value) || major != 4 || value != 8) goto fail;
        s1 = read_text_dyn(blob, blob_size, &pos);
        s2 = read_text_dyn(blob, blob_size, &pos);
        s3 = read_text_dyn(blob, blob_size, &pos);
        s4 = read_text_dyn(blob, blob_size, &pos);
        if (!s1 || !s2 || !s3 || !s4) goto fail;
        FleeceGoapActionDef def;
        memset(&def, 0, sizeof(def));
        def.id = s1;
        def.name = s2;
        def.cost = s3;
        def.dest = s4;
        if (!fleece_cbor_read_num(blob, blob_size, &pos, &def.dur)) goto fail;
        s5 = read_text_dyn(blob, blob_size, &pos);
        if (!s5) goto fail;
        def.exec = s5;
        if (!fleece_cbor_read_head(blob, blob_size, &pos, &major, &value) || major != 4) goto fail;
        char** pre = NULL;
        uint32_t pre_count = 0;
        if (value > 0) {
            pre = (char**)calloc((size_t)value, sizeof(char*));
            if (!pre) goto fail;
            for (uint64_t j = 0; j < value; j++) {
                pre[j] = read_text_dyn(blob, blob_size, &pos);
                if (!pre[j]) {
                    free_strings(pre, (uint32_t)j);
                    pre = NULL;
                    goto fail;
                }
            }
            pre_count = (uint32_t)value;
        }
        if (!fleece_cbor_read_head(blob, blob_size, &pos, &major, &value) || major != 4) {
            free_strings(pre, pre_count);
            pre = NULL;
            goto fail;
        }
        char** eff = NULL;
        uint32_t eff_count = 0;
        if (value > 0) {
            eff = (char**)calloc((size_t)value, sizeof(char*));
            if (!eff) {
                free_strings(pre, pre_count);
                pre = NULL;
                goto fail;
            }
            for (uint64_t j = 0; j < value; j++) {
                eff[j] = read_text_dyn(blob, blob_size, &pos);
                if (!eff[j]) {
                    free_strings(eff, (uint32_t)j);
                    eff = NULL;
                    free_strings(pre, pre_count);
                    pre = NULL;
                    goto fail;
                }
            }
            eff_count = (uint32_t)value;
        }
        def.pre = (const char**)pre;
        def.pre_count = pre_count;
        def.eff = (const char**)eff;
        def.eff_count = eff_count;
        if (fleece_goap_add_action(goap, &def) != 0) {
            free_strings(pre, pre_count);
            free_strings(eff, eff_count);
            pre = eff = NULL;
            goto fail;
        }
        free_strings(pre, pre_count);
        free_strings(eff, eff_count);
        free(s1); free(s2); free(s3); free(s4); free(s5);
        s1 = s2 = s3 = s4 = s5 = NULL;
    }

    // goals
    if (!fleece_cbor_read_head(blob, blob_size, &pos, &major, &value) || major != 4) goto fail;
    uint64_t n_goals = value;
    for (uint64_t i = 0; i < n_goals; i++) {
        if (!fleece_cbor_read_head(blob, blob_size, &pos, &major, &value) || major != 4 || value != 5) goto fail;
        s1 = read_text_dyn(blob, blob_size, &pos);
        s2 = read_text_dyn(blob, blob_size, &pos);
        s3 = read_text_dyn(blob, blob_size, &pos);
        if (!s1 || !s2 || !s3) goto fail;
        FleeceGoapGoalDef def;
        memset(&def, 0, sizeof(def));
        def.id = s1;
        def.name = s2;
        def.expr = s3;
        if (!fleece_cbor_read_num(blob, blob_size, &pos, &def.priority)) goto fail;
        s4 = read_text_dyn(blob, blob_size, &pos);
        if (!s4) goto fail;
        def.curve_id = s4;
        if (fleece_goap_add_goal(goap, &def) != 0) goto fail;
        free(s1); free(s2); free(s3); free(s4);
        s1 = s2 = s3 = s4 = NULL;
    }

    // utilities
    if (!fleece_cbor_read_head(blob, blob_size, &pos, &major, &value) || major != 4) goto fail;
    uint64_t n_utils = value;
    for (uint64_t i = 0; i < n_utils; i++) {
        if (!fleece_cbor_read_head(blob, blob_size, &pos, &major, &value) || major != 4 || value != 7) goto fail;
        s1 = read_text_dyn(blob, blob_size, &pos);
        s2 = read_text_dyn(blob, blob_size, &pos);
        s3 = read_text_dyn(blob, blob_size, &pos);
        if (!s1 || !s2 || !s3) goto fail;
        FleeceGoapUtilityDef def;
        memset(&def, 0, sizeof(def));
        def.id = s1;
        def.name = s2;
        def.dim = s3;
        if (!fleece_cbor_read_num(blob, blob_size, &pos, &def.x_min)) goto fail;
        if (!fleece_cbor_read_num(blob, blob_size, &pos, &def.x_max)) goto fail;
        if (!fleece_cbor_read_head(blob, blob_size, &pos, &major, &value) || major != 4) goto fail;
        FleeceGoapPoint* pts = NULL;
        uint32_t point_count = 0;
        if (value > 0) {
            uint64_t n_pts = value;
            pts = (FleeceGoapPoint*)malloc((size_t)n_pts * sizeof(FleeceGoapPoint));
            if (!pts) goto fail;
            for (uint64_t j = 0; j < n_pts; j++) {
                if (!fleece_cbor_read_head(blob, blob_size, &pos, &major, &value) || major != 4 || value != 2 ||
                    !fleece_cbor_read_num(blob, blob_size, &pos, &pts[j].x) ||
                    !fleece_cbor_read_num(blob, blob_size, &pos, &pts[j].y)) {
                    free(pts);
                    goto fail;
                }
            }
            point_count = (uint32_t)n_pts;
        }
        def.points = pts;
        def.point_count = point_count;
        if (fleece_goap_add_utility(goap, &def) != 0) {
            free(pts);
            goto fail;
        }
        free(pts);
        free(s1); free(s2); free(s3);
        s1 = s2 = s3 = NULL;
    }

    // missions
    if (!fleece_cbor_read_head(blob, blob_size, &pos, &major, &value) || major != 4) goto fail;
    uint64_t n_missions = value;
    for (uint64_t i = 0; i < n_missions; i++) {
        if (!fleece_cbor_read_head(blob, blob_size, &pos, &major, &value) || major != 4 || value != 4) goto fail;
        s1 = read_text_dyn(blob, blob_size, &pos);
        s2 = read_text_dyn(blob, blob_size, &pos);
        if (!s1 || !s2) goto fail;
        FleeceGoapMissionDef def;
        memset(&def, 0, sizeof(def));
        def.id = s1;
        def.name = s2;
        if (!fleece_cbor_read_head(blob, blob_size, &pos, &major, &value) || major != 4) goto fail;
        char** gids = NULL;
        uint32_t gid_count = 0;
        if (value > 0) {
            gids = (char**)calloc((size_t)value, sizeof(char*));
            if (!gids) goto fail;
            for (uint64_t j = 0; j < value; j++) {
                gids[j] = read_text_dyn(blob, blob_size, &pos);
                if (!gids[j]) {
                    free_strings(gids, (uint32_t)j);
                    gids = NULL;
                    goto fail;
                }
            }
            gid_count = (uint32_t)value;
        }
        s3 = read_text_dyn(blob, blob_size, &pos);
        if (!s3) {
            free_strings(gids, gid_count);
            gids = NULL;
            goto fail;
        }
        def.goal_ids = (const char**)gids;
        def.goal_count = gid_count;
        def.note = s3;
        if (fleece_goap_add_mission(goap, &def) != 0) {
            free_strings(gids, gid_count);
            free(s3);
            gids = NULL;
            s3 = NULL;
            goto fail;
        }
        free_strings(gids, gid_count);
        free(s1); free(s2); free(s3);
        s1 = s2 = s3 = NULL;
    }

    free(s1); free(s2); free(s3); free(s4); free(s5);
    return 0;

fail:
    free(s1); free(s2); free(s3); free(s4); free(s5);
    return -1;
}
