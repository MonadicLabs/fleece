#define _POSIX_C_SOURCE 200809L

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <time.h>

#include "fleece_state_manager.h"
#include "fleece_alloc.h"
#include "fleece_cbor.h"

#define FLEECE_MAX_TRACKED_PEERS 32

// Internal state structure for the state manager

struct FleeceStateManager {
    // Fixed-size hash map for storing field values
    // Using a simple array-based approach for microcontrollers
    struct FieldEntry {
        uint32_t key;
        uint8_t* data;
        uint32_t size;
        uint64_t timestamp;
        uint64_t node_id;         // storage/routing owner (local id, a peer id, or FLEECE_SHARED_OWNER_ID)
        uint64_t origin_node_id;  // the REAL node that authored this value (== node_id except for shared fields)
        char name[FLEECE_FIELD_NAME_MAX];  // empty for legacy raw-key entries
        bool exists;
        bool is_tombstone;
    } *fields;
    uint32_t field_count;
    uint32_t field_capacity;
    uint64_t local_timestamp;
    uint64_t node_id;

    uint64_t current_tick;  // advanced by fleece_state_manager_tick(); peer liveness only
    uint64_t last_write_tick;  // current_tick at the most recent local write (UINT64_MAX = never)
    struct PeerSeen {
        uint64_t node_id;
        uint64_t last_seen_tick;
        bool exists;
    } peers_seen[FLEECE_MAX_TRACKED_PEERS];
};

static const uint32_t FIELD_CAPACITY = 128;  // Max fields for microcontrollers (shared across local + peer fields)

#define FLEECE_GOSSIP_MAGIC0 'F'
#define FLEECE_GOSSIP_MAGIC1 'G'
#define FLEECE_GOSSIP_VERSION 5

// Protocol v5: v4 plus a SENDER id in the frame header. v4's header carried
// only the stream owner (for world gossip always FLEECE_SHARED_OWNER_ID), so a
// receiver could tell WHAT it was behind on but not WHO had advertised the
// divergent view - all peers aggregated into one anonymous "mesh" repair
// target, and at N>=6 under loss the mesh never converged within its tick
// budget. The sender id makes divergence attributable per peer: targeted,
// UNICAST index requests become possible, and each peer's own repair state
// tracks just that peer.
//
// As in v4, every frame also carries an order-independent 64-bit digest of the
// sender's live entries for that stream alongside the owner id and records
// (the v3 scalar high-water mark is still present for diagnostics). After
// merging, the receiver computes the same digest over what IT stores and
// flags itself as behind on mismatch. Unlike the v3 hw check - which could
// only detect "I'm missing the stream's newest record" - a digest mismatch
// catches ANY divergence, including a dropped update to a field that is not
// the stream's newest, which LWW would otherwise silently paper over until
// that field's next update.
//
// Tombstones are excluded from the digest on both sides: they are per-key
// delete-markers retained indefinitely (see fleece_state_manager_compact) so
// they cannot create a permanent phantom mismatch between a node that has seen
// a delete and one that compacted it away. Deletes propagate as records and,
// once every peer has applied them, cost only slot space - never bandwidth.

static uint32_t hash_name(const char* name) {
    uint32_t hash = 2166136261u;  // FNV-1a 32-bit
    for (const unsigned char* p = (const unsigned char*)name; *p; p++) {
        hash ^= *p;
        hash *= 16777619u;
    }
    return hash;
}

// --- Stream digests (anti-entropy) ---------------------------------------
//
// A stream's digest is the arithmetic SUM of a 64-bit FNV-1a hash per live
// entry. Summing makes it order-independent: sender and receiver iterate their
// entry arrays in whatever order each happens to store them, and still arrive
// at the same value iff their live views are identical. Two nodes hold
// different live views of a stream with equal digests only via a hash
// collision (negligible at 64 bits, and the failure mode is merely a missed
// resync - the same risk the v3 hw check carried on every frame).

#define FNV64_OFFSET_BASIS 0xcbf29ce484222325ULL
#define FNV64_PRIME        0x100000001b3ULL

static void fnv64_mix(uint64_t* h, const void* bytes, size_t len) {
    const uint8_t* p = (const uint8_t*)bytes;
    for (size_t i = 0; i < len; i++) {
        *h ^= p[i];
        *h *= FNV64_PRIME;
    }
}

static void fnv64_mix_uint(uint64_t* h, uint64_t v) {
    // Fixed-width little-endian mix: never let a value's byte length depend
    // on its magnitude, so both sides hash identical bytes for identical values.
    uint8_t buf[8];
    for (int i = 0; i < 8; i++) buf[i] = (uint8_t)(v >> (8 * i));
    fnv64_mix(h, buf, sizeof(buf));
}

// Per-entry hash: name, author (shared fields have authors distinct from the
// storage owner), timestamp, and payload bytes. Tombstones are excluded by the
// caller - they are convergence metadata, not view state.
static uint64_t entry_digest(const struct FieldEntry* f) {
    uint64_t h = FNV64_OFFSET_BASIS;
    fnv64_mix(&h, f->name, strlen(f->name));
    uint8_t sep = 0xFF;  // name/value delimiter so ("ab","c") != ("a","bc")
    fnv64_mix(&h, &sep, 1);
    fnv64_mix_uint(&h, f->origin_node_id);
    fnv64_mix_uint(&h, f->timestamp);
    fnv64_mix_uint(&h, (uint64_t)f->size);
    if (f->size > 0) fnv64_mix(&h, f->data, f->size);
    return h;
}

// Digest of the live named view a node holds for owner_node_id. Unnamed
// (legacy raw-key) entries are excluded - they never appear in gossip frames,
// so including them would make two identically-synced nodes disagree.
static uint64_t compute_stream_digest(FleeceStateManager* manager, uint64_t owner_node_id) {
    uint64_t sum = 0;
    for (uint32_t i = 0; i < manager->field_capacity; i++) {
        struct FieldEntry* f = &manager->fields[i];
        if (!f->exists || f->is_tombstone || f->node_id != owner_node_id || f->name[0] == '\0') continue;
        sum += entry_digest(f);
    }
    return sum;
}

// Defined further below; the bounded delta exporter needs it for its header.
static uint64_t max_timestamp_for_owner(FleeceStateManager* manager, uint64_t owner_node_id);

// --- Minimal CBOR (RFC 8949) support -----------------------------------
// Only what this file's wire format needs: unsigned integers, byte strings,
// text strings, arrays, and booleans. Not a general-purpose CBOR codec.
//
// Shared with fleece_planner via src/state/fleece_cbor.{c,h}.

