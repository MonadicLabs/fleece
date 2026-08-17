// Fleece Runtime Interface
// Core runtime for swarm coordination

#ifndef FLEECE_RUNTIME_H
#define FLEECE_RUNTIME_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Runtime state type
    typedef struct FleeceRuntime FleeceRuntime;

// Create a new runtime instance
FleeceRuntime* fleece_runtime_create(void);

// Destroy runtime instance
void fleece_runtime_destroy(FleeceRuntime* runtime);

// Load a script's source. Top-level function declarations (init/step/reset/destroy)
// become the lifecycle hooks fleece_runtime_start() calls (Buzz-inspired:
// https://github.com/buzz-lang/Buzz). Call before fleece_runtime_start().
int fleece_runtime_load_script(FleeceRuntime* runtime, const char* source);

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

#ifdef __cplusplus
}
#endif

#endif // FLEECE_RUNTIME_H
