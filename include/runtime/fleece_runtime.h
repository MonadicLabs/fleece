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

// --- Transport-agnostic inbound hooks ---------------------------------------
//
// The built-in comms path delivers frames through fleece_comms_set_receive_callback.
// Transports that merge or route inbound traffic themselves (e.g. the Reticulum
// module, whose gossip aspect merges inside the module and whose control aspect
// arrives via a separate callback) use these entry points instead, so gap
// detection and the targeted-repair handshake work identically regardless of
// which transport delivered the bytes.

// Feed one inbound GOSSIP wire frame (a ['F']['G'] CBOR frame) that was NOT
// delivered through FleeceComms - e.g. merged by an out-of-band transport.
// Runs the normal import plus resync bookkeeping under the pseudo-source
// "mesh". Do NOT also feed the same frame through comms' receive path.
void fleece_runtime_on_gossip_frame(FleeceRuntime* runtime, const uint8_t* data, uint32_t size);

// Feed one inbound CONTROL frame (an ['F']['X'] index/value-request frame).
void fleece_runtime_on_control_frame(FleeceRuntime* runtime, const uint8_t* data, uint32_t size);

// Report the transport's view-digest status after merging a peer's gossip:
// behind_shared == true means this node is missing something the sender holds
// (schedule a targeted repair), false means current. Under the built-in comms
// path this bookkeeping happens automatically; transports that merge in-band
// call this instead.
void fleece_runtime_note_behind_shared(FleeceRuntime* runtime, bool behind_shared);

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

// Swaps in a new GOAP table while the runtime keeps running - the safe way
// to switch missions (e.g. one just received and fleece_goap_deserialize'd
// over the mesh), unlike fleece_goap_reset()'ing the SAME table a running
// brain still holds cached action/goal indices into (that dangles them; do
// not do that). This works because it builds an entirely NEW brain from
// scratch (fresh goal_idx/action_idx/plan/tick_count) rather than mutating
// the live one, so nothing from the old mission's in-flight plan carries
// over - callable any number of times, first attach or replace alike (does
// NOT require fleece_runtime_set_goap() to have been called first). Any
// event/world-model/divergence/max-ticks/cooldown callbacks a caller wants
// on the new brain must be re-registered - a fresh FleeceGoapBrain starts
// with none of the old brain's callbacks. The caller retains ownership of
// both the old and new `goap` (this never destroys either) and must keep
// `new_goap` alive until the runtime is destroyed or replaced again.
int fleece_runtime_replace_goap(FleeceRuntime* runtime, struct FleeceGoap* new_goap);

#ifdef __cplusplus
}
#endif

#endif // FLEECE_RUNTIME_H
