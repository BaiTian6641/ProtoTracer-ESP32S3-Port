#pragma once

#include "Animation.h"
#include "..\Objects\SolidCube.h"
#include "..\Materials\DepthMaterial.h"
#include "..\Materials\LightMaterial.h"
#include "..\Signals\FunctionGenerator.h"

#include "..\Materials\Animated\RainbowSpiral.h"
#include "..\Objects\Background.h"

#include "..\Materials\MaterialAnimator.h"



//#include "Flash\Images\CoelaToot.h"
#include "Flash\ImageSequences\Bonk.h"

class CoelaBonkAnimation : public Animation<1>{
private:
RainbowSpiral rainbowSpiral;
    SolidCube cube;
    Background background;
    RGBColor gradientSpectrum[2] = {RGBColor(10, 14, 210), RGBColor(26, 200, 26)};
    GradientMaterial<2> gradientMat = GradientMaterial<2>(gradientSpectrum, 350.0f, false);

    FunctionGenerator fGenObjRotation = FunctionGenerator(FunctionGenerator::Sine, -360.0f, 360.0f, 6.0f);
    FunctionGenerator fGenObjScale = FunctionGenerator(FunctionGenerator::Sine, 10.0f, 10.0f, 4.0f);
    FunctionGenerator fGenMatPos = FunctionGenerator(FunctionGenerator::Sine, -5.0f, 5.0f, 3.0f);
    FunctionGenerator fGenMatSize = FunctionGenerator(FunctionGenerator::Sine, 150.0f, 175.0f, 2.1f);
    FunctionGenerator fGenMatRot = FunctionGenerator(FunctionGenerator::Sine, -5.0f, 5.0f, 3.2f);
    BonkSequence gif = BonkSequence(Vector2D(200, 145), Vector2D(100, 70), 30);
    
    MaterialAnimator<1> backgroundMaterial;

    float rainbowFace = 0.0f;
    uint8_t rainbowFaceInd = 51;
public:
    CoelaBonkAnimation(){
        scene.AddObject(cube.GetObject());
        scene.AddObject(background.GetObject());

        backgroundMaterial.SetBaseMaterial(Material::Add, &gradientMat);
        backgroundMaterial.AddMaterial(Material::Replace, &rainbowSpiral, 40, 0.0f, 1.0f);
        
        background.GetObject()->SetMaterial(&gradientMat);
        cube.GetObject()->SetMaterial(&gif);
    }

    void FadeIn(float stepRatio) override {}
    void FadeOut(float stepRatio) override {}

    uint8_t GetAccentBrightness(){
        return 50;
    };

    uint8_t GetBrightness(){
        return 50;
    };

    void Reset(){
        gif.Reset();
    }

    void Update(float ratio) override {
        float x = fGenObjRotation.Update();
        float sx = fGenObjScale.Update();
        float shift = fGenMatPos.Update();
        float size = fGenMatSize.Update();
        float rotate = fGenMatRot.Update();
        backgroundMaterial.AddMaterialFrame(rainbowSpiral, rainbowFace);
        
        Quaternion rotation = Rotation(EulerAngles(Vector3D(x, ratio * 720.0f, 0), EulerConstants::EulerOrderXZYS)).GetQuaternion();

        rainbowSpiral.Update(ratio);
        backgroundMaterial.Update();

        gif.SetPosition(Vector2D(0, 50.f));
        gif.SetSize(Vector2D(0.5, 0.5));
        gif.SetRotation(rotate);
        gif.Update();

        cube.GetObject()->ResetVertices();

        cube.GetObject()->GetTransform()->SetRotation(rotation);
        cube.GetObject()->GetTransform()->SetScale(Vector3D(0.5, 0.5,0.5));
        cube.GetObject()->GetTransform()->SetScaleOffset(cube.GetObject()->GetCenterOffset());
        cube.GetObject()->GetTransform()->SetPosition(Vector3D(0.0f, 50.0f, 500.0f));

        cube.GetObject()->UpdateTransform();
    }
};
