// Internal minimal CBOR (RFC 8949) codec, shared by the state manager (gossip
// wire frames) and the GOAP planner (plan blobs). NOT part of the public API.
//
// Supports only the major types fleece actually needs: unsigned integers, byte
// strings, text strings, arrays, booleans, and IEEE-754 doubles. Not a
// general-purpose CBOR codec.

#ifndef FLEECE_CBOR_H
#define FLEECE_CBOR_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- Sizing (for a two-pass encode: measure, then write) ---

size_t fleece_cbor_uint_size(uint64_t v);
size_t fleece_cbor_bytes_size(uint32_t len);
size_t fleece_cbor_text_size(uint32_t len);
size_t fleece_cbor_array_header_size(uint32_t count);
size_t fleece_cbor_num_size(double v);  // uint if integral, else double

// --- Encoding ---

void fleece_cbor_write_head(uint8_t* buf, size_t* pos, uint8_t major, uint64_t v);
void fleece_cbor_write_uint(uint8_t* buf, size_t* pos, uint64_t v);
void fleece_cbor_write_array_header(uint8_t* buf, size_t* pos, uint32_t count);
void fleece_cbor_write_bytes(uint8_t* buf, size_t* pos, const uint8_t* data, uint32_t len);
void fleece_cbor_write_text(uint8_t* buf, size_t* pos, const char* text, uint32_t len);
void fleece_cbor_write_bool(uint8_t* buf, size_t* pos, bool v);
void fleece_cbor_write_num(uint8_t* buf, size_t* pos, double v);

// --- Decoding ---

// Reads one item's head (major type + argument). Does not skip a following
// byte/text payload or array elements - callers use *value as a length/count.
// Returns false on bounds failure or unsupported encoding.
bool fleece_cbor_read_head(const uint8_t* buf, uint32_t size, size_t* pos, uint8_t* major, uint64_t* value);

// Overflow-safe "does [pos, pos+len) fit within [0, size)" check.
bool fleece_cbor_bounds_ok(size_t pos, uint64_t len, uint32_t size);

// Reads a number written by fleece_cbor_write_num (uint or double).
bool fleece_cbor_read_num(const uint8_t* buf, uint32_t size, size_t* pos, double* out);

#ifdef __cplusplus
}
#endif

#endif // FLEECE_CBOR_H
