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
    struct PeerSeen {
        uint64_t node_id;
        uint64_t last_seen_tick;
        bool exists;
    } peers_seen[FLEECE_MAX_TRACKED_PEERS];
};

static const uint32_t FIELD_CAPACITY = 128;  // Max fields for microcontrollers (shared across local + peer fields)

#define FLEECE_GOSSIP_MAGIC0 'F'
#define FLEECE_GOSSIP_MAGIC1 'G'
#define FLEECE_GOSSIP_VERSION 3

// Protocol v3: every gossip frame carries the sender's per-stream high-water
// mark (the highest record timestamp it holds for that stream) alongside the
// owner id and records. A receiver can then detect that it is *behind* - it
// missed a delta - by comparing the advertised hw against the highest record
// timestamp it actually stores for that stream, and request a full resync on
// demand instead of relying on a periodic full-state broadcast.
//
// Known limitation (same tradeoff the benchmark validated): the comparison is
// against a single scalar hw per stream. It reliably detects "I'm missing the
// stream's newest record", but can miss a gap on a non-newest field (LWW means
// only the newest version of each field matters, and a stale non-max field is
// repaired on that field's next update or at the next on-demand resync).

static uint32_t hash_name(const char* name) {
    uint32_t hash = 2166136261u;  // FNV-1a 32-bit
    for (const unsigned char* p = (const unsigned char*)name; *p; p++) {
        hash ^= *p;
        hash *= 16777619u;
    }
    return hash;
}

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
    field->is_tombstone = true;
    return 0;
}

int fleece_state_manager_compact(FleeceStateManager* manager) {
    if (!manager) return -1;

    uint32_t write_idx = 0;
    for (uint32_t read_idx = 0; read_idx < manager->field_capacity; read_idx++) {
        if (manager->fields[read_idx].exists && !manager->fields[read_idx].is_tombstone) {
            if (write_idx != read_idx) {
                manager->fields[write_idx] = manager->fields[read_idx];
                memset(&manager->fields[read_idx], 0, sizeof(struct FieldEntry));
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

// --- Shared fields (owner = FLEECE_SHARED_OWNER_ID; see header for the LWW caveat) ---

int fleece_state_manager_set_shared(FleeceStateManager* manager, const char* name, const uint8_t* data, uint32_t size) {
    if (!manager || !name || !name[0] || !data || size == 0) {
        return -1;
    }

    return upsert_field(manager, hash_name(name), FLEECE_SHARED_OWNER_ID, manager->node_id, name, data, size, ++manager->local_timestamp, false);
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
    field->is_tombstone = true;
    field->timestamp = ++manager->local_timestamp;
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
    // Embedded in every frame so receivers can detect they are behind.
    uint64_t hw = 0;
    for (uint32_t i = 0; i < manager->field_capacity; i++) {
        struct FieldEntry* f = &manager->fields[i];
        if (!f->exists || f->node_id != owner_filter || f->name[0] == '\0') continue;
        if (f->timestamp > hw) hw = f->timestamp;
    }

    uint32_t count = 0;
    size_t body_size = fleece_cbor_array_header_size(3) + fleece_cbor_uint_size(owner_filter)
                     + fleece_cbor_uint_size(hw);
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

    fleece_cbor_write_array_header(buf, &pos, 3);
    fleece_cbor_write_uint(buf, &pos, owner_filter);
    fleece_cbor_write_uint(buf, &pos, hw);
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

// Shared import core (see the two public wrappers below). Parses a v3 gossip
// frame [owner_node_id, hw, [records]], merges its records with LWW, then -
// if behind_self/behind_shared are non-NULL - reports whether the receiver is
// now *behind* on that stream: the advertised hw is higher than the highest
// record timestamp the receiver actually stores for it. Only the flag matching
// the frame's own stream is ever set.
static int import_impl(FleeceStateManager* manager, const uint8_t* frame_data, uint32_t frame_size,
                       bool* behind_self, bool* behind_shared) {
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

    // Outer array: [owner_node_id, hw, records]
    if (!fleece_cbor_read_head(frame_data, frame_size, &pos, &major, &value) || major != 4 || value != 3) return -1;

    if (!fleece_cbor_read_head(frame_data, frame_size, &pos, &major, &value) || major != 0) return -1;
    uint64_t owner_node_id = value;

    if (owner_node_id == manager->node_id) {
        return -1;  // reject a "peer" frame claiming to be us
    }

    bool is_shared_stream = (owner_node_id == FLEECE_SHARED_OWNER_ID);

    if (!fleece_cbor_read_head(frame_data, frame_size, &pos, &major, &value) || major != 0) return -1;
    uint64_t advertised_hw = value;

    touch_peer(manager, owner_node_id);  // even an empty delta counts as "heard from"

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

    // Gap detection: after merging, is the receiver still behind the sender's
    // advertised high-water mark for this stream?
    uint64_t max_ts = 0;
    uint64_t stored_owner = is_shared_stream ? FLEECE_SHARED_OWNER_ID : owner_node_id;
    for (uint32_t i = 0; i < manager->field_capacity; i++) {
        struct FieldEntry* f = &manager->fields[i];
        if (!f->exists || f->node_id != stored_owner) continue;
        if (f->timestamp > max_ts) max_ts = f->timestamp;
    }
    bool behind = (max_ts < advertised_hw);
    if (behind_self) *behind_self = !is_shared_stream && behind;
    if (behind_shared) *behind_shared = is_shared_stream && behind;

    return 0;
}

int fleece_state_manager_import(FleeceStateManager* manager, const uint8_t* frame_data, uint32_t frame_size) {
    return import_impl(manager, frame_data, frame_size, NULL, NULL);
}

int fleece_state_manager_import_ex(FleeceStateManager* manager, const uint8_t* frame_data, uint32_t frame_size,
                                   bool* behind_self, bool* behind_shared) {
    return import_impl(manager, frame_data, frame_size, behind_self, behind_shared);
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
