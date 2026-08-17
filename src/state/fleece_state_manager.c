#define _POSIX_C_SOURCE 200809L

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <time.h>

#include "fleece_state_manager.h"

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
#define FLEECE_GOSSIP_VERSION 2

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

static size_t cbor_uint_size(uint64_t v) {
    if (v < 24) return 1;
    if (v <= 0xFFULL) return 2;
    if (v <= 0xFFFFULL) return 3;
    if (v <= 0xFFFFFFFFULL) return 5;
    return 9;
}

static size_t cbor_bytes_size(uint32_t len) { return cbor_uint_size(len) + len; }
static size_t cbor_text_size(uint32_t len) { return cbor_uint_size(len) + len; }
static size_t cbor_array_header_size(uint32_t count) { return cbor_uint_size(count); }

static void cbor_write_head(uint8_t* buf, size_t* pos, uint8_t major, uint64_t v) {
    uint8_t mt = (uint8_t)(major << 5);
    if (v < 24) {
        buf[(*pos)++] = (uint8_t)(mt | v);
    } else if (v <= 0xFFULL) {
        buf[(*pos)++] = (uint8_t)(mt | 24);
        buf[(*pos)++] = (uint8_t)v;
    } else if (v <= 0xFFFFULL) {
        buf[(*pos)++] = (uint8_t)(mt | 25);
        buf[(*pos)++] = (uint8_t)(v >> 8);
        buf[(*pos)++] = (uint8_t)v;
    } else if (v <= 0xFFFFFFFFULL) {
        buf[(*pos)++] = (uint8_t)(mt | 26);
        for (int i = 3; i >= 0; i--) buf[(*pos)++] = (uint8_t)(v >> (8 * i));
    } else {
        buf[(*pos)++] = (uint8_t)(mt | 27);
        for (int i = 7; i >= 0; i--) buf[(*pos)++] = (uint8_t)(v >> (8 * i));
    }
}

static void cbor_write_uint(uint8_t* buf, size_t* pos, uint64_t v) { cbor_write_head(buf, pos, 0, v); }
static void cbor_write_array_header(uint8_t* buf, size_t* pos, uint32_t count) { cbor_write_head(buf, pos, 4, count); }

static void cbor_write_bytes(uint8_t* buf, size_t* pos, const uint8_t* data, uint32_t len) {
    cbor_write_head(buf, pos, 2, len);
    if (len > 0) {
        memcpy(&buf[*pos], data, len);
        *pos += len;
    }
}

static void cbor_write_text(uint8_t* buf, size_t* pos, const char* text, uint32_t len) {
    cbor_write_head(buf, pos, 3, len);
    if (len > 0) {
        memcpy(&buf[*pos], text, len);
        *pos += len;
    }
}

static void cbor_write_bool(uint8_t* buf, size_t* pos, bool v) {
    buf[(*pos)++] = v ? 0xF5 : 0xF4;
}

// Reads one CBOR item's initial byte + any argument follow-bytes, filling
// *major (0-7) and *value (the unsigned/count/length/simple-value argument).
// Does not itself read a following byte/text-string payload or array
// elements - callers do that using *value as a length/count. Returns false
// (bounds failure or unsupported encoding) rather than reading past size.
static bool cbor_read_head(const uint8_t* buf, uint32_t size, size_t* pos, uint8_t* major, uint64_t* value) {
    if (*pos + 1 > size) return false;
    uint8_t initial = buf[(*pos)++];
    *major = (uint8_t)(initial >> 5);
    uint8_t info = (uint8_t)(initial & 0x1F);

    if (info < 24) {
        *value = info;
        return true;
    }
    if (info == 24) {
        if (*pos + 1 > size) return false;
        *value = buf[(*pos)++];
        return true;
    }
    if (info == 25) {
        if (*pos + 2 > size) return false;
        *value = ((uint64_t)buf[*pos] << 8) | buf[*pos + 1];
        *pos += 2;
        return true;
    }
    if (info == 26) {
        if (*pos + 4 > size) return false;
        uint64_t v = 0;
        for (int i = 0; i < 4; i++) v = (v << 8) | buf[(*pos)++];
        *value = v;
        return true;
    }
    if (info == 27) {
        if (*pos + 8 > size) return false;
        uint64_t v = 0;
        for (int i = 0; i < 8; i++) v = (v << 8) | buf[(*pos)++];
        *value = v;
        return true;
    }
    return false;  // info 28-31: reserved/indefinite-length - not supported
}

// Overflow-safe "does [pos, pos+len) fit within [0, size)" check.
static bool bounds_ok(size_t pos, uint64_t len, uint32_t size) {
    if (len > size) return false;
    return pos <= (size_t)size - (size_t)len;
}

