#pragma once

#include "../Objects/SolidCube.h"

#include "Animation.h"
#include "KeyFrameTrack.h"
#include "EasyEaseAnimator.h"
#include "../Objects/Background.h"
#include "../Objects/LEDStripBackground.h"
#include "../Morph/JsonNukudeFace.h"
#include "../Render/Scene.h"
#include "../Signals/FunctionGenerator.h"

#include "../Menu/ESPMenu.h"

#include "../Materials/Animated/RainbowNoise.h"
#include "../Materials/Animated/RainbowSpiral.h"
#include "../Materials/Animated/SpectrumAnalyzer.h"
#include "../Materials/Animated/AudioReactiveGradient.h"
#include "../Materials/Animated/Oscilloscope.h"

#include "../Materials/MaterialAnimator.h"

#include "AnimationTracks/BlinkTrack.h"

#include "../Signals/FFTVoiceDetection.h"

#include "../Sensors/MicrophoneFourier_MAX9814.h"

#include "../Render/ObjectAlign.h"

#include "../Screenspace/HorizontalBlur.h"
#include "../Screenspace/RadialBlur.h"
#include "../Screenspace/VerticalBlur.h"
#include "../Screenspace/AntiAliasingEffect.h"
#include <LittleFS.h>
#include <M5UnitGLASS2.h>

#ifndef USE_JSON_FACE_MODEL
#define USE_JSON_FACE_MODEL 1
#endif

extern M5UnitGLASS2 display;

extern uint8_t User_R;
extern uint8_t User_G;
extern uint8_t User_B;

class TasSimpleProtogenHUB75Animation : public Animation<1>
{
private:
    static const uint8_t faceCount = 9;
    JsonNukudeFace jsonFace;
    bool jsonFaceLoaded = false;
    Background background;
    EasyEaseAnimator<70> eEA = EasyEaseAnimator<70>(EasyEaseInterpolation::Overshoot, 1.0f, 0.25f);

    // Materials
    RainbowNoise rainbowNoise;
    RainbowSpiral rainbowSpiral;
    SimpleMaterial redMaterial = SimpleMaterial(ProtoRGBColor(255, 0, 0));
    SimpleMaterial orangeMaterial = SimpleMaterial(ProtoRGBColor(255, 165, 0));
    SimpleMaterial whiteMaterial = SimpleMaterial(ProtoRGBColor(255, 255, 255));
    SimpleMaterial greenMaterial = SimpleMaterial(ProtoRGBColor(0, 255, 0));
    SimpleMaterial blueMaterial = SimpleMaterial(ProtoRGBColor(0, 0, 255));
    SimpleMaterial yellowMaterial = SimpleMaterial(ProtoRGBColor(255, 255, 0));
    SimpleMaterial purpleMaterial = SimpleMaterial(ProtoRGBColor(255, 0, 255));

    ProtoRGBColor gradientSpectrum[3] = {ProtoRGBColor(User_R+12, User_G+25, User_B+15), ProtoRGBColor(User_R, User_G, User_B), ProtoRGBColor(User_R-5, User_G-10, User_B-5)};
    ProtoRGBColor rainbowSpectrum[6] = {ProtoRGBColor(255, 0, 0), ProtoRGBColor(255, 255, 0), ProtoRGBColor(0, 255, 0), ProtoRGBColor(0, 255, 255), ProtoRGBColor(0, 0, 255), ProtoRGBColor(255, 0, 255)};
    GradientMaterial<3> gradientMat = GradientMaterial<3>(gradientSpectrum, 200.0f, false);

    ProtoRGBColor backgroundSpectrum[1] = {ProtoRGBColor(0, 0, 0)};
    GradientMaterial<1> backgroundMat = GradientMaterial<1>(backgroundSpectrum, 350.0f, false);

    MaterialAnimator<9> materialAnimator;
    MaterialAnimator<2> backgroundMaterial;

    SpectrumAnalyzer sA = SpectrumAnalyzer(Vector2D(200, 100), Vector2D(100, 50), true, true);

    // Animation controllers
    BlinkTrack<2> blink;

    FunctionGenerator fGenMatXMove = FunctionGenerator(FunctionGenerator::Sine, -2.0f, 2.0f, 5.3f);
    FunctionGenerator fGenMatYMove = FunctionGenerator(FunctionGenerator::Sine, -2.0f, 2.0f, 6.7f);

    FunctionGenerator fGenBlur = FunctionGenerator(FunctionGenerator::Sine, 0.0f, 1.0f, 1.5f);

    Menu espmenu = Menu();
    // APDS9960_Sensor boop;

    FFTVoiceDetection<128> voiceDetection;

    HorizontalBlur blurH = HorizontalBlur(20);
    VerticalBlur blurV = VerticalBlur(20);
    RadialBlur blurR = RadialBlur(10);
    AntiAliasingEffect edgeFeather = AntiAliasingEffect(0.5f);


    float offsetFace = 0.0f;
    float offsetFaceSA = 0.0f;
    uint8_t offsetFaceInd = 50;
    uint8_t offsetFaceIndSA = 51;

    bool enableBlink = true;

    float dummyMorphWeight = 0.0f;

    Object3D *GetFaceObject()
    {
        return jsonFaceLoaded && jsonFace.Loaded() ? jsonFace.GetObject() : nullptr;
    }

    Transform *GetFaceTransform()
    {
        return jsonFaceLoaded && jsonFace.Loaded() ? jsonFace.GetTransform() : nullptr;
    }

    uint16_t GetMorphId(const char *name)
    {
        int idx = jsonFace.FindMorphIndexByName(name);
        return idx >= 0 ? static_cast<uint16_t>(idx) : 0xFFFF;
    }

