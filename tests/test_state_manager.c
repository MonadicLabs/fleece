// Fleece State Manager Tests
// Unit tests for the LWW key-value store implementation

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "state/fleece_state_manager.h"

void test_state_manager_basic() {
    printf("Running state manager basic tests...\n");
    
    FleeceStateManager* manager = fleece_state_manager_create();
    if (!manager) {
        printf("FAILED: Could not create state manager\n");
        return;
    }
    
    // Test setting and getting values
    uint8_t test_data[] = {0x01, 0x02, 0x03, 0x04};
    uint32_t test_size = 4;
    
    int result = fleece_state_manager_set(manager, 1, test_data, test_size);
    if (result != 0) {
        printf("FAILED: Could not set value\n");
        fleece_state_manager_destroy(manager);
        return;
    }
    
    uint8_t* retrieved_data = NULL;
    uint32_t retrieved_size = 0;
    
    result = fleece_state_manager_get(manager, 1, &retrieved_data, &retrieved_size);
    if (result != 0) {
        printf("FAILED: Could not get value\n");
        fleece_state_manager_destroy(manager);
        return;
    }
    
    if (retrieved_size != test_size || memcmp(retrieved_data, test_data, test_size) != 0) {
        printf("FAILED: Retrieved data does not match\n");
        free(retrieved_data);
        fleece_state_manager_destroy(manager);
        return;
    }
    
    free(retrieved_data);
    
    // Test exists
    if (!fleece_state_manager_exists(manager, 1)) {
        printf("FAILED: Field should exist\n");
        fleece_state_manager_destroy(manager);
        return;
    }
    
    // Test non-existent field
    if (fleece_state_manager_exists(manager, 2)) {
        printf("FAILED: Field should not exist\n");
        fleece_state_manager_destroy(manager);
        return;
    }
    
    // Test removal
    result = fleece_state_manager_remove(manager, 1);
    if (result != 0) {
        printf("FAILED: Could not remove field\n");
        fleece_state_manager_destroy(manager);
        return;
    }
    
    if (fleece_state_manager_exists(manager, 1)) {
        printf("FAILED: Field should not exist after removal\n");
        fleece_state_manager_destroy(manager);
        return;
    }
    
    // Test get after removal
    result = fleece_state_manager_get(manager, 1, &retrieved_data, &retrieved_size);
    if (result == 0) {
        printf("FAILED: Should not be able to get removed field\n");
        if (retrieved_data) free(retrieved_data);
        fleece_state_manager_destroy(manager);
        return;
    }
    
    printf("PASSED: Basic state manager tests\n");
    fleece_state_manager_destroy(manager);
}

void test_state_manager_lww() {
    printf("Running state manager LWW tests...\n");
    
    FleeceStateManager* manager = fleece_state_manager_create();
    if (!manager) {
        printf("FAILED: Could not create state manager\n");
        return;
    }
    
    // Set initial value
    uint8_t data1[] = {0x01, 0x02};
    fleece_state_manager_set(manager, 1, data1, sizeof(data1));
    
    // Get version info
    FleeceFieldVersion version1;
    if (fleece_state_manager_get_version(manager, 1, &version1) != 0) {
        printf("FAILED: Could not get version after first set\n");
        fleece_state_manager_destroy(manager);
        return;
    }
    
    uint64_t timestamp1 = version1.timestamp;
    
    // Wait a bit (in real implementation, we'd use a monotonic clock)
    // Set again (simulating newer timestamp)
    uint8_t data2[] = {0x03, 0x04, 0x05};
    fleece_state_manager_set(manager, 1, data2, sizeof(data2));
    
    // Get version again
    FleeceFieldVersion version2;
    if (fleece_state_manager_get_version(manager, 1, &version2) != 0) {
        printf("FAILED: Could not get version after second set\n");
        fleece_state_manager_destroy(manager);
        return;
    }
    
    // Second version should have newer timestamp
    if (version2.timestamp <= timestamp1) {
        printf("FAILED: Timestamp should increase\n");
        fleece_state_manager_destroy(manager);
        return;
    }
    
    printf("PASSED: LWW semantics tests\n");
    fleece_state_manager_destroy(manager);
}

void test_state_manager_compact() {
    printf("Running state manager compact tests...\n");
    
    FleeceStateManager* manager = fleece_state_manager_create();
    if (!manager) {
        printf("FAILED: Could not create state manager\n");
        return;
    }
    
    // Add multiple fields
    for (uint32_t i = 0; i < 10; i++) {
        uint8_t data[] = {(uint8_t)i};
        fleece_state_manager_set(manager, i, data, sizeof(data));
    }
    
    // Remove some fields
    for (uint32_t i = 2; i < 8; i++) {
        fleece_state_manager_remove(manager, i);
    }
    
    // Compact memory
    int result = fleece_state_manager_compact(manager);
    if (result != 0) {
        printf("FAILED: Could not compact memory\n");
        fleece_state_manager_destroy(manager);
        return;
    }
    
    // Check that remaining fields are still accessible
    for (uint32_t i = 0; i < 2; i++) {
        uint8_t* data = NULL;
        uint32_t size = 0;
        if (fleece_state_manager_get(manager, i, &data, &size) != 0) {
            printf("FAILED: Could not get field %u after compact\n", i);
            if (data) free(data);
            fleece_state_manager_destroy(manager);
            return;
        }
        free(data);
    }
    
    // Check that removed fields are not accessible
    for (uint32_t i = 2; i < 8; i++) {
        uint8_t* data = NULL;
        uint32_t size = 0;
        if (fleece_state_manager_get(manager, i, &data, &size) == 0) {
            printf("FAILED: Should not be able to get removed field %u after compact\n", i);
            if (data) free(data);
            fleece_state_manager_destroy(manager);
            return;
        }
    }
    
    printf("PASSED: Compact tests\n");
    fleece_state_manager_destroy(manager);
}

int main(void) {
    printf("Fleece State Manager Unit Tests\n");
    printf("==============================\n\n");
    
    test_state_manager_basic();
    printf("\n");
    
    test_state_manager_lww();
    printf("\n");
    
    test_state_manager_compact();
    printf("\n");
    
    printf("All tests completed!\n");
    
    return 0;
}
