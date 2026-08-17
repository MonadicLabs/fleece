#define _POSIX_C_SOURCE 200809L

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#include "fleece_core.h"
#include "runtime/fleece_runtime.h"
#include "state/fleece_state_manager.h"
#include "comms/fleece_comms.h"
#include "embedded/fleece_embedded.h"

uint64_t get_time_function(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

typedef void (*SensorCallback)(const uint8_t*, uint32_t);
typedef void (*ActuatorCallback)(const uint8_t*, uint32_t);

typedef struct FleeceCore {
    bool is_initialized;
    bool is_running;
    uint64_t node_id;
    uint64_t (*get_time_func)(void);
    
    FleeceRuntime* runtime;
    FleeceStateManager* state_manager;
    FleeceComms* comms;
    FleeceEmbedded* embedded;
    
    // Component callbacks
    SensorCallback sensor_callbacks[16];
    const char* sensor_names[16];
    ActuatorCallback actuator_callbacks[16];
    const char* actuator_names[16];
    uint32_t sensor_count;
    uint32_t actuator_count;
} FleeceCore;

static uint64_t (*get_time_func)(void) = get_time_function;

FleeceCore* fleece_core_create(void) {
    FleeceCore* core = (FleeceCore*)calloc(1, sizeof(FleeceCore));
    if (!core) {
        return NULL;
    }
    
    core->is_initialized = false;
    core->is_running = false;
    core->node_id = 0x123456789ABCDEF0;
    core->get_time_func = get_time_func;
    
    core->sensor_count = 0;
    core->actuator_count = 0;
    
    return core;
}

void fleece_core_destroy(FleeceCore* core) {
    if (!core) return;
    
    if (core->is_initialized) {
        fleece_core_stop(core);
    }
    
    if (core->runtime) {
        fleece_runtime_destroy(core->runtime);
    }
    
    if (core->comms) {
        fleece_comms_destroy(core->comms);
    }
    
    if (core->state_manager) {
        fleece_state_manager_destroy(core->state_manager);
    }
    
    if (core->embedded) {
        fleece_embedded_destroy(core->embedded);
    }
    
    free(core);
}

int fleece_core_initialize(FleeceCore* core) {
    if (!core) return -1;
    
    core->runtime = fleece_runtime_create();
    if (!core->runtime) {
        return -1;
    }
    
    core->state_manager = fleece_state_manager_create();
    if (!core->state_manager) {
        fleece_runtime_destroy(core->runtime);
        return -1;
    }
    
    core->comms = fleece_comms_create();
    if (!core->comms) {
        fleece_state_manager_destroy(core->state_manager);
        fleece_runtime_destroy(core->runtime);
        return -1;
    }
    
    if (fleece_comms_initialize(core->comms) != 0) {
        fleece_comms_destroy(core->comms);
        fleece_state_manager_destroy(core->state_manager);
        fleece_runtime_destroy(core->runtime);
        return -1;
    }
    
    core->embedded = fleece_embedded_create();
    if (!core->embedded) {
        fleece_comms_destroy(core->comms);
        fleece_state_manager_destroy(core->state_manager);
        fleece_runtime_destroy(core->runtime);
        return -1;
    }
    
    if (fleece_embedded_register_c_functions(core->embedded) != 0) {
        fleece_embedded_destroy(core->embedded);
        fleece_comms_destroy(core->comms);
        fleece_state_manager_destroy(core->state_manager);
        fleece_runtime_destroy(core->runtime);
        return -1;
    }
    
    core->is_initialized = true;
    
    printf("Core initialized successfully\n");
    
    return 0;
}

int fleece_core_run(FleeceCore* core) {
    if (!core || !core->is_initialized) return -1;
    
    core->is_running = true;
    
    printf("Starting Fleece core execution loop...\n");
    
    // Main coordination loop
    while (core->is_running) {
        uint64_t loop_start_time = core->get_time_func();
        
        // Phase 1: Input (Sensors/Radio)
        printf("Phase 1: Processing sensor inputs and radio packets\n");
        
        // Phase 2: Gossip (State Synchronization) - see fleece_runtime.c for the live gossip path

        // Phase 3: Script Execution (QuickJS VM)
        if (core->embedded) {
            fleece_embedded_execute(core->embedded, "console.log('Script execution phase');");
        }
        
        // Phase 4: Output (Actuators/Mesh Broadcast)
        printf("Phase 4: Processing actuator outputs and mesh broadcasts\n");
        
        // Small delay to prevent CPU spinning - use nanosleep
        struct timespec ts = {.tv_sec = 0, .tv_nsec = 100000000}; // 100ms
        nanosleep(&ts, NULL);
    }
    
    return 0;
}

void fleece_core_stop(FleeceCore* core) {
    if (core) {
        core->is_running = false;
    }
}

uint64_t fleece_core_get_time(FleeceCore* core) {
    return core ? core->get_time_func() : 0;
}

void fleece_core_set_node_id(FleeceCore* core, uint64_t node_id) {
    if (core) {
        core->node_id = node_id;
    }
}

uint64_t fleece_core_get_node_id(FleeceCore* core) {
    return core ? core->node_id : 0;
}

int fleece_core_register_sensor(FleeceCore* core, const char* name, SensorCallback callback) {
    if (!core || !name || !callback) return -1;
    
    if (core->sensor_count >= 16) {
        return -1;  // Max sensors reached
    }
    
    uint32_t idx = core->sensor_count++;
    core->sensor_names[idx] = name;
    core->sensor_callbacks[idx] = callback;
    
    return 0;
}

int fleece_core_register_actuator(FleeceCore* core, const char* name, ActuatorCallback callback) {
    if (!core || !name || !callback) return -1;
    
    if (core->actuator_count >= 16) {
        return -1;  // Max actuators reached
    }
    
    uint32_t idx = core->actuator_count++;
    core->actuator_names[idx] = name;
    core->actuator_callbacks[idx] = callback;
    
    return 0;
}
