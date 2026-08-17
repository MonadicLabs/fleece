// Fleece Comms Interface
// Abstraction layer for comm bus access

#ifndef FLEECE_COMMS_H
#define FLEECE_COMMS_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FleeceComms FleeceComms;

// Callback invoked after a message is sent
typedef void (*FleeceCommsSendCallback)(const char* destination, const uint8_t* data, uint32_t size, void* user_data);

// Callback invoked when a message is received
typedef void (*FleeceCommsReceiveCallback)(const char* source, const uint8_t* data, uint32_t size, void* user_data);

// Callback invoked once per fleece_comms_process_input() call (i.e. once per
// runtime loop tick - see fleece_runtime.c Phase 1). Fleece has no opinion on
// what happens here: it exists purely so external code can do its own
// periodic I/O (e.g. poll a non-blocking socket) in step with the runtime
// loop, without the comms module needing to know anything about a specific
// transport. See examples/ for a real usage (a UDP multicast backend that
// polls its own socket here and feeds received bytes straight into
// fleece_state_manager_import, entirely outside this module).
typedef void (*FleeceCommsPollCallback)(void* user_data);

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

// Set the callback invoked after a message is sent
void fleece_comms_set_send_callback(FleeceComms* comms, FleeceCommsSendCallback callback, void* user_data);

// Set the callback invoked when a message is received
void fleece_comms_set_receive_callback(FleeceComms* comms, FleeceCommsReceiveCallback callback, void* user_data);

// Set the callback invoked once per fleece_comms_process_input() call
void fleece_comms_set_poll_callback(FleeceComms* comms, FleeceCommsPollCallback callback, void* user_data);

#ifdef __cplusplus
}
#endif

#endif // FLEECE_COMMS_H
