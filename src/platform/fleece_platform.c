#define _POSIX_C_SOURCE 200809L

#include <stdlib.h>
#include <string.h>

#include "platform/fleece_platform.h"
#include "fleece_alloc.h"

#define FLEECE_PLATFORM_MAX_FUNCTIONS 32

struct FleecePlatform {
    struct {
        char name[FLEECE_PLATFORM_FUNCTION_NAME_MAX];
        FleecePlatformFunction fn;
        void* user_data;
        bool exists;
    } functions[FLEECE_PLATFORM_MAX_FUNCTIONS];
};

FleecePlatform* fleece_platform_create(void) {
    return (FleecePlatform*)fleece_calloc(1, sizeof(FleecePlatform));
}

void fleece_platform_destroy(FleecePlatform* platform) {
    fleece_free(platform);
}

static int find_slot(FleecePlatform* platform, const char* name) {
    for (int i = 0; i < FLEECE_PLATFORM_MAX_FUNCTIONS; i++) {
        if (platform->functions[i].exists && strcmp(platform->functions[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

int fleece_platform_register(FleecePlatform* platform, const char* name, FleecePlatformFunction fn, void* user_data) {
    if (!platform || !name || !name[0] || !fn) {
        return -1;
    }

    int idx = find_slot(platform, name);
    if (idx < 0) {
        for (int i = 0; i < FLEECE_PLATFORM_MAX_FUNCTIONS; i++) {
            if (!platform->functions[i].exists) {
                idx = i;
                break;
            }
        }
        if (idx < 0) {
            return -1;  // registry full
        }
    }

    strncpy(platform->functions[idx].name, name, FLEECE_PLATFORM_FUNCTION_NAME_MAX - 1);
    platform->functions[idx].name[FLEECE_PLATFORM_FUNCTION_NAME_MAX - 1] = '\0';
    platform->functions[idx].fn = fn;
    platform->functions[idx].user_data = user_data;
    platform->functions[idx].exists = true;
    return 0;
}

int fleece_platform_unregister(FleecePlatform* platform, const char* name) {
    if (!platform || !name) return -1;

    int idx = find_slot(platform, name);
    if (idx < 0) {
        return -1;
    }

    platform->functions[idx].exists = false;
    platform->functions[idx].fn = NULL;
    platform->functions[idx].user_data = NULL;
    return 0;
}

bool fleece_platform_has_function(FleecePlatform* platform, const char* name) {
    if (!platform || !name) return false;

    return find_slot(platform, name) >= 0;
}

uint32_t fleece_platform_list_functions(FleecePlatform* platform, char names_out[][FLEECE_PLATFORM_FUNCTION_NAME_MAX], uint32_t max_names) {
    if (!platform || !names_out || max_names == 0) return 0;

    uint32_t count = 0;
    for (int i = 0; i < FLEECE_PLATFORM_MAX_FUNCTIONS && count < max_names; i++) {
        if (!platform->functions[i].exists) continue;

        strncpy(names_out[count], platform->functions[i].name, FLEECE_PLATFORM_FUNCTION_NAME_MAX - 1);
        names_out[count][FLEECE_PLATFORM_FUNCTION_NAME_MAX - 1] = '\0';
        count++;
    }
    return count;
}

int fleece_platform_call(FleecePlatform* platform, const char* name, const uint8_t* args_json, uint32_t args_size, uint8_t** result_json, uint32_t* result_size) {
    if (!platform || !name || !result_json || !result_size) {
        return -1;
    }

    int idx = find_slot(platform, name);
    if (idx < 0) {
        return -1;
    }

    *result_json = NULL;
    *result_size = 0;
    return platform->functions[idx].fn(args_json, args_size, result_json, result_size, platform->functions[idx].user_data);
}