FleeceStateManager* fleece_state_manager_create_with_node_id(uint64_t node_id) {
    if (node_id == FLEECE_SHARED_OWNER_ID) {
        return NULL;  // reserved for shared/"world" fields, not a real node identity
    }

    FleeceStateManager* manager = (FleeceStateManager*)fleece_calloc(1, sizeof(FleeceStateManager));
    if (!manager) {
        return NULL;
    }

    manager->fields = (struct FieldEntry*)fleece_calloc(FIELD_CAPACITY, sizeof(struct FieldEntry));
    if (!manager->fields) {
        fleece_free(manager);
        return NULL;
    }

    manager->field_count = 0;
    manager->field_capacity = FIELD_CAPACITY;
    manager->local_timestamp = 0;
    manager->node_id = node_id;
    manager->last_write_tick = UINT64_MAX;

    return manager;
}

FleeceStateManager* fleece_state_manager_create(void) {
    // In a real implementation, node_id would be read from hardware (e.g., MAC address)
    return fleece_state_manager_create_with_node_id(0x123456789ABCDEF0ULL);
}

void fleece_state_manager_destroy(FleeceStateManager* manager) {
    if (!manager) return;

    for (uint32_t i = 0; i < manager->field_capacity; i++) {
        if (manager->fields[i].exists && !manager->fields[i].is_tombstone) {
            fleece_free(manager->fields[i].data);
        }
    }

    fleece_free(manager->fields);
    fleece_free(manager);
}

uint64_t fleece_state_manager_get_node_id(FleeceStateManager* manager) {
    return manager ? manager->node_id : 0;
}

// Records that a frame/merge was received from node_id "now" (current_tick).
// Called from both import() (even for empty deltas) and merge_named() (so
// direct callers, e.g. tests, also register liveness).
static void touch_peer(FleeceStateManager* manager, uint64_t node_id) {
    if (node_id == manager->node_id) return;  // we don't track our own liveness
    if (node_id == FLEECE_SHARED_OWNER_ID) return;  // the shared namespace isn't a real peer

    for (int i = 0; i < FLEECE_MAX_TRACKED_PEERS; i++) {
        if (manager->peers_seen[i].exists && manager->peers_seen[i].node_id == node_id) {
            manager->peers_seen[i].last_seen_tick = manager->current_tick;
            return;
        }
    }
    for (int i = 0; i < FLEECE_MAX_TRACKED_PEERS; i++) {
        if (!manager->peers_seen[i].exists) {
            manager->peers_seen[i].exists = true;
            manager->peers_seen[i].node_id = node_id;
            manager->peers_seen[i].last_seen_tick = manager->current_tick;
            return;
        }
    }
    // tracking table full - silently skip; doesn't affect data merge correctness
}

void fleece_state_manager_tick(FleeceStateManager* manager) {
    if (!manager) return;

    manager->current_tick++;
}

uint64_t fleece_state_manager_ticks_since_seen(FleeceStateManager* manager, uint64_t node_id) {
    if (!manager) return UINT64_MAX;

    for (int i = 0; i < FLEECE_MAX_TRACKED_PEERS; i++) {
        if (manager->peers_seen[i].exists && manager->peers_seen[i].node_id == node_id) {
            return manager->current_tick - manager->peers_seen[i].last_seen_tick;
        }
    }
    return UINT64_MAX;
}

// Finds a live or tombstoned slot for (key, owner_node_id), regardless of tombstone state.
static struct FieldEntry* find_slot_owned(FleeceStateManager* manager, uint32_t key, uint64_t owner_node_id) {
    for (uint32_t i = 0; i < manager->field_capacity; i++) {
        if (manager->fields[i].exists && manager->fields[i].key == key && manager->fields[i].node_id == owner_node_id) {
            return &manager->fields[i];
        }
    }
    return NULL;
}

// Finds a live (non-tombstoned) slot for (key, owner_node_id).
static struct FieldEntry* find_field_owned(FleeceStateManager* manager, uint32_t key, uint64_t owner_node_id) {
    struct FieldEntry* field = find_slot_owned(manager, key, owner_node_id);
    return (field && !field->is_tombstone) ? field : NULL;
}

// Preserves the original raw-key API's behavior: implicitly scoped to the local node.
static struct FieldEntry* find_field(FleeceStateManager* manager, uint32_t key) {
    return find_field_owned(manager, key, manager->node_id);
}

// Inserts or updates the (key, owner_node_id) slot. Allocates the new data buffer
// before touching the slot, so a failed allocation never leaves a dangling pointer.
static int upsert_field(FleeceStateManager* manager, uint32_t key, uint64_t owner_node_id, uint64_t origin_node_id,
                         const char* name, const uint8_t* data, uint32_t size, uint64_t timestamp, bool is_tombstone) {
    uint8_t* new_data = NULL;
    if (!is_tombstone) {
        new_data = (uint8_t*)fleece_malloc(size);
        if (!new_data) {
            return -1;
        }
        memcpy(new_data, data, size);
    }

    struct FieldEntry* field = find_slot_owned(manager, key, owner_node_id);
    if (!field) {
        if (manager->field_count >= manager->field_capacity) {
            fleece_free(new_data);
            return -1;  // Field limit reached
        }
        field = &manager->fields[manager->field_count++];
        field->key = key;
        field->exists = true;
        field->name[0] = '\0';
    }

    uint8_t* old_data = field->data;
    field->data = new_data;
    field->size = is_tombstone ? 0 : size;
    field->timestamp = timestamp;
    field->node_id = owner_node_id;
    field->origin_node_id = origin_node_id;
    field->is_tombstone = is_tombstone;
    if (name) {
        strncpy(field->name, name, FLEECE_FIELD_NAME_MAX - 1);
        field->name[FLEECE_FIELD_NAME_MAX - 1] = '\0';
    }
    fleece_free(old_data);

    return 0;
}

int fleece_state_manager_set(FleeceStateManager* manager, uint32_t key, const uint8_t* data, uint32_t size) {
    if (!manager || !data || size == 0) {
        return -1;
    }

    return upsert_field(manager, key, manager->node_id, manager->node_id, NULL, data, size, ++manager->local_timestamp, false);
    manager->last_write_tick = manager->current_tick;
    manager->last_write_tick = manager->current_tick;
}

int fleece_state_manager_get(FleeceStateManager* manager, uint32_t key, uint8_t** data, uint32_t* size) {
    if (!manager || !data || !size) {
        return -1;
    }

    struct FieldEntry* field = find_field(manager, key);
    if (!field) {
        return -1;
    }

    *data = (uint8_t*)fleece_malloc(field->size);
    if (!*data) {
        return -1;
    }

    memcpy(*data, field->data, field->size);
    *size = field->size;

    return 0;
}

