#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>

#include "fleece_runtime.h"
#include "fleece_alloc.h"
#include "fleece_state_manager.h"
#include "fleece_comms.h"
#include "fleece_embedded.h"
#include "platform/fleece_platform.h"
#include "planner/fleece_planner.h"
#include "embedded/fleece_goap_js.h"

// Runtime implementation

// How often (in loop ticks) to send a full gossip export instead of a delta,
// so peers eventually recover from a dropped packet without waiting forever.
#define FLEECE_GOSSIP_FULL_RESYNC_TICKS 50

struct FleeceRuntime {
    volatile sig_atomic_t is_running;
    FleeceStateManager* state_manager;
    FleeceComms* comms;
    FleeceEmbedded* embedded;
    FleecePlatform* platform;
    pthread_t main_thread;
    int script_fd;
    uint64_t self_gossip_watermark;     // local timestamp as of the last self-stream gossip send
    uint64_t shared_gossip_watermark;   // local timestamp as of the last shared/"world"-stream gossip send
    uint32_t gossip_tick_count;
    FleeceGoapBrain* goap_brain;        // optional behavior-loop driver (see fleece_runtime_set_goap)
    void (*tick_cb)(FleeceRuntime*, void*);  // optional per-tick C hook (see fleece_runtime_set_tick_callback)
    void* tick_ud;
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

static FleeceRuntime* runtime_create_internal(FleeceStateManager* state_manager) {
    FleeceRuntime* runtime = (FleeceRuntime*)fleece_calloc(1, sizeof(FleeceRuntime));
    if (!runtime) {
        fleece_state_manager_destroy(state_manager);
        return NULL;
    }

    runtime->is_running = 0;
    runtime->state_manager = state_manager;
    runtime->comms = fleece_comms_create();
    runtime->embedded = fleece_embedded_create();
    runtime->platform = fleece_platform_create();

    if (!runtime->state_manager || !runtime->comms || !runtime->embedded || !runtime->platform) {
        fleece_runtime_destroy(runtime);
        return NULL;
    }

    fleece_embedded_set_state_manager(runtime->embedded, runtime->state_manager);
    fleece_embedded_set_platform(runtime->embedded, runtime->platform);
    fleece_embedded_register_c_functions(runtime->embedded);

    // The runtime owns the comms receive slot: gossip frames from peers land here.
    fleece_comms_set_receive_callback(runtime->comms, runtime_gossip_receive, runtime);

    global_runtime = runtime;

    return runtime;
}

FleeceRuntime* fleece_runtime_create(void) {
    return runtime_create_internal(fleece_state_manager_create());
}

FleeceRuntime* fleece_runtime_create_with_node_id(uint64_t node_id) {
    return runtime_create_internal(fleece_state_manager_create_with_node_id(node_id));
}

void fleece_runtime_destroy(FleeceRuntime* runtime) {
    if (!runtime) return;

    fleece_runtime_stop(runtime);

    if (runtime->goap_brain) {
        fleece_goap_brain_destroy(runtime->goap_brain);
        runtime->goap_brain = NULL;
    }

    if (runtime->embedded) {
        fleece_embedded_destroy(runtime->embedded);
    }

    if (runtime->platform) {
        fleece_platform_destroy(runtime->platform);
    }

    if (runtime->comms) {
        fleece_comms_destroy(runtime->comms);
    }

    if (runtime->state_manager) {
        fleece_state_manager_destroy(runtime->state_manager);
    }

    fleece_free(runtime);
    global_runtime = NULL;
}

int fleece_runtime_load_script(FleeceRuntime* runtime, const char* source) {
    if (!runtime || !source) {
        return -1;
    }

    return fleece_embedded_load_script(runtime->embedded, source, "<script>");
}

int fleece_runtime_set_peer_ttl_ticks(FleeceRuntime* runtime, uint64_t ttl_ticks) {
    if (!runtime) return -1;

    return fleece_embedded_set_peer_ttl_ticks(runtime->embedded, ttl_ticks);
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
        fleece_state_manager_tick(runtime->state_manager);  // drives peer liveness (swarm TTL); see fleece_state_manager_ticks_since_seen

        // Phase 1: Input (Sensors/Radio)
        fleece_comms_process_input(runtime->comms);

        // Phase 2: C tick hook (optional) + Script Execution (QuickJS VM) - run
        // before gossip so any self.xxx/world.xxx changes made this tick are
        // broadcast this tick, not next.
        if (runtime->tick_cb) {
            runtime->tick_cb(runtime, runtime->tick_ud);
        }
        fleece_embedded_call_step(runtime->embedded);

        // Phase 2.5: GOAP brain (optional) - decides and applies/commits effects;
        // runs after script step so it sees this tick's sensor/state changes, and
        // before gossip so its decisions are broadcast this tick.
        if (runtime->goap_brain) {
            fleece_goap_brain_tick(runtime->goap_brain);
        }

        // Phase 3: Gossip (State Synchronization) - two independent streams, each
        // delta-by-default with a periodic full resync (anti-entropy) so a peer
        // can recover from a dropped packet instead of missing an update forever:
        //   - self: this node's own fields (owner = its own node id)
        //   - shared: the "world" collection (owner = FLEECE_SHARED_OWNER_ID),
        //     which any node may write/relay - not tied to any single node's liveness.
        // Peers' frames arrive via runtime_gossip_receive() and merge into swarm/world.
        bool full_resync = (runtime->gossip_tick_count % FLEECE_GOSSIP_FULL_RESYNC_TICKS) == 0;

        uint8_t* self_frame = NULL;
        uint32_t self_frame_size = 0;
        int self_rc = full_resync
            ? fleece_state_manager_export(runtime->state_manager, &self_frame, &self_frame_size)
            : fleece_state_manager_export_delta(runtime->state_manager, runtime->self_gossip_watermark, &self_frame, &self_frame_size);
        if (self_rc == 0) {
            fleece_comms_send(runtime->comms, "broadcast", self_frame, self_frame_size);
            fleece_free(self_frame);
        }
        runtime->self_gossip_watermark = fleece_state_manager_get_local_timestamp(runtime->state_manager);

        uint8_t* shared_frame = NULL;
        uint32_t shared_frame_size = 0;
        int shared_rc = full_resync
            ? fleece_state_manager_export_shared(runtime->state_manager, &shared_frame, &shared_frame_size)
            : fleece_state_manager_export_shared_delta(runtime->state_manager, runtime->shared_gossip_watermark, &shared_frame, &shared_frame_size);
        if (shared_rc == 0) {
            fleece_comms_send(runtime->comms, "broadcast", shared_frame, shared_frame_size);
            fleece_free(shared_frame);
        }
        runtime->shared_gossip_watermark = fleece_state_manager_get_local_timestamp(runtime->state_manager);

        runtime->gossip_tick_count++;

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

void* fleece_runtime_get_platform(FleeceRuntime* runtime) {
    return runtime ? runtime->platform : NULL;
}

int fleece_runtime_set_goap(FleeceRuntime* runtime, FleeceGoap* goap) {
    if (!runtime || !goap) return -1;
    if (runtime->goap_brain) return -1;  // already attached

    runtime->goap_brain = fleece_goap_brain_create(runtime->embedded, goap);
    return runtime->goap_brain ? 0 : -1;
}

void* fleece_runtime_get_goap_brain(FleeceRuntime* runtime) {
    return runtime ? runtime->goap_brain : NULL;
}

void fleece_runtime_set_tick_callback(FleeceRuntime* runtime, void (*cb)(FleeceRuntime*, void*), void* user_data) {
    if (!runtime) return;
    runtime->tick_cb = cb;
    runtime->tick_ud = user_data;
}
