#include <stdio.h>
#include <stdlib.h>

#include "runtime/fleece_runtime.h"
#include "comms/fleece_comms.h"
#include "state/fleece_state_manager.h"

// Comms is still a single-process simulation (no real socket transport), so there is
// no second process to actually gossip with. This struct wires the send callback up
// to a second, in-process FleeceStateManager acting as a simulated peer, so the demo
// can show fleece_state_manager_export/import/merge really working end to end. See
// src/state/fleece_state_manager.c for the actual wire format and LWW merge logic.
typedef struct GossipLoopback {
    FleeceStateManager* local_manager;
    FleeceStateManager* peer_manager;
} GossipLoopback;

static void on_comms_send(const char* destination, const uint8_t* data, uint32_t size, void* user_data) {
    printf("[send] %u bytes -> %s (", size, destination);
    uint32_t dump = size < 16 ? size : 16;
    for (uint32_t i = 0; i < dump; i++) {
        printf("%02x", data[i]);
    }
    printf("%s)\n", size > dump ? "..." : "");

    GossipLoopback* loopback = (GossipLoopback*)user_data;
    if (!loopback) return;

    // The simulated peer receives what we just broadcast (self or shared stream -
    // import() figures out which from the frame's own header)...
    fleece_state_manager_import(loopback->peer_manager, data, size);

    // ...and we receive whatever the peer currently has to say back, both streams.
    uint8_t* peer_frame = NULL;
    uint32_t peer_frame_size = 0;
    if (fleece_state_manager_export(loopback->peer_manager, &peer_frame, &peer_frame_size) == 0) {
        fleece_state_manager_import(loopback->local_manager, peer_frame, peer_frame_size);
        free(peer_frame);
    }
    peer_frame = NULL;
    peer_frame_size = 0;
    if (fleece_state_manager_export_shared(loopback->peer_manager, &peer_frame, &peer_frame_size) == 0) {
        fleece_state_manager_import(loopback->local_manager, peer_frame, peer_frame_size);
        free(peer_frame);
    }
}

static const char* SCRIPT =
    "function init() {\n"
    "  self.role = 'coordinator';\n"
    "  self.battery = 100;\n"
    "  console.log('init: node', self.id, 'ready');\n"
    "}\n"
    "\n"
    "function step() {\n"
    "  self.uptime = (self.uptime || 0) + 1;\n"
    "\n"
    "  if (self.uptime === 3) {\n"
    "    world.T1 = { lat: 42.1, lon: -71.05, type: 'debris', confidence: 0.87 };\n"
    "    console.log('discovered target T1');\n"
    "  }\n"
    "\n"
    "  var peers = Object.keys(swarm);\n"
    "  console.log('step', self.uptime, '- self.battery =', self.battery, '- peers:', peers.length);\n"
    "  for (var i = 0; i < peers.length; i++) {\n"
    "    var id = peers[i];\n"
    "    console.log('  swarm[' + id + '] =', JSON.stringify(swarm[id]));\n"
    "  }\n"
    "\n"
    "  var knownWorld = Object.keys(world);\n"
    "  console.log('  world:', JSON.stringify(knownWorld));\n"
    "  for (var j = 0; j < knownWorld.length; j++) {\n"
    "    var t = knownWorld[j];\n"
    "    console.log('    world[' + t + '] =', JSON.stringify(world[t]));\n"
    "  }\n"
    "}\n"
    "\n"
    "function destroy() {\n"
    "  console.log('destroy: node shutting down after', self.uptime, 'step(s)');\n"
    "}\n";

int main(void) {
    printf("Fleece Swarm Coordination Example 1\n");
    printf("===================================\n\n");

    // Initialize the runtime
    FleeceRuntime* runtime = fleece_runtime_create();
    if (!runtime) {
        fprintf(stderr, "Failed to create runtime\n");
        return 1;
    }

    FleeceComms* comms = (FleeceComms*)fleece_runtime_get_comms(runtime);
    FleeceStateManager* local_manager = (FleeceStateManager*)fleece_runtime_get_state_manager(runtime);

    FleeceStateManager* simulated_peer = fleece_state_manager_create_with_node_id(0xF00DF00DF00DF00DULL);
    fleece_state_manager_set_named(simulated_peer, "role", (const uint8_t*)"\"scout\"", 7);
    fleece_state_manager_set_named(simulated_peer, "battery", (const uint8_t*)"88", 2);
    // The simulated peer reports a "world" entry of its own, to show it gossiping
    // in both directions (any node can publish/claim one - see
    // fleece_state_manager_set_shared/export_shared).
    fleece_state_manager_set_shared(simulated_peer, "T2", (const uint8_t*)"\"peer-reported\"", 15);
    GossipLoopback loopback = { .local_manager = local_manager, .peer_manager = simulated_peer };

    if (comms) {
        int result = fleece_comms_initialize(comms);
        if (result != 0) {
            fprintf(stderr, "Failed to initialize comms\n");
            fleece_state_manager_destroy(simulated_peer);
            fleece_runtime_destroy(runtime);
            return 1;
        }

        // The runtime owns the receive callback (gossip frames from real peers land
        // there); the example only needs the send side, to drive the loopback above.
        fleece_comms_set_send_callback(comms, on_comms_send, &loopback);
    }

    if (fleece_runtime_load_script(runtime, SCRIPT) != 0) {
        fprintf(stderr, "Failed to load script\n");
    }

    // Run the runtime
    printf("Starting swarm coordination runtime...\n");
    printf("Press Ctrl+C to stop\n\n");

    int result = fleece_runtime_start(runtime);

    // Cleanup
    if (comms) {
        fleece_comms_close(comms);
    }
    fleece_state_manager_destroy(simulated_peer);
    fleece_runtime_destroy(runtime);

    return result;
}
