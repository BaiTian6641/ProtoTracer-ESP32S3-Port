#pragma once

#include "Effect.h"
#include "../Render/IPixelGroup.h"
#include "../Materials/RGBColor.h"

class EdgeFeatherEffect : public Effect {
private:
    float featherStrength = 0.5f; // How much to dim the edge pixels (0.0 - 1.0)

public:
    EdgeFeatherEffect(float strength = 0.5f) {
        this->featherStrength = strength;
    }

    void SetFeatherStrength(float strength) {
        this->featherStrength = strength;
    }

    void ApplyEffect(IPixelGroup* pixelGroup) override {
        unsigned int pixelCount = pixelGroup->GetPixelCount();
        ProtoRGBColor* colors = pixelGroup->GetColors();
        
        // We use a temporary buffer to store the indices of pixels to be dimmed
        // to avoid modifying the color buffer while reading it, although for this specific
        // algorithm (checking for black neighbors), in-place modification is mostly safe
        // as long as we don't turn a pixel black.
        // However, to be perfectly safe and allow for potentially different "background" logic later,
        // we can do a two-pass approach or just modify in place if we are sure.
        // Since we are on a microcontroller (ESP32), memory is precious. 
        // Let's try to do it in-place but be careful.
        // Actually, we only check if neighbor is (0,0,0). Dimming (255,255,255) to (128,128,128) 
        // still leaves it non-black. So in-place is fine.

        for (unsigned int i = 0; i < pixelCount; i++) {
            ProtoRGBColor* c = &colors[i];
            // Skip background pixels
            if (c->R == 0 && c->G == 0 && c->B == 0) continue;

            bool isEdge = false;
            unsigned int neighborIndex;
            
            // Check neighbors. If any neighbor is missing (boundary of group) or black (background), it's an edge.
            
            // Up
            if (!pixelGroup->GetUpIndex(i, &neighborIndex)) {
                isEdge = true;
            } else {
                ProtoRGBColor* n = &colors[neighborIndex];
                if (n->R == 0 && n->G == 0 && n->B == 0) isEdge = true;
            }
            
            if (!isEdge) {
                // Down
                if (!pixelGroup->GetDownIndex(i, &neighborIndex)) {
                    isEdge = true;
                } else {
                    ProtoRGBColor* n = &colors[neighborIndex];
                    if (n->R == 0 && n->G == 0 && n->B == 0) isEdge = true;
                }
            }

            if (!isEdge) {
                // Left
                if (!pixelGroup->GetLeftIndex(i, &neighborIndex)) {
                    isEdge = true;
                } else {
                    ProtoRGBColor* n = &colors[neighborIndex];
                    if (n->R == 0 && n->G == 0 && n->B == 0) isEdge = true;
                }
            }

            if (!isEdge) {
                // Right
                if (!pixelGroup->GetRightIndex(i, &neighborIndex)) {
                    isEdge = true;
                } else {
                    ProtoRGBColor* n = &colors[neighborIndex];
                    if (n->R == 0 && n->G == 0 && n->B == 0) isEdge = true;
                }
            }

            if (isEdge) {
                c->R = (uint8_t)(c->R * featherStrength);
                c->G = (uint8_t)(c->G * featherStrength);
                c->B = (uint8_t)(c->B * featherStrength);
            }
        }
    }
};
