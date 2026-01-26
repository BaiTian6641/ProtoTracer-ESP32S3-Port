#pragma once

#include "Effect.h"
#include "../Render/IPixelGroup.h"
#include "../Materials/RGBColor.h"

class AntiAliasingEffect : public Effect {
private:
    float smoothing = 0.25f; // 0.0 = no smoothing, 1.0 = max smoothing (blur)

public:
    AntiAliasingEffect(float smoothing = 0.25f) {
        this->smoothing = smoothing;
    }

    void SetSmoothing(float smoothing) {
        this->smoothing = smoothing;
    }

    void ApplyEffect(IPixelGroup* pixelGroup) override {
        if (smoothing <= 0.001f) return;

        unsigned int pixelCount = pixelGroup->GetPixelCount();
        ProtoRGBColor* src = pixelGroup->GetColors();
        ProtoRGBColor* dst = pixelGroup->GetColorBuffer(); // Use the secondary buffer as scratchpad

        for (unsigned int i = 0; i < pixelCount; i++) {
            ProtoRGBColor current = src[i];
            
            int r = current.R;
            int g = current.G;
            int b = current.B;
            
            int neighborR = 0;
            int neighborG = 0;
            int neighborB = 0;
            int neighborCount = 0;

            unsigned int idx;
            
            // Accumulate valid neighbors
            if (pixelGroup->GetUpIndex(i, &idx)) {
                neighborR += src[idx].R;
                neighborG += src[idx].G;
                neighborB += src[idx].B;
                neighborCount++;
            }
            if (pixelGroup->GetDownIndex(i, &idx)) {
                neighborR += src[idx].R;
                neighborG += src[idx].G;
                neighborB += src[idx].B;
                neighborCount++;
            }
            if (pixelGroup->GetLeftIndex(i, &idx)) {
                neighborR += src[idx].R;
                neighborG += src[idx].G;
                neighborB += src[idx].B;
                neighborCount++;
            }
            if (pixelGroup->GetRightIndex(i, &idx)) {
                neighborR += src[idx].R;
                neighborG += src[idx].G;
                neighborB += src[idx].B;
                neighborCount++;
            }

            if (neighborCount > 0) {
                // Calculate average of neighbors
                neighborR /= neighborCount;
                neighborG /= neighborCount;
                neighborB /= neighborCount;

                // Blend current with neighbor average based on smoothing factor
                // New = Current * (1 - smoothing) + NeighborAvg * smoothing
                // Using float for precision then casting back
                
                float invSmooth = 1.0f - smoothing;
                
                dst[i].R = (uint8_t)(r * invSmooth + neighborR * smoothing);
                dst[i].G = (uint8_t)(g * invSmooth + neighborG * smoothing);
                dst[i].B = (uint8_t)(b * invSmooth + neighborB * smoothing);
            } else {
                dst[i] = current;
            }
        }

        // Copy back to main buffer
        for (unsigned int i = 0; i < pixelCount; i++) {
            src[i] = dst[i];
        }
    }
};
