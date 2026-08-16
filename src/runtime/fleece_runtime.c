#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>

#include "fleece_runtime.h"
#include "fleece_state_manager.h"
#include "fleece_comms.h"

// Runtime implementation

struct FleeceRuntime {
    bool is_running;
    FleeceStateManager* state_manager;
    FleeceComms* comms;
    pthread_t main_thread;
    int script_fd;
};

static FleeceRuntime* global_runtime = NULL;

static void signal_handler(int signum) {
    if (global_runtime) {
        global_runtime->is_running = false;
    }
}

FleeceRuntime* fleece_runtime_create(void) {
    FleeceRuntime* runtime = (FleeceRuntime*)calloc(1, sizeof(FleeceRuntime));
    if (!runtime) {
        return NULL;
    }
    
    runtime->is_running = false;
    runtime->state_manager = fleece_state_manager_create();
    runtime->comms = fleece_comms_create();
    
    global_runtime = runtime;
    
    return runtime;
}

void fleece_runtime_destroy(FleeceRuntime* runtime) {
    if (!runtime) return;
    
    fleece_runtime_stop(runtime);
    
    if (runtime->comms) {
        fleece_comms_destroy(runtime->comms);
    }
    
    if (runtime->state_manager) {
        fleece_state_manager_destroy(runtime->state_manager);
    }
    
    free(runtime);
    global_runtime = NULL;
}

int fleece_runtime_start(FleeceRuntime* runtime) {
    if (!runtime || runtime->is_running) {
        return -1;
    }
    
    runtime->is_running = true;
    
    // Setup signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    // Main runtime loop
    while (runtime->is_running) {
        // Phase 1: Input (Sensors/Radio)
        fleece_comms_process_input(runtime->comms);
        
        // Phase 2: Gossip (State Synchronization)
        fleece_state_manager_gossip(runtime->state_manager);
        
        // Phase 3: Script Execution (QuickJS VM)
        fleece_runtime_execute_script(runtime, "console.log('Hello from fleece!');");
        
        // Phase 4: Output (Actuators/Mesh Broadcast)
        fleece_comms_process_output(runtime->comms);
    }
    
    return 0;
}

void fleece_runtime_stop(FleeceRuntime* runtime) {
    if (runtime) {
        runtime->is_running = false;
    }
}

bool fleece_runtime_is_running(FleeceRuntime* runtime) {
    return runtime ? runtime->is_running : false;
}

int fleece_runtime_execute_script(FleeceRuntime* runtime, const char* script) {
    if (!runtime || !script) {
        return -1;
    }
    
    printf("Executing script: %s\n", script);
    
    return 0;
}

void* fleece_runtime_get_state_manager(FleeceRuntime* runtime) {
    return runtime ? runtime->state_manager : NULL;
}

void* fleece_runtime_get_comms(FleeceRuntime* runtime) {
    return runtime ? runtime->comms : NULL;
}
