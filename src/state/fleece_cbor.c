// Internal minimal CBOR (RFC 8949) codec shared by fleece_state_manager (gossip
// wire frames) and fleece_planner (plan blobs). Not a general-purpose codec:
// unsigned integers, byte strings, text strings, arrays, booleans, doubles.
//
// Encoding is two-pass: measure with the *_size helpers, then write with the
// fleece_cbor_write_* helpers. Decoding is a single forward pass guarded by
// bounds checks.

#include <string.h>

#include "fleece_cbor.h"

size_t fleece_cbor_uint_size(uint64_t v) {
    if (v < 24) return 1;
    if (v <= 0xFFULL) return 2;
    if (v <= 0xFFFFULL) return 3;
    if (v <= 0xFFFFFFFFULL) return 5;
    return 9;
}

size_t fleece_cbor_bytes_size(uint32_t len) { return fleece_cbor_uint_size(len) + len; }
size_t fleece_cbor_text_size(uint32_t len) { return fleece_cbor_uint_size(len) + len; }
size_t fleece_cbor_array_header_size(uint32_t count) { return fleece_cbor_uint_size(count); }

size_t fleece_cbor_num_size(double v) {
    if (v >= 0.0 && v < 18446744073709551616.0 && v == (double)(uint64_t)v) {
        return fleece_cbor_uint_size((uint64_t)v);
    }
    return 9;  // float64: head byte + 8-byte mantissa/exponent
}

void fleece_cbor_write_head(uint8_t* buf, size_t* pos, uint8_t major, uint64_t v) {
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

void fleece_cbor_write_uint(uint8_t* buf, size_t* pos, uint64_t v) { fleece_cbor_write_head(buf, pos, 0, v); }
void fleece_cbor_write_array_header(uint8_t* buf, size_t* pos, uint32_t count) { fleece_cbor_write_head(buf, pos, 4, count); }

void fleece_cbor_write_bytes(uint8_t* buf, size_t* pos, const uint8_t* data, uint32_t len) {
    fleece_cbor_write_head(buf, pos, 2, len);
    if (len > 0) {
        memcpy(&buf[*pos], data, len);
        *pos += len;
    }
}

void fleece_cbor_write_text(uint8_t* buf, size_t* pos, const char* text, uint32_t len) {
    fleece_cbor_write_head(buf, pos, 3, len);
    if (len > 0) {
        memcpy(&buf[*pos], text, len);
        *pos += len;
    }
}

void fleece_cbor_write_bool(uint8_t* buf, size_t* pos, bool v) {
    buf[(*pos)++] = v ? 0xF5 : 0xF4;
}

void fleece_cbor_write_num(uint8_t* buf, size_t* pos, double v) {
    if (v >= 0.0 && v < 18446744073709551616.0 && v == (double)(uint64_t)v) {
        fleece_cbor_write_uint(buf, pos, (uint64_t)v);
        return;
    }
    // float64 (major 7, additional 27)
    buf[(*pos)++] = 0xFB;
    uint64_t bits;
    memcpy(&bits, &v, sizeof(bits));
    for (int i = 7; i >= 0; i--) buf[(*pos)++] = (uint8_t)(bits >> (8 * i));
}

bool fleece_cbor_read_head(const uint8_t* buf, uint32_t size, size_t* pos, uint8_t* major, uint64_t* value) {
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

bool fleece_cbor_bounds_ok(size_t pos, uint64_t len, uint32_t size) {
    if (len > size) return false;
    return pos <= (size_t)size - (size_t)len;
}

bool fleece_cbor_read_num(const uint8_t* buf, uint32_t size, size_t* pos, double* out) {
    if (*pos >= size) return false;
    uint8_t initial = buf[*pos];
    uint8_t major;
    uint64_t value;
    if (!fleece_cbor_read_head(buf, size, pos, &major, &value)) return false;
    if (major == 0) {
        *out = (double)value;
        return true;
    }
    if (initial == 0xFB) {
        // float64: read_head already consumed the 8-byte payload and returned
        // its big-endian bit pattern in `value`; bit-cast it back to a double.
        double d;
        memcpy(&d, &value, sizeof(d));
        *out = d;
        return true;
    }
    return false;
}
