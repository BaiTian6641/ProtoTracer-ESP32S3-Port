#pragma once
#include <cstddef>
#include <cstdint>

// Phone/offline encoder output: top-left, row-major 64x32. No GIF/LZW on MCU.
namespace MediaCodec {
constexpr size_t Pixels = 64 * 32;
constexpr size_t MaxFrame = 1 + Pixels * 2;
inline uint16_t U16(const uint8_t* p) { return uint16_t(p[0]) | uint16_t(p[1]) << 8; }
inline uint32_t U32(const uint8_t* p) { return uint32_t(U16(p)) | uint32_t(U16(p + 2)) << 16; }
inline uint32_t Hash(const uint8_t* p, size_t n, uint32_t h = 2166136261u) {
    while (n--) h = (h ^ *p++) * 16777619u;
    return h;
}
// Codes 0/1 = RGB332 raw/RLE, 2/3 = RGB565 raw/RLE.
// RLE: count (1..255), then one little-endian color. Always a complete frame.
template<class Pixel>
bool Decode(const uint8_t* data, size_t size, Pixel* out) {
    if (!data || !out || size < 2 || size > MaxFrame || data[0] > 3) return false;
    const uint8_t code = *data++;
    --size;
    const size_t bytes = code < 2 ? 1 : 2;
    size_t written = 0;
    while (size) {
        size_t run = 1;
        if (code & 1) { run = *data++; --size; }
        if (!run || size < bytes || written + run > Pixels) return false;
        const uint16_t c = bytes == 1 ? data[0] : U16(data);
        data += bytes; size -= bytes;
        uint8_t r, g, b;
        if (bytes == 1) {
            r = uint8_t(((c >> 5) & 7) * 255 / 7);
            g = uint8_t(((c >> 2) & 7) * 255 / 7);
            b = uint8_t((c & 3) * 85);
        } else {
            r = uint8_t((c >> 11) * 255 / 31);
            g = uint8_t(((c >> 5) & 63) * 255 / 63);
            b = uint8_t((c & 31) * 255 / 31);
        }
        while (run--) { out[written].R = r; out[written].G = g; out[written].B = b; ++written; }
    }
    return written == Pixels;
}
inline bool Header(const uint8_t* p, size_t n) {
    return n >= 12 && p[0] == 'P' && p[1] == 'T' && p[2] == 'M' && p[3] == '1'
        && p[4] == 64 && p[5] == 32 && U16(p + 6) > 0 && U16(p + 6) <= 1000
        && U32(p + 8) == 0;
}
}
