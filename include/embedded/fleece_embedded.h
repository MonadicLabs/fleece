// Fleece Embedded JavaScript
// QuickJS integration for script execution

#ifndef FLEECE_EMBEDDED_H
#define FLEECE_EMBEDDED_H

#include <stdint.h>
#include <stdbool.h>

#include "quickjs.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FleeceEmbedded FleeceEmbedded;
typedef struct FleeceStateManager FleeceStateManager;
typedef struct FleecePlatform FleecePlatform;

// Create a new embedded JS instance (default libc allocator).
FleeceEmbedded* fleece_embedded_create(void);

// Create a new embedded JS instance using the supplied allocator. Pass mf == NULL
// (or zeroed functions) to use the allocator configured by
// fleece_embedded_set_allocator() - i.e. fleece_embedded_create().
// Pass an explicit JSMallocFunctions to give the QuickJS engine its own
// dedicated arena (e.g. a fixed-size static pool sized for the JS GC heap),
// separate from the fleece-side allocator.
FleeceEmbedded* fleece_embedded_create_with_allocator(const JSMallocFunctions* mf, void* opaque);

// Destroy embedded JS instance
void fleece_embedded_destroy(FleeceEmbedded* embedded);

// Bind the state manager backing the "self" (read/write, local node) and
// "swarm" (read-only, peer nodes) script globals. Call before
// fleece_embedded_register_c_functions().
int fleece_embedded_set_state_manager(FleeceEmbedded* embedded, FleeceStateManager* manager);

// Bind the platform function registry backing the "platform" script global
// (see include/platform/fleece_platform.h - fleece defines no functions of its
// own, only the registry). Optional: if never called, "platform" is empty.
// Call before fleece_embedded_register_c_functions().
int fleece_embedded_set_platform(FleeceEmbedded* embedded, FleecePlatform* platform);

// Sets the peer liveness TTL (in runtime loop ticks) used to hide dead/silent
// peers from the "swarm" script global. Optional - if never called, a
// default (FLEECE_DEFAULT_PEER_TTL_TICKS) applies. A peer not heard from
// (see fleece_state_manager_ticks_since_seen) within this many ticks is
// excluded from swarm, though its underlying data is not deleted - it
// reappears immediately once a new frame arrives. Does not affect "world"
// (shared fields have no single owner to go stale).
int fleece_embedded_set_peer_ttl_ticks(FleeceEmbedded* embedded, uint64_t ttl_ticks);

// Reads back the currently configured peer liveness TTL (the default if
// fleece_embedded_set_peer_ttl_ticks was never called). Used by
// fleece_embedded_claim_best_goal() to judge dead-peer takeover with the
// SAME liveness threshold the "swarm" script global already uses, rather
// than a second, independently-tunable notion of "alive".
uint64_t fleece_embedded_get_peer_ttl_ticks(FleeceEmbedded* embedded);

// Register the native bindings (console.log, self, swarm, platform, world)
// with the JS context. Requires fleece_embedded_set_state_manager() to have
// been called first.
int fleece_embedded_register_c_functions(FleeceEmbedded* embedded);

// Evaluate a script's source once. Top-level function declarations (e.g.
// init/step/reset/destroy) become callable globals.
int fleece_embedded_load_script(FleeceEmbedded* embedded, const char* source, const char* filename);

// Call the script-defined init()/step()/reset()/destroy() functions, if
// defined (Buzz-inspired lifecycle: https://github.com/buzz-lang/Buzz).
// A missing function is a silent no-op, not an error.
int fleece_embedded_call_init(FleeceEmbedded* embedded);
int fleece_embedded_call_step(FleeceEmbedded* embedded);
int fleece_embedded_call_reset(FleeceEmbedded* embedded);
int fleece_embedded_call_destroy(FleeceEmbedded* embedded);

// Execute an arbitrary JavaScript snippet
int fleece_embedded_execute(FleeceEmbedded* embedded, const char* script);

// Set a global value in the embedded JS context (JSON-encoded bytes)
int fleece_embedded_set_value(FleeceEmbedded* embedded, const char* name, const uint8_t* data, uint32_t size);

// Get a global value from the embedded JS context (JSON-encoded bytes)
int fleece_embedded_get_value(FleeceEmbedded* embedded, const char* name, uint8_t** data, uint32_t* size);

// Get the underlying QuickJS context (JSContext*)
void* fleece_embedded_get_context(FleeceEmbedded* embedded);

