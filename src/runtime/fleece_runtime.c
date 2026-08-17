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
#include "fleece_embedded.h"

// Runtime implementation

struct FleeceRuntime {
    volatile sig_atomic_t is_running;
    FleeceStateManager* state_manager;
    FleeceComms* comms;
    FleeceEmbedded* embedded;
    pthread_t main_thread;
    int script_fd;
};

static FleeceRuntime* global_runtime = NULL;

static void signal_handler(int signum) {
    (void)signum;
    if (global_runtime) {
        global_runtime->is_running = 0;
    }
}

// Merges a gossip frame received from a peer into the local swarm view.
static void runtime_gossip_receive(const char* source, const uint8_t* data, uint32_t size, void* user_data) {
    (void)source;
    FleeceRuntime* runtime = (FleeceRuntime*)user_data;
    if (!runtime || !data || size == 0) return;

    fleece_state_manager_import(runtime->state_manager, data, size);
}

FleeceRuntime* fleece_runtime_create(void) {
    FleeceRuntime* runtime = (FleeceRuntime*)calloc(1, sizeof(FleeceRuntime));
    if (!runtime) {
        return NULL;
    }

    runtime->is_running = 0;
    runtime->state_manager = fleece_state_manager_create();
    runtime->comms = fleece_comms_create();
    runtime->embedded = fleece_embedded_create();

    if (!runtime->state_manager || !runtime->comms || !runtime->embedded) {
        fleece_runtime_destroy(runtime);
        return NULL;
    }

    fleece_embedded_set_state_manager(runtime->embedded, runtime->state_manager);
    fleece_embedded_register_c_functions(runtime->embedded);

    // The runtime owns the comms receive slot: gossip frames from peers land here.
    fleece_comms_set_receive_callback(runtime->comms, runtime_gossip_receive, runtime);

    global_runtime = runtime;

    return runtime;
}

void fleece_runtime_destroy(FleeceRuntime* runtime) {
    if (!runtime) return;

    fleece_runtime_stop(runtime);

    if (runtime->embedded) {
        fleece_embedded_destroy(runtime->embedded);
    }

    if (runtime->comms) {
        fleece_comms_destroy(runtime->comms);
    }

    if (runtime->state_manager) {
        fleece_state_manager_destroy(runtime->state_manager);
    }

    free(runtime);
    global_runtime = NULL;
}

int fleece_runtime_load_script(FleeceRuntime* runtime, const char* source) {
    if (!runtime || !source) {
        return -1;
    }

    return fleece_embedded_load_script(runtime->embedded, source, "<script>");
}

int fleece_runtime_start(FleeceRuntime* runtime) {
    if (!runtime || runtime->is_running) {
        return -1;
    }

    runtime->is_running = 1;

    // Setup signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    fleece_embedded_call_init(runtime->embedded);

    // Main runtime loop
    while (runtime->is_running) {
        // Phase 1: Input (Sensors/Radio)
        fleece_comms_process_input(runtime->comms);

        // Phase 2: Gossip (State Synchronization) - broadcast our own fields;
        // peers' fields arrive via runtime_gossip_receive() and merge into swarm.
        uint8_t* gossip_frame = NULL;
        uint32_t gossip_size = 0;
        if (fleece_state_manager_export(runtime->state_manager, &gossip_frame, &gossip_size) == 0) {
            fleece_comms_send(runtime->comms, "broadcast", gossip_frame, gossip_size);
            free(gossip_frame);
        }

        // Phase 3: Script Execution (QuickJS VM)
        fleece_embedded_call_step(runtime->embedded);

        // Phase 4: Output (Actuators/Mesh Broadcast)
        fleece_comms_process_output(runtime->comms);

        // Pacing tick - avoid spinning the loop at full CPU
        struct timespec ts = {.tv_sec = 0, .tv_nsec = 100000000};  // 100ms
        nanosleep(&ts, NULL);
    }

    fleece_embedded_call_destroy(runtime->embedded);

    return 0;
}

void fleece_runtime_stop(FleeceRuntime* runtime) {
    if (runtime) {
        runtime->is_running = 0;
    }
}

bool fleece_runtime_is_running(FleeceRuntime* runtime) {
    return runtime ? runtime->is_running : false;
}

int fleece_runtime_execute_script(FleeceRuntime* runtime, const char* script) {
    if (!runtime || !script) {
        return -1;
    }

    return fleece_embedded_execute(runtime->embedded, script);
}

void* fleece_runtime_get_state_manager(FleeceRuntime* runtime) {
    return runtime ? runtime->state_manager : NULL;
}

void* fleece_runtime_get_comms(FleeceRuntime* runtime) {
    return runtime ? runtime->comms : NULL;
}

void* fleece_runtime_get_embedded(FleeceRuntime* runtime) {
    return runtime ? runtime->embedded : NULL;
}