bool fleece_state_manager_exists(FleeceStateManager* manager, uint32_t key) {
    if (!manager) return false;

    return find_field(manager, key) != NULL;
}

int fleece_state_manager_remove(FleeceStateManager* manager, uint32_t key) {
    if (!manager) return -1;

    struct FieldEntry* field = find_field(manager, key);
    if (!field) {
        return -1;
    }

    fleece_free(field->data);
    field->data = NULL;
    field->size = 0;
    // Stamp the tombstone with a fresh clock tick - same as remove_named.
    // Without this the tombstone carries the deleted VALUE's timestamp and
    // loses its own LWW race on every peer holding that value, so the delete
    // never propagates.
    field->timestamp = ++manager->local_timestamp;
    manager->last_write_tick = manager->current_tick;
    field->is_tombstone = true;
    return 0;
}

// Reclaims slots from legacy unnamed tombstones only. A NAMED tombstone is a
// delete-marker other nodes may still need to learn about (or re-learn after a
// resync); dropping it risks a stale replica resurrecting the deleted field,
// so named tombstones persist. They hold no data buffer, so the retention cost
// is one slot each - deletes permanently consume a slot, which is the price of
// delete correctness without ack-tracking infrastructure.
int fleece_state_manager_compact(FleeceStateManager* manager) {
    if (!manager) return -1;

    uint32_t write_idx = 0;
    for (uint32_t read_idx = 0; read_idx < manager->field_capacity; read_idx++) {
        struct FieldEntry* f = &manager->fields[read_idx];
        // Keep: live entries of any kind, and named tombstones (delete-markers).
        if (f->exists && (!f->is_tombstone || f->name[0] != '\0')) {
            if (write_idx != read_idx) {
                manager->fields[write_idx] = *f;
                memset(f, 0, sizeof(struct FieldEntry));
            }
            write_idx++;
        }
    }

    manager->field_count = write_idx;
    return 0;
}

int fleece_state_manager_get_version(FleeceStateManager* manager, uint32_t key, FleeceFieldVersion* version) {
    if (!manager || !version) return -1;

    struct FieldEntry* field = find_field(manager, key);
    if (!field) {
        return -1;
    }

    version->timestamp = field->timestamp;
    version->node_id = field->node_id;
    version->size = field->size;

    return 0;
}

// --- Named, owner-scoped fields ---

int fleece_state_manager_set_named(FleeceStateManager* manager, const char* name, const uint8_t* data, uint32_t size) {
    if (!manager || !name || !name[0] || !data || size == 0) {
        return -1;
    }

    return upsert_field(manager, hash_name(name), manager->node_id, manager->node_id, name, data, size, ++manager->local_timestamp, false);
    manager->last_write_tick = manager->current_tick;
    manager->last_write_tick = manager->current_tick;
}

int fleece_state_manager_remove_named(FleeceStateManager* manager, const char* name) {
    if (!manager || !name) return -1;

    struct FieldEntry* field = find_field_owned(manager, hash_name(name), manager->node_id);
    if (!field) {
        return -1;
    }

    fleece_free(field->data);
    field->data = NULL;
    field->size = 0;
    field->is_tombstone = true;
    field->timestamp = ++manager->local_timestamp;
    manager->last_write_tick = manager->current_tick;
    return 0;
}

int fleece_state_manager_get_named(FleeceStateManager* manager, uint64_t owner_node_id, const char* name, uint8_t** data, uint32_t* size) {
    if (!manager || !name || !data || !size) {
        return -1;
    }

    struct FieldEntry* field = find_field_owned(manager, hash_name(name), owner_node_id);
    if (!field) {
        return -1;
    }

    *data = (uint8_t*)fleece_malloc(field->size);
    if (!*data) {
        return -1;
    }

    memcpy(*data, field->data, field->size);
    *size = field->size;

    return 0;
}

bool fleece_state_manager_exists_named(FleeceStateManager* manager, uint64_t owner_node_id, const char* name) {
    if (!manager || !name) return false;

    return find_field_owned(manager, hash_name(name), owner_node_id) != NULL;
}

int fleece_state_manager_get_meta_named(FleeceStateManager* manager, uint64_t owner_node_id,
                                        const char* name, uint64_t* ts_out, uint64_t* origin_out) {
    if (!manager || !name || !ts_out || !origin_out) return -1;

    struct FieldEntry* field = find_field_owned(manager, hash_name(name), owner_node_id);
    if (!field) {
        *ts_out = 0;
        *origin_out = 0;
        return -1;
    }
    *ts_out = field->timestamp;
    *origin_out = field->origin_node_id;
    return 0;
}

// --- Shared fields (owner = FLEECE_SHARED_OWNER_ID; see header for the LWW caveat) ---

int fleece_state_manager_set_shared(FleeceStateManager* manager, const char* name, const uint8_t* data, uint32_t size) {
    if (!manager || !name || !name[0] || !data || size == 0) {
        return -1;
    }

    return upsert_field(manager, hash_name(name), FLEECE_SHARED_OWNER_ID, manager->node_id, name, data, size, ++manager->local_timestamp, false);
    manager->last_write_tick = manager->current_tick;
    manager->last_write_tick = manager->current_tick;
}

int fleece_state_manager_remove_shared(FleeceStateManager* manager, const char* name) {
    if (!manager || !name) return -1;

    struct FieldEntry* field = find_field_owned(manager, hash_name(name), FLEECE_SHARED_OWNER_ID);
    if (!field) {
        return -1;
    }

    fleece_free(field->data);
    field->data = NULL;
    field->size = 0;
    // Re-stamp BOTH version components: a fresh logical tick AND this node as
    // the author. The tombstone is a new WRITE by the deleter - if it kept the
    // old value's origin, two indistinguishable versions (same timestamp, same
    // origin, one live one dead) could circulate, and the tie-break could
    // deadlock half the swarm on each side of them.
    field->timestamp = ++manager->local_timestamp;
    manager->last_write_tick = manager->current_tick;
    field->origin_node_id = manager->node_id;
    field->is_tombstone = true;
    return 0;
}