    float *GetFaceMorphWeightReference(const char *name)
    {
        if (jsonFaceLoaded && jsonFace.Loaded())
        {
            float *weight = jsonFace.GetMorphWeightReferenceByName(name);
            if (weight != nullptr)
            {
                return weight;
            }
        }

        return &dummyMorphWeight;
    }

    void SetFaceMorphWeight(const char *name, float weight)
    {
        if (jsonFaceLoaded && jsonFace.Loaded())
        {
            jsonFace.SetMorphWeightByName(name, weight);
        }
    }

    void ResetFace()
    {
        if (jsonFaceLoaded && jsonFace.Loaded())
        {
            jsonFace.Reset();
        }
    }

    void UpdateFace()
    {
        if (jsonFaceLoaded && jsonFace.Loaded())
        {
            jsonFace.Update();
        }
    }

    void LoadJsonFaceBlocking()
    {
        // Block until LittleFS mounts and the face json is successfully loaded, with timeout protection.
        const uint32_t timeoutMs = 10000; // 10 second timeout to prevent infinite blocking
        const uint32_t startMs = millis();
        uint32_t retryCount = 0;
        
        while (USE_JSON_FACE_MODEL && !jsonFaceLoaded)
        {
            // Safety timeout to prevent infinite freeze
            if ((millis() - startMs) > timeoutMs)
            {
                Serial.printf("[ERROR] Face model loading timeout after %lu ms\n", timeoutMs);
                return;
            }
            
            if (!LittleFS.begin(false) && !LittleFS.begin(true))
            {
                Serial.println("[WARN] LittleFS mount failed; retrying...");
                delay(500);
                yield(); // Feed watchdog during retry
                continue;
            }

            if (!LittleFS.exists("/universal_face.json"))
            {
                Serial.printf("[WARN] universal_face.json not found; waiting for file... (attempt %lu)\n", ++retryCount);
                delay(500);
                yield(); // Feed watchdog during retry
                continue;
            }

            jsonFaceLoaded = jsonFace.Load(LittleFS, "/universal_face.json");
            if (!jsonFaceLoaded)
            {
                Serial.printf("[WARN] universal_face.json failed to load (attempt %lu); retrying...\n", ++retryCount);
                delay(500);
                yield(); // Feed watchdog during retry
            }
        }
    }

    bool RoundEye = false;
    bool SwappedEye = false;
    bool ShowMouth = true;
    bool voiceEnable = true;
    bool EyeShapeB = false;
    uint8_t eyeType = 0; // 0: Default, 1: Round, 2: Swapped, 3: SquareHollow, 4: RoundHollow

