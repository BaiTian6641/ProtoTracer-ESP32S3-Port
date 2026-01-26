#pragma once

#include "AnimatedMaterial.h"
#include "../../Signals/FunctionGenerator.h"
#include "../../Materials/SpiralMaterial.h"

class RainbowSpiral : public AnimatedMaterial{
private:
    FunctionGenerator fGenMatBend = FunctionGenerator(FunctionGenerator::Sine, 0.8f, 0.9f, 6.7f);
    ProtoRGBColor rainbowSpectrum[6] = {ProtoRGBColor(255, 0, 0), ProtoRGBColor(255, 255, 0), ProtoRGBColor(0, 255, 0), ProtoRGBColor(0, 255, 255), ProtoRGBColor(0, 0, 255), ProtoRGBColor(255, 0, 255)};
    SpiralMaterial spiralMaterial = SpiralMaterial(6, rainbowSpectrum, 3.0f, 7.0f);

public:
    RainbowSpiral(){}

    void Update(float ratio) override{
        spiralMaterial.SetBend(fGenMatBend.Update());
        spiralMaterial.SetRotationAngle((1.0f - ratio) * 360.0f);
        spiralMaterial.SetPositionOffset(Vector2D(0.0f, 75.0f));
    }

    void HueShift(float hueDeg){
        for(uint8_t i = 0; i < 6; i++){
            rainbowSpectrum[i] = rainbowSpectrum[i].HueShift(hueDeg);
        }
    }

    Material* GetMaterial() override{
        return &spiralMaterial;
    }

    ProtoRGBColor GetRGB(Vector3D position, Vector3D normal, Vector3D uvw) override{
        return spiralMaterial.GetRGB(position, normal, uvw);
    }
};
