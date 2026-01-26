#pragma once

#include "Material.h"
#include "../Math/Vector2D.h"

//Converts gif to RGB XY pixel matrix
class SimpleMaterial : public Material {
private:
    ProtoRGBColor rgb;
    ProtoRGBColor baseRGB;
  
public:
    SimpleMaterial(ProtoRGBColor rgb){
        this->rgb = rgb;
        this->baseRGB = rgb;
    }

    void HueShift(float hueDeg){
        rgb = baseRGB.HueShift(hueDeg);
    }

    void SetRGB(ProtoRGBColor rgb){
        this->rgb = rgb;
        this->baseRGB = rgb;
    }
    
    ProtoRGBColor GetRGB(Vector3D position, Vector3D normal, Vector3D uvw) override{
        return rgb;
    }
};
