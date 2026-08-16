#include <stdio.h>
#include <stdlib.h>

#include "runtime/fleece_runtime.h"
#include "comms/fleece_comms.h"

int main(void) {
    printf("Fleece Swarm Coordination Example 1\n");
    printf("===================================\n\n");
    
    // Initialize the runtime
    FleeceRuntime* runtime = fleece_runtime_create();
    if (!runtime) {
        fprintf(stderr, "Failed to create runtime\n");
        return 1;
    }
    
    // Initialize comms (Reticulum)
    void* comms = fleece_runtime_get_comms(runtime);
    if (comms) {
        int result = fleece_comms_initialize(comms);
        if (result != 0) {
            fprintf(stderr, "Failed to initialize comms\n");
            return 1;
        }
    }
    
    // Run the runtime
    printf("Starting swarm coordination runtime...\n");
    printf("Press Ctrl+C to stop\n\n");
    
    int result = fleece_runtime_start(runtime);
    
    // Cleanup
    if (comms) {
        fleece_comms_close(comms);
    }
    fleece_runtime_destroy(runtime);
    
    return result;
}
