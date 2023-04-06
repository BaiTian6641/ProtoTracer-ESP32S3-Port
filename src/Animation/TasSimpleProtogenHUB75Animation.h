#pragma once

#include "..\Objects\SolidCube.h"

#include "Animation.h"
#include "KeyFrameTrack.h"
#include "EasyEaseAnimator.h"
#include "..\Objects\Background.h"
#include "..\Objects\LEDStripBackground.h"
#include "..\Morph\NukudeFlat.h"
#include "..\Render\Scene.h"
#include "..\Signals\FunctionGenerator.h"
//#include "..\Menu\SingleButtonMenu.h"
#include "..\Menu\ESPNOWMenu.h"
//#include "..\Menu\NeoTrellisMenu.h"
//#include "..\Sensors\APDS9960.h"

#include "..\Materials\Animated\RainbowNoise.h"
#include "..\Materials\Animated\RainbowSpiral.h"
#include "..\Materials\Animated\SpectrumAnalyzer.h"
#include "..\Materials\Animated\AudioReactiveGradient.h"
#include "..\Materials\Animated\Oscilloscope.h"

#include "..\Materials\MaterialAnimator.h"

#include "AnimationTracks\BlinkTrack.h"

#include "..\Signals\FFTVoiceDetection.h"

#include "..\Sensors\MicrophoneFourier_MAX9814.h"

#include "..\Render\ObjectAlign.h"

#include "..\Screenspace\HorizontalBlur.h"
#include "..\Screenspace\RadialBlur.h"
#include "..\Screenspace\VerticalBlur.h"

class TasSimpleProtogenHUB75Animation : public Animation<1> {
private:
    static const uint8_t faceCount = 9;
    NukudeFace pM;
    Background background;
    EasyEaseAnimator<19> eEA = EasyEaseAnimator<19>(EasyEaseInterpolation::Overshoot, 1.0f, 0.35f);
    
    //Materials
    RainbowNoise rainbowNoise;
    RainbowSpiral rainbowSpiral;
    SimpleMaterial redMaterial = SimpleMaterial(RGBColor(255, 0, 0));
    SimpleMaterial orangeMaterial = SimpleMaterial(RGBColor(255, 165, 0));
    SimpleMaterial whiteMaterial = SimpleMaterial(RGBColor(255, 255, 255));
    SimpleMaterial greenMaterial = SimpleMaterial(RGBColor(0, 255, 0));
    SimpleMaterial blueMaterial = SimpleMaterial(RGBColor(0, 0, 255));
    SimpleMaterial yellowMaterial = SimpleMaterial(RGBColor(255, 255, 0));
    SimpleMaterial purpleMaterial = SimpleMaterial(RGBColor(255, 0, 255));
    
    RGBColor gradientSpectrum[3] = {RGBColor(10, 14, 200), RGBColor(235, 26, 26),RGBColor(16, 215, 16)};
    GradientMaterial<3> gradientMat = GradientMaterial<3>(gradientSpectrum, 200.0f, false);

    RGBColor backgroundSpectrum[1] = {RGBColor(0, 0, 0)};
    GradientMaterial<1> backgroundMat = GradientMaterial<1>(backgroundSpectrum, 350.0f, false);
    
    MaterialAnimator<9> materialAnimator;
    MaterialAnimator<2> backgroundMaterial;
    
    SpectrumAnalyzer sA = SpectrumAnalyzer(Vector2D(200, 100), Vector2D(100, 50), true, true); 

    //Animation controllers
    BlinkTrack<1> blink;

    FunctionGenerator fGenMatXMove = FunctionGenerator(FunctionGenerator::Sine, -2.0f, 2.0f, 5.3f);
    FunctionGenerator fGenMatYMove = FunctionGenerator(FunctionGenerator::Sine, -2.0f, 2.0f, 6.7f);

    FunctionGenerator fGenBlur = FunctionGenerator(FunctionGenerator::Sine, 0.0f, 1.0f, 1.5f);

    //APDS9960 boop;

    FFTVoiceDetection<128> voiceDetection;

    HorizontalBlur blurH = HorizontalBlur(20);
    VerticalBlur blurV = VerticalBlur(20);
    RadialBlur blurR = RadialBlur(10);

    float offsetFace = 0.0f;
    float offsetFaceSA = 0.0f;
    uint8_t offsetFaceInd = 50;
    uint8_t offsetFaceIndSA = 51;

