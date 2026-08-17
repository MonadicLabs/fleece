#define _POSIX_C_SOURCE 200809L

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <time.h>

#include "fleece_state_manager.h"

// Internal state structure for the state manager

struct FleeceStateManager {
    // Fixed-size hash map for storing field values
    // Using a simple array-based approach for microcontrollers
    struct FieldEntry {
        uint32_t key;
        uint8_t* data;
        uint32_t size;
        uint64_t timestamp;
        uint64_t node_id;
        char name[FLEECE_FIELD_NAME_MAX];  // empty for legacy raw-key entries
        bool exists;
        bool is_tombstone;
    } *fields;
    uint32_t field_count;
    uint32_t field_capacity;
    uint64_t local_timestamp;
    uint64_t node_id;
};

static const uint32_t FIELD_CAPACITY = 128;  // Max fields for microcontrollers (shared across local + peer fields)

#define FLEECE_GOSSIP_MAGIC0 'F'
#define FLEECE_GOSSIP_MAGIC1 'G'
#define FLEECE_GOSSIP_VERSION 1

static uint32_t hash_name(const char* name) {
    uint32_t hash = 2166136261u;  // FNV-1a 32-bit
    for (const unsigned char* p = (const unsigned char*)name; *p; p++) {
        hash ^= *p;
        hash *= 16777619u;
    }
    return hash;
}

static void write_u32(uint8_t* buf, uint32_t v) {
    buf[0] = (uint8_t)(v & 0xFF);
    buf[1] = (uint8_t)((v >> 8) & 0xFF);
    buf[2] = (uint8_t)((v >> 16) & 0xFF);
    buf[3] = (uint8_t)((v >> 24) & 0xFF);
}

static uint32_t read_u32(const uint8_t* buf) {
    return (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) | ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
}

static void write_u64(uint8_t* buf, uint64_t v) {
    for (int i = 0; i < 8; i++) {
        buf[i] = (uint8_t)((v >> (8 * i)) & 0xFF);
    }
}

static uint64_t read_u64(const uint8_t* buf) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) {
        v |= ((uint64_t)buf[i]) << (8 * i);
    }
    return v;
}