    void LinkEasyEase()
    {
        eEA.AddParameter(GetFaceMorphWeightReference("Anger"), GetMorphId("Anger"), 15, 0.0f, 1.0f);
        eEA.AddParameter(GetFaceMorphWeightReference("Sadness"), GetMorphId("Sadness"), 50, 0.0f, 1.0f);
        eEA.AddParameter(GetFaceMorphWeightReference("Surprised"), GetMorphId("Surprised"), 10, 0.0f, 1.0f);
        eEA.AddParameter(GetFaceMorphWeightReference("Doubt"), GetMorphId("Doubt"), 25, 0.0f, 1.0f);
        eEA.AddParameter(GetFaceMorphWeightReference("Frown"), GetMorphId("Frown"), 45, 0.0f, 1.0f);
        eEA.AddParameter(GetFaceMorphWeightReference("LookUp"), GetMorphId("LookUp"), 30, 0.0f, 1.0f);
        eEA.AddParameter(GetFaceMorphWeightReference("LookDown"), GetMorphId("LookDown"), 30, 0.0f, 1.0f);
        eEA.AddParameter(GetFaceMorphWeightReference("Heart"), GetMorphId("Heart"), 12, 0.0f, 1.0f);
        eEA.AddParameter(GetFaceMorphWeightReference("Happy"), GetMorphId("Happy"), 12, 0.0f, 1.0f);
        eEA.AddParameter(GetFaceMorphWeightReference("XwX"), GetMorphId("XwX"), 12, 0.0f, 1.0f);
        eEA.AddParameter(GetFaceMorphWeightReference("Charge"), GetMorphId("Charge"), 12, 0.0f, 1.0f);
        eEA.AddParameter(GetFaceMorphWeightReference("ChargePercent"), GetMorphId("ChargePercent"), 12, 0.0f, 1.0f);
        eEA.AddParameter(GetFaceMorphWeightReference("RoundEye"), GetMorphId("RoundEye"), 12, 0.0f, 1.0f);
        eEA.AddParameter(GetFaceMorphWeightReference("RoundSurprised"), GetMorphId("RoundSurprised"), 12, 0.0f, 1.0f);
        eEA.AddParameter(GetFaceMorphWeightReference("RoundDoubt"), GetMorphId("RoundDoubt"), 12, 0.0f, 1.0f);
        eEA.AddParameter(GetFaceMorphWeightReference("RoundSadness"), GetMorphId("RoundSadness"), 12, 0.0f, 1.0f);
        eEA.AddParameter(GetFaceMorphWeightReference("RoundAnger"), GetMorphId("RoundAnger"), 12, 0.0f, 1.0f);
        eEA.AddParameter(GetFaceMorphWeightReference("BlushX"), GetMorphId("BlushX"), 12, 0.0f, 1.0f);
        eEA.AddParameter(GetFaceMorphWeightReference("BlushY"), GetMorphId("BlushY"), 12, 0.0f, 1.0f);
        eEA.AddParameter(GetFaceMorphWeightReference("Question"), GetMorphId("Question"), 12, 0.0f, 1.0f);
        //eEA.AddParameter(GetFaceMorphWeightReference("Scale"), GetMorphId("Scale"), 12, 0.0f, 2.0f);
        //eEA.AddParameter(GetFaceMorphWeightReference("Rotate"), GetMorphId("Rotate"), 12, 0.0f, 1.0f);
        eEA.AddParameter(GetFaceMorphWeightReference("HideEye"), GetMorphId("HideEye"), 6, 0.0f, 1.0f);
        eEA.AddParameter(GetFaceMorphWeightReference("CustomSEyeShape1"), GetMorphId("CustomSEyeShape1"), 12, 0.0f, 1.0f);
        eEA.AddParameter(GetFaceMorphWeightReference("SEyeX"), GetMorphId("SEyeX"), 12, 0.0f, 1.0f);
        eEA.AddParameter(GetFaceMorphWeightReference("SEyeY"), GetMorphId("SEyeY"), 12, 0.0f, 2.0f);
        eEA.AddParameter(GetFaceMorphWeightReference("ALLHIDE"), GetMorphId("ALLHIDE"), 3, 0.0f, 1.0f);
        eEA.AddParameter(GetFaceMorphWeightReference("CloseEyes2"), GetMorphId("CloseEyes2"), 12, 0.0f, 2.0f);
        eEA.AddParameter(GetFaceMorphWeightReference("Shy"), GetMorphId("Shy"), 12, 0.0f, 2.0f);
        eEA.AddParameter(GetFaceMorphWeightReference("vrc_v_ee"), GetMorphId("vrc_v_ee"), 2, 0.0f, 1.0f);
        eEA.AddParameter(GetFaceMorphWeightReference("vrc_v_ih"), GetMorphId("vrc_v_ih"), 2, 0.0f, 1.0f);
        eEA.AddParameter(GetFaceMorphWeightReference("vrc_v_oh"), GetMorphId("vrc_v_oh"), 2, 0.0f, 1.0f);
        eEA.AddParameter(GetFaceMorphWeightReference("vrc_v_aa"), GetMorphId("vrc_v_aa"), 2, 0.0f, 1.0f);
        eEA.AddParameter(GetFaceMorphWeightReference("vrc_v_uh"), GetMorphId("vrc_v_uh"), 2, 0.0f, 1.0f);
        eEA.AddParameter(GetFaceMorphWeightReference("vrc_v_aa"), GetMorphId("vrc_v_aa"), 2, 0.0f, 1.0f);
        eEA.AddParameter(GetFaceMorphWeightReference("vrc_v_oh"), GetMorphId("vrc_v_oh"), 2, 0.0f, 1.0f);
        eEA.AddParameter(GetFaceMorphWeightReference("vrc_v_ss"), GetMorphId("vrc_v_ss"), 2, 0.0f, 1.0f);
        eEA.AddParameter(GetFaceMorphWeightReference("HideBlush"), GetMorphId("HideBlush"), 30, 1.0f, 0.0f);
        eEA.AddParameter(GetFaceMorphWeightReference("HideBlush"), GetMorphId("HideBlush"), 30, 1.0f, 0.0f);
        eEA.AddParameter(GetFaceMorphWeightReference("HideSecondEye"), GetMorphId("HideSecondEye"), 30, 1.0f, 0.0f);
        eEA.AddParameter(GetFaceMorphWeightReference("HideSecondEye"), GetMorphId("HideSecondEye"), 30, 1.0f, 0.0f);
        eEA.AddParameter(GetFaceMorphWeightReference("Zzz"), GetMorphId("Zzz"), 12, 1.0f, 0.0f);
        eEA.AddParameter(GetFaceMorphWeightReference("Swapped"), GetMorphId("Swapped"), 4, 0.0f, 1.0f);
        eEA.AddParameter(GetFaceMorphWeightReference("MouthS"), GetMorphId("MouthS"), 4, 0.55f, 1.0f);
        eEA.AddParameter(GetFaceMorphWeightReference("EyeNX"), GetMorphId("EyeNX"), 4, 0.25f, 0.0f);
        eEA.AddParameter(GetFaceMorphWeightReference("MouthX"), GetMorphId("MouthX"), 4, 0.25f, 0.0f);
        eEA.AddParameter(GetFaceMorphWeightReference("MouthEnd"), GetMorphId("MouthEnd"), 10, 0.8f, 0.0f);
        eEA.AddParameter(GetFaceMorphWeightReference("UwU"), GetMorphId("UwU"), 3, 0.0f, 1.0f);
        eEA.AddParameter(GetFaceMorphWeightReference("EyeShape2"), GetMorphId("EyeShape2"), 5, 0.0f, 1.0f);
        eEA.AddParameter(GetFaceMorphWeightReference("EyeShape3"), GetMorphId("EyeShape3"), 5, 0.0f, 1.0f);
        eEA.AddParameter(GetFaceMorphWeightReference("EyeNY"), GetMorphId("EyeNY"), 5, 0.0f, 1.0f);

        eEA.AddParameter(GetFaceMorphWeightReference("MouthY"), GetMorphId("MouthY"), 5, 0.0f, 1.0f);
        eEA.AddParameter(GetFaceMorphWeightReference("ZzzMove"), GetMorphId("ZzzMove"), 5, 0.0f, 1.0f);
        eEA.AddParameter(GetFaceMorphWeightReference("HideMouth"), GetMorphId("HideMouth"), 5, 0.0f, 1.0f);
        eEA.AddParameter(GetFaceMorphWeightReference("Flat"), GetMorphId("Flat"), 5, 0.0f, 1.0f);
        eEA.AddParameter(GetFaceMorphWeightReference("EyeShape6"), GetMorphId("EyeShape6"), 5, 0.0f, 1.0f);
        eEA.AddParameter(GetFaceMorphWeightReference("EyeShape6Suprised"), GetMorphId("EyeShape6Suprised"), 5, 0.0f, 1.0f);
        eEA.AddParameter(GetFaceMorphWeightReference("MouthShape2"), GetMorphId("MouthShape2"), 5, 0.0f, 1.0f);
        //eEA.AddParameter(GetFaceMorphWeightReference("Error"), GetMorphId("Error"), 5, 0.0f, 1.0f);
        eEA.AddParameter(GetFaceMorphWeightReference("ExpressionArrow"), GetMorphId("ExpressionArrow"), 5, 0.0f, 1.0f);
        eEA.AddParameter(GetFaceMorphWeightReference("HollowSquareEye"), GetMorphId("HollowSquareEye"), 5, 0.0f, 1.0f);
        eEA.AddParameter(GetFaceMorphWeightReference("HollowRoundEye"), GetMorphId("HollowRoundEye"), 5, 0.0f, 1.0f);

        eEA.AddParameter(GetFaceMorphWeightReference("Exclamation"), GetMorphId("Exclamation"), 8, 0.0f, 1.0f);
        eEA.AddParameter(GetFaceMorphWeightReference("Tones"), GetMorphId("Tones"), 8, 0.0f, 1.0f);

        eEA.AddParameter(&offsetFace, offsetFaceInd, 40, 0.0f, 1.0f);
        eEA.AddParameter(&offsetFaceSA, offsetFaceIndSA, 40, 0.0f, 1.0f);
    }

