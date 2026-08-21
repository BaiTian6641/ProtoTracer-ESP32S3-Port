#pragma once

#include <math.h>

// FastTrig — small sine/cosine lookup table with linear interpolation.
//
// Jet-derived idea (reference/Jet TrigLUT): replace libm sinf/cosf in
// *non-precision-critical* per-frame paths (easing curves, generators) with a
// table lookup. ESP32-S3 has a single-precision FPU but no hardware trig, so
// sinf/cosf cost tens of cycles each via software polynomial evaluation.
//
// Accuracy: 256-entry full-period table + linear interpolation.
//   max abs error of sin ≈ (2π/256)² / 8 ≈ 7.5e-5, which is far below the
//   visual threshold for animation easing / effect modulation.
//
// NOT for: rotation math (Rotation.h/Quaternion), camera transforms, or any
// place where small angle errors could accumulate or alter geometry — those
// keep libm sinf/cosf. Only easing/generator paths should call these.
//
// The table is built once (first call) into a 1 KB static array.
class FastTrig {
public:
    static constexpr int kN = 256;              // table entries over [0, 2π)
    static constexpr float kTwoPi = 6.28318530717958647692f;
    static constexpr float kScale = (float)kN / kTwoPi;   // rad → table index
    static constexpr float kInvN = kTwoPi / (float)kN;

    // Fast sine in radians. ~1e-4 max abs error at N=256 after interpolation.
    static inline float Sin(float rad) {
        EnsureInit();
        // Range-reduce to [0, 2π) without fmodf (floorf is cheaper), then to
        // table units [0, kN). kN is a power of two so the table wraps with &.
        const float wrapped = rad - floorf(rad * (1.0f / kTwoPi)) * kTwoPi; // [0, 2π)
        const float fi = wrapped * kScale;             // [0, kN)
        const int i0 = (int)fi;                        // 0..kN-1
        const float frac = fi - (float)i0;
        const int i1 = (i0 + 1) & (kN - 1);
        const float a = sTable[i0];
        const float b = sTable[i1];
        return a + (b - a) * frac;
    }

    // Fast cosine in radians (phase-shifted sine).
    static inline float Cos(float rad) {
        return Sin(rad + kTwoPi * 0.25f);
    }

private:
    static float sTable[kN];
    static bool sReady;

    static void EnsureInit() {
        if (sReady) return;
        for (int i = 0; i < kN; i++) {
            sTable[i] = sinf((float)i * kInvN);
        }
        sReady = true;
    }
};

// Static member definitions (C++17: inline to stay header-only, single TU).
inline float FastTrig::sTable[FastTrig::kN];
inline bool FastTrig::sReady = false;
