// Fleece State Manager Interface
// LWW key-value store for virtual stigmergy

#ifndef FLEECE_STATE_MANAGER_H
#define FLEECE_STATE_MANAGER_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FleeceFieldVersion {
    uint64_t timestamp;
    uint64_t node_id;
    uint32_t size;
} FleeceFieldVersion;

typedef struct FleeceFieldValue {
    uint32_t key;
    const uint8_t* data;
    uint32_t size;
} FleeceFieldValue;

typedef struct FleeceStateManager FleeceStateManager;

// Create a new state manager instance
FleeceStateManager* fleece_state_manager_create(void);

// Destroy state manager instance
void fleece_state_manager_destroy(FleeceStateManager* manager);

// Set a field value with LWW semantics
int fleece_state_manager_set(FleeceStateManager* manager, uint32_t key, const uint8_t* data, uint32_t size);

// Get a field value
int fleece_state_manager_get(FleeceStateManager* manager, uint32_t key, uint8_t** data, uint32_t* size);

// Check if a field exists
bool fleece_state_manager_exists(FleeceStateManager* manager, uint32_t key);

// Remove a field
int fleece_state_manager_remove(FleeceStateManager* manager, uint32_t key);

// Gossip with peer nodes
void fleece_state_manager_gossip(FleeceStateManager* manager);

// Compact memory (remove tombstones)
int fleece_state_manager_compact(FleeceStateManager* manager);

// Get field version info
int fleece_state_manager_get_version(FleeceStateManager* manager, uint32_t key, FleeceFieldVersion* version);

// Export state to CBOR format
int fleece_state_manager_export(FleeceStateManager* manager, uint8_t** cbor_data, uint32_t* cbor_size);

// Import state from CBOR format
int fleece_state_manager_import(FleeceStateManager* manager, const uint8_t* cbor_data, uint32_t cbor_size);

#ifdef __cplusplus
}
#endif

#endif // FLEECE_STATE_MANAGER_H
