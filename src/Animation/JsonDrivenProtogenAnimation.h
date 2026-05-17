#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <vector>
#include <utility>
#include <memory>
#if defined(ESP32)
#include <esp_heap_caps.h>
#endif

#include "Animation.h"
#include "EasyEaseAnimator.h"
#include "KeyFrameTrack.h"
#include "../Objects/Background.h"
#include "../Morph/JsonNukudeFace.h"
#include "../Render/Scene.h"
#include "../Render/ObjectAlign.h"
#include "../Signals/FunctionGenerator.h"
#include "../Materials/GradientMaterial.h"
#include "../Materials/SimpleMaterial.h"
#include "../Materials/Animated/RainbowNoise.h"
#include "../Materials/Animated/RainbowSpiral.h"
#include "../Materials/Animated/SpectrumAnalyzer.h"
#include "../Materials/MaterialAnimator.h"
#include "AnimationTracks/BlinkTrack.h"
#include "../Signals/FFTVoiceDetection.h"
#include "../Sensors/MicrophoneFourier_MAX9814.h"
#include "../Screenspace/HorizontalBlur.h"
#include "../Screenspace/RadialBlur.h"
#include "../Screenspace/VerticalBlur.h"
#include "../Screenspace/AntiAliasingEffect.h"
#include "../Menu/ESPMenu.h"
#include "../Network/UserConfigManager.h"
#include "../Network/AnimationDownloader.h"

#ifndef USE_JSON_FACE_MODEL
#define USE_JSON_FACE_MODEL 1
#endif

// Opt-in: allocate animation JSON parsing buffers in PSRAM to avoid IRAM/DRAM pressure on large files.
#ifndef USE_PSRAM_FOR_ANIM_JSON
#define USE_PSRAM_FOR_ANIM_JSON 1
#endif

#if defined(ESP32) && USE_PSRAM_FOR_ANIM_JSON
struct AnimJsonPsramAllocator
{
    void *allocate(size_t size) { return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
    void deallocate(void *ptr) { heap_caps_free(ptr); }
};
using AnimJsonDocument = BasicJsonDocument<AnimJsonPsramAllocator>;
#else
using AnimJsonDocument = DynamicJsonDocument;
#endif

extern uint8_t User_R;
extern uint8_t User_G;
extern uint8_t User_B;

class M5UnitGLASS2;

// JSON-driven animation that maps morphs/materials/effects from a user-provided
// <device_id>_animation.json file. Falls back to /example_animation.json or a
// built-in minimal preset when missing. Designed to keep animation tweaks OTA-ready.
class JsonDrivenProtogenAnimation : public Animation<1>
{
private:
    struct SceneEffectConfig
    {
        String type;
        bool enable = false;
    };

    struct InterpolationConfig
    {
        String morphName;
        int dictionary = -1; // fallback when morph name missing
        EasyEaseInterpolation::InterpolationMethod method = EasyEaseInterpolation::Linear;
    };

    struct ExpressionConfig
    {
        bool reset = false;
        bool hasFaceMat = false;
        bool hasBackgroundMat = false;
        String faceMat;
        String backgroundMat;
        bool voice_enable = true;
        bool blink = true;
        bool show_mouth = true;
        bool eye_shape = true;
        SceneEffectConfig sceneEffect;
        std::vector<InterpolationConfig> interpolation;
        std::vector<std::pair<String, float>> animParameters;
    };

    JsonNukudeFace jsonFace;
    bool jsonFaceLoaded = false;
    Background background;
    EasyEaseAnimator<180> eEA = EasyEaseAnimator<180>(EasyEaseInterpolation::Overshoot, 1.0f, 0.25f);

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

    ProtoRGBColor gradientSpectrum[3] = {ProtoRGBColor(User_R, User_G, User_B), ProtoRGBColor(User_R, User_G, User_B), ProtoRGBColor(User_R, User_G, User_B)};
    ProtoRGBColor rainbowSpectrum[6] = {ProtoRGBColor(255, 0, 0), ProtoRGBColor(255, 255, 0), ProtoRGBColor(0, 255, 0), ProtoRGBColor(0, 255, 255), ProtoRGBColor(0, 0, 255), ProtoRGBColor(255, 0, 255)};
    GradientMaterial<3> gradientMat = GradientMaterial<3>(gradientSpectrum, 200.0f, false);
    GradientMaterial<6> rainbowMat = GradientMaterial<6>(rainbowSpectrum, 200.0f, false);

    ProtoRGBColor backgroundSpectrum[1] = {ProtoRGBColor(0, 0, 0)};
    GradientMaterial<1> backgroundMat = GradientMaterial<1>(backgroundSpectrum, 350.0f, false);

    MaterialAnimator<10> materialAnimator;

    SpectrumAnalyzer sA = SpectrumAnalyzer(Vector2D(200, 100), Vector2D(100, 50), true, true);

    // Animation controllers
    BlinkTrack<2> blink;

    FunctionGenerator fGenMatXMove = FunctionGenerator(FunctionGenerator::Sine, -2.0f, 2.0f, 5.3f);
    FunctionGenerator fGenMatYMove = FunctionGenerator(FunctionGenerator::Sine, -2.0f, 2.0f, 6.7f);
    FunctionGenerator fGenBlur = FunctionGenerator(FunctionGenerator::Sine, 0.0f, 1.0f, 1.5f);

    Menu espmenu = Menu();

    FFTVoiceDetection<128> voiceDetection;

    HorizontalBlur blurH = HorizontalBlur(20);
    VerticalBlur blurV = VerticalBlur(20);
    RadialBlur blurR = RadialBlur(10);
    AntiAliasingEffect edgeFeather = AntiAliasingEffect(0.5f);