int fleece_state_manager_set_shared_cas(FleeceStateManager* manager, const char* name,
                                          const uint8_t* expected_data, uint32_t expected_size,
                                          const uint8_t* new_data, uint32_t new_size) {
    if (!manager || !name || !name[0] || !new_data || new_size == 0) {
        return -1;
    }

    uint32_t key = hash_name(name);
    struct FieldEntry* current = find_field_owned(manager, key, FLEECE_SHARED_OWNER_ID);

    if (expected_data == NULL) {
        if (current != NULL) return 1;  // expected absent, but it exists
    } else {
        if (current == NULL) return 1;  // expected present, but it's absent
        if (current->size != expected_size || memcmp(current->data, expected_data, expected_size) != 0) {
            return 1;  // current value doesn't match what the caller expected
        }
    }

    if (upsert_field(manager, key, FLEECE_SHARED_OWNER_ID, manager->node_id, name, new_data, new_size, ++manager->local_timestamp, false) != 0) {
    manager->last_write_tick = manager->current_tick;
        return -1;
    }
    return 0;
}

uint32_t fleece_state_manager_list_nodes(FleeceStateManager* manager, uint64_t* node_ids_out, uint32_t max_nodes) {
    if (!manager || !node_ids_out || max_nodes == 0) return 0;

    uint32_t count = 0;
    for (uint32_t i = 0; i < manager->field_capacity && count < max_nodes; i++) {
        struct FieldEntry* f = &manager->fields[i];
        if (!f->exists || f->is_tombstone || f->node_id == FLEECE_SHARED_OWNER_ID) continue;  // not a real node

        bool seen = false;
        for (uint32_t j = 0; j < count; j++) {
            if (node_ids_out[j] == f->node_id) {
                seen = true;
                break;
            }
        }
        if (!seen) {
            node_ids_out[count++] = f->node_id;
        }
    }
    return count;
}

uint32_t fleece_state_manager_list_fields(FleeceStateManager* manager, uint64_t owner_node_id, char names_out[][FLEECE_FIELD_NAME_MAX], uint32_t max_names) {
    if (!manager || !names_out || max_names == 0) return 0;

    uint32_t count = 0;
    for (uint32_t i = 0; i < manager->field_capacity && count < max_names; i++) {
        struct FieldEntry* f = &manager->fields[i];
        if (!f->exists || f->is_tombstone || f->node_id != owner_node_id || f->name[0] == '\0') continue;

        strncpy(names_out[count], f->name, FLEECE_FIELD_NAME_MAX - 1);
        names_out[count][FLEECE_FIELD_NAME_MAX - 1] = '\0';
        count++;
    }
    return count;
}

int fleece_state_manager_merge_named(FleeceStateManager* manager, uint64_t owner_node_id, const char* name,
                                      const uint8_t* data, uint32_t size, uint64_t remote_timestamp, bool is_tombstone) {
    if (!manager || !name || !name[0]) return -1;
    if (!is_tombstone && (!data || size == 0)) return -1;
    if (owner_node_id == manager->node_id) return -1;  // never let the network overwrite our own self

    touch_peer(manager, owner_node_id);
    manager->local_timestamp = manager->local_timestamp > remote_timestamp ? manager->local_timestamp : remote_timestamp;

    uint32_t key = hash_name(name);
    struct FieldEntry* incumbent = find_slot_owned(manager, key, owner_node_id);
    if (incumbent && incumbent->timestamp >= remote_timestamp) {
        return 0;  // incumbent is newer or tied; keep it (safe: this (key,owner) pair only ever has one real writer)
    }

    return upsert_field(manager, key, owner_node_id, owner_node_id, name, data, size, remote_timestamp, is_tombstone);
}

int fleece_state_manager_merge_shared(FleeceStateManager* manager, uint64_t origin_node_id, const char* name,
                                       const uint8_t* data, uint32_t size, uint64_t remote_timestamp, bool is_tombstone) {
    if (!manager || !name || !name[0]) return -1;
    if (!is_tombstone && (!data || size == 0)) return -1;

    manager->local_timestamp = manager->local_timestamp > remote_timestamp ? manager->local_timestamp : remote_timestamp;

    uint32_t key = hash_name(name);
    struct FieldEntry* incumbent = find_slot_owned(manager, key, FLEECE_SHARED_OWNER_ID);
    if (incumbent) {
        if (incumbent->timestamp > remote_timestamp) return 0;  // incumbent strictly newer
        // Exact-timestamp tie: break deterministically by origin id so every
        // node converges on the SAME winner, instead of each side keeping
        // whatever it already had (see header comment).
        if (incumbent->timestamp == remote_timestamp && incumbent->origin_node_id >= origin_node_id) return 0;
    }

    return upsert_field(manager, key, FLEECE_SHARED_OWNER_ID, origin_node_id, name, data, size, remote_timestamp, is_tombstone);
}

uint64_t fleece_state_manager_get_local_timestamp(FleeceStateManager* manager) {
    return manager ? manager->local_timestamp : 0;
}

uint64_t fleece_state_manager_ticks_since_last_write(FleeceStateManager* manager) {
    if (!manager || manager->last_write_tick == UINT64_MAX) return UINT64_MAX;
    if (manager->current_tick < manager->last_write_tick) return 0;  // tick wrap/reinit safety
    return manager->current_tick - manager->last_write_tick;
}