    void LinkParameters()
    {
        blink.AddParameter(GetFaceMorphWeightReference("Blink"));
        blink.AddParameter(GetFaceMorphWeightReference("SEyeBlink"));
    }

    void ChangeInterpolationMethods()
    {
        eEA.SetInterpolationMethod(GetMorphId("HideBlush"), EasyEaseInterpolation::Cosine);
        eEA.SetInterpolationMethod(GetMorphId("Sadness"), EasyEaseInterpolation::Cosine);

        eEA.SetInterpolationMethod(GetMorphId("vrc_v_ee"), EasyEaseInterpolation::Linear);
        eEA.SetInterpolationMethod(GetMorphId("vrc_v_ih"), EasyEaseInterpolation::Linear);
        eEA.SetInterpolationMethod(GetMorphId("vrc_v_dd"), EasyEaseInterpolation::Linear);
        eEA.SetInterpolationMethod(GetMorphId("vrc_v_rr"), EasyEaseInterpolation::Linear);
        eEA.SetInterpolationMethod(GetMorphId("vrc_v_ch"), EasyEaseInterpolation::Linear);
        eEA.SetInterpolationMethod(GetMorphId("vrc_v_aa"), EasyEaseInterpolation::Linear);
        eEA.SetInterpolationMethod(GetMorphId("vrc_v_oh"), EasyEaseInterpolation::Linear);
        eEA.SetInterpolationMethod(GetMorphId("vrc_v_ss"), EasyEaseInterpolation::Linear);
    }

    void SetMaterialLayers()
    {

        materialAnimator.SetBaseMaterial(Material::Add, &gradientMat);
        materialAnimator.AddMaterial(Material::Replace, &orangeMaterial, 40, 0.0f, 1.0f); // layer 1
        materialAnimator.AddMaterial(Material::Replace, &whiteMaterial, 40, 0.0f, 1.0f);  // layer 2
        materialAnimator.AddMaterial(Material::Replace, &greenMaterial, 40, 0.0f, 1.0f);  // layer 3
        materialAnimator.AddMaterial(Material::Replace, &yellowMaterial, 40, 0.0f, 1.0f); // layer 4
        materialAnimator.AddMaterial(Material::Replace, &purpleMaterial, 40, 0.0f, 1.0f); // layer 5
        materialAnimator.AddMaterial(Material::Replace, &redMaterial, 40, 0.0f, 1.0f);    // layer 6
        materialAnimator.AddMaterial(Material::Replace, &blueMaterial, 40, 0.0f, 1.0f);   // layer 7
        materialAnimator.AddMaterial(Material::Replace, &rainbowSpiral, 40, 0.0f, 1.0f);  // layer 8
        materialAnimator.AddMaterial(Material::Replace, &rainbowNoise, 40, 0.15f, 1.0f);  // layer 9
    }

    void UpdateKeyFrameTracks()
    {
        if (enableBlink)
            blink.Update();
    }

    void DefaultSetUp()
    {
        eEA.SetInterpolationMethod(3,EasyEaseInterpolation::Overshoot);
        if (ShowMouth)
            eEA.AddParameterFrame(GetMorphId("HideMouth"), 0.0f);
        else
            eEA.AddParameterFrame(GetMorphId("HideMouth"), 1.0f);
        background.GetObject()->SetMaterial(&backgroundMat);
        scene.DisableEffect();
        scene.SetEffect(&edgeFeather);
        scene.EnableEffect();
        voiceEnable = true;
        Object3D *faceObject = GetFaceObject();
        if (faceObject)
        {
            faceObject->SetMaterial(&gradientMat);
        }
        enableBlink = true;
        eEA.AddParameterFrame(GetMorphId("HideEye"), 0.0f);
        ShowMouth = true;

        eEA.AddParameterFrame(GetMorphId("HideSecondEye"), 1.0f);
        if (RoundEye)
        {
            eEA.AddParameterFrame(GetMorphId("RoundEye"), 1.0f);
        }
        else if (SwappedEye)
        {
            eEA.AddParameterFrame(GetMorphId("HideSecondEye"), 0.0f);
            eEA.AddParameterFrame(GetMorphId("Swapped"), 1.0f);
        }else if (EyeShapeB){
            eEA.AddParameterFrame(GetMorphId("EyeShape2"), 1.0f);
        }
    }

