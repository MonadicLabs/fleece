#define _POSIX_C_SOURCE 200809L

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#include "fleece_comms.h"

struct FleeceComms {
    bool is_connected;
    bool is_initialized;
    uint32_t packet_count;
    uint32_t max_packets;

    FleeceCommsSendCallback send_callback;
    void* send_callback_user_data;

    FleeceCommsReceiveCallback receive_callback;
    void* receive_callback_user_data;
};

FleeceComms* fleece_comms_create(void) {
    FleeceComms* comms = (FleeceComms*)calloc(1, sizeof(FleeceComms));
    if (!comms) {
        return NULL;
    }
    
    comms->is_connected = false;
    comms->is_initialized = false;
    comms->packet_count = 0;
    comms->max_packets = 1024;  // Limit for microcontrollers
    
    return comms;
}

void fleece_comms_destroy(FleeceComms* comms) {
    if (!comms) return;
    
    fleece_comms_close(comms);
    free(comms);
}

void fleece_comms_process_input(FleeceComms* comms) {
    if (!comms || !comms->is_initialized) return;
    
    // Simulate receiving packets (in real implementation, this would read from radio)
    printf("Processing incoming comms (packets received: %u)\n", comms->packet_count);
}

void fleece_comms_process_output(FleeceComms* comms) {
    if (!comms || !comms->is_initialized) return;
    
    // Simulate sending packets (in real implementation, this would write to radio)
    printf("Processing outgoing comms (packets sent: %u)\n", comms->packet_count);
}

int fleece_comms_send(FleeceComms* comms, const char* destination, const uint8_t* data, uint32_t size) {
    if (!comms || !comms->is_initialized || !destination || !data || size == 0) {
        return -1;
    }
    
    if (comms->packet_count >= comms->max_packets) {
        return -1;  // Packet limit reached
    }
    
    printf("Sending %u bytes to %s\n", size, destination);
    comms->packet_count++;

    if (comms->send_callback) {
        comms->send_callback(destination, data, size, comms->send_callback_user_data);
    }

    return 0;
}

int fleece_comms_receive(FleeceComms* comms, char* destination, uint8_t** data, uint32_t* size) {
    if (!comms || !comms->is_initialized) return -1;
    
    // In a real implementation, this would receive from radio
    // For now, it's a placeholder
    if (destination) strcpy(destination, "node_001");
    *data = NULL;
    *size = 0;

    if (comms->receive_callback && *data && *size > 0) {
        comms->receive_callback(destination, *data, *size, comms->receive_callback_user_data);
    }

    return 0;
}

int fleece_comms_initialize(FleeceComms* comms) {
    if (!comms) {
        return -1;
    }
    
    comms->is_initialized = true;
    comms->is_connected = true;  // Simulate connection for demo
    
    printf("Comms initialized and connected\n");
    
    return 0;
}

void fleece_comms_close(FleeceComms* comms) {
    if (!comms) return;
    
    if (comms->is_initialized) {
        // Close comms stack
        comms->is_initialized = false;
        comms->is_connected = false;
    }
}

bool fleece_comms_is_connected(FleeceComms* comms) {
    return comms ? comms->is_connected : false;
}

void fleece_comms_set_send_callback(FleeceComms* comms, FleeceCommsSendCallback callback, void* user_data) {
    if (!comms) return;

    comms->send_callback = callback;
    comms->send_callback_user_data = user_data;
}

void fleece_comms_set_receive_callback(FleeceComms* comms, FleeceCommsReceiveCallback callback, void* user_data) {
    if (!comms) return;

    comms->receive_callback = callback;
    comms->receive_callback_user_data = user_data;
}