    void LinkEasyEase(){
        eEA.AddParameter(pM.GetMorphWeightReference(NukudeFace::Anger), NukudeFace::Anger, 15, 0.0f, 1.0f);
        eEA.AddParameter(pM.GetMorphWeightReference(NukudeFace::Sadness), NukudeFace::Sadness, 50, 0.0f, 1.0f);
        eEA.AddParameter(pM.GetMorphWeightReference(NukudeFace::Surprised), NukudeFace::Surprised, 10, 0.0f, 1.0f);
        eEA.AddParameter(pM.GetMorphWeightReference(NukudeFace::Doubt), NukudeFace::Doubt, 25, 0.0f, 1.0f);
        eEA.AddParameter(pM.GetMorphWeightReference(NukudeFace::Frown), NukudeFace::Frown, 45, 0.0f, 1.0f);
        eEA.AddParameter(pM.GetMorphWeightReference(NukudeFace::LookUp), NukudeFace::LookUp, 30, 0.0f, 1.0f);
        eEA.AddParameter(pM.GetMorphWeightReference(NukudeFace::LookDown), NukudeFace::LookDown, 30, 0.0f, 1.0f);

        eEA.AddParameter(pM.GetMorphWeightReference(NukudeFace::vrc_v_ee), NukudeFace::vrc_v_ee, 2, 0.0f, 1.0f);
        eEA.AddParameter(pM.GetMorphWeightReference(NukudeFace::vrc_v_ih), NukudeFace::vrc_v_ih, 2, 0.0f, 1.0f);
        eEA.AddParameter(pM.GetMorphWeightReference(NukudeFace::vrc_v_dd), NukudeFace::vrc_v_dd, 2, 0.0f, 1.0f);
        eEA.AddParameter(pM.GetMorphWeightReference(NukudeFace::vrc_v_rr), NukudeFace::vrc_v_rr, 2, 0.0f, 1.0f);
        eEA.AddParameter(pM.GetMorphWeightReference(NukudeFace::vrc_v_ch), NukudeFace::vrc_v_ch, 2, 0.0f, 1.0f);
        eEA.AddParameter(pM.GetMorphWeightReference(NukudeFace::vrc_v_aa), NukudeFace::vrc_v_aa, 2, 0.0f, 1.0f);
        eEA.AddParameter(pM.GetMorphWeightReference(NukudeFace::vrc_v_oh), NukudeFace::vrc_v_oh, 2, 0.0f, 1.0f);
        eEA.AddParameter(pM.GetMorphWeightReference(NukudeFace::vrc_v_ss), NukudeFace::vrc_v_ss, 2, 0.0f, 1.0f);
        
        eEA.AddParameter(pM.GetMorphWeightReference(NukudeFace::HideBlush), NukudeFace::HideBlush, 30, 1.0f, 0.0f);
        eEA.AddParameter(pM.GetMorphWeightReference(NukudeFace::HideBlush), NukudeFace::HideBlush, 30, 1.0f, 0.0f);

        eEA.AddParameter(&offsetFace, offsetFaceInd, 40, 0.0f, 1.0f);
        eEA.AddParameter(&offsetFaceSA, offsetFaceIndSA, 40, 0.0f, 1.0f);
    }

    void LinkParameters(){
        blink.AddParameter(pM.GetMorphWeightReference(NukudeFace::Blink));
    }

    void ChangeInterpolationMethods(){
        eEA.SetInterpolationMethod(NukudeFace::HideBlush, EasyEaseInterpolation::Cosine);
        eEA.SetInterpolationMethod(NukudeFace::Sadness, EasyEaseInterpolation::Cosine);
        
        eEA.SetInterpolationMethod(NukudeFace::vrc_v_ee, EasyEaseInterpolation::Linear);
        eEA.SetInterpolationMethod(NukudeFace::vrc_v_ih, EasyEaseInterpolation::Linear);
        eEA.SetInterpolationMethod(NukudeFace::vrc_v_dd, EasyEaseInterpolation::Linear);
        eEA.SetInterpolationMethod(NukudeFace::vrc_v_rr, EasyEaseInterpolation::Linear);
        eEA.SetInterpolationMethod(NukudeFace::vrc_v_ch, EasyEaseInterpolation::Linear);
        eEA.SetInterpolationMethod(NukudeFace::vrc_v_aa, EasyEaseInterpolation::Linear);
        eEA.SetInterpolationMethod(NukudeFace::vrc_v_oh, EasyEaseInterpolation::Linear);
        eEA.SetInterpolationMethod(NukudeFace::vrc_v_ss, EasyEaseInterpolation::Linear);
    }