    void Default()
    {
        ShowMouth = true;
        DefaultSetUp();
        // eEA.AddParameterFrame(NukudeFace::Anger, 1.0f);
        // eEA.AddParameterFrame(NukudeFace::DiamondEye, 0.0f);
        //eEA.AddParameterFrame(NukudeFace::EyeShape2, 0.45f);
        //eEA.AddParameterFrame(NukudeFace::EyeShape3, 0.3f);
        //eEA.AddParameterFrame(NukudeFace::EyeShape6, 0.38f);
        eEA.AddParameterFrame(GetMorphId("EyeNY"), 0.3f);
        //scene.DisableEffect();
    }

    void DefaultHideMouth()
    {
        ShowMouth = false;
        DefaultSetUp();
        // eEA.AddParameterFrame(NukudeFace::Anger, 1.0f);
        // eEA.AddParameterFrame(NukudeFace::DiamondEye, 0.0f);
        //eEA.AddParameterFrame(NukudeFace::EyeShape2, 0.45f);
        //eEA.AddParameterFrame(NukudeFace::EyeShape3, 0.3f);
        //eEA.AddParameterFrame(NukudeFace::EyeShape6, 0.38f);
        eEA.AddParameterFrame(GetMorphId("EyeNY"), 0.35f);
        //scene.DisableEffect();
    }

    void Heart()
    {
        DefaultSetUp();
        enableBlink = false;
        if (RoundEye)
            eEA.AddParameterFrame(GetMorphId("RoundEye"), 0.0f);
        else if (SwappedEye)
            eEA.AddParameterFrame(GetMorphId("Swapped"), 0.0f);
        eEA.AddParameterFrame(GetMorphId("EyeShape2"), 0.0f);
        eEA.AddParameterFrame(GetMorphId("Heart"), 1.0f);
    }

    void Nonword()
    {
        DefaultSetUp();
        enableBlink = false;
        if (RoundEye)
            eEA.AddParameterFrame(GetMorphId("RoundEye"), 1.0f);
        else if (SwappedEye)
            eEA.AddParameterFrame(GetMorphId("Swapped"), 0.0f);
        eEA.AddParameterFrame(GetMorphId("EyeShape2"), 0.0f);
        eEA.AddParameterFrame(GetMorphId("RoundEye"), 1.0f);
        eEA.AddParameterFrame(GetMorphId("Flat"), 1.0f);
    }

    void UwU()
    {
        DefaultSetUp();
        enableBlink = false;
        if (RoundEye)
            eEA.AddParameterFrame(GetMorphId("RoundEye"), 0.0f);
        else if (SwappedEye)
            eEA.AddParameterFrame(GetMorphId("Swapped"), 0.0f);
        eEA.AddParameterFrame(GetMorphId("EyeShape2"), 0.0f);
        eEA.AddParameterFrame(GetMorphId("EyeNX"), 0.0f);
        eEA.AddParameterFrame(GetMorphId("UwU"), 1.0f);
    }

    void XwX()
    {
        DefaultSetUp();
        eEA.AddParameterFrame(GetMorphId("HideSecondEye"), 1.0f);
        eEA.AddParameterFrame(GetMorphId("HideEye"), 1.0f);
            Object3D *faceObject = GetFaceObject();
            if (faceObject)
            {
                faceObject->SetMaterial(&gradientMat);
            }
    }

    void Happy()
    {
        DefaultSetUp();
        enableBlink = false;
        if (RoundEye)
            eEA.AddParameterFrame(GetMorphId("RoundEye"), 0.0f);
        else if (SwappedEye)
            eEA.AddParameterFrame(GetMorphId("Swapped"), 0.0f);
        eEA.AddParameterFrame(GetMorphId("EyeShape2"), 0.0f);
        eEA.AddParameterFrame(GetMorphId("Happy"), 1.0f);
    }

    void Angry()
    {
        DefaultSetUp();
        enableBlink = false;
        Object3D *faceObject = GetFaceObject();
        if (faceObject)
        {
            faceObject->SetMaterial(&redMaterial);
        }
        if (RoundEye)
            eEA.AddParameterFrame(GetMorphId("RoundAnger"), 1.0f);
        else if (SwappedEye){
            eEA.AddParameterFrame(GetMorphId("Swapped"), 0.0f);
            eEA.AddParameterFrame(GetMorphId("Anger"), 1.0f);
        }else{
            eEA.AddParameterFrame(GetMorphId("Anger"), 1.0f);
        }
        eEA.AddParameterFrame(GetMorphId("EyeShape2"), 0.0f);
        
    }

    void Sad()
    {
        DefaultSetUp();
        Object3D *faceObject = GetFaceObject();
        if (faceObject)
        {
            faceObject->SetMaterial(&blueMaterial);
        }
        if (RoundEye)
            eEA.AddParameterFrame(GetMorphId("RoundSadness"), 1.0f);
        else if (SwappedEye){
            eEA.AddParameterFrame(GetMorphId("Swapped"), 0.0f);
            eEA.AddParameterFrame(GetMorphId("Sadness"), 1.0f);
        }else{
            eEA.AddParameterFrame(GetMorphId("Sadness"), 1.0f);
        }
        eEA.AddParameterFrame(GetMorphId("EyeShape2"), 0.0f);
        eEA.AddParameterFrame(GetMorphId("Frown"), 1.0f);
    }

