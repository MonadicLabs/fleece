// Fleece Core
// Main coordination loop and system integration

#ifndef FLEECE_CORE_H
#define FLEECE_CORE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FleeceCore FleeceCore;

// Create a new core instance
FleeceCore* fleece_core_create(void);

// Destroy core instance
void fleece_core_destroy(FleeceCore* core);

// Initialize core components
int fleece_core_initialize(FleeceCore* core);

// Run the main time-step loop
int fleece_core_run(FleeceCore* core);

// Stop the core execution
void fleece_core_stop(FleeceCore* core);

// Get current system time
uint64_t fleece_core_get_time(FleeceCore* core);

// Set local node ID
void fleece_core_set_node_id(FleeceCore* core, uint64_t node_id);

// Get local node ID
uint64_t fleece_core_get_node_id(FleeceCore* core);

// Register a sensor callback
int fleece_core_register_sensor(FleeceCore* core, const char* name, void (*callback)(const uint8_t*, uint32_t));

// Register an actuator callback
int fleece_core_register_actuator(FleeceCore* core, const char* name, void (*callback)(const uint8_t*, uint32_t));

#ifdef __cplusplus
}
#endif

#endif // FLEECE_CORE_H
