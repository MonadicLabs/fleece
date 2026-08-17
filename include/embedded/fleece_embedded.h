// Fleece Embedded JavaScript
// QuickJS integration for script execution

#ifndef FLEECE_EMBEDDED_H
#define FLEECE_EMBEDDED_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FleeceEmbedded FleeceEmbedded;
typedef struct FleeceStateManager FleeceStateManager;
typedef struct FleecePlatform FleecePlatform;

// Create a new embedded JS instance
FleeceEmbedded* fleece_embedded_create(void);

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

// Register the native bindings (console.log, self, swarm, platform) with the
// JS context. Requires fleece_embedded_set_state_manager() to have been
// called first.
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

#ifdef __cplusplus
}
#endif

#endif // FLEECE_EMBEDDED_H