    void Surprised()
    {
        background.GetObject()->SetMaterial(&backgroundMat);
        DefaultSetUp();
        enableBlink = false;
        scene.SetEffect(&blurV);
        scene.EnableEffect();
        if (RoundEye)
            eEA.AddParameterFrame(GetMorphId("RoundSurprised"), 1.0f);
        else if (SwappedEye){
            eEA.AddParameterFrame(GetMorphId("Swapped"), 0.0f);
            eEA.AddParameterFrame(GetMorphId("Surprised"), 1.0f);
        }else{
            eEA.AddParameterFrame(GetMorphId("Surprised"), 1.0f);
        }
        eEA.AddParameterFrame(GetMorphId("EyeShape2"), 0.0f);
        eEA.AddParameterFrame(GetMorphId("HideSecondEye"), 1.0f);
        
        eEA.AddParameterFrame(GetMorphId("HideBlush"), 0.0f);
        Object3D *faceObject = GetFaceObject();
        if (faceObject)
        {
            faceObject->SetMaterial(&rainbowSpiral);
        }
    }

    void Doubt()
    {
        background.GetObject()->SetMaterial(&backgroundMat);
        scene.DisableEffect();
        DefaultSetUp();
        enableBlink = false;
        if (RoundEye)
            eEA.AddParameterFrame(GetMorphId("RoundDoubt"), 1.0f);
        else if (SwappedEye){
            eEA.AddParameterFrame(GetMorphId("Swapped"), 0.0f);
            eEA.AddParameterFrame(GetMorphId("Doubt"), 1.0f);
        }
        eEA.AddParameterFrame(GetMorphId("EyeShape2"), 0.0f);
        
    }

    void Frown()
    {
        background.GetObject()->SetMaterial(&backgroundMat);
        scene.DisableEffect();
        eEA.AddParameterFrame(GetMorphId("HideSecondEye"), 1.0f);
        eEA.AddParameterFrame(GetMorphId("Frown"), 1.0f);
    }

    void LookUp()
    {
        background.GetObject()->SetMaterial(&backgroundMat);
        scene.DisableEffect();
        eEA.AddParameterFrame(GetMorphId("LookUp"), 1.0f);
    }

    void LookDown()
    {
        background.GetObject()->SetMaterial(&backgroundMat);
        scene.DisableEffect();
        eEA.AddParameterFrame(GetMorphId("LookDown"), 1.0f);
    }

    void Zzz()
    {
        DefaultSetUp();
        enableBlink = false;
        scene.DisableEffect();
        eEA.AddParameterFrame(GetMorphId("HideEye"), 1.0f);
        eEA.AddParameterFrame(GetMorphId("HideSecondEye"), 1.0f);
        eEA.AddParameterFrame(GetMorphId("Zzz"), 0.0f);
    }

    void Shy()
    {
        DefaultSetUp();
        enableBlink = false;
        eEA.AddParameterFrame(GetMorphId("RoundEye"), 0.0f);
        eEA.AddParameterFrame(GetMorphId("Swapped"), 0.0f);
        eEA.AddParameterFrame(GetMorphId("EyeShape2"), 0.0f);
        eEA.AddParameterFrame(GetMorphId("HideSecondEye"), 1.0f);
        eEA.AddParameterFrame(GetMorphId("Shy"), 1.0f);
    }

    void Question()
    {
        DefaultSetUp();
        //GetFaceObject()->SetMaterial(&whiteMaterial);
        enableBlink = false;
        eEA.AddParameterFrame(GetMorphId("ALLHIDE"), 1.0f);
        eEA.AddParameterFrame(GetMorphId("HideSecondEye"), 0.0f);
        eEA.AddParameterFrame(GetMorphId("HideBlush"), 0.0f);
        eEA.AddParameterFrame(GetMorphId("Question"), 1.0f);
    }

    void SquareEye(){
        DefaultSetUp();
        enableBlink = true;
        eEA.AddParameterFrame(GetMorphId("HideEye"), 1.0f);
        eEA.AddParameterFrame(GetMorphId("HollowSquareEye"), 0.9f);
    }

    void RoundHollow(){
        DefaultSetUp();
        eEA.SetInterpolationMethod(0,EasyEaseInterpolation::Overshoot);
        enableBlink = true;
        eEA.AddParameterFrame(GetMorphId("HideEye"), 1.0f);
        eEA.AddParameterFrame(GetMorphId("HollowSquareEye"), 0.9f);
        eEA.AddParameterFrame(GetMorphId("HollowRoundEye"), 1.0f);
    }

    void SpectrumAnalyzerFace()
    {
        scene.DisableEffect();
        eEA.AddParameterFrame(GetMorphId("ALLHIDE"), 1.0f);
        //eEA.AddParameterFrame(offsetFaceInd, 1.0f);
        //eEA.AddParameterFrame(offsetFaceIndSA, 1.0f);
        background.GetObject()->SetMaterial(&sA);
    }

    void Tones()
    {
        ShowMouth = true;
        DefaultSetUp();
        eEA.AddParameterFrame(GetMorphId("HideEye"), 1.0f);
        eEA.AddParameterFrame(GetMorphId("Tones"), 1.0f);
        // eEA.AddParameterFrame(NukudeFace::Anger, 1.0f);
        // eEA.AddParameterFrame(NukudeFace::DiamondEye, 0.0f);
        //eEA.AddParameterFrame(NukudeFace::EyeNY, 0.5);
        scene.DisableEffect();
    }

    void Exclamation()
    {
        ShowMouth = true;
        DefaultSetUp();
        eEA.AddParameterFrame(GetMorphId("HideEye"), 1.0f);
        eEA.AddParameterFrame(GetMorphId("Exclamation"), 1.0f);
        // eEA.AddParameterFrame(NukudeFace::Anger, 1.0f);
        // eEA.AddParameterFrame(NukudeFace::DiamondEye, 0.0f);
        //eEA.AddParameterFrame(NukudeFace::EyeNY, 0.5);
        scene.DisableEffect();
    }