    void SetMaterialLayers(){
        
        materialAnimator.SetBaseMaterial(Material::Add, &gradientMat);
        materialAnimator.AddMaterial(Material::Replace, &orangeMaterial, 40, 0.0f, 1.0f);//layer 1
        materialAnimator.AddMaterial(Material::Replace, &whiteMaterial, 40, 0.0f, 1.0f);//layer 2
        materialAnimator.AddMaterial(Material::Replace, &greenMaterial, 40, 0.0f, 1.0f);//layer 3
        materialAnimator.AddMaterial(Material::Replace, &yellowMaterial, 40, 0.0f, 1.0f);//layer 4
        materialAnimator.AddMaterial(Material::Replace, &purpleMaterial, 40, 0.0f, 1.0f);//layer 5
        materialAnimator.AddMaterial(Material::Replace, &redMaterial, 40, 0.0f, 1.0f);//layer 6
        materialAnimator.AddMaterial(Material::Replace, &blueMaterial, 40, 0.0f, 1.0f);//layer 7
        materialAnimator.AddMaterial(Material::Replace, &rainbowSpiral, 40, 0.0f, 1.0f);//layer 8
        materialAnimator.AddMaterial(Material::Replace, &rainbowNoise, 40, 0.15f, 1.0f);//layer 9
    }

    void UpdateKeyFrameTracks(){
        blink.Update();
    }

    void Default(){
        scene.SetEffect(&blurH);
        scene.DisableEffect();
        pM.GetObject()->SetMaterial(&gradientMat);
    }

    void Angry(){
        eEA.AddParameterFrame(NukudeFace::Anger, 1.0f);
        pM.GetObject()->SetMaterial(&redMaterial);
    }

    void Sad(){
        eEA.AddParameterFrame(NukudeFace::Sadness, 1.0f);
        eEA.AddParameterFrame(NukudeFace::Frown, 1.0f);
        pM.GetObject()->SetMaterial(&blueMaterial);
    }

    void Surprised(){
        scene.SetEffect(&blurV);
        scene.EnableEffect();
        eEA.AddParameterFrame(NukudeFace::Surprised, 1.0f);
        eEA.AddParameterFrame(NukudeFace::HideBlush, 0.0f);
        pM.GetObject()->SetMaterial(&rainbowSpiral);
    }
    
    void Doubt(){
        eEA.AddParameterFrame(NukudeFace::Doubt, 1.0f);
    }
    
    void Frown(){
        eEA.AddParameterFrame(NukudeFace::Frown, 1.0f);
    }

    void LookUp(){
        eEA.AddParameterFrame(NukudeFace::LookUp, 1.0f);
    }

    void LookDown(){
        eEA.AddParameterFrame(NukudeFace::LookDown, 1.0f);
    }

    void SpectrumAnalyzerFace(){
        eEA.AddParameterFrame(offsetFaceInd, 1.0f);
        eEA.AddParameterFrame(offsetFaceIndSA, 1.0f);
        background.GetObject()->SetMaterial(&sA);
    }

    void UpdateFFTVisemes(){
        if(true){
            eEA.AddParameterFrame(NukudeFace::vrc_v_ss, MicrophoneFourierIT::GetCurrentMagnitude() / 2.0f);

            if(MicrophoneFourierIT::GetCurrentMagnitude() > 0.05f){
                voiceDetection.Update(MicrophoneFourierIT::GetFourierFiltered(), MicrophoneFourierIT::GetSampleRate());
        
                eEA.AddParameterFrame(NukudeFace::vrc_v_ee, voiceDetection.GetViseme(voiceDetection.EE));
                eEA.AddParameterFrame(NukudeFace::vrc_v_ih, voiceDetection.GetViseme(voiceDetection.AH));
                eEA.AddParameterFrame(NukudeFace::vrc_v_dd, voiceDetection.GetViseme(voiceDetection.UH));
                eEA.AddParameterFrame(NukudeFace::vrc_v_rr, voiceDetection.GetViseme(voiceDetection.AR));
                eEA.AddParameterFrame(NukudeFace::vrc_v_ch, voiceDetection.GetViseme(voiceDetection.ER));
                eEA.AddParameterFrame(NukudeFace::vrc_v_aa, voiceDetection.GetViseme(voiceDetection.AH));
                eEA.AddParameterFrame(NukudeFace::vrc_v_oh, voiceDetection.GetViseme(voiceDetection.OO));
            }
        }
    }

