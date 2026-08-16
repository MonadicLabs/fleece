// Fleece Comms Interface
// Abstraction layer for mesh networking with Reticulum

#ifndef FLEECE_COMMS_H
#define FLEECE_COMMS_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FleeceComms FleeceComms;

// Create a new comms instance
FleeceComms* fleece_comms_create(void);

// Destroy comms instance
void fleece_comms_destroy(FleeceComms* comms);

// Process incoming messages from radio
void fleece_comms_process_input(FleeceComms* comms);

// Process outgoing messages to radio
void fleece_comms_process_output(FleeceComms* comms);

// Send a message to a destination
int fleece_comms_send(FleeceComms* comms, const char* destination, const uint8_t* data, uint32_t size);

// Receive a message
int fleece_comms_receive(FleeceComms* comms, char* destination, uint8_t** data, uint32_t* size);

// Initialize the comms stack
int fleece_comms_initialize(FleeceComms* comms);

// Close the comms stack
void fleece_comms_close(FleeceComms* comms);

// Get comms status
bool fleece_comms_is_connected(FleeceComms* comms);

#ifdef __cplusplus
}
#endif

#endif // FLEECE_COMMS_H