    void ZzzMoved()
    {
        DefaultSetUp();
        enableBlink = false;
        if (RoundEye)
            eEA.AddParameterFrame(GetMorphId("RoundEye"), 0.0f);
        else if (SwappedEye)
            eEA.AddParameterFrame(GetMorphId("Swapped"), 0.0f);
        //pM.GetObject()->SetMaterial(&whiteMaterial);
        eEA.AddParameterFrame(GetMorphId("EyeShape2"), 0.0f);
        eEA.AddParameterFrame(GetMorphId("ALLHIDE"), 1.0f);
        eEA.AddParameterFrame(GetMorphId("ZzzMove"), 1.0f);
        eEA.AddParameterFrame(GetMorphId("Zzz"), 0.0f);
    }

    void ArrowEyes()
    {
        DefaultSetUp();
        enableBlink = false;
        if (RoundEye)
            eEA.AddParameterFrame(GetMorphId("RoundEye"), 0.0f);
        else if (SwappedEye)
            eEA.AddParameterFrame(GetMorphId("Swapped"), 0.0f);
        //pM.GetObject()->SetMaterial(&whiteMaterial);
        eEA.AddParameterFrame(GetMorphId("EyeShape2"), 0.0f);
        eEA.AddParameterFrame(GetMorphId("Flat"), 1.0f);
        eEA.AddParameterFrame(GetMorphId("ExpressionArrow"), 1.0f);
    }

    void UpdateFFTVisemes()
    {
        if (Menu::GetvoiceDetectionEnable() && voiceEnable)
        {
            eEA.AddParameterFrame(GetMorphId("vrc_v_ss"), MicrophoneFourierIT::GetCurrentMagnitude() / 2.0f);

            if (MicrophoneFourierIT::GetCurrentMagnitude() > 0.05f)
            {
                voiceDetection.Update(MicrophoneFourierIT::GetFourierFiltered(), MicrophoneFourierIT::GetSampleRate());

                eEA.AddParameterFrame(GetMorphId("vrc_v_ee"), voiceDetection.GetViseme(voiceDetection.EE));
                eEA.AddParameterFrame(GetMorphId("vrc_v_ih"), voiceDetection.GetViseme(voiceDetection.AH));
                eEA.AddParameterFrame(GetMorphId("vrc_v_dd"), voiceDetection.GetViseme(voiceDetection.UH));
                eEA.AddParameterFrame(GetMorphId("vrc_v_rr"), voiceDetection.GetViseme(voiceDetection.AR));
                eEA.AddParameterFrame(GetMorphId("vrc_v_ch"), voiceDetection.GetViseme(voiceDetection.ER));
                eEA.AddParameterFrame(GetMorphId("vrc_v_aa"), voiceDetection.GetViseme(voiceDetection.AH));
                eEA.AddParameterFrame(GetMorphId("vrc_v_oh"), voiceDetection.GetViseme(voiceDetection.OO));
            }
        }
    }

    void SetMaterialColor()
    {
        switch (Menu::GetFaceColor())
        {
        case 1:
            materialAnimator.AddMaterialFrame(redMaterial, 0.8f);
            break;
        case 2:
            materialAnimator.AddMaterialFrame(orangeMaterial, 0.8f);
            break;
        case 3:
            materialAnimator.AddMaterialFrame(whiteMaterial, 0.8f);
            break;
        case 4:
            materialAnimator.AddMaterialFrame(greenMaterial, 0.8f);
            break;
        case 5:
            materialAnimator.AddMaterialFrame(blueMaterial, 0.8f);
            break;
        case 6:
            materialAnimator.AddMaterialFrame(yellowMaterial, 0.8f);
            break;
        case 7:
            materialAnimator.AddMaterialFrame(purpleMaterial, 0.8f);
            break;
        case 8:
            materialAnimator.AddMaterialFrame(rainbowSpiral, 0.8f);
            break;
        case 9:
            materialAnimator.AddMaterialFrame(rainbowNoise, 0.8f);
            break;
        default:
            break;
        }
    }

public:
    TasSimpleProtogenHUB75Animation() {}

    void Initialize()
    {
        // Refresh gradient colors from user-configurable globals.
        gradientSpectrum[0] = ProtoRGBColor(User_R + 12, User_G + 25, User_B + 15);
        gradientSpectrum[1] = ProtoRGBColor(User_R, User_G, User_B);
        gradientSpectrum[2] = ProtoRGBColor(User_R - 5, User_G - 10, User_B - 5);
        gradientMat = GradientMaterial<3>(gradientSpectrum, 200.0f, false);

        #ifdef VERBOSE_STARTUP
        display.println("初始化动画...");
        #else
        display.progressBar(14,50,100,8,5);
        #endif
        delay(100);
        Serial.begin(115200);
#if USE_JSON_FACE_MODEL
        LoadJsonFaceBlocking();
#endif

        Object3D *faceObject = GetFaceObject();
        if (!faceObject)
        {
            Serial.println("[WARN] Face object not available; skipping scene attach.");
        }

        if (faceObject)
        {
            scene.AddObject(faceObject);
        }
        scene.AddObject(background.GetObject());

        LinkEasyEase();
        LinkParameters();
        ChangeInterpolationMethods();

        SetMaterialLayers();

        if (faceObject)
        {
            faceObject->SetMaterial(&gradientMat);
        }
        background.GetObject()->SetMaterial(&backgroundMat);
        #ifdef VERBOSE_STARTUP
        display.println("初始化动画完成");
        #else
        display.progressBar(14,50,100,8,10);
        #endif

        #ifdef VERBOSE_STARTUP
        display.println("初始化麦克风...");
        #else
        display.progressBar(14,50,100,8,12);
        #endif

        #ifdef TASESP32S3
        MicrophoneFourierIT::Initialize(1, 8000, 68.0f, 120.0f); // 8KHz sample rate, 50dB min, 120dB max
        #elif defined(TASESP32P4)
        MicrophoneFourierIT::Initialize(23, 8000, 68.0f, 120.0f); // 8KHz sample rate, 50dB min, 120dB max
        #endif
        #ifdef VERBOSE_STARTUP
        display.println("初始化麦克风完成");
        #else
        display.progressBar(14,50,100,8,15);
        #endif
        // Menu::Initialize(9);//NeoTrellis
        espmenu.Initialize(17, 200); // 7 is number of faces
        Serial.print("Animation Init Successfully!");
    }

