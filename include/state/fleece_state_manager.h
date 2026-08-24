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

// Compact memory. Drops tombstones for legacy unnamed (raw-key) entries and
// reclaims their slots. NAMED tombstones are RETAINED: a delete-marker is
// convergence metadata - dropping it lets a stale replica's older live copy
// resurrect the deleted field on the next resync. Named tombstones carry no
// payload (one slot, no data buffer), so retaining them is cheap; they exist
// because deletes must outlive any peer that has not yet heard about them.
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

// Merge a shared/"world" field received via gossip (owner = FLEECE_SHARED_OWNER_ID).
// origin_node_id is the real node that authored this value - distinct from the
// storage owner, and used to break exact-timestamp ties deterministically
// (see the "Shared fields" section below). LWW otherwise works the same way
// as fleece_state_manager_merge_named.
int fleece_state_manager_merge_shared(FleeceStateManager* manager, uint64_t origin_node_id, const char* name, const uint8_t* data, uint32_t size, uint64_t remote_timestamp, bool is_tombstone);

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
// A separate, narrower problem IS fixed: if two nodes write the same field
// with the exact same timestamp (plausible - local_timestamp is a small
// per-node counter, not a high-resolution clock), fleece_state_manager_import
// / fleece_state_manager_merge_shared track each value's true author
// (origin_node_id, distinct from the FLEECE_SHARED_OWNER_ID used for storage/
// routing) and break exact ties deterministically (higher origin_node_id
// wins) - so every node converges on the SAME winner, rather than each side
// stubbornly keeping its own write forever.
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

// Local compare-and-set for a shared field: applies new_data only if the
// CURRENTLY stored bytes for `name` exactly match expected_data/expected_size.
// Pass expected_data == NULL (expected_size ignored) to require the field be
// currently absent/tombstoned - e.g. "claim only if unclaimed". Returns 0 if
// the write was applied, 1 if the comparison failed (not an error - just
// "someone/something else got there first" from this node's local point of
// view), -1 on a real error (bad arguments, capacity).
//
// This is a LOCAL compare-and-set only: it compares against what THIS node
// currently knows, so two nodes can still both succeed "locally" before
// either has heard from the other - there is no central lock or consensus
// here, by design. Combined with the deterministic origin-id tie-break above
// (so every node converges on the same eventual winner even after such a
// race), the recommended pattern for claiming something contested is:
// optimistically CAS your claim in, then a few ticks later re-check that your
// claim is still the one that stuck (gossip needs time to propagate) and back
// off if it isn't.
int fleece_state_manager_set_shared_cas(FleeceStateManager* manager, const char* name,
                                          const uint8_t* expected_data, uint32_t expected_size,
                                          const uint8_t* new_data, uint32_t new_size);

// Gossip wire frames are real CBOR (RFC 8949), not a bespoke format: a 3-byte
// ['F']['G'][version] prefix (a quick sanity/version check that isn't itself
// part of the CBOR payload) followed by a CBOR-encoded
// [owner_node_id, hw, digest, [record, record, ...]] array.
//
// `hw` is the sender's per-stream high-water mark: the highest record timestamp
// it holds for that stream - kept for diagnostics and resync probing.
//
// `digest` is an order-independent 64-bit hash (FNV-1a sum of per-entry hashes)
// over the sender's LIVE entries for that stream. The receiver computes the same
// digest over what it stores and compares: any divergence - not just a missing
// newest record, which is all the v3 scalar-hw check could catch - flags the
// receiver as behind and triggers an on-demand full resync. Tombstones are
// excluded from the digest on both sides, so they cannot cause phantom
// mismatch; deletes travel as records and persist until every peer has applied
// them (see fleece_state_manager_compact).
//
// A self-stream record is [is_tombstone, name, timestamp, data]; a
// shared/"world"-stream record additionally carries its author up front:
// [origin_node_id, is_tombstone, name, timestamp, data] (see
// fleece_state_manager_merge_shared). Only the CBOR major types actually needed
// here are supported (uint, bstring, tstring, array, bool) - this is not a
// general-purpose CBOR codec.
//
// Note: as of the world-centric model, the runtime only gossips the SHARED
// stream - each node's "self" object is node-local storage. A node publishes a
// self field to the swarm by writing it to world explicitly (see
// fleece_state_manager_set_shared). The self-stream export/import API remains
// for tests and offline tooling.

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
// the fields land and which record shape (see above) to expect.
int fleece_state_manager_import(FleeceStateManager* manager, const uint8_t* frame_data, uint32_t frame_size);