// Serializes fields owned by owner_filter (the local node's own id, or
// FLEECE_SHARED_OWNER_ID) with timestamp > since_timestamp as a CBOR gossip
// frame. since_timestamp == 0 yields every such field (a full export), since
// real timestamps start at 1 (see upsert_field's ++manager->local_timestamp).
static int export_fields_since(FleeceStateManager* manager, uint64_t owner_filter, uint64_t since_timestamp, uint8_t** frame_data, uint32_t* frame_size) {
    if (!manager || !frame_data || !frame_size) {
        return -1;
    }

    bool is_shared_stream = (owner_filter == FLEECE_SHARED_OWNER_ID);

    // High-water mark of the stream: the highest record timestamp the sender
    // holds for this stream (all named fields, regardless of the delta cutoff).
    uint64_t hw = 0;
    for (uint32_t i = 0; i < manager->field_capacity; i++) {
        struct FieldEntry* f = &manager->fields[i];
        if (!f->exists || f->node_id != owner_filter || f->name[0] == '\0') continue;
        if (f->timestamp > hw) hw = f->timestamp;
    }

    // Digest of the sender's FULL live view of this stream (not just the delta
    // slice) - receivers compare it against their own merged view to detect
    // any divergence, delta-sized or not.
    uint64_t digest = compute_stream_digest(manager, owner_filter);

    uint32_t count = 0;
    // v5: outer array is [sender, owner, hw, digest, [records]] - one more
    // header-sized element than v4.
    size_t body_size = fleece_cbor_array_header_size(5) + fleece_cbor_uint_size(manager->node_id)
                     + fleece_cbor_uint_size(owner_filter)
                     + fleece_cbor_uint_size(hw)
                     + fleece_cbor_uint_size(digest);
    for (uint32_t i = 0; i < manager->field_capacity; i++) {
        struct FieldEntry* f = &manager->fields[i];
        if (!f->exists || f->node_id != owner_filter || f->name[0] == '\0' || f->timestamp <= since_timestamp) continue;

        uint32_t name_len = (uint32_t)strlen(f->name);
        uint32_t data_len = f->is_tombstone ? 0 : f->size;

        body_size += fleece_cbor_array_header_size(is_shared_stream ? 5 : 4);
        if (is_shared_stream) body_size += fleece_cbor_uint_size(f->origin_node_id);
        body_size += 1;  // bool is always 1 byte
        body_size += fleece_cbor_text_size(name_len);
        body_size += fleece_cbor_uint_size(f->timestamp);
        body_size += fleece_cbor_bytes_size(data_len);
        count++;
    }
    body_size += fleece_cbor_array_header_size(count);

    uint8_t* buf = (uint8_t*)fleece_malloc(3 + body_size);
    if (!buf) {
        return -1;
    }

    size_t pos = 0;
    buf[pos++] = FLEECE_GOSSIP_MAGIC0;
    buf[pos++] = FLEECE_GOSSIP_MAGIC1;
    buf[pos++] = FLEECE_GOSSIP_VERSION;

    fleece_cbor_write_array_header(buf, &pos, 5);
    fleece_cbor_write_uint(buf, &pos, manager->node_id);  // v5: who is advertising this view
    fleece_cbor_write_uint(buf, &pos, owner_filter);
    fleece_cbor_write_uint(buf, &pos, hw);
    fleece_cbor_write_uint(buf, &pos, digest);
    fleece_cbor_write_array_header(buf, &pos, count);

    for (uint32_t i = 0; i < manager->field_capacity; i++) {
        struct FieldEntry* f = &manager->fields[i];
        if (!f->exists || f->node_id != owner_filter || f->name[0] == '\0' || f->timestamp <= since_timestamp) continue;

        uint32_t name_len = (uint32_t)strlen(f->name);
        uint32_t data_len = f->is_tombstone ? 0 : f->size;

        fleece_cbor_write_array_header(buf, &pos, is_shared_stream ? 5 : 4);
        if (is_shared_stream) fleece_cbor_write_uint(buf, &pos, f->origin_node_id);
        fleece_cbor_write_bool(buf, &pos, f->is_tombstone);
        fleece_cbor_write_text(buf, &pos, f->name, name_len);
        fleece_cbor_write_uint(buf, &pos, f->timestamp);
        fleece_cbor_write_bytes(buf, &pos, f->data, data_len);
    }

    *frame_data = buf;
    *frame_size = (uint32_t)pos;
    return 0;
}

int fleece_state_manager_export(FleeceStateManager* manager, uint8_t** frame_data, uint32_t* frame_size) {
    if (!manager) return -1;
    return export_fields_since(manager, manager->node_id, 0, frame_data, frame_size);
}

int fleece_state_manager_export_delta(FleeceStateManager* manager, uint64_t since_timestamp, uint8_t** frame_data, uint32_t* frame_size) {
    if (!manager) return -1;
    return export_fields_since(manager, manager->node_id, since_timestamp, frame_data, frame_size);
}

int fleece_state_manager_export_shared(FleeceStateManager* manager, uint8_t** frame_data, uint32_t* frame_size) {
    if (!manager) return -1;
    return export_fields_since(manager, FLEECE_SHARED_OWNER_ID, 0, frame_data, frame_size);
}

int fleece_state_manager_export_shared_delta(FleeceStateManager* manager, uint64_t since_timestamp, uint8_t** frame_data, uint32_t* frame_size) {
    if (!manager) return -1;
    return export_fields_since(manager, FLEECE_SHARED_OWNER_ID, since_timestamp, frame_data, frame_size);
}