    uint8_t GetAccentBrightness()
    {
        return Menu::GetAccentBrightness();
    };

    uint8_t GetBrightness()
    {
        return Menu::GetBrightness();
    };

    void FadeIn(float stepRatio) override {}
    void FadeOut(float stepRatio) override {}

    Object3D *GetObject()
    {
        return GetFaceObject();
    }

    void MenuUpdate()
    {
        espmenu.Update();
    }

    int GetDisplayMode()
    {
        MenuUpdate();
        return espmenu.displayMode();
    }

    void Update(float ratio) override
    {
        ResetFace();

        float xOffset = fGenMatXMove.Update();
        float yOffset = fGenMatYMove.Update();

        MenuUpdate();

        blurH.SetRatio(fGenBlur.Update());
        blurV.SetRatio(fGenBlur.Update());
        blurR.SetRatio(fGenBlur.Update());

        SetMaterialColor();

        bool isBooped = Menu::UseBoopSensor() ? Menu::isBooped() : 0;
        // bool ShowMouth = Menu::GetMouthState();
        uint8_t mode = Menu::GetFaceState(); // change by button press
        gradientMat.HueShift(Menu::GetHueShift());
        rainbowSpectrum->HueShift(Menu::GetHueShift());

        MicrophoneFourierIT::Update();

        voiceDetection.SetThreshold(map(Menu::GetMicLevel(), 0, 10, 1000, 50));

        UpdateFFTVisemes();

        if (isBooped)
        {
            Surprised();
        }
        else
        {
            if (mode == 0)
                Default(); // Green
            else if (mode == 1)
                RoundHollow();
            else if (mode == 2)
                Angry(); // Red
            else if (mode == 3)
                Doubt(); // Orange
            else if (mode == 4)
                SquareEye(); // Cyan
            else if (mode == 5)
                Heart(); // Purple
            else if (mode == 6)
                Sad(); // Blue
            else if (mode == 7)
                Surprised(); // Blue
            else if (mode == 8)
                Happy();
            else if (mode == 9)
                Zzz();
            else if (mode == 10)
                Shy();
            else if (mode == 11)
                Question();
            else if (mode == 12)
                XwX();
            else if (mode == 13)
               ZzzMoved();
            else if (mode == 14)
                Nonword();
            else if (mode == 15)
                UwU();
            else if (mode == 16)
                ArrowEyes();
            else if (mode == 17)
                Exclamation();
            else if (mode == 18)
                Tones();

            else
            { // Yellow
                sA.Update(MicrophoneFourierIT::GetFourierFiltered());
                SpectrumAnalyzerFace();
                //enableBlink = false;
                //XwX();
                //background.GetObject()->SetMaterial(&backgroundMat);
                //eEA.AddParameterFrame(NukudeFace::ALLHIDE, 1.0f);
            }
        }
        eEA.SetInterpolationMethod(0,EasyEaseInterpolation::Overshoot);

        UpdateKeyFrameTracks();

        SetFaceMorphWeight("BiggerNose", 1.0f);
        SetFaceMorphWeight("MoveEye", 1.0f);

        sA.SetHueAngle(ratio * 360.0f * 4.0f);
        sA.SetMirrorYState(Menu::MirrorSpectrumAnalyzer());
        sA.SetFlipYState(!Menu::MirrorSpectrumAnalyzer());

        eEA.Update();
        UpdateFace();

        rainbowNoise.Update(ratio);
        rainbowSpiral.Update(ratio);
        materialAnimator.Update();
        // backgroundMaterial.Update();

        uint8_t faceSize = 0;
        const float scale = 17.5 * 0.5f + 0.45f;
        const float faceSizeOffset = 0 * 8.0f;

        //Serial.print("X: ");
        //Serial.println(xOffset);
        //Serial.println("Y: ");
        //Serial.println(yOffset);

        Transform *faceTransform = GetFaceTransform();
        if (!faceTransform)
        {
            return;
        }

        faceTransform->SetRotation(Vector3D(0.0f, 0.0f, -7.5f));

        float xShift = (1.0f - 1) * -10.0f;
        float yShift = (1.0f - 1) * 70.0f + offsetFaceSA * -150.0f;
        float adjustFacePos = float(4 - faceSize) * 5.0f;
        float adjustFaceX = float(faceSize) * 0.05f;

        faceTransform->SetPosition(Vector3D(88.0f + xOffset - xShift + adjustFacePos, -20.5f + yOffset + yShift, 550.0f));
        faceTransform->SetScale(Vector3D(-0.975f + adjustFaceX, 0.59f, 0.65f).Multiply(scale));

        GetFaceObject()->UpdateTransform();
    }
};