// Import with on-demand resync support. Same merge as fleece_state_manager_import,
// and additionally reports (via behind_self / behind_shared - both optional,
// NULL is fine) whether the receiver is now *behind* on the stream the frame
// carried: the sender's advertised high-water mark exceeds the highest record
// timestamp the receiver stores for that stream, meaning a delta was dropped
// and a full resync from this peer is warranted. Only the flag matching the
// frame's own stream is set; the other is left untouched. The caller can then
// request a full export from the sender (see fleece_runtime.c Phase 3).
//
// Known limitation: the gap check compares a single scalar hw per stream. It
// reliably detects "I'm missing the stream's newest record", but can miss a
// gap on a non-newest field; LWW means only the newest version of each field
// matters, and a stale non-max field is repaired on that field's next update
// or at the next on-demand resync.
int fleece_state_manager_import_ex(FleeceStateManager* manager, const uint8_t* frame_data, uint32_t frame_size,
                                   bool* behind_self, bool* behind_shared);

// Per-stream high-water marks: the highest record timestamp the manager holds
// for its own self stream, the shared stream, or a stored peer's self stream.
// Convenience for the runtime's on-demand resync probe and for convergence
// checks (all nodes should agree once the swarm has synchronized).
uint64_t fleece_state_manager_get_self_hw(FleeceStateManager* manager);
uint64_t fleece_state_manager_get_shared_hw(FleeceStateManager* manager);
uint64_t fleece_state_manager_get_peer_self_hw(FleeceStateManager* manager, uint64_t peer_id);

// Order-independent 64-bit digest over the LIVE (non-tombstoned) named entries
// owned by owner_node_id - the same value a gossip frame carries in its `digest`
// field. Two nodes hold identical views of a stream iff this digest matches
// (modulo hash collisions); used by import gap detection and by tests to assert
// convergence. Returns 0 on success, -1 on bad arguments.
// Ticks elapsed since the local node last WROTE any field (set/remove/CAS,
// named or shared). Returns UINT64_MAX if this node never wrote anything.
// Used to distinguish "my view differs because I just changed things" (benign
// churn - do not repair) from "my view differs because a delta was lost"
// (worth repairing): anti-entropy repairs are meaningful only once the local
// node has gone quiet.
uint64_t fleece_state_manager_ticks_since_last_write(FleeceStateManager* manager);

int fleece_state_manager_stream_digest(FleeceStateManager* manager, uint64_t owner_node_id, uint64_t* digest_out);

// --- Targeted anti-entropy (control frames) ---------------------------------
//
// A digest mismatch says "our views differ" - but under ordinary churn that is
// usually BENIGN CONCURRENCY (each side holds a fresh write the other has not
// heard yet), not a lost packet. Answering every mismatch with a full-snapshot
// pull wastes O(world) airtime on gaps that are often empty. These helpers
// support the cheap two-step repair instead:
//
//   1. receiver asks the sender for its stream INDEX,
//   2. receiver diffs the index against its own store (entry present? whose
//      timestamp is newer?), and requests ONLY the keys it is missing or
//      stale on; the reply is a normal gossip frame carrying just those
//      records, merged by the regular import path.
//
// Control frames carry a ['F']['X'][version] prefix followed by tagged CBOR.
// This version constant is SHARED with the runtime's FX frame parser
// (FLEECE_RESYNC_VERSION in fleece_runtime.c) - if they disagree, every index
// reply is silently rejected at the receiver's version check. They last moved
// together at v2 (origin_node_id added for unicast routing).
#define FLEECE_CONTROL_PROTOCOL_VERSION 2
//   [1, [[key_hash, timestamp], ...]]          - stream index reply
//   [2, [key_hash, ...]]                       - value request (which keys)
// The value reply is a NORMAL gossip frame (['F']['G'][version] ...) filtered
// to the requested keys, so it merges through the standard import path with
// no special handling on the receiving side.

// Export the shared/"world" stream index: [['F']['X'][version], [1, entries]]
// where entries is [[name_hash, timestamp], ...] over LIVE entries only.
// Tombstones are deliberately absent: their absence plus a stale timestamp in
// the requester's store is indistinguishable from "nothing to fetch", which is
// exactly the semantics a deleted key should have.
int fleece_state_manager_export_shared_index(FleeceStateManager* manager, uint8_t** frame_data, uint32_t* frame_size);

// Export a NORMAL gossip frame containing only the live shared entries whose
// key hash appears in hashes[] (unknown hashes simply match nothing). Used to
// answer a value request; mergeable via fleece_state_manager_import like any
// other gossip frame.
int fleece_state_manager_export_shared_by_hash(FleeceStateManager* manager, const uint32_t* hashes, uint32_t count, uint8_t** frame_data, uint32_t* frame_size);

// Local check used when diffing an index against the store: returns 1 if the
// shared stream already holds an entry (live OR tombstone) for key_hash with
// timestamp >= ts (nothing to fetch), 0 if the key is missing or stale
// (fetch it), -1 on bad arguments.
int fleece_state_manager_shared_at_least(FleeceStateManager* manager, uint32_t key_hash, uint64_t ts);

#ifdef __cplusplus
}
#endif

#endif // FLEECE_STATE_MANAGER_H
