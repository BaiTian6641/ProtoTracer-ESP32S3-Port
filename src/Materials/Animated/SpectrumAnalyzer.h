#pragma once

#include "../Material.h"
#include "../GradientMaterial.h"
#include "../../Controls/BouncePhysics.h"

class SpectrumAnalyzer : public Material {
private:
    BouncePhysics* bPhy[128];
    Vector2D size;
    Vector2D offset;
    float angle = 0.0f;
    float hueAngle = 0.0f;
    uint8_t colors;
    float* data;
    float bounceData[128];
    uint8_t bins = 128;
    bool mirrorY = false;
    bool flipY = false;
    bool bounce = false;
    
    ProtoRGBColor rainbowSpectrum[6] = {ProtoRGBColor(255, 0, 0), ProtoRGBColor(255, 255, 0), ProtoRGBColor(0, 255, 0), ProtoRGBColor(0, 255, 255), ProtoRGBColor(0, 0, 255), ProtoRGBColor(255, 0, 255)};
    GradientMaterial<6> gM = GradientMaterial<6>(rainbowSpectrum, 1.0f, false);

    Material* material;

public:
    SpectrumAnalyzer(Vector2D size, Vector2D offset, bool bounce = false, bool flipY = false, bool mirrorY = false){
        this->size = size.Divide(2.0f);
        this->offset = offset;
        this->mirrorY = mirrorY;
        this->flipY = flipY;
        this->bounce = bounce;
        this->material = &gM;

        if (bounce){
            for (uint8_t i = 0; i < 128; i++){
                bPhy[i] = new BouncePhysics(35.0f, 15.0f);
            }
        }
    }

    ~SpectrumAnalyzer(){
        for (uint8_t i = 0; i < 128; i++){
            delete bPhy[i];
        }

    }

    void SetMirrorYState(bool state){
        mirrorY = state;
    }

    void SetFlipYState(bool state){
        flipY = state;
    }

    void SetMaterial(Material* material){
        this->material = material;
    }

    float* GetFourierData(){
        if(bounce){
            return bounceData;
        }
        else{
            return data;
        }
    }

    void SetSize(Vector2D size){
        this->size = size.Divide(2.0f);
    }

    void SetPosition(Vector2D offset){
        this->offset = offset;
    }

    void SetRotation(float angle){
        this->angle = angle;
    }

    void SetHueAngle(float hueAngle){
        this->hueAngle = hueAngle;
    }

    void Update(float* readData){
        data = readData;
        
        for (uint8_t i = 0; i < 128; i++){
            if(bounce){
                bounceData[i] = bPhy[i]->Calculate(data[i], 0.1f);
            }
            else{
                bounceData[i] = data[i];
            }
        }
    }

    ProtoRGBColor GetRGB(Vector3D position, Vector3D normal, Vector3D uvw) override {
        Vector2D rPos = Mathematics::IsClose(angle, 0.0f, 0.1f) ? Vector2D(position.X, position.Y) - offset : Vector2D(position.X, position.Y).Rotate(angle, offset) - offset;

        // Fix: use || for out-of-range rejection (was impossible &&)
        if (-size.X > rPos.X || size.X < rPos.X) return ProtoRGBColor();
        if (-size.Y > rPos.Y || size.Y < rPos.Y) return ProtoRGBColor();
        
        // Use int16_t for intermediate bin math to avoid uint8_t overflow and sign issues
        int16_t x = (int16_t)Mathematics::Map(rPos.X, -size.X, size.X, float(bins), 0.0f);

        // Clamp x to valid range before reading x+1
        if (x < 0 || x >= (int16_t)(bins - 1)) return ProtoRGBColor();

        float xDistance = size.X / float(bins) * (float)x - size.X;
        float xDistance2 = size.X / float(bins) * (float)(x + 1) - size.X;
        float ratio = Mathematics::Map(rPos.X, xDistance, xDistance2, 0.0f, 1.0f);//ratio between two bins
        float height = bounce ? Mathematics::CosineInterpolation(bounceData[x], bounceData[x + 1], ratio) : Mathematics::CosineInterpolation(data[x], data[x + 1], ratio);//0->1.0f of max height of color
        float yColor;

        height = height * 3.0f;

        if(mirrorY){
            yColor = Mathematics::Map(fabsf(rPos.Y), size.Y, 0.0f, 1.0f, 0.0f);
        }
        else{
            yColor = Mathematics::Map(rPos.Y, -size.Y, size.Y, 1.0f, 0.0f);
        }
        
        if(flipY) yColor = 1.0f - yColor;

        if (yColor <= height){
            return material->GetRGB(Vector3D(1.0f - height - yColor, 0, 0), Vector3D(), Vector3D()).HueShift(hueAngle);
        }
        else{
            return ProtoRGBColor(0, 0, 0);
        }
    }
};
