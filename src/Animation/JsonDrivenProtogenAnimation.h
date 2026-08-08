#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <vector>
#include <utility>
#include <memory>
#include <ProtoGC.h>

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

#ifndef BOOP_DEBUG_LOG
#define BOOP_DEBUG_LOG 0
#endif

#if BOOP_DEBUG_LOG
#define BOOP_LOG_PRINTF(...) Serial.printf(__VA_ARGS__)
#define BOOP_LOG_PRINTLN(msg) Serial.println(msg)
#else
#define BOOP_LOG_PRINTF(...) do { } while (0)
#define BOOP_LOG_PRINTLN(msg) do { } while (0)
#endif

#if defined(ESP32) && USE_PSRAM_FOR_ANIM_JSON
using AnimJsonDocument = BasicJsonDocument<protogc::ProtoJsonPsramAllocator>;
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
class JsonDrivenProtogenAnimation : public Animation<2>
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
        int16_t brightness = -1; // per-animation brightness override (0-255); -1 = follow menu/user default
        SceneEffectConfig sceneEffect;
        std::vector<InterpolationConfig> interpolation;
        std::vector<std::pair<String, float>> animParameters;
        // Pre-resolved morph IDs — filled after JSON load to avoid per-frame GetMorphId scans
        std::vector<std::pair<uint16_t, float>> resolvedAnimParams;
    };

    JsonNukudeFace jsonFace;
    bool jsonFaceLoaded = false;
    Background background;
    EasyEaseAnimator<180> eEA = EasyEaseAnimator<180>(EasyEaseInterpolation::Overshoot, 1.0f, 0.25f);

    // Materials
    RainbowSpiral rainbowSpiral;
    SimpleMaterial expressionColor = SimpleMaterial(ProtoRGBColor(User_R, User_G, User_B));

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
    bool menuInitialized = false;

    // Per-animation brightness: -1 = follow the menu value (BLE/app adjustable,
    // initialised from user config user_brightness); >= 0 = forced by the active expression.
    int16_t activeBrightness = -1;

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
        uint32_t periodMs = 2800; // hold duration for this expression after trigger
        int16_t expressionIndex = -1; // pre-resolved index into expressionOrder (set after JSON load)
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
    uint32_t boopWindowStart = 0;        // start timestamp of current boop count window
    uint32_t boopLastPulseTime = 0;      // last pulse timestamp for diagnostics
    uint16_t boopCount = 0;              // persistent boop count inside current window
    uint32_t boopWindowMs = 30000;       // configurable boop count window (default 30s)
    uint8_t boopSensorThreshold = 180;   // optional JSON override for Menu boop sensitivity
    int16_t activeBoopExpressionIndex = -1;  // pre-resolved expression index, -1 = none (replaces String to avoid heap alloc)
    uint32_t activeBoopUntil = 0;
    static constexpr uint32_t kDefaultBoopHoldMs = 2800;  // fallback hold duration when period_ms is omitted

    // Cached viseme morph IDs — pre-resolved after face load to avoid per-frame string scans
    uint16_t mVrcSsId = 0xFFFF;
    uint16_t mVrcEeId = 0xFFFF;
    uint16_t mVrcIhId = 0xFFFF;
    uint16_t mVrcDdId = 0xFFFF;
    uint16_t mVrcRrId = 0xFFFF;
    uint16_t mVrcChId = 0xFFFF;
    uint16_t mVrcAaId = 0xFFFF;
    uint16_t mVrcOhId = 0xFFFF;
    uint16_t mHideMouthId = 0xFFFF;

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

    String NormalizeBoopExpressionName(const char *name) const
    {
        if (name == nullptr || !name[0])
        {
            return "";
        }

        String candidate = name;
        if (candidate.equalsIgnoreCase("Suprise") ||
            candidate.equalsIgnoreCase("Surprize") ||
            candidate.equalsIgnoreCase("Suprised") ||
            candidate.equalsIgnoreCase("Surprise"))
        {
            return "Surprised";
        }

        return String(name);
    }

    int16_t FindExpressionIndex(const String &name) const
    {
        for (size_t i = 0; i < expressionOrder.size(); i++)
        {
            if (expressionOrder[i].equalsIgnoreCase(name))
                return (int16_t)i;
        }
        // Try normalized alias
        const String alias = NormalizeBoopExpressionName(name.c_str());
        if (alias.length() > 0 && !alias.equalsIgnoreCase(name))
        {
            for (size_t i = 0; i < expressionOrder.size(); i++)
            {
                if (expressionOrder[i].equalsIgnoreCase(alias))
                    return (int16_t)i;
            }
        }
        return -1;
    }

    ExpressionConfig *FindExpressionConfig(const String &name)
    {
        for (auto &pair : expressions)
        {
            if (pair.first.equalsIgnoreCase(name))
            {
                return &pair.second;
            }
        }

        const String alias = NormalizeBoopExpressionName(name.c_str());
        if (alias.length() > 0 && !alias.equalsIgnoreCase(name))
        {
            for (auto &pair : expressions)
            {
                if (pair.first.equalsIgnoreCase(alias))
                {
                    return &pair.second;
                }
            }
        }

        return nullptr;
    }

    const BoopMorphSpec *FindBoopSpecByTimes(uint16_t times) const
    {
        for (const auto &spec : boopMorphSpecs)
        {
            if (spec.times == times && spec.name.length() > 0)
            {
                return &spec;
            }
        }
        return nullptr;
    }

    const BoopMorphSpec *ResolveBoopSpecForCount(uint16_t count) const
    {
        // Priority: explicit count rule (e.g. 10 -> XwX), then multiples rule (3 -> Angry), then default (1 -> Surprised).
        if (const BoopMorphSpec *exact = FindBoopSpecByTimes(count))
        {
            return exact;
        }

        if ((count % 3) == 0)
        {
            if (const BoopMorphSpec *multiple = FindBoopSpecByTimes(3))
            {
                return multiple;
            }
        }

        if (const BoopMorphSpec *fallback = FindBoopSpecByTimes(1))
        {
            return fallback;
        }

        return nullptr;
    }

    void ChangeInterpolationMethods()
    {
        // Use cached morph IDs (pre-resolved in LoadAnimationConfig)
        eEA.SetInterpolationMethod(mVrcEeId, EasyEaseInterpolation::Linear);
        eEA.SetInterpolationMethod(mVrcIhId, EasyEaseInterpolation::Linear);
        eEA.SetInterpolationMethod(mVrcDdId, EasyEaseInterpolation::Linear);
        eEA.SetInterpolationMethod(mVrcRrId, EasyEaseInterpolation::Linear);
        eEA.SetInterpolationMethod(mVrcChId, EasyEaseInterpolation::Linear);
        eEA.SetInterpolationMethod(mVrcAaId, EasyEaseInterpolation::Linear);
        eEA.SetInterpolationMethod(mVrcOhId, EasyEaseInterpolation::Linear);
        eEA.SetInterpolationMethod(mVrcSsId, EasyEaseInterpolation::Linear);
    }


    void LoadJsonFaceBlocking()
    {
        const String preferredFacePath = deviceId.length() > 0 ? "/" + deviceId + String("_face.json") : String();
        const String fallbackFacePath = "/universal_face.json";

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
                Serial.printf("[WARN] %s failed to load (attempt %lu); retrying...\n", preferredFacePath.c_str(), ++retryCount);
            }

            if (fallbackExists)
            {
                Serial.printf("[WARN] %s failed to load (attempt %lu); retrying...\n", fallbackFacePath.c_str(), ++retryCount);
            }
            else if (!preferredExists)
            {
                if (!preferredFacePath.isEmpty())
                {
                    Serial.printf("[WARN] Face model not found (%s or %s); waiting for file... (attempt %lu)\n", preferredFacePath.c_str(), fallbackFacePath.c_str(), ++retryCount);
                }
                else
                {
                    Serial.printf("[WARN] %s not found; waiting for file... (attempt %lu)\n", fallbackFacePath.c_str(), ++retryCount);
                }
            }

            delay(500);
            yield(); // Feed watchdog during retry loop
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
        // All expression color layers share one SimpleMaterial — ApplyExpression()
        // changes its RGB to match the active expression's color.
        materialAnimator.AddMaterial(Material::Replace, &expressionColor, 40, 0.0f, 1.0f); // layer 1 — orange
        materialAnimator.AddMaterial(Material::Replace, &expressionColor, 40, 0.0f, 1.0f); // layer 2 — white
        materialAnimator.AddMaterial(Material::Replace, &expressionColor, 40, 0.0f, 1.0f); // layer 3 — green
        materialAnimator.AddMaterial(Material::Replace, &expressionColor, 40, 0.0f, 1.0f); // layer 4 — yellow
        materialAnimator.AddMaterial(Material::Replace, &expressionColor, 40, 0.0f, 1.0f); // layer 5 — purple
        materialAnimator.AddMaterial(Material::Replace, &expressionColor, 40, 0.0f, 1.0f); // layer 6 — red
        materialAnimator.AddMaterial(Material::Replace, &expressionColor, 40, 0.0f, 1.0f); // layer 7 — blue
        materialAnimator.AddMaterial(Material::Replace, &rainbowSpiral, 40, 0.0f, 1.0f);  // layer 8
        // layer 9 removed (was rainbowNoise)

        // Registry for name lookup
        RegisterMaterial("gradientSpectrum", &gradientMat);
        RegisterMaterial("rainbowMat", &rainbowMat);
        RegisterMaterial("backgroundMat", &backgroundMat);
        RegisterMaterial("rainbowSpiral", &rainbowSpiral);
        RegisterMaterial("SpectrumAnalyzer", &sA);
        // Color aliases — all point to the single expressionColor material
        RegisterMaterial("redMat", &expressionColor);
        RegisterMaterial("greenMat", &expressionColor);
        RegisterMaterial("blueMat", &expressionColor);
        RegisterMaterial("yellowMat", &expressionColor);
        RegisterMaterial("purpleMat", &expressionColor);
        RegisterMaterial("whiteMat", &expressionColor);
        RegisterMaterial("orangeMat", &expressionColor);

        RegisterHueShiftable(&gradientMat);
        RegisterHueShiftable(&rainbowMat);
        RegisterHueShiftable(&backgroundMat);
        RegisterHueShiftable(&expressionColor);
        RegisterHueShiftable(&rainbowSpiral);
    }

    // Auto-switch the shared expressionColor material's RGB when a named color
    // alias is selected. Returns true if the name matched a known color.
    bool ApplyExpressionColorByName(const String &name) {
        struct ColorEntry { const char* alias; uint8_t r, g, b; };
        static const ColorEntry kColorMap[] = {
            {"redMat",    255,   0,   0},
            {"orangeMat", 255, 165,   0},
            {"whiteMat",  255, 255, 255},
            {"greenMat",    0, 255,   0},
            {"blueMat",     0,   0, 255},
            {"yellowMat", 255, 255,   0},
            {"purpleMat", 255,   0, 255},
        };
        for (const auto &e : kColorMap) {
            if (name.equalsIgnoreCase(e.alias)) {
                expressionColor.SetRGB(ProtoRGBColor(e.r, e.g, e.b));
                return true;
            }
        }
        return false;
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
        activeBrightness = resetState.brightness;

        // Hide/Show mouth (use cached morph ID)
        if (mHideMouthId != 0xFFFF)
        {
            eEA.AddParameterFrame(mHideMouthId, resetState.show_mouth ? 0.0f : 1.0f);
        }

        ApplySceneEffect(resetState.sceneEffect);

        SetBackgroundMaterial(resetState.hasBackgroundMat ? resetState.backgroundMat : String());

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

        for (const auto &ap : resetState.resolvedAnimParams)
        {
            eEA.AddParameterFrame(ap.first, ap.second);
        }
    }

    Material *ResolveMaterialOrDefault(const String &name, Material *fallback)
    {
        if (name.length() == 0 || name.equalsIgnoreCase("default"))
        {
            return fallback;
        }

        for (auto &entry : materialRegistry)
        {
            if (entry.first.equalsIgnoreCase(name))
            {
                return entry.second;
            }
        }
        return fallback;
    }

    Material *ResolveMaterial(const String &name)
    {
        return ResolveMaterialOrDefault(name, &gradientMat);
    }

    Material *ResolveBackgroundMaterial(const String &name)
    {
        return ResolveMaterialOrDefault(name, &backgroundMat);
    }

    void SetBackgroundMaterial(const String &name)
    {
        Object3D *bgObject = background.GetObject();
        if (!bgObject)
        {
            return;
        }

        if (name.length() == 0 || name.equalsIgnoreCase("default"))
        {
            bgObject->SetMaterial(&backgroundMat);
            bgObject->Disable();
            return;
        }

        bgObject->SetMaterial(ResolveBackgroundMaterial(name));
        bgObject->Enable();
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
        cfg.brightness = obj["brightness"] | -1;
        if (cfg.brightness >= 0)
        {
            cfg.brightness = constrain(cfg.brightness, 0, 255);
        }

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
        ExpressionConfig *target = FindExpressionConfig(name);

        if (!target)
        {
            Serial.printf("[BOOP] Expression '%s' not found in expressions map\n", name.c_str());
            return;
        }

        if (target->reset)
        {
            ApplyResetState();
        }

        // Per-animation brightness: the expression's own value wins; reset
        // expressions without one inherit reset_state's value (applied inside
        // ApplyResetState above); anything else follows the menu/user default.
        if (target->brightness >= 0)
        {
            activeBrightness = target->brightness;
        }
        else if (!target->reset)
        {
            activeBrightness = -1;
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
                Material *mat = ResolveMaterial(target->faceMat);
                faceObject->SetMaterial(mat);
                // Auto-switch the shared expressionColor's RGB when a color
                // alias is selected (redMat→red, blueMat→blue, etc.)
                if (mat == &expressionColor) {
                    ApplyExpressionColorByName(target->faceMat);
                }
            }
        }

        if (target->hasBackgroundMat)
        {
            SetBackgroundMaterial(target->backgroundMat);
        }

        ApplySceneEffect(target->sceneEffect);

        for (const auto &ap : target->resolvedAnimParams)
        {
            eEA.AddParameterFrame(ap.first, ap.second);
        }
    }

    const String *GetExpressionByIndexPtr(uint8_t idx) const
    {
        if (expressionOrder.empty())
        {
            return nullptr;
        }
        if (idx >= expressionOrder.size())
        {
            idx = idx % expressionOrder.size();
        }
        return &expressionOrder[idx];
    }

    bool LoadAnimationConfig(const UserConfig &config)
    {
        deviceId = config.device_id;
        String filename = config.user_animation.length() > 0 ? config.user_animation : (deviceId + String("_animation.json"));
        String path = "/" + filename;
        String fallbackPath = "/example_animation.json";
        String loadedPath;

        // LittleFS should already be mounted by RemoteFileSync::EnsureFsMounted() during setup
        // If not mounted, attempt mounting with timeout protection
        if (!LittleFS.begin(false))
        {
            if (!LittleFS.begin(true))
            {
                Serial.println("[ERROR] LittleFS mount failed for animation config; using minimal default.");
                // Fall through to built-in default below
            }
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

        boopSensorThreshold = 180;
        int parsedBoopThreshold = doc["boop_threshold"] | 200;
        if (parsedBoopThreshold < 0)
        {
            parsedBoopThreshold = doc["boop_sensor_threshold"] | 200;
        }
        if (parsedBoopThreshold >= 0)
        {
            boopSensorThreshold = static_cast<uint8_t>(constrain(parsedBoopThreshold, 0, 255));
        }
        Serial.printf("[INFO] Boop sensor threshold set to %u\n", static_cast<unsigned>(boopSensorThreshold));

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
                String rawName = obj["name"] | String("");
                spec.name = NormalizeBoopExpressionName(rawName.c_str());
                spec.periodMs = std::max<uint32_t>(200, obj["period_ms"] | kDefaultBoopHoldMs);
                if (spec.name.length() > 0)
                {
                    boopMorphSpecs.push_back(spec);
                    BOOP_LOG_PRINTF("[BOOP] Registered: %d times -> '%s' (period=%lu ms)\n",
                                    spec.times,
                                    spec.name.c_str(),
                                    static_cast<unsigned long>(spec.periodMs));
                }
            }
        }

        if (doc.containsKey("boop_window_ms"))
        {
            boopWindowMs = std::max<uint32_t>(1000, doc["boop_window_ms"].as<uint32_t>());
        }
        else if (doc.containsKey("boop_window_seconds"))
        {
            boopWindowMs = std::max<uint32_t>(1000, doc["boop_window_seconds"].as<uint32_t>() * 1000UL);
        }

        boopWindowStart = 0;
        boopLastPulseTime = 0;
        boopCount = 0;
        activeBoopExpressionIndex = -1;
        activeBoopUntil = 0;

        BOOP_LOG_PRINTF("[BOOP] Count window configured: %lu ms\n", static_cast<unsigned long>(boopWindowMs));
        if (boopMorphSpecs.size() == 0)
        {
            BOOP_LOG_PRINTLN("[BOOP] WARNING: No boop_morphs configured in JSON!");
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

        // Fallback: no boop_morphs configured → map a single boop to Surprised
        // so the boop sensor always has a visible effect, even with an
        // animation JSON that lacks boop configuration.
        if (boopMorphSpecs.empty())
        {
            BoopMorphSpec fallback;
            fallback.times = 1;
            fallback.name = NormalizeBoopExpressionName("Surprised");
            fallback.periodMs = kDefaultBoopHoldMs;
            if (fallback.name.length() > 0)
            {
                boopMorphSpecs.push_back(fallback);
                BOOP_LOG_PRINTF("[BOOP] No boop_morphs in JSON — fallback: 1 boop -> '%s' (period=%lu ms)\n",
                                fallback.name.c_str(),
                                static_cast<unsigned long>(fallback.periodMs));
            }
        }

        // Pre-resolve boop morph expression names to indices to avoid per-frame string lookups and heap allocation
        for (auto &spec : boopMorphSpecs)
        {
            spec.expressionIndex = FindExpressionIndex(spec.name);
            if (spec.expressionIndex < 0)
            {
                BOOP_LOG_PRINTF("[BOOP] WARNING: expression '%s' not found in animation JSON — boop mapping inactive\n",
                                spec.name.c_str());
            }
        }

        // Pre-resolve all expression anim_parameter morph names to IDs
        for (auto &expr : expressions)
        {
            expr.second.resolvedAnimParams.clear();
            expr.second.resolvedAnimParams.reserve(expr.second.animParameters.size());
            for (const auto &ap : expr.second.animParameters)
            {
                uint16_t morphId = GetMorphId(ap.first.c_str());
                expr.second.resolvedAnimParams.push_back({morphId, ap.second});
            }
        }

        // Also resolve resetState
        resetState.resolvedAnimParams.clear();
        resetState.resolvedAnimParams.reserve(resetState.animParameters.size());
        for (const auto &ap : resetState.animParameters)
        {
            uint16_t morphId = GetMorphId(ap.first.c_str());
            resetState.resolvedAnimParams.push_back({morphId, ap.second});
        }

        // Reserve containers to prevent reallocation during animation
        expressions.reserve(expressions.size() + 4);
        expressionOrder.reserve(expressionOrder.size() + 4);
        autoLinkSpecs.reserve(autoLinkSpecs.size());
        boopMorphSpecs.reserve(boopMorphSpecs.size());
        flippedMorphs.reserve(flippedMorphs.size());

        // Cache voice viseme and common morph IDs to avoid per-frame string scans
        if (jsonFaceLoaded && jsonFace.Loaded())
        {
            mVrcSsId = GetMorphId("vrc_v_ss");
            mVrcEeId = GetMorphId("vrc_v_ee");
            mVrcIhId = GetMorphId("vrc_v_ih");
            mVrcDdId = GetMorphId("vrc_v_dd");
            mVrcRrId = GetMorphId("vrc_v_rr");
            mVrcChId = GetMorphId("vrc_v_ch");
            mVrcAaId = GetMorphId("vrc_v_aa");
            mVrcOhId = GetMorphId("vrc_v_oh");
            mHideMouthId = GetMorphId("HideMouth");
        }

        return true;
    }

    const String *ResolveBoopExpressionPtr()
    {
        uint32_t now = millis();
        
        // Return active boop expression if still within hold window
        if (activeBoopExpressionIndex >= 0)
        {
            if (now < activeBoopUntil)
            {
                return &expressionOrder[activeBoopExpressionIndex];
            }
            // Hold window expired
            activeBoopExpressionIndex = -1;
        }

        // Only check for new boops if sensor is enabled
        if (!Menu::UseBoopSensor())
        {
            return nullptr;
        }

        // Expire the persistent count window even without new pulses.
        if (boopCount > 0 && boopWindowStart > 0 && (now - boopWindowStart) >= boopWindowMs)
        {
            BOOP_LOG_PRINTF("[BOOP] Window expired after %lu ms, resetting count (%u -> 0)\n",
                            static_cast<unsigned long>(boopWindowMs),
                            static_cast<unsigned>(boopCount));
            boopCount = 0;
            boopWindowStart = 0;
            boopLastPulseTime = 0;
        }

        if (Menu::ConsumeBoopPulse())
        {
            if (boopWindowStart == 0)
            {
                boopWindowStart = now;
                boopCount = 0;
                BOOP_LOG_PRINTF("[BOOP] Starting count window (%lu ms)\n", static_cast<unsigned long>(boopWindowMs));
            }

            boopCount++;
            boopLastPulseTime = now;

            BOOP_LOG_PRINTF("[BOOP] Pulse count=%u elapsed=%lu/%lu ms\n",
                            static_cast<unsigned>(boopCount),
                            static_cast<unsigned long>(now - boopWindowStart),
                            static_cast<unsigned long>(boopWindowMs));

            const BoopMorphSpec *spec = ResolveBoopSpecForCount(boopCount);
            if (!spec)
            {
                BOOP_LOG_PRINTF("[BOOP] No expression mapping for count=%u\n", static_cast<unsigned>(boopCount));
                return nullptr;
            }

            BOOP_LOG_PRINTF("[BOOP] Count %u -> '%s' (hold=%lu ms)\n",
                            static_cast<unsigned>(boopCount),
                            spec->name.c_str(),
                            static_cast<unsigned long>(spec->periodMs));

            activeBoopExpressionIndex = spec->expressionIndex;
            activeBoopUntil = now + spec->periodMs;

            if (activeBoopExpressionIndex >= 0 && activeBoopExpressionIndex < (int16_t)expressionOrder.size())
                return &expressionOrder[activeBoopExpressionIndex];
            return nullptr;
        }

        // No new pulse this frame.
        return nullptr;
    }

