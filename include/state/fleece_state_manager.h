// Fleece State Manager Interface
// LWW key-value store for virtual stigmergy

#ifndef FLEECE_STATE_MANAGER_H
#define FLEECE_STATE_MANAGER_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FLEECE_FIELD_NAME_MAX 32

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

// Create a new state manager instance (local node id defaults to a fixed placeholder)
FleeceStateManager* fleece_state_manager_create(void);

// Create a new state manager instance with an explicit local node id.
// Needed so distinct nodes/peers don't collide under the same id (e.g. gossip
// simulation, tests with more than one manager in the same process).
FleeceStateManager* fleece_state_manager_create_with_node_id(uint64_t node_id);

// Destroy state manager instance
void fleece_state_manager_destroy(FleeceStateManager* manager);

// Get the local node's own id
uint64_t fleece_state_manager_get_node_id(FleeceStateManager* manager);

// Set a field value with LWW semantics (raw key, implicitly owned by the local node)
int fleece_state_manager_set(FleeceStateManager* manager, uint32_t key, const uint8_t* data, uint32_t size);

// Get a field value (raw key, scoped to the local node's own fields)
int fleece_state_manager_get(FleeceStateManager* manager, uint32_t key, uint8_t** data, uint32_t* size);

// Check if a field exists (raw key, scoped to the local node's own fields)
bool fleece_state_manager_exists(FleeceStateManager* manager, uint32_t key);

// Remove a field (raw key, scoped to the local node's own fields)
int fleece_state_manager_remove(FleeceStateManager* manager, uint32_t key);

// Compact memory (remove tombstones)
int fleece_state_manager_compact(FleeceStateManager* manager);

// Get field version info (raw key, scoped to the local node's own fields)
int fleece_state_manager_get_version(FleeceStateManager* manager, uint32_t key, FleeceFieldVersion* version);

// --- Named, owner-scoped fields (backs the "self" / "swarm" script objects) ---

// Set a named field owned by the local node
int fleece_state_manager_set_named(FleeceStateManager* manager, const char* name, const uint8_t* data, uint32_t size);

// Remove a named field owned by the local node
int fleece_state_manager_remove_named(FleeceStateManager* manager, const char* name);

// Get a named field owned by the given node (local or a known peer)
int fleece_state_manager_get_named(FleeceStateManager* manager, uint64_t owner_node_id, const char* name, uint8_t** data, uint32_t* size);

// Check whether a named field owned by the given node exists
bool fleece_state_manager_exists_named(FleeceStateManager* manager, uint64_t owner_node_id, const char* name);

// List distinct node ids currently known to this manager (local + any merged peers).
// Returns the number of ids written to node_ids_out (capped at max_nodes).
uint32_t fleece_state_manager_list_nodes(FleeceStateManager* manager, uint64_t* node_ids_out, uint32_t max_nodes);

// List field names owned by the given node. Returns the number of names written
// to names_out (capped at max_names).
uint32_t fleece_state_manager_list_fields(FleeceStateManager* manager, uint64_t owner_node_id, char names_out[][FLEECE_FIELD_NAME_MAX], uint32_t max_names);

// Merge a field received from a peer via gossip, using LWW on the given remote
// timestamp (strictly newer wins; ties keep the incumbent). Never touches the
// local node's own fields (owner_node_id must not equal the local node id).
int fleece_state_manager_merge_named(FleeceStateManager* manager, uint64_t owner_node_id, const char* name, const uint8_t* data, uint32_t size, uint64_t remote_timestamp, bool is_tombstone);

// Export the local node's own named fields as a gossip wire frame
int fleece_state_manager_export(FleeceStateManager* manager, uint8_t** frame_data, uint32_t* frame_size);

// Import a gossip wire frame received from a peer, merging its fields with LWW
int fleece_state_manager_import(FleeceStateManager* manager, const uint8_t* frame_data, uint32_t frame_size);

#ifdef __cplusplus
}
#endif

#endif // FLEECE_STATE_MANAGER_H