// Get the bound state manager (FleeceStateManager*), or NULL if none was set.
void* fleece_embedded_get_state_manager(FleeceEmbedded* embedded);

// --- claimBestGoal: native CBBA-over-the-CRDT claiming ---------------------
//
// A generic, mission-agnostic primitive: auctioning a shared pool of world
// records via scored compare-and-set, with dead-peer takeover and automatic
// re-assertion of a held claim so it keeps getting re-sent by the normal
// delta-gossip path (a claim written exactly once, by design, is otherwise
// never re-transmitted - found running this under load, two nodes that
// each locally win a genuine simultaneous claim race can then disagree
// indefinitely, since neither side's write ever gets a second chance at an
// LWW comparison on the other).
//
// `allow_reassert` gates ONLY the same-value CAS touch that keeps an
// already-held claim propagating - the scan/eligibility/switch logic above
// always runs. The caller MUST throttle this to true well below its own
// call frequency (goal_pool_tick() calls this every brain tick; reasserting
// on every one of those is itself a convergence bug, not a fix: a local
// reassert bumps this node's own LWW timestamp on every call, so if that
// happens faster than a round of gossip can carry a genuinely conflicting
// peer's claim back and forth, this node's own copy is ALWAYS the more
// "recent" one by the time the peer's write arrives - merge_shared() keeps
// the (correctly, by LWW rules) newer incumbent every time, and the
// conflict never gets a fair single comparison. Found live: two to three
// drones that each locally won a simultaneous claim on the same goal
// disagreed *forever*, one initial claim event each and then total silence,
// because every drone's own goal-pool tick re-touched its claim far faster
// than the batched gossip cadence could ever land a competing write while
// the local timestamp held still. Throttling reassertion to a cadence at or
// below the gossip send cadence gives an actually-conflicting peer claim a
// real window to be merged and change what `current_key` resolves to next
// tick - see goal_pool_tick()'s own throttle in fleece_goap_js.c.
//
// This is the CORE REASONING LOOP (CAS, contest margin, liveness, periodic
// re-assert) - deliberately pure C, not JS: it is the part every mission
// wanting this pattern would otherwise reimplement from scratch, and the
// part where a hand-rolled JS version hit the convergence bug above.
// SCORING one candidate is the one piece that stays a JS-authored
// heuristic (distance, priority, capability match are mission concerns
// fleece has no business knowing about): this function calls back into a
// well-known global JS function, `scoreGoal(key, record) -> number`, that
// the loaded script must define. NaN, a thrown exception, or no such
// function marks a candidate ineligible.
//
// Scans world keys with the given prefix, skips `current_key` (scored
// separately, for the "should I keep holding it" comparison), and claims
// the best eligible candidate: unclaimed, held by a peer silent for more
// than fleece_embedded_get_peer_ttl_ticks(), or held by a live peer but
// beaten by more than contest_margin. Switching away from current_key
// itself needs the same contest_margin win. Writes the resulting key (or
// empty string if nothing is held/eligible) into out_key (capacity
// out_key_cap, NUL-terminated, truncated safely if it doesn't fit).
// Returns 0 on success, -1 on a real error (no manager, no scoreGoal
// defined, bad arguments).
int fleece_embedded_claim_best_goal(FleeceEmbedded* embedded, const char* prefix,
                                     const char* current_key, double contest_margin,
                                     bool allow_reassert,
                                     char* out_key, uint32_t out_key_cap);

// Install the allocator used by the whole fleece stack - the library's own
// allocations (state-manager field values, GOAP planner A* tables,
// embedded/comms/runtime/platform structs, JS context scratch buffers) AND
// the QuickJS engine's GC heap, which is auto-wrapped into a JSMallocFunctions
// on the next fleece_embedded_create(). Defaults to libc
// malloc/calloc/realloc/free. Call before creating any fleece object to plug in
// a static pool/arena. Pass NULL for all four to reset to the defaults.
// To give the JS engine a separate dedicated arena, skip this and use
// fleece_embedded_create_with_allocator() with an explicit JSMallocFunctions
// instead.
void fleece_embedded_set_allocator(void* (*malloc_fn)(size_t),
                                   void (*free_fn)(void*),
                                   void* (*calloc_fn)(size_t, size_t),
                                   void* (*realloc_fn)(void*, size_t));

#ifdef __cplusplus
}
#endif

#endif // FLEECE_EMBEDDED_H
