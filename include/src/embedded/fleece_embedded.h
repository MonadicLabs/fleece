// Fleece Embedded JavaScript
// QuickJS integration for script execution

#ifndef FLEECE_EMBEDDED_H
#define FLEECE_EMBEDDED_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FleeceEmbedded FleeceEmbedded;

// Create a new embedded JS instance
FleeceEmbedded* fleece_embedded_create(void);

// Destroy embedded JS instance
void fleece_embedded_destroy(FleeceEmbedded* embedded);

// Execute a JavaScript script
int fleece_embedded_execute(FleeceEmbedded* embedded, const char* script);

// Set a value in the embedded JS context
int fleece_embedded_set_value(FleeceEmbedded* embedded, const char* name, const uint8_t* data, uint32_t size);

// Get a value from the embedded JS context
int fleece_embedded_get_value(FleeceEmbedded* embedded, const char* name, uint8_t** data, uint32_t* size);

// Register C functions with the JavaScript VM
int fleece_embedded_register_c_functions(FleeceEmbedded* embedded);

// Get the QuickJS context
void* fleece_embedded_get_context(FleeceEmbedded* embedded);

#ifdef __cplusplus
}
#endif

#endif // FLEECE_EMBEDDED_H