// Bounded variant: like export_shared_delta, but stops adding records once
// the serialized frame reaches cap_bytes, and reports through included_max_ts
// the highest record timestamp ACTUALLY carried - the value the caller must
// advance its watermark to. (Advancing past a truncation point would drop
// the excluded records from every future delta.) Keeping frames under the
// transport's single-packet ceiling avoids the Resource/Link fallback, whose
// pending transfers pin fixed-pool memory and starve the whole node.
// Bounded variant: like export_shared_delta, but stops adding records once
// the serialized frame reaches cap_bytes, and reports through included_max_ts
// the highest record timestamp ACTUALLY carried - the value the caller must
// advance its watermark to. (Advancing past a truncation point would drop
// the excluded records from every future delta.) Keeping frames under the
// transport's single-packet ceiling avoids the Resource/Link fallback, whose
// pending transfers pin fixed-pool memory and starve the whole node.
int fleece_state_manager_export_shared_delta_bounded(FleeceStateManager* manager, uint64_t since_timestamp,
                                                     size_t cap_bytes, uint8_t** frame_data, uint32_t* frame_size,
                                                     uint64_t* included_max_ts) {
    if (!manager || !frame_data || !frame_size) return -1;

    bool take[FIELD_CAPACITY];
    uint32_t count = 0;
    // Header: magic(3, outside `body`) + [sender, owner, hw, digest, count].
    // The count header is budgeted at its 3-byte worst case; the hw/digest at
    // theirs. Oversizing by a few bytes is fine - undersizing would corrupt.
    size_t body = fleece_cbor_array_header_size(5) + fleece_cbor_uint_size(manager->node_id)
                 + fleece_cbor_uint_size(FLEECE_SHARED_OWNER_ID)
                 + 9 + 9 + 3;
    /* Oldest-first, ALWAYS: the delta is a cap-bounded WINDOW over the
     * stream of changed records. Packing in slot order let the watermark
     * advance past records that didn't fit, permanently silencing them
     * whenever the world held more changed data than one frame carries
     * (found live: a POI written on every drone never reached the GC-SPU
     * because telemetry keys monopolized the frame). Oldest-first with the
     * watermark at the last SENT record rotates everything through within
     * a few frames, no matter how large the backlog grows. */
    uint32_t eligible[FIELD_CAPACITY];
    uint64_t eligible_ts[FIELD_CAPACITY];
    uint32_t eligible_n = 0;
    for (uint32_t i = 0; i < manager->field_capacity; i++) {
        struct FieldEntry* f = &manager->fields[i];
        if (!f->exists || f->node_id != FLEECE_SHARED_OWNER_ID || f->name[0] == '\0' || f->timestamp <= since_timestamp) continue;
        eligible[eligible_n] = i;
        eligible_ts[eligible_n] = f->timestamp;
        eligible_n++;
    }
    /* insertion sort by timestamp ascending (n <= capacity, nearly sorted) */
    for (uint32_t a = 1; a < eligible_n; a++) {
        uint32_t idx = eligible[a];
        uint64_t ts = eligible_ts[a];
        uint32_t b = a;
        while (b > 0 && eligible_ts[b - 1] > ts) {
            eligible[b] = eligible[b - 1];
            eligible_ts[b] = eligible_ts[b - 1];
            b--;
        }
        eligible[b] = idx;
        eligible_ts[b] = ts;
    }

    uint64_t max_ts = 0;
    for (uint32_t i = 0; i < manager->field_capacity; i++) take[i] = false;
    for (uint32_t e = 0; e < eligible_n; e++) {
        uint32_t i = eligible[e];
        struct FieldEntry* f = &manager->fields[i];
        if (f->timestamp > max_ts) max_ts = f->timestamp;
        uint32_t name_len = (uint32_t)strlen(f->name);
        uint32_t data_len = f->is_tombstone ? 0 : f->size;
        size_t rec = fleece_cbor_array_header_size(5)
                   + fleece_cbor_uint_size(f->origin_node_id)
                   + 1
                   + fleece_cbor_text_size(name_len)
                   + fleece_cbor_uint_size(f->timestamp)
                   + fleece_cbor_bytes_size(data_len);
        if (count > 0 && body + rec > cap_bytes) break;  /* oldest-first window ends here */
        body += rec;
        take[i] = true;
        count++;
    }
    if (included_max_ts) *included_max_ts = max_ts;

    uint8_t* buf = (uint8_t*)fleece_malloc(3 + body);
    if (!buf) return -1;
    size_t pos = 0;
    buf[pos++] = FLEECE_GOSSIP_MAGIC0;
    buf[pos++] = FLEECE_GOSSIP_MAGIC1;
    buf[pos++] = FLEECE_GOSSIP_VERSION;
    fleece_cbor_write_array_header(buf, &pos, 5);
    fleece_cbor_write_uint(buf, &pos, manager->node_id);
    fleece_cbor_write_uint(buf, &pos, FLEECE_SHARED_OWNER_ID);
    fleece_cbor_write_uint(buf, &pos, max_timestamp_for_owner(manager, FLEECE_SHARED_OWNER_ID));
    fleece_cbor_write_uint(buf, &pos, compute_stream_digest(manager, FLEECE_SHARED_OWNER_ID));
    fleece_cbor_write_array_header(buf, &pos, count);
    for (uint32_t i = 0; i < manager->field_capacity; i++) {
        struct FieldEntry* f = &manager->fields[i];
        if (!f->exists || !take[i]) continue;
        fleece_cbor_write_array_header(buf, &pos, 5);
        fleece_cbor_write_uint(buf, &pos, f->origin_node_id);
        fleece_cbor_write_bool(buf, &pos, f->is_tombstone);
        fleece_cbor_write_text(buf, &pos, f->name, (uint32_t)strlen(f->name));
        fleece_cbor_write_uint(buf, &pos, f->timestamp);
        fleece_cbor_write_bytes(buf, &pos, f->data, f->is_tombstone ? 0 : f->size);
    }
    *frame_data = buf;
    *frame_size = (uint32_t)pos;
    return 0;
}

uint32_t fleece_state_manager_count_new_shared(FleeceStateManager* manager, uint64_t since_timestamp) {
    if (!manager) return 0;
    uint32_t count = 0;
    for (uint32_t i = 0; i < manager->field_capacity; i++) {
        struct FieldEntry* f = &manager->fields[i];
        if (!f->exists || f->node_id != FLEECE_SHARED_OWNER_ID || f->name[0] == '\0' || f->timestamp <= since_timestamp) continue;
        count++;
    }
    return count;
}

// Shared import core (see the two public wrappers below). Parses a v5 gossip
// frame [sender_node_id, owner_node_id, hw, digest, [records]], merges its
// records with LWW, then - if behind_self/behind_shared are non-NULL - reports
// whether the receiver is now *behind* on that stream: its own computed view
// digest differs from the sender's advertised digest, meaning this receiver is
// missing (or stale on) at least one live field - a delta was dropped somewhere,
// and a resync from this peer is warranted. Only the flag matching the frame's
// own stream is set; the other is left untouched. The sender's node id is
// reported back through sender_node_id so the caller can run the repair
// handshake against THAT peer alone (see fleece_runtime.c Phase 3).
static int import_impl(FleeceStateManager* manager, const uint8_t* frame_data, uint32_t frame_size,
                       bool* behind_self, bool* behind_shared, uint64_t* sender_node_id) {
    if (!manager || !frame_data) {
        return -1;
    }
    if (frame_size < 3) {
        return -1;
    }
    if (frame_data[0] != FLEECE_GOSSIP_MAGIC0 || frame_data[1] != FLEECE_GOSSIP_MAGIC1 || frame_data[2] != FLEECE_GOSSIP_VERSION) {
        return -1;
    }

    size_t pos = 3;
    uint8_t major;
    uint64_t value;

    // Outer array: [sender_node_id, owner_node_id, hw, digest, records]
    if (!fleece_cbor_read_head(frame_data, frame_size, &pos, &major, &value) || major != 4 || value != 5) return -1;

    if (!fleece_cbor_read_head(frame_data, frame_size, &pos, &major, &value) || major != 0) return -1;
    uint64_t sender = value;

    // Reject loopback and reserved ids: the sender field must name a real
    // third-party node for per-peer attribution to mean anything.
    if (sender == manager->node_id || sender == FLEECE_SHARED_OWNER_ID) return -1;
    if (sender_node_id) *sender_node_id = sender;

    if (!fleece_cbor_read_head(frame_data, frame_size, &pos, &major, &value) || major != 0) return -1;
    uint64_t owner_node_id = value;

    bool is_shared_stream = (owner_node_id == FLEECE_SHARED_OWNER_ID);

    // v4 behavior kept: a SELF-stream claiming us as owner is bogus (the v5
    // sender check above already catches the honest form of this mistake).
    if (!is_shared_stream && owner_node_id == manager->node_id) return -1;

    if (!fleece_cbor_read_head(frame_data, frame_size, &pos, &major, &value) || major != 0) return -1;
    (void)value;  // hw: kept on the wire for diagnostics; the digest below supersedes it

    if (!fleece_cbor_read_head(frame_data, frame_size, &pos, &major, &value) || major != 0) return -1;
    uint64_t advertised_digest = value;

    touch_peer(manager, sender);  // even an empty delta counts as "heard from"; v5 makes the real relay visible

    if (!fleece_cbor_read_head(frame_data, frame_size, &pos, &major, &value) || major != 4) return -1;
    uint64_t count = value;

    for (uint64_t i = 0; i < count; i++) {
        if (!fleece_cbor_read_head(frame_data, frame_size, &pos, &major, &value) || major != 4) return -1;
        if (value != (uint64_t)(is_shared_stream ? 5 : 4)) return -1;

        uint64_t origin_node_id = owner_node_id;  // self-stream: origin is always the frame's owner
        if (is_shared_stream) {
            if (!fleece_cbor_read_head(frame_data, frame_size, &pos, &major, &value) || major != 0) return -1;
            origin_node_id = value;
        }

        if (!fleece_cbor_read_head(frame_data, frame_size, &pos, &major, &value) || major != 7 || (value != 20 && value != 21)) return -1;
        bool is_tombstone = (value == 21);

        if (!fleece_cbor_read_head(frame_data, frame_size, &pos, &major, &value) || major != 3) return -1;  // text string (name)
        if (!fleece_cbor_bounds_ok(pos, value, frame_size)) return -1;
        char name[FLEECE_FIELD_NAME_MAX];
        uint32_t copy_len = (uint32_t)(value < FLEECE_FIELD_NAME_MAX - 1 ? value : FLEECE_FIELD_NAME_MAX - 1);
        memcpy(name, &frame_data[pos], copy_len);
        name[copy_len] = '\0';
        pos += value;

        if (!fleece_cbor_read_head(frame_data, frame_size, &pos, &major, &value) || major != 0) return -1;
        uint64_t timestamp = value;

        if (!fleece_cbor_read_head(frame_data, frame_size, &pos, &major, &value) || major != 2) return -1;  // byte string (data)
        if (!fleece_cbor_bounds_ok(pos, value, frame_size)) return -1;
        const uint8_t* data = &frame_data[pos];
        uint32_t data_len = (uint32_t)value;
        pos += value;

        if (copy_len == 0) continue;  // skip malformed/empty names defensively

        if (is_shared_stream) {
            fleece_state_manager_merge_shared(manager, origin_node_id, name, is_tombstone ? NULL : data, data_len, timestamp, is_tombstone);
        } else {
            fleece_state_manager_merge_named(manager, owner_node_id, name, is_tombstone ? NULL : data, data_len, timestamp, is_tombstone);
        }
    }

    // Gap detection: after merging, does the receiver's live view of this
    // stream still differ from the sender's? A digest mismatch means at least
    // one field is missing or stale here (a dropped delta), regardless of
    // whether the missing record happened to be the stream's newest.
    uint64_t local_digest = compute_stream_digest(manager, is_shared_stream ? FLEECE_SHARED_OWNER_ID : owner_node_id);
    bool behind = (local_digest != advertised_digest);
    if (behind_self) *behind_self = !is_shared_stream && behind;
    if (behind_shared) *behind_shared = is_shared_stream && behind;

    return 0;
}