    float offsetFace = 0.0f;
    float offsetFaceSA = 0.0f;
    static constexpr uint16_t kOffsetFaceInd = 50000;
    static constexpr uint16_t kOffsetFaceIndSA = 50001;
    float baseXOffset = 95.0f;
    float baseYOffset = -20.5f;

    bool enableBlink = true;
    bool voiceEnable = true;
    bool ShowMouth = true;
    bool EyeShapeB = true;

    String deviceId;
    String animationName;

    ExpressionConfig resetState;
    std::vector<String> expressionOrder;
    std::vector<std::pair<String, ExpressionConfig>> expressions;
    std::vector<std::unique_ptr<Material>> ownedMaterials; // owned dynamic materials from JSON
    std::vector<std::pair<String, Material *>> materialRegistry; // name->material* lookup
    std::vector<std::unique_ptr<Effect>> ownedEffects;   // owned screen-space effects from JSON
    std::vector<std::pair<String, Effect *>> effectRegistry; // name->effect* lookup
    std::vector<String> flippedMorphs; // morphs that should animate in reverse
    struct AutoLinkSpec
    {
        String name;
        uint16_t frames = 12;
        float basis = 0.0f;
        float goal = 1.0f;
    };
    struct BoopMorphSpec
    {
        uint8_t times = 1;      // number of boops to trigger this expression
        String name;             // expression name to trigger
    };
    struct HueShiftBinding
    {
        Material *material = nullptr;
        void *instance = nullptr;
        void (*apply)(void *, float) = nullptr;
    };
    std::vector<AutoLinkSpec> autoLinkSpecs; // optional per-morph overrides from JSON
    std::vector<HueShiftBinding> hueShiftRegistry; // material* -> concrete HueShift handler
    std::vector<BoopMorphSpec> boopMorphSpecs; // boop count -> expression mapping from JSON
    uint32_t lastBoopTime = 0;           // timestamp of last boop (in millis)
    uint8_t boopCount = 0;               // count of boops in current window
    bool lastBoopState = false;          // previous frame's boop state for edge detection
    String activeBoopExpression;
    uint32_t activeBoopUntil = 0;
    static constexpr uint32_t kBoopTimeWindow = 1200; // window to count boops
    static constexpr uint32_t kBoopHoldMs = 1800;     // keep resolved boop expression visible

    // Helpers
    Object3D *GetFaceObject()
    {
        return jsonFaceLoaded && jsonFace.Loaded() ? jsonFace.GetObject() : nullptr;
    }

    Transform *GetFaceTransform()
    {
        return jsonFaceLoaded && jsonFace.Loaded() ? jsonFace.GetTransform() : nullptr;
    }

    const char *ResolveMorphAlias(const char *name) const
    {
        if (!name)
        {
            return nullptr;
        }

        String morphName = name;
        if (morphName.equalsIgnoreCase("vrc_v_uh"))
        {
            return "vrc_v_dd";
        }
        if (morphName.equalsIgnoreCase("vrc_v_dd"))
        {
            return "vrc_v_uh";
        }

        return nullptr;
    }

    bool MorphNameEqualsIgnoreCase(const std::string &name, const char *candidate) const
    {
        return candidate && String(name.c_str()).equalsIgnoreCase(candidate);
    }

    int FindMorphIndex(const char *name) const
    {
        if (!(jsonFaceLoaded && jsonFace.Loaded()) || !name || !*name)
        {
            return -1;
        }

        int idx = jsonFace.FindMorphIndexByName(name);
        if (idx >= 0)
        {
            return idx;
        }

        const auto &names = jsonFace.GetMorphNames();
        for (size_t i = 0; i < names.size(); ++i)
        {
            if (MorphNameEqualsIgnoreCase(names[i], name))
            {
                return static_cast<int>(i);
            }
        }

        const char *alias = ResolveMorphAlias(name);
        if (!alias)
        {
            return -1;
        }

        idx = jsonFace.FindMorphIndexByName(alias);
        if (idx >= 0)
        {
            return idx;
        }

        for (size_t i = 0; i < names.size(); ++i)
        {
            if (MorphNameEqualsIgnoreCase(names[i], alias))
            {
                return static_cast<int>(i);
            }
        }

        return -1;
    }

    bool IsFastVisemeMorph(const std::string &name) const
    {
        return name.rfind("vrc_v_", 0) == 0;
    }

    bool TryGetAutoLinkOverride(uint16_t morphId, AutoLinkSpec &specOut) const
    {
        for (const auto &spec : autoLinkSpecs)
        {
            if (GetMorphId(spec.name.c_str()) == morphId)
            {
                specOut = spec;
                return true;
            }
        }

        return false;
    }

    bool IsFlippedMorph(uint16_t morphId, const char *name) const
    {
        for (const auto &fm : flippedMorphs)
        {
            if (fm.equalsIgnoreCase(name))
            {
                return true;
            }
            if (GetMorphId(fm.c_str()) == morphId)
            {
                return true;
            }
        }

        return false;
    }

    uint16_t GetMorphId(const char *name) const
    {
        int idx = FindMorphIndex(name);
        return idx >= 0 ? static_cast<uint16_t>(idx) : 0xFFFF;
    }

    float *GetFaceMorphWeightReference(const char *name)
    {
        if (jsonFaceLoaded && jsonFace.Loaded())
        {
            int idx = FindMorphIndex(name);
            if (idx >= 0)
            {
                return jsonFace.GetMorphWeightReferenceByIndex(static_cast<size_t>(idx));
            }
        }

        return nullptr;
    }