FleeceStateManager* fleece_state_manager_create_with_node_id(uint64_t node_id) {
    if (node_id == FLEECE_SHARED_OWNER_ID) {
        return NULL;  // reserved for shared/"world" fields, not a real node identity
    }

    FleeceStateManager* manager = (FleeceStateManager*)calloc(1, sizeof(FleeceStateManager));
    if (!manager) {
        return NULL;
    }

    manager->fields = (struct FieldEntry*)calloc(FIELD_CAPACITY, sizeof(struct FieldEntry));
    if (!manager->fields) {
        free(manager);
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
            free(manager->fields[i].data);
        }
    }

    free(manager->fields);
    free(manager);
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
        new_data = (uint8_t*)malloc(size);
        if (!new_data) {
            return -1;
        }
        memcpy(new_data, data, size);
    }

    struct FieldEntry* field = find_slot_owned(manager, key, owner_node_id);
    if (!field) {
        if (manager->field_count >= manager->field_capacity) {
            free(new_data);
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
    free(old_data);

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

    *data = (uint8_t*)malloc(field->size);
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

    free(field->data);
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

    free(field->data);
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

    *data = (uint8_t*)malloc(field->size);
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

    free(field->data);
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

    uint32_t count = 0;
    size_t body_size = cbor_array_header_size(2) + cbor_uint_size(owner_filter);
    for (uint32_t i = 0; i < manager->field_capacity; i++) {
        struct FieldEntry* f = &manager->fields[i];
        if (!f->exists || f->node_id != owner_filter || f->name[0] == '\0' || f->timestamp <= since_timestamp) continue;

        uint32_t name_len = (uint32_t)strlen(f->name);
        uint32_t data_len = f->is_tombstone ? 0 : f->size;

        body_size += cbor_array_header_size(is_shared_stream ? 5 : 4);
        if (is_shared_stream) body_size += cbor_uint_size(f->origin_node_id);
        body_size += 1;  // bool is always 1 byte
        body_size += cbor_text_size(name_len);
        body_size += cbor_uint_size(f->timestamp);
        body_size += cbor_bytes_size(data_len);
        count++;
    }
    body_size += cbor_array_header_size(count);

    uint8_t* buf = (uint8_t*)malloc(3 + body_size);
    if (!buf) {
        return -1;
    }

    size_t pos = 0;
    buf[pos++] = FLEECE_GOSSIP_MAGIC0;
    buf[pos++] = FLEECE_GOSSIP_MAGIC1;
    buf[pos++] = FLEECE_GOSSIP_VERSION;

    cbor_write_array_header(buf, &pos, 2);
    cbor_write_uint(buf, &pos, owner_filter);
    cbor_write_array_header(buf, &pos, count);

    for (uint32_t i = 0; i < manager->field_capacity; i++) {
        struct FieldEntry* f = &manager->fields[i];
        if (!f->exists || f->node_id != owner_filter || f->name[0] == '\0' || f->timestamp <= since_timestamp) continue;

        uint32_t name_len = (uint32_t)strlen(f->name);
        uint32_t data_len = f->is_tombstone ? 0 : f->size;

        cbor_write_array_header(buf, &pos, is_shared_stream ? 5 : 4);
        if (is_shared_stream) cbor_write_uint(buf, &pos, f->origin_node_id);
        cbor_write_bool(buf, &pos, f->is_tombstone);
        cbor_write_text(buf, &pos, f->name, name_len);
        cbor_write_uint(buf, &pos, f->timestamp);
        cbor_write_bytes(buf, &pos, f->data, data_len);
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

int fleece_state_manager_import(FleeceStateManager* manager, const uint8_t* frame_data, uint32_t frame_size) {
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

    // Outer array: [owner_node_id, records]
    if (!cbor_read_head(frame_data, frame_size, &pos, &major, &value) || major != 4 || value != 2) return -1;

    if (!cbor_read_head(frame_data, frame_size, &pos, &major, &value) || major != 0) return -1;
    uint64_t owner_node_id = value;

    if (owner_node_id == manager->node_id) {
        return -1;  // reject a "peer" frame claiming to be us
    }

    bool is_shared_stream = (owner_node_id == FLEECE_SHARED_OWNER_ID);

    touch_peer(manager, owner_node_id);  // even an empty delta counts as "heard from"

    if (!cbor_read_head(frame_data, frame_size, &pos, &major, &value) || major != 4) return -1;
    uint64_t count = value;

    for (uint64_t i = 0; i < count; i++) {
        if (!cbor_read_head(frame_data, frame_size, &pos, &major, &value) || major != 4) return -1;
        if (value != (uint64_t)(is_shared_stream ? 5 : 4)) return -1;

        uint64_t origin_node_id = owner_node_id;  // self-stream: origin is always the frame's owner
        if (is_shared_stream) {
            if (!cbor_read_head(frame_data, frame_size, &pos, &major, &value) || major != 0) return -1;
            origin_node_id = value;
        }

        if (!cbor_read_head(frame_data, frame_size, &pos, &major, &value) || major != 7 || (value != 20 && value != 21)) return -1;
        bool is_tombstone = (value == 21);

        if (!cbor_read_head(frame_data, frame_size, &pos, &major, &value) || major != 3) return -1;  // text string (name)
        if (!bounds_ok(pos, value, frame_size)) return -1;
        char name[FLEECE_FIELD_NAME_MAX];
        uint32_t copy_len = (uint32_t)(value < FLEECE_FIELD_NAME_MAX - 1 ? value : FLEECE_FIELD_NAME_MAX - 1);
        memcpy(name, &frame_data[pos], copy_len);
        name[copy_len] = '\0';
        pos += value;

        if (!cbor_read_head(frame_data, frame_size, &pos, &major, &value) || major != 0) return -1;
        uint64_t timestamp = value;

        if (!cbor_read_head(frame_data, frame_size, &pos, &major, &value) || major != 2) return -1;  // byte string (data)
        if (!bounds_ok(pos, value, frame_size)) return -1;
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

    return 0;
}