FleeceStateManager* fleece_state_manager_create_with_node_id(uint64_t node_id) {
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
static int upsert_field(FleeceStateManager* manager, uint32_t key, uint64_t owner_node_id, const char* name,
                         const uint8_t* data, uint32_t size, uint64_t timestamp, bool is_tombstone) {
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

    return upsert_field(manager, key, manager->node_id, NULL, data, size, ++manager->local_timestamp, false);
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

    return upsert_field(manager, hash_name(name), manager->node_id, name, data, size, ++manager->local_timestamp, false);
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

uint32_t fleece_state_manager_list_nodes(FleeceStateManager* manager, uint64_t* node_ids_out, uint32_t max_nodes) {
    if (!manager || !node_ids_out || max_nodes == 0) return 0;

    uint32_t count = 0;
    for (uint32_t i = 0; i < manager->field_capacity && count < max_nodes; i++) {
        struct FieldEntry* f = &manager->fields[i];
        if (!f->exists || f->is_tombstone) continue;

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

    manager->local_timestamp = manager->local_timestamp > remote_timestamp ? manager->local_timestamp : remote_timestamp;

    uint32_t key = hash_name(name);
    struct FieldEntry* incumbent = find_slot_owned(manager, key, owner_node_id);
    if (incumbent && incumbent->timestamp >= remote_timestamp) {
        return 0;  // incumbent is newer or tied; keep it
    }

    return upsert_field(manager, key, owner_node_id, name, data, size, remote_timestamp, is_tombstone);
}

int fleece_state_manager_export(FleeceStateManager* manager, uint8_t** frame_data, uint32_t* frame_size) {
    if (!manager || !frame_data || !frame_size) {
        return -1;
    }

    uint32_t count = 0;
    size_t total = 3 + 8 + 4;  // magic+version, owner_node_id, field_count
    for (uint32_t i = 0; i < manager->field_capacity; i++) {
        struct FieldEntry* f = &manager->fields[i];
        if (!f->exists || f->node_id != manager->node_id || f->name[0] == '\0') continue;

        total += 1 + 1 + strlen(f->name) + 8 + 4 + (f->is_tombstone ? 0 : f->size);
        count++;
    }

    uint8_t* buf = (uint8_t*)malloc(total);
    if (!buf) {
        return -1;
    }

    size_t pos = 0;
    buf[pos++] = FLEECE_GOSSIP_MAGIC0;
    buf[pos++] = FLEECE_GOSSIP_MAGIC1;
    buf[pos++] = FLEECE_GOSSIP_VERSION;
    write_u64(&buf[pos], manager->node_id); pos += 8;
    write_u32(&buf[pos], count); pos += 4;

    for (uint32_t i = 0; i < manager->field_capacity; i++) {
        struct FieldEntry* f = &manager->fields[i];
        if (!f->exists || f->node_id != manager->node_id || f->name[0] == '\0') continue;

        uint8_t name_len = (uint8_t)strlen(f->name);
        uint32_t data_len = f->is_tombstone ? 0 : f->size;

        buf[pos++] = f->is_tombstone ? 0x01 : 0x00;
        buf[pos++] = name_len;
        memcpy(&buf[pos], f->name, name_len); pos += name_len;
        write_u64(&buf[pos], f->timestamp); pos += 8;
        write_u32(&buf[pos], data_len); pos += 4;
        if (data_len > 0) {
            memcpy(&buf[pos], f->data, data_len);
            pos += data_len;
        }
    }

    *frame_data = buf;
    *frame_size = (uint32_t)pos;
    return 0;
}

int fleece_state_manager_import(FleeceStateManager* manager, const uint8_t* frame_data, uint32_t frame_size) {
    if (!manager || !frame_data) {
        return -1;
    }
    if (frame_size < 3 + 8 + 4) {
        return -1;
    }
    if (frame_data[0] != FLEECE_GOSSIP_MAGIC0 || frame_data[1] != FLEECE_GOSSIP_MAGIC1 || frame_data[2] != FLEECE_GOSSIP_VERSION) {
        return -1;
    }

    size_t pos = 3;
    uint64_t owner_node_id = read_u64(&frame_data[pos]); pos += 8;
    uint32_t count = read_u32(&frame_data[pos]); pos += 4;

    if (owner_node_id == manager->node_id) {
        return -1;  // reject a "peer" frame claiming to be us
    }

    for (uint32_t i = 0; i < count; i++) {
        if (pos + 2 > frame_size) return -1;
        uint8_t flags = frame_data[pos++];
        uint8_t name_len = frame_data[pos++];

        if (pos + name_len > frame_size) return -1;
        char name[FLEECE_FIELD_NAME_MAX];
        uint32_t copy_len = name_len < FLEECE_FIELD_NAME_MAX - 1 ? name_len : FLEECE_FIELD_NAME_MAX - 1;
        memcpy(name, &frame_data[pos], copy_len);
        name[copy_len] = '\0';
        pos += name_len;

        if (pos + 8 + 4 > frame_size) return -1;
        uint64_t timestamp = read_u64(&frame_data[pos]); pos += 8;
        uint32_t data_len = read_u32(&frame_data[pos]); pos += 4;

        if (pos + data_len > frame_size) return -1;
        const uint8_t* data = &frame_data[pos];
        pos += data_len;

        if (copy_len == 0) continue;  // skip malformed/empty names defensively

        bool is_tombstone = (flags & 0x01) != 0;
        fleece_state_manager_merge_named(manager, owner_node_id, name, is_tombstone ? NULL : data, data_len, timestamp, is_tombstone);
    }

    return 0;
}
