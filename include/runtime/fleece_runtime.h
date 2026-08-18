// Fleece Runtime Interface
// Core runtime for swarm coordination

#ifndef FLEECE_RUNTIME_H
#define FLEECE_RUNTIME_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FleeceRuntime FleeceRuntime;
struct FleeceGoap;

// Create a new runtime instance (local node id defaults to a fixed placeholder -
// see fleece_state_manager_create)
FleeceRuntime* fleece_runtime_create(void);

// Create a new runtime instance with an explicit local node id. Needed to run
// more than one real node id in the same process, or to give a node a stable
// identity across restarts (e.g. derived from a CLI argument or hardware id)
// instead of the shared placeholder every fleece_runtime_create() instance
// otherwise gets. Returns NULL if node_id == FLEECE_SHARED_OWNER_ID (see
// fleece_state_manager_create_with_node_id).
FleeceRuntime* fleece_runtime_create_with_node_id(uint64_t node_id);

// Destroy runtime instance
void fleece_runtime_destroy(FleeceRuntime* runtime);

// Load a script's source. Top-level function declarations (init/step/reset/destroy)
// become the lifecycle hooks fleece_runtime_start() calls (Buzz-inspired:
// https://github.com/buzz-lang/Buzz). Call before fleece_runtime_start().
int fleece_runtime_load_script(FleeceRuntime* runtime, const char* source);

// Register a C function invoked once per main-loop tick, before the script's
// step() and the GOAP brain, so it can mutate state (sensors/environment) that
// the rest of the loop observes the same tick. Useful when no script is loaded
// and the whole behavior loop runs in C (e.g. a pure-GOAP example). May be
// NULL to clear. Call before fleece_runtime_start().
void fleece_runtime_set_tick_callback(FleeceRuntime* runtime,
                                      void (*cb)(FleeceRuntime* runtime, void* user_data),
                                      void* user_data);

// Sets the peer liveness TTL (in loop ticks) used to hide dead/silent peers
// from the "swarm" script global. Optional - defaults to
// FLEECE_DEFAULT_PEER_TTL_TICKS (see fleece_embedded.h) if never called.
// Call before fleece_runtime_start().
int fleece_runtime_set_peer_ttl_ticks(FleeceRuntime* runtime, uint64_t ttl_ticks);

// Start the runtime's main loop
int fleece_runtime_start(FleeceRuntime* runtime);

// Stop the runtime's main loop
void fleece_runtime_stop(FleeceRuntime* runtime);

// Get runtime state
bool fleece_runtime_is_running(FleeceRuntime* runtime);

// Execute a JavaScript script
int fleece_runtime_execute_script(FleeceRuntime* runtime, const char* script);

// Get the state manager from runtime
void* fleece_runtime_get_state_manager(FleeceRuntime* runtime);

// Get the comms interface from runtime
void* fleece_runtime_get_comms(FleeceRuntime* runtime);

// Get the embedded JS engine from runtime
void* fleece_runtime_get_embedded(FleeceRuntime* runtime);

// Get the platform function registry from runtime (see include/platform/fleece_platform.h).
// Register hardware-specific functions on it (fleece_platform_register) before
// fleece_runtime_start() to make them callable from script as platform.<name>(...).
void* fleece_runtime_get_platform(FleeceRuntime* runtime);

// Attach a GOAP planner (see planner/fleece_planner.h and
// embedded/fleece_goap_js.h). The runtime builds a behavior-loop "brain"
// (plan -> select action -> execute over its `dur` ticks -> replan) driven once
// per main-loop tick after the script's step(). Call before
// fleece_runtime_start(). The caller retains ownership of `goap` and must keep
// it alive until the runtime is destroyed.
int fleece_runtime_set_goap(FleeceRuntime* runtime, struct FleeceGoap* goap);

// Get the attached GOAP brain (FleeceGoapBrain*, or NULL if none was set) -
// e.g. to register a decision event callback (fleece_goap_brain_set_event_callback)
// before fleece_runtime_start().
void* fleece_runtime_get_goap_brain(FleeceRuntime* runtime);

#ifdef __cplusplus
}
#endif

#endif // FLEECE_RUNTIME_H