int fleece_state_manager_import(FleeceStateManager* manager, const uint8_t* frame_data, uint32_t frame_size) {
    return import_impl(manager, frame_data, frame_size, NULL, NULL, NULL);
}

int fleece_state_manager_import_ex(FleeceStateManager* manager, const uint8_t* frame_data, uint32_t frame_size,
                                   bool* behind_self, bool* behind_shared) {
    return import_impl(manager, frame_data, frame_size, behind_self, behind_shared, NULL);
}

int fleece_state_manager_import_from(FleeceStateManager* manager, const uint8_t* frame_data, uint32_t frame_size,
                                     bool* behind_self, bool* behind_shared, uint64_t* sender_node_id) {
    return import_impl(manager, frame_data, frame_size, behind_self, behind_shared, sender_node_id);
}

// --- Per-stream high-water marks ------------------------------------------
// Max record timestamp the manager holds for a stream. Used by the runtime's
// on-demand resync (probe peers whose hw we're behind on) and by tests to
// assert convergence. Same helper the exporter and importer use internally.

static uint64_t max_timestamp_for_owner(FleeceStateManager* manager, uint64_t owner_node_id) {
    uint64_t hw = 0;
    for (uint32_t i = 0; i < manager->field_capacity; i++) {
        struct FieldEntry* f = &manager->fields[i];
        if (!f->exists || f->node_id != owner_node_id) continue;
        if (f->timestamp > hw) hw = f->timestamp;
    }
    return hw;
}

uint64_t fleece_state_manager_get_self_hw(FleeceStateManager* manager) {
    if (!manager) return 0;
    return max_timestamp_for_owner(manager, manager->node_id);
}

uint64_t fleece_state_manager_get_shared_hw(FleeceStateManager* manager) {
    if (!manager) return 0;
    return max_timestamp_for_owner(manager, FLEECE_SHARED_OWNER_ID);
}

uint64_t fleece_state_manager_get_peer_self_hw(FleeceStateManager* manager, uint64_t peer_id) {
    if (!manager || peer_id == FLEECE_SHARED_OWNER_ID) return 0;
    return max_timestamp_for_owner(manager, peer_id);
}

int fleece_state_manager_stream_digest(FleeceStateManager* manager, uint64_t owner_node_id, uint64_t* digest_out) {
    if (!manager || !digest_out) return -1;
    *digest_out = compute_stream_digest(manager, owner_node_id);
    return 0;
}

// --- Targeted anti-entropy (control frames) ---------------------------------

#define FLEECE_CONTROL_MAGIC0 'F'
#define FLEECE_CONTROL_MAGIC1 'X'
#define FLEECE_CONTROL_VERSION FLEECE_CONTROL_PROTOCOL_VERSION  // must match runtime FX parser

