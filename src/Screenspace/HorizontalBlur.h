#pragma once

#include "Effect.h"

class HorizontalBlur: public Effect {
private:
    const uint8_t pixels;

public:
    HorizontalBlur(uint8_t pixels) : pixels(pixels){}

    void ApplyEffect(IPixelGroup* pixelGroup) override {
        ProtoRGBColor* pixelColors = pixelGroup->GetColors();
        ProtoRGBColor* colorBuffer = pixelGroup->GetColorBuffer();
        unsigned int pixelCount = pixelGroup->GetPixelCount();

        // Compute blur range once — same for all pixels in this pass
        uint16_t blurRange = (uint16_t)Mathematics::Map(ratio, 0.0f, 1.0f, 1.0f, float(pixels / 2));
        if (blurRange < 1) blurRange = 1;

        for (unsigned int i = 0; i < pixelCount; i++){
            unsigned int indexLeft = i;
            unsigned int indexRight = i;
            unsigned int tIndexLeft = 0;
            unsigned int tIndexRight = 0;
            bool validL = true;
            bool validR = true;

            uint16_t R = (uint16_t)pixelColors[i].R;
            uint16_t G = (uint16_t)pixelColors[i].G;
            uint16_t B = (uint16_t)pixelColors[i].B;
            uint16_t sampleCount = 1; // start counting with center pixel
            
            for (uint8_t j = 1; j < blurRange + 1; j++){
                validL = pixelGroup->GetLeftIndex(indexLeft, &tIndexLeft);
                validR = pixelGroup->GetRightIndex(indexRight, &tIndexRight);

                indexLeft = tIndexLeft;
                indexRight = tIndexRight;

                if (validL) {
                    R += (uint16_t)pixelColors[indexLeft].R;
                    G += (uint16_t)pixelColors[indexLeft].G;
                    B += (uint16_t)pixelColors[indexLeft].B;
                    sampleCount++;
                }
                if (validR) {
                    R += (uint16_t)pixelColors[indexRight].R;
                    G += (uint16_t)pixelColors[indexRight].G;
                    B += (uint16_t)pixelColors[indexRight].B;
                    sampleCount++;
                }
            }
            
            // Correct channel order: R→R, G→G, B→B
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