    void SetMaterialColor(){
        switch(Menu::GetFaceColor()){
            case 1: materialAnimator.AddMaterialFrame(redMaterial, 0.8f); break;
            case 2: materialAnimator.AddMaterialFrame(orangeMaterial, 0.8f); break;
            case 3: materialAnimator.AddMaterialFrame(whiteMaterial, 0.8f); break;
            case 4: materialAnimator.AddMaterialFrame(greenMaterial, 0.8f); break;
            case 5: materialAnimator.AddMaterialFrame(blueMaterial, 0.8f); break;
            case 6: materialAnimator.AddMaterialFrame(yellowMaterial, 0.8f); break;
            case 7: materialAnimator.AddMaterialFrame(purpleMaterial, 0.8f); break;
            case 8: materialAnimator.AddMaterialFrame(rainbowSpiral, 0.8f); break;
            case 9: materialAnimator.AddMaterialFrame(rainbowNoise, 0.8f); break;
            default: break;
        }
    }

public:
    TasSimpleProtogenHUB75Animation() {
        Serial0.begin(115200);
        scene.AddObject(pM.GetObject());
        scene.AddObject(background.GetObject());

        LinkEasyEase();
        LinkParameters();
        ChangeInterpolationMethods();

        SetMaterialLayers();

        pM.GetObject()->SetMaterial(&gradientMat);
        background.GetObject()->SetMaterial(&backgroundMat);

        //boop.Initialize(5);
        

        MicrophoneFourierIT::Initialize(1, 8000, 60.0f, 120.0f);//8KHz sample rate, 50dB min, 120dB max
        //Menu::Initialize(9);//NeoTrellis
        Menu::Initialize(6);//7 is number of faces
        Serial0.print("Animation Init Successfully!");
    }

    uint8_t GetAccentBrightness(){
        return Menu::GetAccentBrightness();
    };

    uint8_t GetBrightness(){
        return Menu::GetBrightness();
    };

    void FadeIn(float stepRatio) override {}
    void FadeOut(float stepRatio) override {}

    Object3D* GetObject(){
        return pM.GetObject();
    }

    void Update(float ratio) override {
        pM.Reset();

        float xOffset = fGenMatXMove.Update();
        float yOffset = fGenMatYMove.Update();
        
        Menu::Update(ratio);

        blurH.SetRatio(fGenBlur.Update());
        blurV.SetRatio(fGenBlur.Update());
        blurR.SetRatio(fGenBlur.Update());

        SetMaterialColor();

        //bool isBooped = Menu::UseBoopSensor() ? boop.isBooped() : 0;
        uint8_t mode = Menu::GetFaceState();//change by button press

        MicrophoneFourierIT::Update();
        
        voiceDetection.SetThreshold(map(Menu::GetMicLevel(), 0, 10, 1000, 50));

        UpdateFFTVisemes();

        //if (true){
        //    Default();
        //}
        //else{
            if (mode == 0) Default();      //Green
            else if (mode == 1) Angry();   //Red
            else if (mode == 2) Doubt();   //Orange
            else if (mode == 3) Frown();   //Cyan
            else if (mode == 4) LookUp();  //Purple
            else if (mode == 5) Sad();     //Blue
            else {                         //Yellow
                sA.Update(MicrophoneFourierIT::GetFourierFiltered());
                SpectrumAnalyzerFace();
            }
        //}

        UpdateKeyFrameTracks();
        
        pM.SetMorphWeight(NukudeFace::BiggerNose, 1.0f);
        pM.SetMorphWeight(NukudeFace::MoveEye, 1.0f);

        sA.SetHueAngle(ratio * 360.0f * 4.0f);
        sA.SetMirrorYState(Menu::MirrorSpectrumAnalyzer());
        sA.SetFlipYState(!Menu::MirrorSpectrumAnalyzer());

        eEA.Update();
        pM.Update();

        rainbowNoise.Update(ratio);
        rainbowSpiral.Update(ratio);
        materialAnimator.Update();
        //backgroundMaterial.Update();

        uint8_t faceSize = 0;
        float scale = 1 * 0.5f + 0.45f;
        float faceSizeOffset = 0 * 8.0f;
    
        Serial0.print("X: ");
        Serial0.println(xOffset);
        Serial0.println("Y: ");
        Serial0.println(yOffset);

        pM.GetObject()->GetTransform()->SetRotation(Vector3D(0.0f, 0.0f, -7.5f));
        
        float xShift = (1.0f - 1) * -10.0f;
        float yShift = (1.0f - 1) * 70.0f + offsetFaceSA * -150.0f;
        float adjustFacePos = float(4 - faceSize) * 5.0f;
        float adjustFaceX = float(faceSize) * 0.05f;
        
        pM.GetObject()->GetTransform()->SetPosition(Vector3D(85.0f + xOffset - xShift + adjustFacePos, -19.5f + yOffset + yShift, 550.0f));
        pM.GetObject()->GetTransform()->SetScale(Vector3D(-0.975f + adjustFaceX, 0.62f, 0.65f).Multiply(scale));

        pM.GetObject()->UpdateTransform();
    }
};