public:
    JsonDrivenProtogenAnimation() {}

    void InitializeMenuPeripherals(uint8_t faceCount, uint8_t threshold)
    {
        if (menuInitialized)
        {
            Menu::SetThreshold(threshold);
            return;
        }

        espmenu.Initialize(faceCount, threshold);
        menuInitialized = true;
    }

    bool Initialize(const UserConfig &config, const char *githubAnimBase = nullptr, const char *giteeAnimBase = nullptr, const char *githubToken = nullptr, const char *giteeToken = nullptr, M5GFX *downloadDisplay = nullptr, bool verboseDownload = true)
    {
        // Serial already initialized by main.cpp setup()
        deviceId = config.device_id;

        // Boot-time default brightness comes from the user config; the menu/BLE
        // value starts there and per-animation overrides apply on top of it.
        Menu::SetBrightness(config.user_brightness);
        activeBrightness = -1;

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
        SetBackgroundMaterial(String());

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
        // BLE + gesture are initialized from main.cpp after WiFi shutdown so the
        // controller has a clean internal-DRAM heap.
        ChangeInterpolationMethods();
        return true;
    }

    uint8_t GetBoopSensorThreshold() const { return boopSensorThreshold; }

    uint8_t GetAccentBrightness()
    {
        return Menu::GetAccentBrightness();
    };

    uint8_t GetBrightness()
    {
        // Per-animation brightness override wins; otherwise follow the menu
        // value (BLE/app adjustable, initialised from user config user_brightness).
        return activeBrightness >= 0 ? static_cast<uint8_t>(activeBrightness) : Menu::GetBrightness();
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
        //if (!animationName.isEmpty())
        //{
        //    display->drawString(animationName, 5, 24);
        //}
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

        // MenuUpdate() is now called from main loop (core 1) to avoid I2C
        // contention with the animation task on core 0.

        blurH.SetRatio(fGenBlur.Update());
        blurV.SetRatio(fGenBlur.Update());
        blurR.SetRatio(fGenBlur.Update());

        uint8_t mode = Menu::GetFaceState();

        MicrophoneFourierIT::Update();
        voiceDetection.SetThreshold(map(Menu::GetMicLevel(), 0, 10, 1000, 50));

        if (Menu::GetvoiceDetectionEnable() && voiceEnable)
        {
            eEA.AddParameterFrame(mVrcSsId, MicrophoneFourierIT::GetCurrentMagnitude() / 2.0f);

            if (MicrophoneFourierIT::GetCurrentMagnitude() > 0.05f)
            {
                voiceDetection.Update(MicrophoneFourierIT::GetFourierFiltered(), MicrophoneFourierIT::GetSampleRate());

                eEA.AddParameterFrame(mVrcEeId, voiceDetection.GetViseme(voiceDetection.EE));
                eEA.AddParameterFrame(mVrcIhId, voiceDetection.GetViseme(voiceDetection.AH));
                eEA.AddParameterFrame(mVrcDdId, voiceDetection.GetViseme(voiceDetection.UH));
                eEA.AddParameterFrame(mVrcRrId, voiceDetection.GetViseme(voiceDetection.AR));
                eEA.AddParameterFrame(mVrcChId, voiceDetection.GetViseme(voiceDetection.ER));
                eEA.AddParameterFrame(mVrcAaId, voiceDetection.GetViseme(voiceDetection.AH));
                eEA.AddParameterFrame(mVrcOhId, voiceDetection.GetViseme(voiceDetection.OO));
            }
        }

        // Resolve boop sequence (called every frame for state tracking)
        const String *targetExpr = ResolveBoopExpressionPtr();
        if (!targetExpr)
        {
            targetExpr = GetExpressionByIndexPtr(mode);
        }

        // Intercept animation selection from JSON definitions.
        if (targetExpr && targetExpr->length() > 0)
        {
            ApplyExpression(*targetExpr);
        }

        ApplyHueShiftToCurrentFaceMaterial(Menu::GetHueShift());

        sA.SetHueAngle(ratio * 360.0f * 4.0f);
        sA.SetMirrorYState(Menu::MirrorSpectrumAnalyzer());
        sA.SetFlipYState(!Menu::MirrorSpectrumAnalyzer());

        eEA.Update();

        // Blink must run after eEA so the BlinkTrack overrides the auto-linked
        // Blink/SEyeBlink morph weights that eEA would otherwise zero out.
        if (enableBlink)
        {
            blink.Update();
        }

        UpdateFace();

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

        faceTransform->SetRotation(Vector3D(0.0f, 0.0f, 7.5f));

        const float scale = 17.5f * 0.5f + 0.45f;
        faceTransform->SetPosition(Vector3D(baseXOffset - 25.0f - xOffset, baseYOffset + yOffset, 550.0f));
        faceTransform->SetScale(Vector3D(0.975f, 0.59f, -0.65f).Multiply(scale));

        GetFaceObject()->UpdateTransform();
    }
};
