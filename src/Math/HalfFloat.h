#pragma once

#include <cstdint>
#include <cstring>

// HalfFloat — minimal IEEE-754 binary16 (half) <-> float32 converters.
//
// Used to store morph-target deltas at half precision (6 B/vec instead of
// 12 B). ESP32-S3 has no half-float hardware, so conversion is a pure
// bit-twiddle done ONLY at morph-apply time (per affected vertex of active
// morphs — a handful per frame), never in the render hot path.
//
// This type is for morph delta STORAGE only. Do NOT use it for positions,
// transforms, normals, or any accumulation math — see
// docs/face-morph-memory-optimization-plan.zh.md §4.
using float16 = uint16_t;

// float32 -> binary16 (round-to-nearest on the mantissa).
inline float16 FloatToHalf(float f) {
    uint32_t x;
    std::memcpy(&x, &f, 4);
    const uint32_t sign = (x >> 16) & 0x8000u;
    int exp = (int)((x >> 23) & 0xFF) - 127 + 15;      // rebias
    uint32_t mant = x & 0x7FFFFFu;

    if (exp <= 0) return (float16)sign;                // underflow -> signed zero
    if (exp >= 31) return (float16)(sign | 0x7C00u);   // overflow -> inf

    // Round-to-nearest: add half of the dropped-mantissa LSB, then shift.
    mant += 0x00001000u;                               // +0.5 ULP of kept mantissa
    if (mant & 0x00800000u) {                          // mantissa overflow -> bump exponent
        mant = 0;
        exp++;
        if (exp >= 31) return (float16)(sign | 0x7C00u);
    }
    return (float16)(sign | ((uint32_t)exp << 10) | (mant >> 13));
}

// binary16 -> float32.
inline float HalfToFloat(float16 h) {
    const uint32_t sign = ((uint32_t)h & 0x8000u) << 16;
    const uint32_t exp = ((uint32_t)h >> 10) & 0x1Fu;
    uint32_t mant = (uint32_t)h & 0x3FFu;
    uint32_t x;

    if (exp == 0) {
        if (mant == 0) {
            x = sign;                                  // +/-0
        } else {
            // subnormal: normalize
            int e = 127 - 15 + 1;
            while ((mant & 0x400u) == 0) { mant <<= 1; e--; }
            mant &= 0x3FFu;
            x = sign | ((uint32_t)e << 23) | (mant << 13);
        }
    } else if (exp == 31) {
        x = sign | 0x7F800000u | (mant << 13);         // inf / nan
    } else {
        x = sign | ((exp - 15 + 127) << 23) | (mant << 13);
    }
    float f;
    std::memcpy(&f, &x, 4);
    return f;
}
