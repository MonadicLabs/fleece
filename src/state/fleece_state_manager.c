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
        bool exists;
        bool is_tombstone;
    } *fields;
    uint32_t field_count;
    uint32_t field_capacity;
    uint64_t local_timestamp;
    uint64_t node_id;
};

static const uint32_t FIELD_CAPACITY = 128;  // Max fields for microcontrollers
static const size_t MEMORY_POOL_SIZE = 4096;  // Memory pool for all allocations

FleeceStateManager* fleece_state_manager_create(void) {
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
    // In a real implementation, node_id would be read from hardware (e.g., MAC address)
    manager->node_id = 0x123456789ABCDEF0;
    
    return manager;
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

static struct FieldEntry* find_field(FleeceStateManager* manager, uint32_t key) {
    for (uint32_t i = 0; i < manager->field_capacity; i++) {
        if (manager->fields[i].exists && !manager->fields[i].is_tombstone && manager->fields[i].key == key) {
            return &manager->fields[i];
        }
    }
    return NULL;
}

int fleece_state_manager_set(FleeceStateManager* manager, uint32_t key, const uint8_t* data, uint32_t size) {
    if (!manager || !data || size == 0) {
        return -1;
    }
    
    struct FieldEntry* field = find_field(manager, key);
    
    if (field) {
        // Update existing field
        free(field->data);
        field->data = (uint8_t*)malloc(size);
        if (!field->data) {
            return -1;
        }
        memcpy(field->data, data, size);
        field->size = size;
        field->timestamp = ++manager->local_timestamp;
        field->node_id = manager->node_id;
    } else {
        // Add new field
        if (manager->field_count >= manager->field_capacity) {
            return -1;  // Field limit reached
        }
        
        uint32_t idx = manager->field_count++;
        manager->fields[idx].key = key;
        manager->fields[idx].data = (uint8_t*)malloc(size);
        if (!manager->fields[idx].data) {
            return -1;
        }
        memcpy(manager->fields[idx].data, data, size);
        manager->fields[idx].size = size;
        manager->fields[idx].timestamp = ++manager->local_timestamp;
        manager->fields[idx].node_id = manager->node_id;
        manager->fields[idx].exists = true;
        manager->fields[idx].is_tombstone = false;
    }
    
    return 0;
}

int fleece_state_manager_get(FleeceStateManager* manager, uint32_t key, uint8_t** data, uint32_t* size) {
    if (!manager || !data || !size) {
        return -1;
    }
    
    struct FieldEntry* field = find_field(manager, key);
    if (!field || field->is_tombstone) {
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
    
    struct FieldEntry* field = find_field(manager, key);
    return field && !field->is_tombstone;
}

int fleece_state_manager_remove(FleeceStateManager* manager, uint32_t key) {
    if (!manager) return -1;
    
    struct FieldEntry* field = find_field(manager, key);
    if (!field) {
        return -1;
    }
    
    free(field->data);
    field->is_tombstone = true;
    return 0;
}

void fleece_state_manager_gossip(FleeceStateManager* manager) {
    if (!manager) return;
    
    // In a real implementation, this would send serialized state to peers
    // For now, it's a placeholder
    printf("Gossiping state with %u fields\n", manager->field_count);
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
    if (!field || field->is_tombstone) {
        return -1;
    }
    
    version->timestamp = field->timestamp;
    version->node_id = field->node_id;
    version->size = field->size;
    
    return 0;
}

int fleece_state_manager_export(FleeceStateManager* manager, uint8_t** cbor_data, uint32_t* cbor_size) {
    if (!manager || !cbor_data || !cbor_size) {
        return -1;
    }
    
    // In a real implementation, this would serialize all fields to CBOR
    // For now, it's a placeholder
    printf("Exporting %u fields to CBOR\n", manager->field_count);
    *cbor_data = NULL;
    *cbor_size = 0;
    
    return 0;
}

int fleece_state_manager_import(FleeceStateManager* manager, const uint8_t* cbor_data, uint32_t cbor_size) {
    if (!manager || !cbor_data) {
        return -1;
    }
    
    // In a real implementation, this would deserialize CBOR into fields
    // For now, it's a placeholder
    printf("Importing %u bytes from CBOR\n", cbor_size);
    
    return 0;
}