// Index reply: ['F']['X'][version] + CBOR [1, [[key_hash, timestamp], ...]].
// Live entries only - see the header for why tombstones stay out.
int fleece_state_manager_export_shared_index(FleeceStateManager* manager, uint8_t** frame_data, uint32_t* frame_size) {
    if (!manager || !frame_data || !frame_size) return -1;

    // Two passes over the store: count matching entries for sizing, then write.
    uint32_t count = 0;
    for (uint32_t i = 0; i < manager->field_capacity; i++) {
        struct FieldEntry* f = &manager->fields[i];
        if (!f->exists || f->is_tombstone || f->node_id != FLEECE_SHARED_OWNER_ID || f->name[0] == '\0') continue;
        count++;
    }

    uint64_t self_id = manager->node_id;
    size_t total = 3 + fleece_cbor_array_header_size(3)
                 + 1  // tag byte
                 + fleece_cbor_uint_size(self_id)
                 + fleece_cbor_array_header_size(count)
                 + (size_t)count * (fleece_cbor_array_header_size(2)
                                    + fleece_cbor_uint_size(UINT32_MAX)
                                    + fleece_cbor_uint_size(UINT64_MAX));

    uint8_t* buf = (uint8_t*)fleece_malloc(total);
    if (!buf) return -1;

    size_t pos = 0;
    buf[pos++] = FLEECE_CONTROL_MAGIC0;
    buf[pos++] = FLEECE_CONTROL_MAGIC1;
    buf[pos++] = FLEECE_CONTROL_VERSION;
    fleece_cbor_write_array_header(buf, &pos, 3);
    fleece_cbor_write_uint(buf, &pos, 1);  // tag: index reply
    fleece_cbor_write_uint(buf, &pos, self_id);  // v2: who answered (unicast routing)
    fleece_cbor_write_array_header(buf, &pos, count);
    for (uint32_t i = 0; i < manager->field_capacity; i++) {
        struct FieldEntry* f = &manager->fields[i];
        if (!f->exists || f->is_tombstone || f->node_id != FLEECE_SHARED_OWNER_ID || f->name[0] == '\0') continue;
        fleece_cbor_write_array_header(buf, &pos, 2);
        fleece_cbor_write_uint(buf, &pos, f->key);      // key IS the FNV-1a name hash
        fleece_cbor_write_uint(buf, &pos, f->timestamp);
    }

    *frame_data = buf;
    *frame_size = (uint32_t)pos;
    return 0;
}

// Value reply: a normal gossip frame filtered to the requested key hashes.
// Unknown hashes match nothing; tombstoned keys are skipped (a delete has no
// value to ship - the requester learns deletes only from records or by never
// having had the entry).
//
// BYTE CAP: the reply stays under the transport's single-packet ceiling
// whenever possible. Oversized frames fall back to Resource/Link transfers,
// which on real radios establish slowly, stall under loss, and pin fixed-pool
// memory while pending (measured: multi-key value replies drove exactly the
// pool-exhaustion shedding that starves everything else). Anything left over
// is fetched by the requester's NEXT diff round - its view digest still
// differs, so the handshake simply repeats. At least one record is always
// included so every round makes progress.
#define FLEECE_VALUE_FRAME_BYTE_CAP 320
int fleece_state_manager_export_shared_by_hash(FleeceStateManager* manager, const uint32_t* hashes, uint32_t count, uint8_t** frame_data, uint32_t* frame_size) {
    if (!manager || !frame_data || !frame_size || (!hashes && count > 0)) return -1;

    uint64_t hw = 0;
    // First pass: size the header and find which requested entries fit under
    // the single-packet byte cap (at least one - progress is mandatory).
    size_t body = fleece_cbor_array_header_size(5) + fleece_cbor_uint_size(manager->node_id)
                 + fleece_cbor_uint_size(FLEECE_SHARED_OWNER_ID)
                 + 9   // worst-case hw encoding
                 + fleece_cbor_uint_size(compute_stream_digest(manager, FLEECE_SHARED_OWNER_ID))
                 + fleece_cbor_array_header_size(count);
    bool take[FIELD_CAPACITY];
    uint32_t matched = 0;
    for (uint32_t i = 0; i < manager->field_capacity; i++) take[i] = false;
    for (uint32_t i = 0; i < manager->field_capacity; i++) {
        struct FieldEntry* f = &manager->fields[i];
        if (!f->exists || f->is_tombstone || f->node_id != FLEECE_SHARED_OWNER_ID || f->name[0] == '\0') continue;
        if (f->timestamp > hw) hw = f->timestamp;
        bool hit = false;
        for (uint32_t h = 0; h < count; h++) {
            if (hashes[h] == f->key) { hit = true; break; }
        }
        if (!hit) continue;
        uint32_t name_len = (uint32_t)strlen(f->name);
        size_t rec = fleece_cbor_array_header_size(5)
                   + fleece_cbor_uint_size(f->origin_node_id)
                   + 1  // bool
                   + fleece_cbor_text_size(name_len)
                   + fleece_cbor_uint_size(f->timestamp)
                   + fleece_cbor_bytes_size(f->size);
        if (matched > 0 && body + rec > FLEECE_VALUE_FRAME_BYTE_CAP) continue;
        body += rec;
        take[i] = true;
        matched++;
    }
    // Shrink the record-count header to what we actually wrote.
    body -= fleece_cbor_array_header_size(count);
    body += fleece_cbor_array_header_size(matched);

    uint8_t* buf = (uint8_t*)fleece_malloc(3 + body);
    if (!buf) return -1;

    size_t pos = 0;
    buf[pos++] = FLEECE_GOSSIP_MAGIC0;
    buf[pos++] = FLEECE_GOSSIP_MAGIC1;
    buf[pos++] = FLEECE_GOSSIP_VERSION;
    // v5 header, same shape as a regular gossip frame: repair VALUE frames
    // merge through the ordinary import path, which now requires the sender
    // id (and rejects its absence as a malformed arity-4 frame).
    fleece_cbor_write_array_header(buf, &pos, 5);
    fleece_cbor_write_uint(buf, &pos, manager->node_id);
    fleece_cbor_write_uint(buf, &pos, FLEECE_SHARED_OWNER_ID);
    fleece_cbor_write_uint(buf, &pos, hw);
    fleece_cbor_write_uint(buf, &pos, compute_stream_digest(manager, FLEECE_SHARED_OWNER_ID));
    fleece_cbor_write_array_header(buf, &pos, matched);

    for (uint32_t i = 0; i < manager->field_capacity; i++) {
        struct FieldEntry* f = &manager->fields[i];
        if (!f->exists || !take[i]) continue;
        uint32_t name_len = (uint32_t)strlen(f->name);
        fleece_cbor_write_array_header(buf, &pos, 5);
        fleece_cbor_write_uint(buf, &pos, f->origin_node_id);
        fleece_cbor_write_bool(buf, &pos, false);
        fleece_cbor_write_text(buf, &pos, f->name, name_len);
        fleece_cbor_write_uint(buf, &pos, f->timestamp);
        fleece_cbor_write_bytes(buf, &pos, f->data, f->size);
    }

    *frame_data = buf;
    *frame_size = (uint32_t)pos;
    return 0;
}

int fleece_state_manager_shared_at_least(FleeceStateManager* manager, uint32_t key_hash, uint64_t ts) {
    if (!manager) return -1;
    struct FieldEntry* f = find_slot_owned(manager, key_hash, FLEECE_SHARED_OWNER_ID);
    if (!f) return 0;  // absent entirely
    return f->timestamp >= ts ? 1 : 0;  // stale counts as "worth fetching"
}
