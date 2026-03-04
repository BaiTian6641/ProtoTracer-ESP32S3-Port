#pragma once

#include "../Render/IPixelGroup.h"

/**
 * @brief Screen-space effect type identification.
 *
 * Used by GPUDriverController to map ProtoTracer Effect subclasses
 * to ProtoGL shader commands (PglShaderClass + params) without RTTI.
 */
enum class EffectType : uint8_t {
    Unknown           = 0,
    HorizontalBlur    = 1,   // → PGL_SHADER_CONVOLUTION, angle=0°
    VerticalBlur      = 2,   // → PGL_SHADER_CONVOLUTION, angle=90°
    RadialBlur        = 3,   // → PGL_SHADER_CONVOLUTION, auto-rotate
    PhaseOffsetX      = 4,   // → PGL_SHADER_DISPLACEMENT, axis=X
    PhaseOffsetY      = 5,   // → PGL_SHADER_DISPLACEMENT, axis=Y
    PhaseOffsetR      = 6,   // → PGL_SHADER_DISPLACEMENT, axis=Radial
    EdgeFeather       = 7,   // → PGL_SHADER_COLOR_ADJUST, op=EdgeFeather
    AntiAliasing      = 8,   // → PGL_SHADER_CONVOLUTION, separable 2D
};

class Effect {
protected:
    float ratio = 0.0f;

public:
    Effect(){}

    void SetRatio(float ratio){
        this->ratio = ratio;
    }

    float GetRatio() const { return ratio; }

    /// Identify the effect type for GPU shader mapping.
    virtual EffectType GetEffectType() const { return EffectType::Unknown; }

    virtual void ApplyEffect(IPixelGroup* pixelGroup) = 0;

};
