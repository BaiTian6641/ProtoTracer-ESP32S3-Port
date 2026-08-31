#pragma once
// Host-test shim for ESP-DSP (Windows x64, MSVC).
// Scalar reference implementations of the vector ops used by the render core.
//
// IMPORTANT for A/B determinism: both the legacy and the direct rasterizer
// builds must use the SAME implementation of these ops, so the shim is kept
// bit-identical across builds (it is, being a single header).

inline void dsps_mulc_f32(const float* in, float* out, int n, float c, int stepIn, int stepOut) {
    for (int i = 0; i < n; i++) {
        out[i * stepOut] = in[i * stepIn] * c;
    }
}

inline void dsps_add_f32(const float* in1, const float* in2, float* out, int n, int stepIn1, int stepIn2, int stepOut) {
    for (int i = 0; i < n; i++) {
        out[i * stepOut] = in1[i * stepIn1] + in2[i * stepIn2];
    }
}

inline void dsps_sub_f32(const float* in1, const float* in2, float* out, int n, int stepIn1, int stepIn2, int stepOut) {
    for (int i = 0; i < n; i++) {
        out[i * stepOut] = in1[i * stepIn1] - in2[i * stepIn2];
    }
}

inline void dsps_dotprod_f32_ae32(const float* in1, const float* in2, int n, float* out) {
    float sum = 0.0f;
    for (int i = 0; i < n; i++) {
        sum += in1[i] * in2[i];
    }
    *out = sum;
}
