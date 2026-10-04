#pragma once

#include "Material.h"
#include "../Math/Mathematics.h"
#include "../Math/Vector2D.h"
#include "../Math/Rotation.h"
#include "PreparedMaterialRotation.h"
#include <vector>

class SpiralMaterial : public Material {
private:
    std::vector<ProtoRGBColor> rgbColors;
    std::vector<ProtoRGBColor> baseRGBColors;
    uint8_t colorCount;
    Vector2D positionOffset;
    Vector2D rotationOffset;//point to rotate about
    float width;
    float bend;
    float rotationAngle = 0.0f;//rotate input xyPosition
    PreparedMaterialRotation rotation;
  
public:
    SpiralMaterial(uint8_t colorCount, ProtoRGBColor* rgbColors, float width, float bend){
        this->colorCount = colorCount;
        this->width = width;
        this->bend = bend;

        this->rgbColors.resize(colorCount);
        this->baseRGBColors.resize(colorCount);

        for(int i = 0; i < colorCount; i++){
            this->rgbColors[i] = rgbColors[i];
            this->baseRGBColors[i] = rgbColors[i];
        }
    }

    void SetPositionOffset(Vector2D positionOffset){
        this->positionOffset = positionOffset;
    }
    
    void SetRotationOffset(Vector2D rotationOffset){
        this->rotationOffset = rotationOffset;
    }

    void SetRotationAngle(float rotationAngle){
        if (this->rotationAngle == rotationAngle) return;
        this->rotationAngle = rotationAngle;
        rotation.SetAngle(rotationAngle);
    }

    void SetWidth(float width){
        this->width = width;
    }

    void SetBend(float bend){
        this->bend = bend;  
    }


    void HueShift(float hueDeg){
        for(int i = 0; i < colorCount; i++){
            rgbColors[i] = baseRGBColors[i].HueShift(hueDeg);
        }
    }
    
    ProtoRGBColor GetRGB(Vector3D position, Vector3D normal, Vector3D uvw) override {
        if(rotationAngle != 0){
            position = position - Vector3D(positionOffset.X, positionOffset.Y, 0);
            position = rotation.Apply(position);
        }

        
        //from x position, fit into bucket ratio
        //modulo x value into x range from start position to end position

        float radius = Vector2D(position).Magnitude();
        float angle = atan2(position.Y, position.X);
        float ratio = Mathematics::Fract(width * angle / Mathematics::MPI + bend * powf(radius, 0.3f));
        
        int startBox = floor(ratio * colorCount);

        ProtoRGBColor rgb = rgbColors[startBox];

        return rgb;
    }
};
