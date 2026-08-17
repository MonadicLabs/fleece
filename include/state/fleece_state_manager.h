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

// Reserved node id representing swarm-shared state (see fleece_state_manager_set_shared).
// A real node's own id must never be this value - fleece_state_manager_create_with_node_id
// rejects it.
#define FLEECE_SHARED_OWNER_ID 0

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
// simulation, tests with more than one manager in the same process). Returns
// NULL if node_id == FLEECE_SHARED_OWNER_ID (reserved).
FleeceStateManager* fleece_state_manager_create_with_node_id(uint64_t node_id);

// Destroy state manager instance
void fleece_state_manager_destroy(FleeceStateManager* manager);

// Get the local node's own id
uint64_t fleece_state_manager_get_node_id(FleeceStateManager* manager);

// Get the local node's current logical clock value (monotonically increasing
// on every local write via set_named/remove_named). Used as a gossip delta
// watermark - see fleece_state_manager_export_delta().
uint64_t fleece_state_manager_get_local_timestamp(FleeceStateManager* manager);

// Advance the manager's internal tick counter by one. Call once per runtime
// loop iteration. Used only for peer liveness tracking (ticks_since_seen) -
// LWW always compares timestamps from the same peer's own local clock, never
// this counter.
void fleece_state_manager_tick(FleeceStateManager* manager);

// Ticks elapsed since a frame was last received from the given node id (see
// fleece_state_manager_import - even an empty delta counts as "heard from",
// and fleece_state_manager_merge_named() also counts as hearing from its
// owner_node_id). Returns UINT64_MAX if never heard from. Used to detect
// dead/out-of-range peers so scripts can stop seeing them in "swarm" - see
// fleece_embedded_set_peer_ttl_ticks() in fleece_embedded.h.
uint64_t fleece_state_manager_ticks_since_seen(FleeceStateManager* manager, uint64_t node_id);

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

// List distinct node ids currently known to this manager (local + any merged
// peers - never includes FLEECE_SHARED_OWNER_ID, which is not a real node).
// Returns the number of ids written to node_ids_out (capped at max_nodes).
uint32_t fleece_state_manager_list_nodes(FleeceStateManager* manager, uint64_t* node_ids_out, uint32_t max_nodes);

// List field names owned by the given node. Returns the number of names written
// to names_out (capped at max_names).
uint32_t fleece_state_manager_list_fields(FleeceStateManager* manager, uint64_t owner_node_id, char names_out[][FLEECE_FIELD_NAME_MAX], uint32_t max_names);

// Merge a field received from a peer via gossip, using LWW on the given remote
// timestamp (strictly newer wins; ties keep the incumbent). Never touches the
// local node's own fields (owner_node_id must not equal the local node id).
int fleece_state_manager_merge_named(FleeceStateManager* manager, uint64_t owner_node_id, const char* name, const uint8_t* data, uint32_t size, uint64_t remote_timestamp, bool is_tombstone);

// --- Shared fields (backs the "world" script object) ---
//
// Unlike self (owned by the local node) or swarm (a read-only view of peers'
// self fields), shared fields have no single owner: any node may create or
// update one, and conflicting writes resolve with the same LWW-by-timestamp
// rule as everything else. That means, unlike self/swarm (where a given
// (key, owner) pair only ever has one real writer, so timestamp comparisons
// are always within one node's own clock), shared-field comparisons ARE
// across different nodes' independent local clocks: a long-uptime node's
// claim can occasionally out-rank a genuinely more recent claim from a
// freshly-booted node with a smaller counter. Fixing that needs a
// synchronized wall clock or a vector clock - neither exists here; this is a
// known, accepted limitation, not a bug.
//
// Shared fields are NOT tied to any real node's liveness (see
// fleece_state_manager_ticks_since_seen) - a target persists until explicitly
// removed, independent of which node(s) discovered or last touched it.

// Set a shared field (owner = FLEECE_SHARED_OWNER_ID)
int fleece_state_manager_set_shared(FleeceStateManager* manager, const char* name, const uint8_t* data, uint32_t size);

// Remove a shared field
int fleece_state_manager_remove_shared(FleeceStateManager* manager, const char* name);
// (reads reuse fleece_state_manager_get_named/exists_named/list_fields with
// owner_node_id = FLEECE_SHARED_OWNER_ID - no separate read API needed)

// Export the local node's own named fields as a gossip wire frame (full state)
int fleece_state_manager_export(FleeceStateManager* manager, uint8_t** frame_data, uint32_t* frame_size);

// Export only the local node's own named fields whose timestamp is newer than
// since_timestamp (delta gossip). Pass 0 for the full state (equivalent to
// fleece_state_manager_export). Deletions since since_timestamp are included
// as tombstone records, same as a full export.
int fleece_state_manager_export_delta(FleeceStateManager* manager, uint64_t since_timestamp, uint8_t** frame_data, uint32_t* frame_size);

// Same as export/export_delta, but for shared fields (owner = FLEECE_SHARED_OWNER_ID)
// instead of the local node's own fields. Sent as a separate gossip frame -
// see src/runtime/fleece_runtime.c.
int fleece_state_manager_export_shared(FleeceStateManager* manager, uint8_t** frame_data, uint32_t* frame_size);
int fleece_state_manager_export_shared_delta(FleeceStateManager* manager, uint64_t since_timestamp, uint8_t** frame_data, uint32_t* frame_size);

// Import a gossip wire frame received from a peer, merging its fields with LWW.
// Handles both self-stream and shared-stream frames - the frame's own
// owner_node_id (a real peer id, or FLEECE_SHARED_OWNER_ID) determines where
// the fields land.
int fleece_state_manager_import(FleeceStateManager* manager, const uint8_t* frame_data, uint32_t frame_size);

#ifdef __cplusplus
}
#endif

#endif // FLEECE_STATE_MANAGER_H
