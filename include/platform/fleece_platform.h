// Fleece Platform Interface
// A named-function registry exposed to scripts as the "platform" global.
//
// fleece has no built-in notion of what a "platform" is or does - no arm/disarm,
// no moveTo, no payload verbs, nothing UAV- or robot-specific. It is purely a
// name -> native function registry: whoever is integrating a specific piece of
// hardware (a drone, a rover, a manipulator arm, a camera turret, ...) registers
// whatever functions make sense for that hardware, and scripts call them as
// platform.<name>(...). An unregistered platform (nothing ever called
// fleece_platform_register()) is simply empty.

#ifndef FLEECE_PLATFORM_H
#define FLEECE_PLATFORM_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FLEECE_PLATFORM_FUNCTION_NAME_MAX 32

typedef struct FleecePlatform FleecePlatform;

// A registered platform function. args_json/args_size is the JSON-encoded
// argument array as called from script. On success, implementations should
// malloc() *result_json (the caller frees it) with a JSON-encoded result and
// return 0; returning 0 with *result_json left NULL means "no return value".
// Returning nonzero signals failure (surfaces as a thrown JS exception) and
// must leave *result_json untouched.
typedef int (*FleecePlatformFunction)(const uint8_t* args_json, uint32_t args_size,
                                       uint8_t** result_json, uint32_t* result_size,
                                       void* user_data);

// Create an empty platform registry
FleecePlatform* fleece_platform_create(void);

// Destroy a platform registry
void fleece_platform_destroy(FleecePlatform* platform);

// Register (or replace) a named function, callable from script as platform.<name>(...)
int fleece_platform_register(FleecePlatform* platform, const char* name, FleecePlatformFunction fn, void* user_data);

// Remove a previously registered function
int fleece_platform_unregister(FleecePlatform* platform, const char* name);

// Check whether a function is currently registered
bool fleece_platform_has_function(FleecePlatform* platform, const char* name);

// List currently registered function names. Returns the number written to
// names_out (capped at max_names).
uint32_t fleece_platform_list_functions(FleecePlatform* platform, char names_out[][FLEECE_PLATFORM_FUNCTION_NAME_MAX], uint32_t max_names);

// Invoke a registered function directly (engine-agnostic - usable from C or
// tests without going through the JS bridge). args_json/result_json are raw
// JSON bytes; *result_json is owned by the caller and must be free()d.
int fleece_platform_call(FleecePlatform* platform, const char* name, const uint8_t* args_json, uint32_t args_size, uint8_t** result_json, uint32_t* result_size);

#ifdef __cplusplus
}
#endif

#endif // FLEECE_PLATFORM_H
