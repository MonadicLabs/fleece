// fleece GOAP editor - CBOR codec.
// Byte-compatible with src/state/fleece_cbor.c for the types fleece uses
// (uints, text strings, arrays, float64). Env-agnostic: works in the browser
// (loaded as a plain <script>) and under Node (module.exports).
(function (root) {
    'use strict';

    const UINT_LEN = (v) => {
        if (v < 24) return 1;
        if (v <= 0xFF) return 2;
        if (v <= 0xFFFF) return 3;
        if (v <= 0xFFFFFFFF) return 5;
        return 9;
    };

    // Returns true when fleece would encode `v` as a CBOR uint (vs float64).
    // Mirrors fleece_cbor_num_size()/write_num(): v >= 0, integral, < 2^64.
    function numIsUint(v) {
        return typeof v === 'number' && Number.isInteger(v) && v >= 0 && v < 18446744073709551616;
    }

    // Text byte-length (UTF-8), the same value fleece uses via strlen().
    function textByteLength(s) {
        if (typeof TextEncoder !== 'undefined') return new TextEncoder().encode(s).length;
        return unescape(encodeURIComponent(s)).length; // node fallback
    }

    class Writer {
        constructor() {
            this.out = [];
        }

        head(major, v) {
            const mt = major << 5;
            if (v < 24) {
                this.out.push(mt | v);
            } else if (v <= 0xFF) {
                this.out.push(mt | 24, v);
            } else if (v <= 0xFFFF) {
                this.out.push(mt | 25, (v >> 8) & 0xFF, v & 0xFF);
            } else if (v <= 0xFFFFFFFF) {
                this.out.push(mt | 26);
                for (let i = 3; i >= 0; i--) this.out.push((v >> (8 * i)) & 0xFF);
            } else {
                this.out.push(mt | 27);
                for (let i = 7; i >= 0; i--) this.out.push((v / 256 ** i) & 0xFF);
            }
        }

        uint(v) { this.head(0, v); }

        arrayHeader(count) { this.head(4, count); }

        text(s) {
            s = s == null ? '' : String(s);
            const enc = typeof TextEncoder !== 'undefined' ? new TextEncoder().encode(s) : null;
            let buf;
            if (enc) {
                buf = enc;
            } else {
                buf = new Uint8Array(textByteLength(s));
                for (let i = 0; i < buf.length; i++) buf[i] = s.charCodeAt(i) & 0xFF;
            }
            this.head(3, buf.length);
            for (let i = 0; i < buf.length; i++) this.out.push(buf[i]);
        }

        // Numbers: uint when integral (matching the C writer), else float64 0xFB.
        num(v) {
            if (numIsUint(v)) { this.uint(v); return; }
            const dv = new DataView(new ArrayBuffer(8));
            dv.setFloat64(0, v, false); // big-endian, like the C memcpy loop
            this.out.push(0xFB);
            for (let i = 0; i < 8; i++) this.out.push(dv.getUint8(i));
        }

        toBytes() {
            return new Uint8Array(this.out);
        }
    }

    // --- Reading (used to re-import an existing plan blob) ---

    class Reader {
        constructor(buf) {
            this.buf = buf;
            this.pos = 0;
        }

        readHead() {
            if (this.pos >= this.buf.length) throw new Error('CBOR: unexpected end of input');
            const b = this.buf[this.pos++];
            const major = b >> 5;
            const ai = b & 0x1F;
            let value;
            if (ai < 24) value = ai;
            else if (ai === 24) value = this.readInt(1);
            else if (ai === 25) value = this.readInt(2);
            else if (ai === 26) value = this.readInt(4);
            else if (ai === 27) value = this.readInt8();
            else throw new Error('CBOR: unsupported additional info ' + ai);
            return { major, value };
        }

        readInt(n) {
            let v = 0;
            for (let i = 0; i < n; i++) v = v * 256 + this.buf[this.pos++];
            return v;
        }

        readInt8() {
            let v = 0;
            for (let i = 0; i < 8; i++) v = v * 256 + this.buf[this.pos++];
            return v;
        }

        text() {
            const { major, value } = this.readHead();
            if (major !== 3) throw new Error('CBOR: expected text string');
            if (this.pos + value > this.buf.length) throw new Error('CBOR: string out of bounds');
            let s = '';
            const dec = typeof TextDecoder !== 'undefined' ? new TextDecoder() : null;
            if (dec) {
                s = dec.decode(this.buf.subarray(this.pos, this.pos + value));
            } else {
                for (let i = 0; i < value; i++) s += String.fromCharCode(this.buf[this.pos + i]);
            }
            this.pos += value;
            return s;
        }

        arrayHeader() {
            const { major, value } = this.readHead();
            if (major !== 4) throw new Error('CBOR: expected array');
            return value;
        }

        num() {
            const { major, value } = this.readHead();
            if (major === 0) return value;
            if (major === 7 && value === 27) {
                const dv = new DataView(this.buf.buffer, this.buf.byteOffset + this.pos, 8);
                const v = dv.getFloat64(0, false);
                this.pos += 8;
                return v;
            }
            throw new Error('CBOR: expected number');
        }

        skip() {
            const { major, value } = this.readHead();
            switch (major) {
                case 0: case 1: case 7: return;         // uint / negint / float
                case 2: case 3:                          // bytes / text
                    if (this.pos + value > this.buf.length) throw new Error('CBOR: out of bounds');
                    this.pos += value;
                    return;
                case 4: {                                 // array
                    for (let i = 0; i < value; i++) this.skip();
                    return;
                }
                case 5: {                                 // map (unsupported by fleece but skip safely)
                    for (let i = 0; i < value * 2; i++) this.skip();
                    return;
                }
                default: throw new Error('CBOR: unsupported major type ' + major);
            }
        }

        done() { return this.pos >= this.buf.length; }
    }

    root.FleeceCbor = { Writer, Reader, numIsUint, textByteLength };
    if (typeof module !== 'undefined' && module.exports) module.exports = root.FleeceCbor;
})(typeof self !== 'undefined' ? self : globalThis);