    void SetFaceMorphWeight(const char *name, float weight)
    {
        if (jsonFaceLoaded && jsonFace.Loaded())
        {
            int idx = FindMorphIndex(name);
            if (idx >= 0)
            {
                float *weightRef = jsonFace.GetMorphWeightReferenceByIndex(static_cast<size_t>(idx));
                if (weightRef)
                {
                    *weightRef = weight;
                }
            }
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

    bool TryLoadFaceFromPath(const String &path)
    {
        if (!LittleFS.exists(path))
        {
            return false;
        }

        jsonFaceLoaded = jsonFace.Load(LittleFS, path.c_str());
        if (jsonFaceLoaded)
        {
            Serial.printf("[INFO] Loaded face model from %s\n", path.c_str());
        }
        return jsonFaceLoaded;
    }

    const char *NormalizeBoopExpressionName(const char *name) const
    {
        if (name == nullptr)
        {
            return nullptr;
        }

        String candidate = name;
        if (candidate.equalsIgnoreCase("Suprise") || candidate.equalsIgnoreCase("Surprize"))
        {
            return "Surprised";
        }

        return name;
    }

    void ChangeInterpolationMethods()
    {
        eEA.SetInterpolationMethod(GetMorphId("vrc_v_ee"), EasyEaseInterpolation::Linear);
        eEA.SetInterpolationMethod(GetMorphId("vrc_v_ih"), EasyEaseInterpolation::Linear);
        eEA.SetInterpolationMethod(GetMorphId("vrc_v_dd"), EasyEaseInterpolation::Linear);
        eEA.SetInterpolationMethod(GetMorphId("vrc_v_rr"), EasyEaseInterpolation::Linear);
        eEA.SetInterpolationMethod(GetMorphId("vrc_v_ch"), EasyEaseInterpolation::Linear);
        eEA.SetInterpolationMethod(GetMorphId("vrc_v_aa"), EasyEaseInterpolation::Linear);
        eEA.SetInterpolationMethod(GetMorphId("vrc_v_oh"), EasyEaseInterpolation::Linear);
        eEA.SetInterpolationMethod(GetMorphId("vrc_v_ss"), EasyEaseInterpolation::Linear);
    }


    void LoadJsonFaceBlocking()
    {
        const String preferredFacePath = deviceId.length() > 0 ? "/" + deviceId + String("_face.json") : String();
        const String fallbackFacePath = "/universal_face.json";

        // Block until LittleFS mounts and the face json is successfully loaded.
        while (USE_JSON_FACE_MODEL && !jsonFaceLoaded)
        {
            if (!LittleFS.begin(false) && !LittleFS.begin(true))
            {
                Serial.println("[WARN] LittleFS mount failed; retrying...");
                delay(500);
                continue;
            }

            bool preferredExists = !preferredFacePath.isEmpty() && LittleFS.exists(preferredFacePath);
            bool fallbackExists = LittleFS.exists(fallbackFacePath);

            if (preferredExists && TryLoadFaceFromPath(preferredFacePath))
            {
                return;
            }

            if (fallbackExists && TryLoadFaceFromPath(fallbackFacePath))
            {
                return;
            }

            if (preferredExists)
            {
                Serial.printf("[WARN] %s failed to load; retrying...\n", preferredFacePath.c_str());
            }

            if (fallbackExists)
            {
                Serial.printf("[WARN] %s failed to load; retrying...\n", fallbackFacePath.c_str());
            }
            else if (!preferredExists)
            {
                if (!preferredFacePath.isEmpty())
                {
                    Serial.printf("[WARN] Face model not found (%s or %s); waiting for file...\n", preferredFacePath.c_str(), fallbackFacePath.c_str());
                }
                else
                {
                    Serial.printf("[WARN] %s not found; waiting for file...\n", fallbackFacePath.c_str());
                }
            }

            delay(500);
        }
    }

    void AutoLinkMorphs(uint16_t frames = 10, float basis = 0.0f, float goal = 1.0f)
    {
        if (!(jsonFaceLoaded && jsonFace.Loaded()))
        {
            return;
        }

        const auto &names = jsonFace.GetMorphNames();

        for (size_t i = 0; i < names.size(); ++i)
        {
            float *ptr = jsonFace.GetMorphWeightReferenceByIndex(i);
            if (!ptr)
            {
                continue;
            }

            const uint16_t morphId = static_cast<uint16_t>(i);
            AutoLinkSpec spec;
            bool hasOverride = TryGetAutoLinkOverride(morphId, spec);
            uint16_t morphFrames = hasOverride ? spec.frames : (IsFastVisemeMorph(names[i]) ? 2 : frames);
            float morphBasis = hasOverride ? spec.basis : basis;
            float morphGoal = hasOverride ? spec.goal : goal;

            bool flip = IsFlippedMorph(morphId, names[i].c_str());
            float b = flip ? morphGoal : morphBasis;
            float g = flip ? morphBasis : morphGoal;
            eEA.AddParameter(ptr, morphId, morphFrames, b, g);
        }

        eEA.AddParameter(&offsetFace, kOffsetFaceInd, 40, 0.0f, 1.0f);
        eEA.AddParameter(&offsetFaceSA, kOffsetFaceIndSA, 40, 0.0f, 1.0f);
    }

    void LinkParameters()
    {
        float *blinkPtr = GetFaceMorphWeightReference("Blink");
        if (blinkPtr)
        {
            blink.AddParameter(blinkPtr);
        }
        float *sBlinkPtr = GetFaceMorphWeightReference("SEyeBlink");
        if (sBlinkPtr)
        {
            blink.AddParameter(sBlinkPtr);
        }
    }

    EasyEaseInterpolation::InterpolationMethod ParseInterpolationMethod(const String &method) const
    {
        if (method.equalsIgnoreCase("Cosine"))
            return EasyEaseInterpolation::Cosine;
        if (method.equalsIgnoreCase("Bounce"))
            return EasyEaseInterpolation::Bounce;
        if (method.equalsIgnoreCase("Overshoot"))
            return EasyEaseInterpolation::Overshoot;
        return EasyEaseInterpolation::Linear;
    }

    ProtoRGBColor ToColor(JsonObject obj, const ProtoRGBColor &fallback) const
    {
        return ProtoRGBColor(obj["R"] | fallback.R, obj["G"] | fallback.G, obj["B"] | fallback.B);
    }

    void ResetMaterialPalettes()
    {
        gradientSpectrum[0] = ProtoRGBColor(User_R, User_G, User_B);
        gradientSpectrum[1] = ProtoRGBColor(User_R, User_G, User_B);
        gradientSpectrum[2] = ProtoRGBColor(User_R, User_G, User_B);

        rainbowSpectrum[0] = ProtoRGBColor(255, 0, 0);
        rainbowSpectrum[1] = ProtoRGBColor(255, 255, 0);
        rainbowSpectrum[2] = ProtoRGBColor(0, 255, 0);
        rainbowSpectrum[3] = ProtoRGBColor(0, 255, 255);
        rainbowSpectrum[4] = ProtoRGBColor(0, 0, 255);
        rainbowSpectrum[5] = ProtoRGBColor(255, 0, 255);

        backgroundSpectrum[0] = ProtoRGBColor(0, 0, 0);

        gradientMat = GradientMaterial<3>(gradientSpectrum, 200.0f, false);
        rainbowMat = GradientMaterial<6>(rainbowSpectrum, 200.0f, false);
        backgroundMat = GradientMaterial<1>(backgroundSpectrum, 350.0f, false);
    }

    void RegisterDefaultMaterials()
    {
        ResetMaterialPalettes();
        materialRegistry.clear();

        // Base + stock palette materials
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

        // Registry for name lookup
        RegisterMaterial("gradientSpectrum", &gradientMat);
        RegisterMaterial("rainbowMat", &rainbowMat);
        RegisterMaterial("backgroundMat", &backgroundMat);
        RegisterMaterial("rainbowNoise", &rainbowNoise);
        RegisterMaterial("rainbowSpiral", &rainbowSpiral);
        RegisterMaterial("SpectrumAnalyzer", &sA);
        RegisterMaterial("redMat", &redMaterial);
        RegisterMaterial("greenMat", &greenMaterial);
        RegisterMaterial("blueMat", &blueMaterial);
        RegisterMaterial("yellowMat", &yellowMaterial);
        RegisterMaterial("purpleMat", &purpleMaterial);
        RegisterMaterial("whiteMat", &whiteMaterial);
        RegisterMaterial("orangeMat", &orangeMaterial);

        RegisterHueShiftable(&gradientMat);
        RegisterHueShiftable(&rainbowMat);
        RegisterHueShiftable(&backgroundMat);
        RegisterHueShiftable(&redMaterial);
        RegisterHueShiftable(&orangeMaterial);
        RegisterHueShiftable(&whiteMaterial);
        RegisterHueShiftable(&greenMaterial);
        RegisterHueShiftable(&blueMaterial);
        RegisterHueShiftable(&yellowMaterial);
        RegisterHueShiftable(&purpleMaterial);
        RegisterHueShiftable(&rainbowSpiral);
    }

    void RegisterDefaultEffects()
    {
        effectRegistry.clear();
        // Built-in defaults
        RegisterEffect("blurH", &blurH);
        RegisterEffect("blurV", &blurV);
        RegisterEffect("blurR", &blurR);
        RegisterEffect("edgeFeather", &edgeFeather);
    }

    Effect *ResolveEffect(const String &name)
    {
        for (auto &entry : effectRegistry)
        {
            if (entry.first.equalsIgnoreCase(name))
            {
                return entry.second;
            }
        }
        return nullptr;
    }

    void ApplySceneEffect(const SceneEffectConfig &cfg)
    {
        if (!cfg.enable)
        {
            scene.DisableEffect();
            return;
        }

        Effect *effect = ResolveEffect(cfg.type);
        if (!effect)
        {
            // fallback
            effect = &edgeFeather;
        }
        scene.SetEffect(effect);
        scene.EnableEffect();
    }

    void ApplyResetState()
    {
        // Reset interpolations first.
        for (const auto &interp : resetState.interpolation)
        {
            uint16_t id = interp.dictionary >= 0 ? static_cast<uint16_t>(interp.dictionary) : GetMorphId(interp.morphName.c_str());
            eEA.SetInterpolationMethod(id, interp.method);
        }

        // Basic toggles.
        enableBlink = resetState.blink;
        voiceEnable = resetState.voice_enable;
        ShowMouth = resetState.show_mouth;
        EyeShapeB = resetState.eye_shape;

        // Hide/Show mouth.
        uint16_t hideMouthId = GetMorphId("HideMouth");
        if (hideMouthId != 0xFFFF)
        {
            eEA.AddParameterFrame(hideMouthId, resetState.show_mouth ? 0.0f : 1.0f);
        }

        ApplySceneEffect(resetState.sceneEffect);

        if (resetState.hasBackgroundMat)
        {
            background.GetObject()->SetMaterial(ResolveMaterial(resetState.backgroundMat));
        }
        else
        {
            background.GetObject()->SetMaterial(&backgroundMat);
        }

        Object3D *faceObject = GetFaceObject();
        if (faceObject)
        {
            if (resetState.hasFaceMat)
            {
                faceObject->SetMaterial(ResolveMaterial(resetState.faceMat));
            }
            else
            {
                faceObject->SetMaterial(&gradientMat);
            }
        }

        for (const auto &ap : resetState.animParameters)
        {
            AddMorphFrame(ap.first, ap.second);
        }
    }

    Material *ResolveMaterial(const String &name)
    {
        for (auto &entry : materialRegistry)
        {
            if (entry.first.equalsIgnoreCase(name))
            {
                return entry.second;
            }
        }
        return &gradientMat;
    }

    void AddMorphFrame(const String &name, float value)
    {
        uint16_t id = GetMorphId(name.c_str());
        if (id != 0xFFFF)
        {
            eEA.AddParameterFrame(id, value);
        }
    }

    void RegisterMaterial(const String &name, Material *mat)
    {
        for (auto &entry : materialRegistry)
        {
            if (entry.first.equalsIgnoreCase(name))
            {
                entry.second = mat;
                return;
            }
        }
        materialRegistry.push_back({name, mat});
    }

    void RegisterEffect(const String &name, Effect *effect)
    {
        for (auto &entry : effectRegistry)
        {
            if (entry.first.equalsIgnoreCase(name))
            {
                entry.second = effect;
                return;
            }
        }
        effectRegistry.push_back({name, effect});
    }

    template <size_t N>
    GradientMaterial<N> *CreateGradientMaterial(JsonArray colors, float period, bool isRadial)
    {
        ProtoRGBColor spectrum[N];
        for (size_t i = 0; i < N; ++i)
        {
            spectrum[i] = ProtoRGBColor(0, 0, 0);
            if (i < colors.size())
            {
                spectrum[i] = ToColor(colors[i], spectrum[i]);
            }
        }
        ownedMaterials.push_back(std::make_unique<GradientMaterial<N>>(spectrum, period, isRadial));
        GradientMaterial<N> *mat = static_cast<GradientMaterial<N> *>(ownedMaterials.back().get());
        RegisterHueShiftable(mat);
        return mat;
    }

    template <typename T>
    static void ApplyHueShiftTyped(void *instance, float hueDeg)
    {
        static_cast<T *>(instance)->HueShift(hueDeg);
    }

    template <typename T>
    void RegisterHueShiftable(T *mat)
    {
        Material *asMaterial = static_cast<Material *>(mat);
        for (auto &entry : hueShiftRegistry)
        {
            if (entry.material == asMaterial)
            {
                entry.instance = mat;
                entry.apply = &ApplyHueShiftTyped<T>;
                return;
            }
        }
        hueShiftRegistry.push_back({asMaterial, mat, &ApplyHueShiftTyped<T>});
    }

    void ApplyHueShiftToCurrentFaceMaterial(float hueDeg)
    {
        Object3D *faceObject = GetFaceObject();
        if (!faceObject)
        {
            return;
        }

        Material *current = faceObject->GetMaterial();
        if (!current)
        {
            return;
        }

        for (auto &entry : hueShiftRegistry)
        {
            if (entry.material == current && entry.apply)
            {
                entry.apply(entry.instance, hueDeg);
                return;
            }
        }
    }

    Material *RegisterGradientFromJson(const String &name, JsonObject obj)
    {
        float period = obj["gradientPeriod"] | 200.0f;
        bool isRadial = obj["isRadial"] | false;
        uint8_t colourNum = obj["colour_num"] | 0;
        JsonArray colors = obj.containsKey("colour") ? obj["colour"].as<JsonArray>() : JsonArray();

        if (colourNum == 0 && colors.size() > 0)
        {
            colourNum = colors.size();
        }

        Material *mat = nullptr;
        switch (colourNum)
        {
        case 1:
            mat = CreateGradientMaterial<1>(colors, period, isRadial);
            break;
        case 2:
            mat = CreateGradientMaterial<2>(colors, period, isRadial);
            break;
        case 3:
            mat = CreateGradientMaterial<3>(colors, period, isRadial);
            break;
        case 4:
            mat = CreateGradientMaterial<4>(colors, period, isRadial);
            break;
        case 5:
            mat = CreateGradientMaterial<5>(colors, period, isRadial);
            break;
        default:
            mat = CreateGradientMaterial<6>(colors, period, isRadial);
            break;
        }

        RegisterMaterial(name, mat);
        return mat;
    }

    void RegisterMaterialsFromJson(JsonObject mats)
    {
        // Keep ownedMaterials alive across calls; clear registry to avoid stale pointers.
        ownedMaterials.clear();
        materialRegistry.clear();
        hueShiftRegistry.clear();

        // Always ensure stock materials are present.
        RegisterDefaultMaterials();
        RegisterDefaultEffects();

        for (JsonPair kv : mats)
        {
            String name = kv.key().c_str();
            JsonObject mObj = kv.value().as<JsonObject>();

            if (name.equalsIgnoreCase("gradientSpectrum"))
            {
                bool useUser = mObj["use_user_data"] | false;
                float gradientPeriod = mObj["gradientPeriod"] | 200.0f;
                bool isRadial = mObj["isRadial"] | false;
                if (useUser)
                {
                    gradientSpectrum[0] = ProtoRGBColor(User_R, User_G, User_B);
                    gradientSpectrum[1] = ProtoRGBColor(User_R, User_G, User_B);
                    gradientSpectrum[2] = ProtoRGBColor(User_R, User_G, User_B);
                }
                if (mObj.containsKey("colour"))
                {
                    JsonArray colors = mObj["colour"].as<JsonArray>();
                    if (colors.size() > 0)
                        gradientSpectrum[0] = ToColor(colors[0], gradientSpectrum[0]);
                    if (colors.size() > 1)
                        gradientSpectrum[1] = ToColor(colors[1], gradientSpectrum[1]);
                    if (colors.size() > 2)
                        gradientSpectrum[2] = ToColor(colors[2], gradientSpectrum[2]);
                }
                gradientMat = GradientMaterial<3>(gradientSpectrum, gradientPeriod, isRadial);
                RegisterMaterial(name, &gradientMat);
                RegisterHueShiftable(&gradientMat);
                continue;
            }

            if (name.equalsIgnoreCase("rainbowMat"))
            {
                float gradientPeriod = mObj["gradientPeriod"] | 200.0f;
                bool isRadial = mObj["isRadial"] | false;
                if (mObj.containsKey("colour"))
                {
                    JsonArray colors = mObj["colour"].as<JsonArray>();
                    for (uint8_t i = 0; i < 6 && i < colors.size(); ++i)
                    {
                        rainbowSpectrum[i] = ToColor(colors[i], rainbowSpectrum[i]);
                    }
                }
                rainbowMat = GradientMaterial<6>(rainbowSpectrum, gradientPeriod, isRadial);
                RegisterMaterial(name, &rainbowMat);
                RegisterHueShiftable(&rainbowMat);
                continue;
            }

            if (name.equalsIgnoreCase("backgroundMat"))
            {
                float gradientPeriod = mObj["gradientPeriod"] | 350.0f;
                bool isRadial = mObj["isRadial"] | false;
                if (mObj.containsKey("colour"))
                {
                    JsonArray colors = mObj["colour"].as<JsonArray>();
                    if (colors.size() > 0)
                        backgroundSpectrum[0] = ToColor(colors[0], backgroundSpectrum[0]);
                }
                backgroundMat = GradientMaterial<1>(backgroundSpectrum, gradientPeriod, isRadial);
                RegisterMaterial(name, &backgroundMat);
                RegisterHueShiftable(&backgroundMat);
                continue;
            }

            // Unknown material: attempt to build gradient of requested size.
            RegisterGradientFromJson(name, mObj);
        }
    }

    void RegisterEffectsFromJson(JsonObject effects)
    {
        ownedEffects.clear();
        effectRegistry.clear();
        RegisterDefaultEffects();

        for (JsonPair kv : effects)
        {
            String name = kv.key().c_str();
            JsonObject eObj = kv.value().as<JsonObject>();
            String type = eObj["type"] | "";
            float value = eObj["value"] | 0.0f;

            if (type.equalsIgnoreCase("HorizontalBlur"))
            {
                ownedEffects.push_back(std::make_unique<HorizontalBlur>(uint8_t(value)));
                RegisterEffect(name, ownedEffects.back().get());
            }
            else if (type.equalsIgnoreCase("VerticalBlur"))
            {
                ownedEffects.push_back(std::make_unique<VerticalBlur>(uint8_t(value)));
                RegisterEffect(name, ownedEffects.back().get());
            }
            else if (type.equalsIgnoreCase("RadialBlur"))
            {
                ownedEffects.push_back(std::make_unique<RadialBlur>(uint8_t(value)));
                RegisterEffect(name, ownedEffects.back().get());
            }
            else if (type.equalsIgnoreCase("AntiAliasingEffect"))
            {
                ownedEffects.push_back(std::make_unique<AntiAliasingEffect>(value > 0.0f ? value : 0.25f));
                RegisterEffect(name, ownedEffects.back().get());
            }
        }
    }

    ExpressionConfig ParseExpressionConfig(JsonObject obj)
    {
        ExpressionConfig cfg;
        cfg.reset = obj["reset"] | false;
        cfg.faceMat = obj["faceMat"] | "";
        cfg.backgroundMat = obj["backgroundMat"] | "";
        cfg.hasFaceMat = obj.containsKey("faceMat");
        cfg.hasBackgroundMat = obj.containsKey("backgroundMat");
        cfg.voice_enable = obj["voice_enable"] | true;
        cfg.blink = obj["blink"] | true;
        cfg.show_mouth = obj["show_mouth"] | true;
        cfg.eye_shape = obj["eye_shape"] | true;

        if (obj.containsKey("scene_effect"))
        {
            JsonObject effect = obj["scene_effect"].as<JsonObject>();
            cfg.sceneEffect.type = effect["type"] | "";
            cfg.sceneEffect.enable = effect["enable"] | false;
        }

        if (obj.containsKey("interpolation"))
        {
            JsonObject interp = obj["interpolation"].as<JsonObject>();
            InterpolationConfig interpCfg;
            interpCfg.morphName = interp["name"] | "";
            interpCfg.dictionary = interp["dictionary"] | -1;
            String method = interp["method"] | "Linear";
            interpCfg.method = ParseInterpolationMethod(method);
            cfg.interpolation.push_back(interpCfg);
        }

        if (obj.containsKey("anim_parameter"))
        {
            JsonObject params = obj["anim_parameter"].as<JsonObject>();
            for (JsonPair kv : params)
            {
                cfg.animParameters.push_back({String(kv.key().c_str()), kv.value().as<float>()});
            }
        }

        return cfg;
    }

    void ApplyExpression(const String &name)
    {
        ExpressionConfig *target = nullptr;
        for (auto &pair : expressions)
        {
            if (pair.first.equalsIgnoreCase(name))
            {
                target = &pair.second;
                break;
            }
        }

        if (!target)
        {
            return;
        }

        if (target->reset)
        {
            ApplyResetState();
        }

        for (const auto &interp : target->interpolation)
        {
            uint16_t id = interp.dictionary >= 0 ? static_cast<uint16_t>(interp.dictionary) : GetMorphId(interp.morphName.c_str());
            eEA.SetInterpolationMethod(id, interp.method);
        }

        enableBlink = target->blink;
        voiceEnable = target->voice_enable;
        ShowMouth = target->show_mouth;
        EyeShapeB = target->eye_shape;

        if (target->hasFaceMat)
        {
            Object3D *faceObject = GetFaceObject();
            if (faceObject)
            {
                faceObject->SetMaterial(ResolveMaterial(target->faceMat));
            }
        }

        if (target->hasBackgroundMat)
        {
            background.GetObject()->SetMaterial(ResolveMaterial(target->backgroundMat));
        }

        ApplySceneEffect(target->sceneEffect);

        for (const auto &ap : target->animParameters)
        {
            AddMorphFrame(ap.first, ap.second);
        }
    }

    String GetExpressionByIndex(uint8_t idx) const
    {
        if (expressionOrder.empty())
        {
            return "";
        }
        if (idx >= expressionOrder.size())
        {
            idx = idx % expressionOrder.size();
        }
        return expressionOrder[idx];
    }

    bool LoadAnimationConfig(const UserConfig &config)
    {
        deviceId = config.device_id;
        String filename = config.user_animation.length() > 0 ? config.user_animation : (deviceId + String("_animation.json"));
        String path = "/" + filename;
        String fallbackPath = "/example_animation.json";
        String loadedPath;

        if (!LittleFS.begin(false) && !LittleFS.begin(true))
        {
            Serial.println("[WARN] LittleFS mount failed for animation config.");
            return false;
        }

        String jsonText;
        if (LittleFS.exists(path))
        {
            File f = LittleFS.open(path, "r");
            if (f)
            {
                jsonText = f.readString();
                f.close();
                loadedPath = path;
                Serial.printf("[INFO] Loaded animation config from %s\n", path.c_str());
            }
            else
            {
                Serial.printf("[WARN] Failed to open animation config %s\n", path.c_str());
            }
        }
        else
        {
            Serial.printf("[WARN] Animation config %s not found; checking fallback %s\n", path.c_str(), fallbackPath.c_str());
        }

        if (jsonText.isEmpty() && LittleFS.exists(fallbackPath))
        {
            File f = LittleFS.open(fallbackPath, "r");
            if (f)
            {
                jsonText = f.readString();
                f.close();
                loadedPath = fallbackPath;
                Serial.printf("[INFO] Using fallback animation config %s\n", fallbackPath.c_str());
            }
            else
            {
                Serial.printf("[WARN] Failed to open fallback animation config %s\n", fallbackPath.c_str());
            }
        }

        if (jsonText.isEmpty())
        {
            // Minimal default
            jsonText = "{\"expressions\":{\"Default\":{\"reset\":true}}}";
            loadedPath = "<built-in default>";
            Serial.printf("[WARN] Animation JSON missing for %s and %s; using minimal default.\n", path.c_str(), fallbackPath.c_str());
        }

        AnimJsonDocument doc(jsonText.length() + 2048);
        DeserializationError err = deserializeJson(doc, jsonText);
        if (err)
        {
            Serial.printf("[WARN] Failed to parse animation JSON from %s: %s\n", loadedPath.c_str(), err.c_str());
            return false;
        }

        animationName = doc["animation_name"] | doc["display_name"] | doc["name"] | doc["user"] | String("");
        if (animationName.isEmpty())
        {
            animationName = loadedPath;
            if (animationName.startsWith("/"))
            {
                animationName.remove(0, 1);
            }
            const int dot = animationName.lastIndexOf('.');
            if (dot > 0)
            {
                animationName = animationName.substring(0, dot);
            }
        }

        if (doc.containsKey("Mat_register"))
        {
            JsonObject mats = doc["Mat_register"].as<JsonObject>();
            RegisterMaterialsFromJson(mats);
        }

        if (doc.containsKey("screen_effect_register"))
        {
            JsonObject eff = doc["screen_effect_register"].as<JsonObject>();
            RegisterEffectsFromJson(eff);
        }

        flippedMorphs.clear();
        autoLinkSpecs.clear();
        if (doc.containsKey("flipped_morphs"))
        {
            JsonArray flips = doc["flipped_morphs"].as<JsonArray>();
            for (JsonVariant v : flips)
            {
                if (v.is<const char *>())
                {
                    flippedMorphs.push_back(String(v.as<const char *>()));
                }
            }
        }

        if (doc.containsKey("auto_link"))
        {
            JsonArray links = doc["auto_link"].as<JsonArray>();
            for (JsonVariant v : links)
            {
                if (!v.is<JsonObject>())
                {
                    continue;
                }
                JsonObject obj = v.as<JsonObject>();
                AutoLinkSpec spec;
                spec.name = obj["name"] | String("");
                spec.frames = obj["frames"] | 12;
                spec.basis = obj["basis"] | 0.0f;
                spec.goal = obj["goal"] | 1.0f;
                if (spec.name.length() > 0)
                {
                    autoLinkSpecs.push_back(spec);
                }
            }
        }

        boopMorphSpecs.clear();
        if (doc.containsKey("boop_morphs"))
        {
            JsonArray boops = doc["boop_morphs"].as<JsonArray>();
            for (JsonVariant v : boops)
            {
                if (!v.is<JsonObject>())
                {
                    continue;
                }
                JsonObject obj = v.as<JsonObject>();
                BoopMorphSpec spec;
                spec.times = obj["times"] | 1;
                spec.name = NormalizeBoopExpressionName(obj["name"] | String(""));
                if (spec.name.length() > 0)
                {
                    boopMorphSpecs.push_back(spec);
                }
            }
        }

        baseXOffset = doc["x_offset"] | baseXOffset;
        baseYOffset = doc["y_offset"] | baseYOffset;

        expressions.clear();
        expressionOrder.clear();
        resetState = ExpressionConfig();

        if (doc.containsKey("expressions"))
        {
            JsonObject exprObj = doc["expressions"].as<JsonObject>();
            for (JsonPair kv : exprObj)
            {
                String name = kv.key().c_str();
                ExpressionConfig cfg = ParseExpressionConfig(kv.value().as<JsonObject>());
                if (name.equalsIgnoreCase("reset_state"))
                {
                    resetState = cfg;
                    resetState.reset = true; // ensure reset semantics
                }
                else
                {
                    expressions.push_back({name, cfg});
                    expressionOrder.push_back(name);
                }
            }
        }

        return true;
    }

    String ResolveBoopExpression()
    {
        // Note: This is called every frame from Update(), so we track boop state changes
        if (activeBoopExpression.length() > 0)
        {
            const uint32_t now = millis();
            if (now < activeBoopUntil)
            {
                return activeBoopExpression;
            }

            activeBoopExpression.clear();
        }

        bool currentBoopState = Menu::UseBoopSensor() ? Menu::isBooped() : false;
        uint32_t now = millis();

        // Edge detection: transition from not booped to booped
        if (currentBoopState && !lastBoopState)
        {
            // New boop detected
            uint32_t timeSinceLastBoop = now - lastBoopTime;
            
            // If first boop or within time window, increment counter
            if (boopCount == 0 || timeSinceLastBoop < kBoopTimeWindow)
            {
                boopCount++;
            }
            else
            {
                // Time window expired, reset counter and start new sequence
                boopCount = 1;
            }
            
            lastBoopTime = now;
        }

        lastBoopState = currentBoopState;

        // Check if window has closed (no new boops for kBoopTimeWindow ms)
        if (boopCount > 0 && !currentBoopState && (now - lastBoopTime) > kBoopTimeWindow)
        {
            // Window closed, resolve the boop sequence
            String result = "";
            
            // Find matching expression by boop count
            for (const auto &spec : boopMorphSpecs)
            {
                if (spec.times == boopCount)
                {
                    result = spec.name;
                    break;
                }
            }

            if (!result.isEmpty())
            {
                activeBoopExpression = result;
                activeBoopUntil = now + kBoopHoldMs;
            }
            
            // Reset counters
            boopCount = 0;
            lastBoopTime = 0;
            
            return result;
        }

        // Window still open or no boops counted
        return "";
    }

public:
    JsonDrivenProtogenAnimation() {}

    bool Initialize(const UserConfig &config, const char *githubAnimBase = nullptr, const char *giteeAnimBase = nullptr, const char *githubToken = nullptr, const char *giteeToken = nullptr, M5UnitGLASS2 *downloadDisplay = nullptr, bool verboseDownload = true)
    {
        Serial.begin(115200);
        deviceId = config.device_id;

        LoadJsonFaceBlocking();

        Object3D *faceObject = GetFaceObject();
        if (faceObject)
        {
            scene.AddObject(faceObject);
        }
        scene.AddObject(background.GetObject());
        RegisterDefaultMaterials();

        if (faceObject)
        {
            faceObject->SetMaterial(&gradientMat);
        }
        background.GetObject()->SetMaterial(&backgroundMat);

        // Download user animation JSON if remote URLs are provided. AnimationDownloader prefers Gitee, then GitHub.
        String animFilename = config.user_animation.length() > 0 ? config.user_animation : (config.device_id + String("_animation.json"));
        AnimationDownloadConfig dlCfg{githubAnimBase, giteeAnimBase, githubToken, giteeToken, downloadDisplay, verboseDownload};
        AnimationDownloader::Download(dlCfg, animFilename);

        LoadAnimationConfig(config);
        AutoLinkMorphs();
        LinkParameters();

        if (resetState.reset)
        {
            ApplyResetState();
        }

        #ifdef TASESP32S3
        MicrophoneFourierIT::Initialize(1, 8000, 68.0f, 120.0f); // 8KHz sample rate, 50dB min, 120dB max
        #elif defined(TASESP32P4)
        MicrophoneFourierIT::Initialize(23, 8000, 68.0f, 120.0f);
        #endif
        espmenu.Initialize(17, 200);
        ChangeInterpolationMethods();
        return true;
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
        if (!animationName.isEmpty())
        {
            display.drawString(animationName, 5, 24);
        }
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

        uint8_t mode = Menu::GetFaceState();

        MicrophoneFourierIT::Update();
        voiceDetection.SetThreshold(map(Menu::GetMicLevel(), 0, 10, 1000, 50));

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

        // Resolve boop sequence (called every frame for state tracking)
        String boopExpr = ResolveBoopExpression();
        // Intercept animation selection from JSON definitions.
        String targetExpr = !boopExpr.isEmpty() ? boopExpr : GetExpressionByIndex(mode);
        if (!targetExpr.isEmpty())
        {
            ApplyExpression(targetExpr);
        }

        ApplyHueShiftToCurrentFaceMaterial(Menu::GetHueShift());

        if (enableBlink)
        {
            blink.Update();
        }

        sA.SetHueAngle(ratio * 360.0f * 4.0f);
        sA.SetMirrorYState(Menu::MirrorSpectrumAnalyzer());
        sA.SetFlipYState(!Menu::MirrorSpectrumAnalyzer());

        eEA.Update();
        UpdateFace();

        rainbowNoise.Update(ratio);
        rainbowSpiral.Update(ratio);
        Object3D *bgObj = background.GetObject();
        if (bgObj && bgObj->GetMaterial() == &sA)
        {
            sA.Update(MicrophoneFourierIT::GetFourierFiltered());
        }
        materialAnimator.Update();

        Transform *faceTransform = GetFaceTransform();
        if (!faceTransform)
        {
            return;
        }

        faceTransform->SetRotation(Vector3D(0.0f, 0.0f, -7.5f));

        const float scale = 17.5f * 0.5f + 0.45f;
        faceTransform->SetPosition(Vector3D(baseXOffset + xOffset, baseYOffset + yOffset, 550.0f));
        faceTransform->SetScale(Vector3D(-0.975f, 0.59f, 0.65f).Multiply(scale));

        GetFaceObject()->UpdateTransform();
    }
};
