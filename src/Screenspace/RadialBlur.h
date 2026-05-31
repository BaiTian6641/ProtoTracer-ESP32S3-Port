#pragma once

#include "Effect.h"

class RadialBlur: public Effect {
private:
    const uint8_t pixels;
    FunctionGenerator fGenRotation = FunctionGenerator(FunctionGenerator::Sawtooth, 0.0f, 360.0f, 3.7f);

public:
    RadialBlur(uint8_t pixels) : pixels(pixels){}

    void ApplyEffect(IPixelGroup* pixelGroup){
        unsigned int pixelCount = pixelGroup->GetPixelCount();
        ProtoRGBColor* pixelColors = pixelGroup->GetColors();
        ProtoRGBColor* colorBuffer = pixelGroup->GetColorBuffer();

        float rotation = fGenRotation.Update();

        // Compute blur range once
        uint16_t blurRange = (uint16_t)Mathematics::Map(ratio, 0.0f, 1.0f, 1.0f, float(pixels / 2));
        if (blurRange < 1) blurRange = 1;

        for (unsigned int i = 0; i < pixelCount; i++){
            unsigned int indexV = i, indexD = i;
            unsigned int tIndexV = 0, tIndexD = 0;
            bool validV = true, validD = true;

            uint16_t R = (uint16_t)pixelColors[i].R;
            uint16_t G = (uint16_t)pixelColors[i].G;
            uint16_t B = (uint16_t)pixelColors[i].B;
            uint16_t sampleCount = 1;
            
            for (uint8_t j = 1; j < blurRange + 1; j++){
                validV = pixelGroup->GetRadialIndex(indexV, &tIndexV, blurRange, rotation);
                validD = pixelGroup->GetRadialIndex(indexD, &tIndexD, blurRange, rotation + 180.0f);

                indexV = tIndexV;
                indexD = tIndexD;

                if (validV) {
                    R += (uint16_t)pixelColors[indexV].R;
                    G += (uint16_t)pixelColors[indexV].G;
                    B += (uint16_t)pixelColors[indexV].B;
                    sampleCount++;
                }
                if (validD) {
                    // Fix: use indexD (the index variable), not validD (the boolean)
                    R += (uint16_t)pixelColors[indexD].R;
                    G += (uint16_t)pixelColors[indexD].G;
                    B += (uint16_t)pixelColors[indexD].B;
                    sampleCount++;
                }
            }
            
            colorBuffer[i].R = (uint8_t)Mathematics::Constrain(R / sampleCount, 0, 255);
            colorBuffer[i].G = (uint8_t)Mathematics::Constrain(G / sampleCount, 0, 255);
            colorBuffer[i].B = (uint8_t)Mathematics::Constrain(B / sampleCount, 0, 255);
        }
        
        for (unsigned int i = 0; i < pixelCount; i++){
            pixelColors[i].R = colorBuffer[i].R;
            pixelColors[i].G = colorBuffer[i].G;
            pixelColors[i].B = colorBuffer[i].B;
        }
    }
};

