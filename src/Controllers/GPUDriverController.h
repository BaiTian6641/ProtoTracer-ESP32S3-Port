/**
 * @file GPUDriverController.h
 * @brief Controller that offloads rendering to the RP2350 GPU via ProtoGL.
 *
 * Instead of rasterizing locally (like TasESP32S3KitV1), this controller
 * encodes the scene into ProtoGL wire-format commands and DMA-transfers them
 * to the RP2350 GPU over Octal SPI.  The GPU performs rasterisation, effect
 * processing, and HUB75 output autonomously.
 *
 * M2 deliverable — ProtoGL API v0.3 (FROZEN)
 *
 * Usage:
 *   PglDeviceConfig cfg;
 *   cfg.spiDataPins = {...};
 *   cfg.spiClkPin   = LCD_CLK;
 *   cfg.spiCsPin    = LCD_CS;
 *   cfg.dirPin      = LCD_DIR;
 *   cfg.irqPin      = GPU_IRQ;
 *   cfg.i2cSdaPin   = GPU_SDA;
 *   cfg.i2cSclPin   = GPU_SCL;
 *
 *   GPUDriverController controller(cameras, 2, 128, 64, cfg);
 *   controller.Initialize();
 *   // In loop:
 *   controller.Render(scene);   // encodes + DMA
 *   controller.Display();       // no-op (GPU drives HUB75)
 */

#pragma once

#include "Controller.h"
#include "../Render/Camera.h"
#include "../Render/Scene.h"
#include "../Render/Object3D.h"

#include "../Screenspace/Effect.h"
#include "../Screenspace/HorizontalBlur.h"
#include "../Screenspace/VerticalBlur.h"
#include "../Screenspace/RadialBlur.h"
#include "../Screenspace/PhaseOffsetX.h"
#include "../Screenspace/PhaseOffsetY.h"
#include "../Screenspace/PhaseOffsetR.h"
#include "../Screenspace/EdgeFeatherEffect.h"
#include "../Screenspace/AntiAliasingEffect.h"

#include <ProtoGL.h>
#include <new>

// Material subclass includes for AutoRegister template dispatch
#include "../Materials/SimpleMaterial.h"
#include "../Materials/NormalMaterial.h"
#include "../Materials/DepthMaterial.h"
#include "../Materials/GradientMaterial.h"
#include "../Materials/LightMaterial.h"
#include "../Materials/SimplexNoise.h"

// ─── Resource Tracking ──────────────────────────────────────────────────────

/// Tracks the mapping between a ProtoTracer Object3D and a GPU-side mesh.
struct GpuMeshRecord {
    Object3D*   object   = nullptr;
    PglMesh     meshId   = PGL_INVALID_MESH;
    uint32_t    vertHash = 0;          ///< FNV-1a hash of vertex data for dirty check
    uint16_t    vertCount = 0;
    bool        created  = false;

    // Cached world-space AABB — updated when vertex hash changes.
    // Used for host-side frustum culling to avoid sending off-screen objects.
    Vector3D    aabbMin;
    Vector3D    aabbMax;
    bool        aabbValid = false;
};

/// Tracks the mapping between a ProtoTracer Material* and a GPU-side material.
struct GpuMaterialRecord {
    Material*       material   = nullptr;
    PglMaterial     materialId = PGL_INVALID_MATERIAL;
    PglMaterialType type       = PGL_MAT_SIMPLE;
    bool            created    = false;
    bool            registered = false;   ///< true if user called RegisterMaterial()
    uint8_t         paramBuf[64]{};       ///< cached material params (sent to GPU)
    uint16_t        paramSize  = 0;
    uint32_t        paramHash  = 0;       ///< FNV-1a hash of paramBuf for dirty check
};

/// Tracks pixel layout upload state per camera.
struct GpuLayoutRecord {
    bool uploaded = false;
};

// ─── GPU Driver Controller ──────────────────────────────────────────────────

class GPUDriverController : public Controller {
public:
    /**
     * @param cameras             Array of CameraBase* (owned by caller).
     * @param cameraCount         Number of cameras.
     * @param maxBrightness       Max brightness for soft-start.
     * @param maxAccentBrightness Max accent brightness.
     * @param gpuConfig           PglDevice configuration (pin mapping, clock, etc.)
     */
    GPUDriverController(CameraBase** cameras, uint8_t cameraCount,
                        uint8_t maxBrightness, uint8_t maxAccentBrightness,
                        const PglDeviceConfig& gpuConfig)
        : Controller(cameras, cameraCount, maxBrightness, maxAccentBrightness)
        , gpuConfig_(gpuConfig)
    {}

    ~GPUDriverController() override = default;

    // ─── Controller Interface ───────────────────────────────────────────

    void Initialize() override {
        if (!device_.Initialize(gpuConfig_)) {
            Serial.println("[GPUDriver] PglDevice init FAILED");
            return;
        }

        // Query GPU to verify connectivity
        PglCapabilityResponse cap = device_.QueryCapability();
        if (cap.maxVertices > 0) {
            gpuMaxVertices_  = cap.maxVertices;
            gpuMaxTriangles_ = cap.maxTriangles;
            gpuMaxMeshes_    = cap.maxMeshes;
            gpuMaxMaterials_ = cap.maxMaterials;
            gpuCapFlags_     = cap.capFlags;
            Serial.printf("[GPUDriver] GPU: proto v%u, %u cores @ %u MHz, %u verts, %u tris, %uKB SRAM\n",
                          cap.protoVersion, cap.coreCount, cap.coreFreqMHz,
                          cap.maxVertices, cap.maxTriangles, cap.sramKB);

            // Log VRAM detection results
            if (cap.capFlags & PGL_CAP_OPI_VRAM) {
                Serial.println("[GPUDriver]   External VRAM: PIO2 memory detected (OPI PSRAM or QSPI MRAM)");
            }
            if (cap.capFlags & PGL_CAP_QSPI_VRAM) {
                Serial.println("[GPUDriver]   External VRAM: QSPI memory detected on CS1");
                // Chip type will be reported in extended status (qspiChipType byte)
            }
            if (!(cap.capFlags & (PGL_CAP_OPI_VRAM | PGL_CAP_QSPI_VRAM))) {
                Serial.println("[GPUDriver]   External VRAM: none detected (SRAM only)");
            }
            if (cap.capFlags & PGL_CAP_DYNAMIC_CLOCK) {
                Serial.println("[GPUDriver]   Dynamic clock adjustment: supported");
            }
            if (cap.capFlags & PGL_CAP_TEMP_SENSOR) {
                Serial.println("[GPUDriver]   On-die temperature sensor: available");
            }
        } else {
            Serial.println("[GPUDriver] GPU capability query returned zero — using defaults");
        }

        // Set initial brightness via I2C
        device_.SetBrightness(brightness);

        Serial.println("[GPUDriver] Initialized");
    }

    /**
     * @brief Forward brightness to GPU via I2C; periodically log diagnostics.
     *
     * The GPU drives HUB75 directly — no local framebuffer output needed.
     * Every kDiagIntervalFrames, queries extended status for temperature,
     * GPU usage, clock frequency, and VRAM utilisation if available.
     */
    void Display() override {
        if (lastBrightness_ != brightness) {
            device_.SetBrightness(brightness);
            lastBrightness_ = brightness;
        }

        // Periodic GPU health diagnostics
        if (frameNumber_ > 0 && (frameNumber_ % kDiagIntervalFrames) == 0) {
            uint32_t dropped  = device_.GetDroppedFrames();
            uint32_t overflow = device_.GetOverflowFrames();
            uint32_t stalls   = device_.GetGpuStalls();

            // Basic transport stats (always available)
            if (dropped > lastDiagDropped_ || overflow > lastDiagOverflow_ || stalls > 0) {
                Serial.printf("[GPU] frame %lu | dropped %lu (+%lu) | overflow %lu (+%lu) | stalls %lu\n",
                              (unsigned long)frameNumber_,
                              (unsigned long)dropped,  (unsigned long)(dropped - lastDiagDropped_),
                              (unsigned long)overflow, (unsigned long)(overflow - lastDiagOverflow_),
                              (unsigned long)stalls);
                lastDiagDropped_  = dropped;
                lastDiagOverflow_ = overflow;
            }

            // Extended status (temperature, GPU usage, clock, VRAM)
            PglExtendedStatusResponse ext = device_.QueryExtendedStatus();
            lastExtStatus_ = ext;

            float tempC = static_cast<float>(ext.temperatureQ8) / 256.0f;

            Serial.printf("[GPU] %u FPS | GPU %u%% | C0 %u%% C1 %u%% | %.1f°C | %u MHz\n",
                          ext.currentFPS, ext.gpuUsagePercent,
                          ext.core0UsagePercent, ext.core1UsagePercent,
                          tempC, ext.currentClockMHz);

            // Frame timing details
            Serial.printf("[GPU] frame %uus (raster %uus + xfer %uus) | HUB75 %u Hz\n",
                          ext.frameTimeUs, ext.rasterTimeUs,
                          ext.transferTimeUs, ext.hub75RefreshHz);

            // VRAM utilisation (only if external memory is present)
            if (ext.vramTierFlags & (PGL_VRAM_OPI_DETECTED | PGL_VRAM_QSPI_DETECTED)) {
                Serial.printf("[GPU] SRAM free %uKB", ext.sramFreeKB);
                if (ext.vramTierFlags & PGL_VRAM_OPI_DETECTED) {
                    Serial.printf(" | PIO2 %u/%uKB", ext.opiVramFreeKB, ext.opiVramTotalKB);
                }
                if (ext.vramTierFlags & PGL_VRAM_QSPI_DETECTED) {
                    const char* chipName = "unknown";
                    switch (ext.qspiChipType) {
                        case PGL_QSPI_CHIP_MRAM_MR10Q010:  chipName = "MRAM"; break;
                        case PGL_QSPI_CHIP_PSRAM_APS6408L: chipName = "PSRAM"; break;
                        case PGL_QSPI_CHIP_PSRAM_ESP:      chipName = "ESP-PSRAM"; break;
                        default: break;
                    }
                    Serial.printf(" | QSPI(%s) %u/%uKB",
                                  chipName, ext.qspiVramFreeKB, ext.qspiVramTotalKB);
                }
                Serial.println();
            }
        }
    }

    /**
     * @brief Encode the scene into ProtoGL commands and DMA-transfer to GPU.
     *
     * This HIDES the base-class non-virtual Render(Scene*).
     * The call site in main.cpp uses the concrete GPUDriverController type,
     * so C++ name lookup resolves to this version.
     */
    void Render(Scene* scene) {
        if (!device_.IsInitialized() || !scene) return;

        device_.BeginFrame(frameNumber_, static_cast<uint32_t>(micros()));
        PglEncoder* enc = device_.GetEncoder();
        if (!enc) return;

        // ── 1. Set Cameras + Pixel Layouts ──────────────────────────────
        uint8_t camCount = GetCameraCount();
        CameraBase** cams = GetCameras();

        for (uint8_t ci = 0; ci < camCount && ci < PGL_MAX_CAMERAS; ++ci) {
            Camera* cam = static_cast<Camera*>(cams[ci]);
            EncodeCamera(cam, ci, enc);
        }

        // ── 2. Encode Objects (Create/Update meshes & materials, Draw) ──
        Object3D** objects = scene->GetObjects();
        unsigned int objCount = scene->GetObjectCount();

        // Cache camera parameters for host-side frustum culling.
        // The GPU uses: fovFactor = screenW * 0.5, screenW = 128, screenH = 64
        // Camera 0 is typically the main face camera.
        // GPU computes: camRot = QuatMul(cam.rotation, cam.baseRotation)
        // where cam.rotation = layout->GetRotation() and
        //       cam.baseRotation = transform->GetBaseRotation()
        // (see PrepareFrame in rasterizer.cpp).
        Vector3D cullCamPos;
        Quaternion cullCamRot;
        bool cullCamValid = false;
        if (camCount > 0) {
            Camera* cam0 = static_cast<Camera*>(cams[0]);
            cullCamPos = cam0->GetTransform()->GetPosition();
            // Match GPU: QuatMul(rotation, baseRotation)
            Quaternion layoutRot = cam0->GetCameraLayout()->GetRotation();
            Quaternion baseRot   = cam0->GetTransform()->GetBaseRotation();
            cullCamRot = layoutRot * baseRot;
            cullCamValid = true;
        }

        for (unsigned int oi = 0; oi < objCount; ++oi) {
            Object3D* obj = objects[oi];
            if (!obj) continue;

            // ── Host-side frustum cull ───────────────────────────────
            // Skip objects that are completely off-screen to save SPI
            // bandwidth (no CMD_UPDATE_VERTICES or CMD_DRAW_OBJECT sent).
            // The mesh must have been created first (so the GPU has it),
            // but we can skip the draw + vertex update if not visible.
            bool meshAlreadyCreated = false;
            int existingRecordIdx = -1;
            for (uint16_t i = 0; i < meshCount_; ++i) {
                if (meshRecords_[i].object == obj) {
                    meshAlreadyCreated = meshRecords_[i].created;
                    existingRecordIdx = static_cast<int>(i);
                    break;
                }
            }

            // Skip disabled objects entirely if mesh already created
            if (!obj->IsEnabled() && meshAlreadyCreated) continue;

            // Frustum cull: if mesh exists and has a valid AABB, test visibility
            if (meshAlreadyCreated && existingRecordIdx >= 0 &&
                meshRecords_[existingRecordIdx].aabbValid && cullCamValid) {
                if (!IsObjectVisible(obj, meshRecords_[existingRecordIdx], cullCamPos, cullCamRot)) {
                    continue;  // entirely off-screen — skip encoding
                }
            }

            PglMesh     meshId = FindOrCreateMesh(obj, enc);
            PglMaterial matId  = FindOrCreateMaterial(obj->GetMaterial(), enc);

            if (meshId == PGL_INVALID_MESH) continue;

            EncodeDrawObject(obj, meshId, matId, enc);
        }

        // ── 3. Encode screen-space effects ─────────────────────────────
        //
        // Maps ProtoTracer Effect subclasses to ProtoGL shader commands.
        // Each effect is sent to all cameras on shader slot 0.
        // When effects are disabled, we clear slot 0 to remove any
        // previously active shader.

        if (scene->UseEffect() && scene->GetEffect()) {
            Effect* fx = scene->GetEffect();
            EncodeEffect(fx, camCount, enc);
        } else if (lastEffectType_ != EffectType::Unknown) {
            // Effects were disabled — clear shader slot on all cameras
            for (uint8_t ci = 0; ci < camCount && ci < PGL_MAX_CAMERAS; ++ci) {
                enc->ClearShader(static_cast<PglCamera>(ci), 0);
            }
            lastEffectType_ = EffectType::Unknown;
        }

        // ── 4. Finalize & DMA ───────────────────────────────────────────
        device_.EndFrame();
        frameNumber_++;
    }

    // ─── Material Registration ──────────────────────────────────────────
    //
    // Since ProtoTracer's Material base class has no virtual GetType(),
    // the caller must register materials with their PglMaterialType mapping.
    // Unregistered materials default to PGL_MAT_PRERENDERED (fallback).

    /**
     * @brief Register a material's ProtoGL type so the encoder can create
     *        the correct GPU-side resource.
     *
     * @param mat       Pointer to the ProtoTracer material instance.
     * @param type      ProtoGL material type enum.
     * @param params    Material-specific parameter struct (e.g. PglParamSimple).
     * @param paramSize Size of the params struct in bytes (max 32).
     */
    void RegisterMaterial(Material* mat, PglMaterialType type,
                          const void* params = nullptr, uint16_t paramSize = 0) {
        if (!mat) return;

        // Check if already tracked
        for (uint16_t i = 0; i < materialCount_; ++i) {
            if (materialRecords_[i].material == mat) {
                materialRecords_[i].type       = type;
                materialRecords_[i].registered = true;
                if (params && paramSize > 0 && paramSize <= sizeof(materialRecords_[i].paramBuf)) {
                    std::memcpy(materialRecords_[i].paramBuf, params, paramSize);
                    materialRecords_[i].paramSize = paramSize;
                }
                return;
            }
        }

        // New entry
        if (materialCount_ < kMaxMaterials) {
            GpuMaterialRecord& rec = materialRecords_[materialCount_++];
            rec.material   = mat;
            rec.materialId = PGL_INVALID_MATERIAL;  // Assigned on first encode
            rec.type       = type;
            rec.registered = true;
            rec.created    = false;
            if (params && paramSize > 0 && paramSize <= sizeof(rec.paramBuf)) {
                std::memcpy(rec.paramBuf, params, paramSize);
                rec.paramSize = paramSize;
            }
        }
    }

    /// Convenience: register a SimpleMaterial.
    void RegisterSimpleMaterial(Material* mat, uint8_t r, uint8_t g, uint8_t b) {
        PglParamSimple p{};
        p.r = r;
        p.g = g;
        p.b = b;
        RegisterMaterial(mat, PGL_MAT_SIMPLE, &p, sizeof(p));
    }

    /// Convenience: register a NormalMaterial (no params needed).
    void RegisterNormalMaterial(Material* mat) {
        RegisterMaterial(mat, PGL_MAT_NORMAL, nullptr, 0);
    }

    /// Convenience: register a DepthMaterial.
    void RegisterDepthMaterial(Material* mat,
                               uint8_t nearR, uint8_t nearG, uint8_t nearB,
                               uint8_t farR, uint8_t farG, uint8_t farB,
                               float nearZ, float farZ) {
        PglParamDepth p{};
        p.nearR = nearR; p.nearG = nearG; p.nearB = nearB;
        p.farR  = farR;  p.farG  = farG;  p.farB  = farB;
        p.nearZ = nearZ; p.farZ  = farZ;
        RegisterMaterial(mat, PGL_MAT_DEPTH, &p, sizeof(p));
    }

    /// Convenience: register a GradientMaterial (up to 7 stops).
    void RegisterGradientMaterial(Material* mat,
                                  const PglGradientStop* stops,
                                  uint8_t stopCount,
                                  uint8_t axis,
                                  float rangeMin,
                                  float rangeMax) {
        if (!stops || stopCount == 0) return;
        if (stopCount > 7) stopCount = 7;

        uint8_t payload[64]{};
        uint16_t offset = 0;

        PglParamGradientHeader hdr{};
        hdr.stopCount = stopCount;
        std::memcpy(payload + offset, &hdr, sizeof(hdr));
        offset += sizeof(hdr);

        const uint16_t stopsBytes = static_cast<uint16_t>(stopCount * sizeof(PglGradientStop));
        std::memcpy(payload + offset, stops, stopsBytes);
        offset += stopsBytes;

        payload[offset++] = axis;
        std::memcpy(payload + offset, &rangeMin, sizeof(rangeMin));
        offset += sizeof(rangeMin);
        std::memcpy(payload + offset, &rangeMax, sizeof(rangeMax));
        offset += sizeof(rangeMax);

        RegisterMaterial(mat, PGL_MAT_GRADIENT, payload, offset);
    }

    /// Convenience: register a LightMaterial.
    void RegisterLightMaterial(Material* mat,
                               float dirX, float dirY, float dirZ,
                               uint8_t ambR, uint8_t ambG, uint8_t ambB,
                               uint8_t difR, uint8_t difG, uint8_t difB) {
        PglParamLight p{};
        p.lightDirX = dirX; p.lightDirY = dirY; p.lightDirZ = dirZ;
        p.ambientR = ambR; p.ambientG = ambG; p.ambientB = ambB;
        p.diffuseR = difR; p.diffuseG = difG; p.diffuseB = difB;
        RegisterMaterial(mat, PGL_MAT_LIGHT, &p, sizeof(p));
    }

    /// Convenience: register a SimplexNoise material.
    void RegisterSimplexNoiseMaterial(Material* mat,
                                      float scaleX, float scaleY, float scaleZ,
                                      float speed,
                                      uint8_t colorAR, uint8_t colorAG, uint8_t colorAB,
                                      uint8_t colorBR, uint8_t colorBG, uint8_t colorBB) {
        PglParamSimplexNoise p{};
        p.scaleX = scaleX; p.scaleY = scaleY; p.scaleZ = scaleZ;
        p.speed  = speed;
        p.colorAR = colorAR; p.colorAG = colorAG; p.colorAB = colorAB;
        p.colorBR = colorBR; p.colorBG = colorBG; p.colorBB = colorBB;
        RegisterMaterial(mat, PGL_MAT_SIMPLEX_NOISE, &p, sizeof(p));
    }

    /// Convenience: register a RainbowNoise material.
    void RegisterRainbowNoiseMaterial(Material* mat, float scale, float speed) {
        PglParamRainbowNoise p{};
        p.scale = scale;
        p.speed = speed;
        RegisterMaterial(mat, PGL_MAT_RAINBOW_NOISE, &p, sizeof(p));
    }

    /// Convenience: register an Image material (texture-mapped).
    void RegisterImageMaterial(Material* mat, uint16_t textureId,
                               float offsetX = 0.0f, float offsetY = 0.0f,
                               float scaleX = 1.0f, float scaleY = 1.0f) {
        PglParamImage p{};
        p.textureId = textureId;
        p.offsetX = offsetX; p.offsetY = offsetY;
        p.scaleX  = scaleX;  p.scaleY  = scaleY;
        RegisterMaterial(mat, PGL_MAT_IMAGE, &p, sizeof(p));
    }

    /// Convenience: register a CombineMaterial (blend two materials).
    void RegisterCombineMaterial(Material* mat,
                                 PglMaterial materialIdA, PglMaterial materialIdB,
                                 PglBlendMode blendMode, float opacity = 1.0f) {
        PglParamCombine p{};
        p.materialIdA = materialIdA;
        p.materialIdB = materialIdB;
        p.blendMode   = static_cast<uint8_t>(blendMode);
        p.opacity     = opacity;
        RegisterMaterial(mat, PGL_MAT_COMBINE, &p, sizeof(p));
    }

    /// Convenience: register a MaterialMask.
    void RegisterMaskMaterial(Material* mat,
                              PglMaterial baseMaterialId,
                              PglMaterial maskMaterialId,
                              float threshold = 0.5f) {
        PglParamMask p{};
        p.baseMaterialId = baseMaterialId;
        p.maskMaterialId = maskMaterialId;
        p.threshold      = threshold;
        RegisterMaterial(mat, PGL_MAT_MASK, &p, sizeof(p));
    }

    /// Convenience: register a MaterialAnimator (lerps between two materials).
    void RegisterAnimatorMaterial(Material* mat,
                                  PglMaterial materialIdA, PglMaterial materialIdB,
                                  uint8_t interpMode = 0, float ratio = 0.5f) {
        PglParamAnimator p{};
        p.materialIdA = materialIdA;
        p.materialIdB = materialIdB;
        p.interpMode  = interpMode;
        p.ratio       = ratio;
        RegisterMaterial(mat, PGL_MAT_ANIMATOR, &p, sizeof(p));
    }

    /// Convenience: register a PreRendered material (pre-baked texture).
    void RegisterPreRenderedMaterial(Material* mat, uint16_t textureId) {
        PglParamPreRendered p{};
        p.textureId = textureId;
        RegisterMaterial(mat, PGL_MAT_PRERENDERED, &p, sizeof(p));
    }

    /**
     * @brief Update a registered material's params without changing its type.
     *
     * Call this each frame for materials with dynamic properties (e.g.,
     * MaterialAnimator ratio changes, SimplexNoise speed tweaks).
     * The dirty check in FindOrCreateMaterial() will detect the change
     * and send CMD_UPDATE_MATERIAL to the GPU.
     */
    void UpdateMaterialParams(Material* mat, const void* params, uint16_t paramSize) {
        if (!mat || !params || paramSize == 0) return;
        for (uint16_t i = 0; i < materialCount_; ++i) {
            if (materialRecords_[i].material == mat) {
                if (paramSize <= sizeof(materialRecords_[i].paramBuf)) {
                    std::memcpy(materialRecords_[i].paramBuf, params, paramSize);
                    materialRecords_[i].paramSize = paramSize;
                }
                return;
            }
        }
    }

    // ─── Compile-Time Auto-Registration ─────────────────────────────────
    //
    // Deduces PglMaterialType from the concrete C++ type at compile time.
    // No RTTI required — uses template overloads.
    //
    //   controller.AutoRegister(&mySimpleMat);           // auto-detects SimpleMaterial
    //   controller.AutoRegister(&myGradient);            // auto-detects GradientMaterial
    //   controller.AutoRegister(&unknownMat);            // fallback to PGL_MAT_PRERENDERED

    /// Fallback: any unknown Material subclass → PGL_MAT_PRERENDERED.
    void AutoRegister(Material* mat) {
        RegisterMaterial(mat, PGL_MAT_PRERENDERED);
    }

    /// SimpleMaterial → PGL_MAT_SIMPLE (extracts rgb from the material).
    void AutoRegister(SimpleMaterial* mat) {
        ProtoRGBColor c = mat->GetRGB(Vector3D(), Vector3D(), Vector3D());
        RegisterSimpleMaterial(mat, c.R, c.G, c.B);
    }

    /// NormalMaterial → PGL_MAT_NORMAL (no params).
    void AutoRegister(NormalMaterial* mat) {
        RegisterNormalMaterial(mat);
    }

    /// DepthMaterial → PGL_MAT_DEPTH.
    void AutoRegister(DepthMaterial* mat) {
        // DepthMaterial stores axis, depth, zOffset in its constructor
        // Default registration — params populated at creation time
        RegisterMaterial(mat, PGL_MAT_DEPTH);
    }

    /// LightMaterial → PGL_MAT_LIGHT (default lighting params).
    void AutoRegister(LightMaterial* mat) {
        RegisterMaterial(mat, PGL_MAT_LIGHT);
    }

    /**
     * @brief Batch-register all materials from a scene's objects.
     *
     * Iterates every Object3D in the scene and ensures their materials
     * are tracked.  Materials already registered are skipped (idempotent).
     * Unrecognised types default to PGL_MAT_PRERENDERED.
     *
     * Call once (or after scene composition changes) before the render loop.
     */
    void RegisterSceneMaterials(Scene* scene) {
        if (!scene) return;
        Object3D** objects = scene->GetObjects();
        unsigned int count = scene->GetObjectCount();

        for (unsigned int i = 0; i < count; ++i) {
            if (!objects[i]) continue;
            Material* mat = objects[i]->GetMaterial();
            if (!mat) continue;

            // Skip if already registered
            bool found = false;
            for (uint16_t j = 0; j < materialCount_; ++j) {
                if (materialRecords_[j].material == mat) { found = true; break; }
            }
            if (found) continue;

            // Register with fallback type
            AutoRegister(mat);
        }
    }

    // ─── Device Access (for diagnostics) ────────────────────────────────

    PglDevice& GetDevice() { return device_; }
    const PglDevice& GetDevice() const { return device_; }
    uint32_t GetFrameNumber() const { return frameNumber_; }

    /**
     * @brief Query the GPU's full extended status (temperature, usage, VRAM,
     *        clock, frame timing).
     *
     * Convenience wrapper that also caches the most recent response.
     */
    PglExtendedStatusResponse QueryGpuHealth() {
        lastExtStatus_ = device_.QueryExtendedStatus();
        return lastExtStatus_;
    }

    /// Return the most recently cached extended status (no I2C transaction).
    const PglExtendedStatusResponse& GetCachedGpuHealth() const {
        return lastExtStatus_;
    }

    /// Get GPU die temperature in °C from the last extended status query.
    float GetGpuTemperature() const {
        return static_cast<float>(lastExtStatus_.temperatureQ8) / 256.0f;
    }

    /// Get GPU usage percentage (0–100) from the last extended status query.
    uint8_t GetGpuUsagePercent() const {
        return lastExtStatus_.gpuUsagePercent;
    }

    /// Get current GPU clock in MHz from the last extended status query.
    uint16_t GetGpuClockMHz() const {
        return lastExtStatus_.currentClockMHz;
    }

    /// True if any external VRAM was detected at boot (cached from capability query).
    bool HasExternalVram() const {
        return (gpuCapFlags_ & (PGL_CAP_OPI_VRAM | PGL_CAP_QSPI_VRAM)) != 0;
    }

    /**
     * @brief Request a GPU core clock change.
     *
     * @param targetMHz  One of 150, 200, 250, 266, 300  (0 = query only).
     * @param autoVolt   true = let GPU pick the correct VREG voltage.
     */
    void SetGpuClock(uint16_t targetMHz, bool autoVolt = true) {
        uint8_t flags = PGL_CLOCK_RECONFIGURE_PIO;
        device_.SetClockFrequency(targetMHz, autoVolt ? 0 : 0xFF, flags);
    }

private:
    // ─── Constants ──────────────────────────────────────────────────────

    static constexpr uint16_t kMaxMeshes    = 128;
    static constexpr uint16_t kMaxMaterials = 128;

    // ─── Conversion Helpers ─────────────────────────────────────────────

    static PglVec3 ToWire(const Vector3D& v) {
        return { v.X, v.Y, v.Z };
    }

    static PglQuat ToWire(const Quaternion& q) {
        return { q.W, q.X, q.Y, q.Z };
    }

    static PglVec2 ToWire(const Vector2D& v) {
        return { v.X, v.Y };
    }

    static PglIndex3 ToWireIndex(const IndexGroup& ig) {
        return { static_cast<uint16_t>(ig.A),
                 static_cast<uint16_t>(ig.B),
                 static_cast<uint16_t>(ig.C) };
    }

    // ─── FNV-1a Hash for Dirty Tracking ─────────────────────────────────

    static uint32_t HashVertices(const Vector3D* verts, int count) {
        uint32_t hash = 0x811c9dc5u;
        const uint8_t* data = reinterpret_cast<const uint8_t*>(verts);
        const size_t len = static_cast<size_t>(count) * sizeof(Vector3D);
        for (size_t i = 0; i < len; ++i) {
            hash ^= data[i];
            hash *= 0x01000193u;
        }
        return hash;
    }

    /// FNV-1a hash of an arbitrary byte buffer.
    static uint32_t HashBytes(const uint8_t* data, size_t len) {
        uint32_t hash = 0x811c9dc5u;
        for (size_t i = 0; i < len; ++i) {
            hash ^= data[i];
            hash *= 0x01000193u;
        }
        return hash;
    }

    // ─── Camera Encoding ────────────────────────────────────────────────

    void EncodeCamera(Camera* cam, uint8_t camId, PglEncoder* enc) {
        Transform* t = cam->GetTransform();
        CameraLayout* layout = cam->GetCameraLayout();

        enc->SetCamera(
            static_cast<PglCamera>(camId),
            static_cast<PglLayout>(camId),  // Layout ID = Camera ID for simplicity
            ToWire(t->GetPosition()),
            ToWire(layout->GetRotation()),
            ToWire(t->GetScale()),
            ToWire(Quaternion()),            // lookOffset — identity for now
            ToWire(t->GetBaseRotation()),
            false                            // is2D — 3D by default
        );

        // Upload pixel layout once (first frame only)
        if (camId < PGL_MAX_CAMERAS && !layoutRecords_[camId].uploaded) {
            UploadPixelLayout(cam, camId, enc);
            layoutRecords_[camId].uploaded = true;
        }
    }

    void UploadPixelLayout(Camera* cam, uint8_t camId, PglEncoder* enc) {
        PixelGroup* pg = cam->GetPixelGroup();
        if (!pg) return;

        const unsigned int pixelCount = pg->GetPixelCount();
        const Vector2D* coords = pg->GetCoordinateArray();

        if (coords && pixelCount > 0) {
            // Convert Vector2D[] to PglVec2[] (same layout, but type-safe copy)
            // Vector2D has {float X, float Y} and PglVec2 has {float x, float y}
            // Layout is identical — use reinterpret for zero-copy if Vector2D is
            // packed the same way.  For safety, we do a typed copy into a temp buffer.
            //
            // For large layouts (>1024 pixels), we chunk to avoid stack overflow.
            static constexpr uint16_t kChunkSize = 256;
            PglVec2 chunk[kChunkSize];

            if (pixelCount <= kChunkSize) {
                for (unsigned int i = 0; i < pixelCount; ++i) {
                    chunk[i] = ToWire(coords[i]);
                }
                enc->SetPixelLayoutIrregular(
                    static_cast<PglLayout>(camId),
                    chunk,
                    static_cast<uint16_t>(pixelCount),
                    pg->IsCoordinateArrayReversed()
                );
            } else {
                // For large layouts, send as irregular with a heap-allocated temp
                PglVec2* tmp = new (std::nothrow) PglVec2[pixelCount];
                if (tmp) {
                    for (unsigned int i = 0; i < pixelCount; ++i) {
                        tmp[i] = ToWire(coords[i]);
                    }
                    enc->SetPixelLayoutIrregular(
                        static_cast<PglLayout>(camId),
                        tmp,
                        static_cast<uint16_t>(pixelCount),
                        pg->IsCoordinateArrayReversed()
                    );
                    delete[] tmp;
                }
            }
        }
    }

    // ─── Mesh Management ────────────────────────────────────────────────

    PglMesh FindOrCreateMesh(Object3D* obj, PglEncoder* enc) {
        // Look up existing record
        for (uint16_t i = 0; i < meshCount_; ++i) {
            if (meshRecords_[i].object == obj) {
                return UpdateMeshIfDirty(i, enc);
            }
        }

        // Create new mesh
        if (meshCount_ >= kMaxMeshes) return PGL_INVALID_MESH;

        TriangleGroup* tg = obj->GetTriangleGroup();
        if (!tg) return PGL_INVALID_MESH;

        const PglMesh meshId = nextMeshId_++;
        GpuMeshRecord& rec = meshRecords_[meshCount_++];
        rec.object    = obj;
        rec.meshId    = meshId;
        rec.vertCount = static_cast<uint16_t>(tg->GetVertexCount());

        // Build wire-format vertex + index arrays
        const int vertCount = tg->GetVertexCount();
        const int triCount  = tg->GetTriangleCount();
        const Vector3D* srcVerts = tg->GetVertices();
        const IndexGroup* srcIdx = tg->GetIndexGroup();

        // Hash for dirty tracking
        rec.vertHash = HashVertices(srcVerts, vertCount);
        rec.created  = true;

        // Compute cached AABB for host-side frustum culling
        ComputeMeshAABB(rec);

        // Convert vertices
        PglVec3* wireVerts = new (std::nothrow) PglVec3[vertCount];
        PglIndex3* wireIdx = new (std::nothrow) PglIndex3[triCount];
        if (!wireVerts || !wireIdx) {
            delete[] wireVerts;
            delete[] wireIdx;
            return PGL_INVALID_MESH;
        }

        for (int i = 0; i < vertCount; ++i) {
            wireVerts[i] = ToWire(srcVerts[i]);
        }
        for (int i = 0; i < triCount; ++i) {
            wireIdx[i] = ToWireIndex(srcIdx[i]);
        }

        // Check for UV data
        Vector2D* uvVerts = tg->GetUVVertices();
        bool hasUV = (uvVerts != nullptr);

        // TODO: UV index groups — for now, pass nullptr
        enc->CreateMesh(
            meshId,
            wireVerts, static_cast<uint16_t>(vertCount),
            wireIdx,   static_cast<uint16_t>(triCount),
            false, nullptr, 0, nullptr   // UV support deferred
        );

        delete[] wireVerts;
        delete[] wireIdx;

        return meshId;
    }

    PglMesh UpdateMeshIfDirty(uint16_t recordIdx, PglEncoder* enc) {
        GpuMeshRecord& rec = meshRecords_[recordIdx];
        Object3D* obj = rec.object;
        TriangleGroup* tg = obj->GetTriangleGroup();
        if (!tg) return rec.meshId;

        const Vector3D* srcVerts = tg->GetVertices();
        const int vertCount = tg->GetVertexCount();
        const uint32_t newHash = HashVertices(srcVerts, vertCount);

        if (newHash != rec.vertHash) {
            // Vertices changed — update GPU-side copy
            PglVec3* wireVerts = new (std::nothrow) PglVec3[vertCount];
            if (wireVerts) {
                for (int i = 0; i < vertCount; ++i) {
                    wireVerts[i] = ToWire(srcVerts[i]);
                }
                enc->UpdateVertices(rec.meshId, wireVerts, static_cast<uint16_t>(vertCount));
                delete[] wireVerts;
            }
            rec.vertHash  = newHash;
            rec.vertCount = static_cast<uint16_t>(vertCount);

            // Recompute cached AABB after vertex update
            ComputeMeshAABB(rec);
        }

        return rec.meshId;
    }

    // ─── Material Management ────────────────────────────────────────────

    PglMaterial FindOrCreateMaterial(Material* mat, PglEncoder* enc) {
        if (!mat) return PGL_INVALID_MATERIAL;

        // Look up existing record
        for (uint16_t i = 0; i < materialCount_; ++i) {
            if (materialRecords_[i].material == mat) {
                if (!materialRecords_[i].created) {
                    CreateGpuMaterial(i, enc);
                } else if (materialRecords_[i].registered &&
                           materialRecords_[i].paramSize > 0) {
                    // Check if params have changed since last sent
                    // (handles MaterialAnimator ratio updates, color changes, etc.)
                    uint32_t newHash = HashBytes(materialRecords_[i].paramBuf,
                                                  materialRecords_[i].paramSize);
                    if (newHash != materialRecords_[i].paramHash) {
                        enc->UpdateMaterial(
                            materialRecords_[i].materialId,
                            materialRecords_[i].paramBuf,
                            materialRecords_[i].paramSize);
                        materialRecords_[i].paramHash = newHash;
                    }
                }
                return materialRecords_[i].materialId;
            }
        }

        // New unregistered material — create with default type
        if (materialCount_ >= kMaxMaterials) return PGL_INVALID_MATERIAL;

        GpuMaterialRecord& rec = materialRecords_[materialCount_++];
        rec.material   = mat;
        rec.materialId = PGL_INVALID_MATERIAL;
        rec.type       = PGL_MAT_PRERENDERED;  // fallback until registered
        rec.registered = false;
        rec.created    = false;
        rec.paramSize  = 0;

        CreateGpuMaterial(materialCount_ - 1, enc);
        return rec.materialId;
    }

    void CreateGpuMaterial(uint16_t recordIdx, PglEncoder* enc) {
        GpuMaterialRecord& rec = materialRecords_[recordIdx];
        if (rec.created) return;

        rec.materialId = nextMaterialId_++;
        rec.created    = true;

        // Store initial param hash for dirty tracking
        if (rec.paramSize > 0) {
            rec.paramHash = HashBytes(rec.paramBuf, rec.paramSize);
        }

        enc->CreateMaterial(
            rec.materialId,
            rec.type,
            PGL_BLEND_BASE,  // Default blend mode
            rec.paramSize > 0 ? rec.paramBuf : nullptr,
            rec.paramSize
        );
    }

    // ─── Shader / Effect Encoding ────────────────────────────────────────

    /**
     * @brief Encode a ProtoTracer Effect into ProtoGL shader commands.
     *
     * Dispatches on EffectType to call the appropriate PglEncoder convenience
     * method for each active camera.  Tracks the last effect type so we only
     * re-send when the effect changes.
     */
    void EncodeEffect(Effect* fx, uint8_t camCount, PglEncoder* enc) {
        EffectType et = fx->GetEffectType();

        for (uint8_t ci = 0; ci < camCount && ci < PGL_MAX_CAMERAS; ++ci) {
            PglCamera cam = static_cast<PglCamera>(ci);

            switch (et) {
                case EffectType::HorizontalBlur: {
                    auto* e = static_cast<HorizontalBlur*>(fx);
                    enc->SetHorizontalBlur(cam, 0, fx->GetRatio(), e->GetPixels() / 2);
                    break;
                }
                case EffectType::VerticalBlur: {
                    auto* e = static_cast<VerticalBlur*>(fx);
                    enc->SetVerticalBlur(cam, 0, fx->GetRatio(), e->GetPixels() / 2);
                    break;
                }
                case EffectType::RadialBlur: {
                    auto* e = static_cast<RadialBlur*>(fx);
                    enc->SetRadialBlur(cam, 0, fx->GetRatio(), e->GetPixels() / 2);
                    break;
                }
                case EffectType::PhaseOffsetX: {
                    auto* e = static_cast<PhaseOffsetX*>(fx);
                    enc->SetPhaseOffsetX(cam, 0, fx->GetRatio(), e->GetPixels());
                    break;
                }
                case EffectType::PhaseOffsetY: {
                    auto* e = static_cast<PhaseOffsetY*>(fx);
                    enc->SetPhaseOffsetY(cam, 0, fx->GetRatio(), e->GetPixels());
                    break;
                }
                case EffectType::PhaseOffsetR: {
                    auto* e = static_cast<PhaseOffsetR*>(fx);
                    enc->SetPhaseOffsetR(cam, 0, fx->GetRatio(), e->GetPixels());
                    break;
                }
                case EffectType::EdgeFeather: {
                    auto* e = static_cast<EdgeFeatherEffect*>(fx);
                    enc->SetEdgeFeather(cam, 0, fx->GetRatio(), e->GetFeatherStrength());
                    break;
                }
                case EffectType::AntiAliasing: {
                    auto* e = static_cast<AntiAliasingEffect*>(fx);
                    enc->SetAntiAliasing(cam, 0, fx->GetRatio(), e->GetSmoothing());
                    break;
                }
                case EffectType::Unknown:
                default:
                    break;
            }
        }

        lastEffectType_ = et;
    }

    // ─── Draw ───────────────────────────────────────────────────────────

    void EncodeDrawObject(Object3D* obj, PglMesh meshId, PglMaterial matId,
                          PglEncoder* enc) {
        Transform* t = obj->GetTransform();

        enc->DrawObject(
            meshId,
            matId,
            ToWire(t->GetPosition()),
            ToWire(t->GetRotation()),
            ToWire(t->GetScale()),
            ToWire(t->GetBaseRotation()),
            ToWire(t->GetScaleRotationOffset()),
            ToWire(t->GetScaleOffset()),
            ToWire(t->GetRotationOffset()),
            obj->IsEnabled()
        );
    }

    // ─── Host-Side Frustum Culling ──────────────────────────────────────
    //
    // Performs the same AABB → transform → project → screen-bounds test
    // that the GPU does in PrepareFrame(), but on the ESP32-S3 host side.
    // This allows skipping SPI bandwidth for off-screen objects.
    //
    // The test is conservative: if ANY projected corner is on-screen, the
    // object is considered visible.  This avoids false-negative culling
    // from large objects that partially overlap the screen.

    /// Compute AABB of the mesh's current vertex data and cache it.
    static void ComputeMeshAABB(GpuMeshRecord& rec) {
        Object3D* obj = rec.object;
        if (!obj) { rec.aabbValid = false; return; }
        TriangleGroup* tg = obj->GetTriangleGroup();
        if (!tg || tg->GetVertexCount() == 0) { rec.aabbValid = false; return; }

        const Vector3D* verts = tg->GetVertices();
        const int count = tg->GetVertexCount();
        Vector3D mn = verts[0], mx = verts[0];
        for (int i = 1; i < count; ++i) {
            const Vector3D& v = verts[i];
            if (v.X < mn.X) mn.X = v.X;
            if (v.Y < mn.Y) mn.Y = v.Y;
            if (v.Z < mn.Z) mn.Z = v.Z;
            if (v.X > mx.X) mx.X = v.X;
            if (v.Y > mx.Y) mx.Y = v.Y;
            if (v.Z > mx.Z) mx.Z = v.Z;
        }
        rec.aabbMin = mn;
        rec.aabbMax = mx;
        rec.aabbValid = true;
    }

    /// Apply the DrawObject transform to a single vertex (mirrors GPU TransformVertex).
    static Vector3D ApplyTransform(Transform* t, const Vector3D& v) {
        // 1. Scale around scaleOffset
        Vector3D offset = v - t->GetScaleOffset();
        Vector3D scaled = offset * t->GetScale();
        // Apply scale rotation offset (if non-identity)
        Vector3D rotScaled = t->GetScaleRotationOffset().RotateVector(scaled);
        Vector3D reoffset = rotScaled + t->GetScaleOffset();

        // 2. Rotate around rotationOffset
        Vector3D rOff = reoffset - t->GetRotationOffset();
        Quaternion fullRot = t->GetRotation();  // GetRotation() returns rotation*baseRotation
        Vector3D finalRot = fullRot.RotateVector(rOff);
        Vector3D result = finalRot + t->GetRotationOffset();

        // 3. Translate
        return result + t->GetPosition();
    }

    /// Perspective-project a world-space point to screen (mirrors GPU PerspectiveProject).
    static Vector2D PerspProject(const Vector3D& worldPos, const Vector3D& camPos,
                                  Quaternion camRot, float fovFactor,
                                  float screenW, float screenH, float& outZ) {
        // Camera-relative
        Vector3D rel = worldPos - camPos;
        Quaternion invRot = camRot.Conjugate();
        Vector3D view = invRot.RotateVector(rel);

        float z = view.Z;
        if (z < 0.001f) z = 0.001f;
        outZ = z;

        float invZ = fovFactor / z;
        return Vector2D(view.X * invZ + screenW * 0.5f,
                        view.Y * invZ + screenH * 0.5f);
    }

    /// Test if an object's AABB projects onto the screen (128×64).
    /// Returns true if any portion is potentially visible.
    bool IsObjectVisible(Object3D* obj, const GpuMeshRecord& rec,
                         const Vector3D& camPos, Quaternion camRot) {
        if (!rec.aabbValid) return true;  // no AABB → assume visible (conservative)

        // GPU uses: fovFactor = screenW * 0.5
        constexpr float kScreenW = 128.0f;
        constexpr float kScreenH = 64.0f;
        constexpr float kFovFactor = kScreenW * 0.5f;  // = 64.0

        // Get the object's current transform (same as sent in EncodeDrawObject)
        Transform* xform = obj->GetTransform();

        // Build 8 AABB corners
        const Vector3D& mn = rec.aabbMin;
        const Vector3D& mx = rec.aabbMax;
        const Vector3D corners[8] = {
            Vector3D(mn.X, mn.Y, mn.Z), Vector3D(mx.X, mn.Y, mn.Z),
            Vector3D(mn.X, mx.Y, mn.Z), Vector3D(mx.X, mx.Y, mn.Z),
            Vector3D(mn.X, mn.Y, mx.Z), Vector3D(mx.X, mn.Y, mx.Z),
            Vector3D(mn.X, mx.Y, mx.Z), Vector3D(mx.X, mx.Y, mx.Z),
        };

        // Transform each corner by the object's transform, then project.
        // If any corner lands on-screen and in front of camera → visible.
        float sMinX = 1e30f, sMinY = 1e30f;
        float sMaxX = -1e30f, sMaxY = -1e30f;
        bool anyInFront = false;

        for (int c = 0; c < 8; ++c) {
            Vector3D tv = ApplyTransform(xform, corners[c]);
            float cz;
            Vector2D sp = PerspProject(tv, camPos, camRot, kFovFactor,
                                        kScreenW, kScreenH, cz);
            if (cz > 0.0f) {
                anyInFront = true;
                if (sp.X < sMinX) sMinX = sp.X;
                if (sp.Y < sMinY) sMinY = sp.Y;
                if (sp.X > sMaxX) sMaxX = sp.X;
                if (sp.Y > sMaxY) sMaxY = sp.Y;
            }
        }

        // Off-screen if: all behind camera, or projected AABB entirely off-screen
        if (!anyInFront) return false;
        if (sMaxX < 0.0f || sMinX >= kScreenW) return false;
        if (sMaxY < 0.0f || sMinY >= kScreenH) return false;

        return true;  // at least partially visible
    }

    // ─── Member Data ────────────────────────────────────────────────────

    PglDevice       device_;
    PglDeviceConfig gpuConfig_;
    uint32_t        frameNumber_ = 0;

    // Resource pools
    GpuMeshRecord       meshRecords_[kMaxMeshes]{};
    GpuMaterialRecord   materialRecords_[kMaxMaterials]{};
    GpuLayoutRecord     layoutRecords_[PGL_MAX_CAMERAS]{};
    uint16_t            meshCount_     = 0;
    uint16_t            materialCount_ = 0;
    PglMesh             nextMeshId_     = 0;
    PglMaterial         nextMaterialId_ = 0;

    // GPU limits (filled by QueryCapability, with fallback defaults)
    uint16_t gpuMaxVertices_  = PGL_MAX_VERTICES;
    uint16_t gpuMaxTriangles_ = PGL_MAX_TRIANGLES;
    uint16_t gpuMaxMeshes_    = PGL_MAX_MESHES;
    uint16_t gpuMaxMaterials_ = PGL_MAX_MATERIALS;
    uint8_t  gpuCapFlags_     = 0;     ///< PglCapabilityFlags from boot query

    // Brightness tracking to avoid redundant I2C writes
    uint8_t lastBrightness_ = 0;

    // Effect tracking — only re-send shader when effect changes/disables
    EffectType lastEffectType_ = EffectType::Unknown;

    // Extended status cache (refreshed in Display() diagnostics or QueryGpuHealth())
    PglExtendedStatusResponse lastExtStatus_{};

    // Periodic diagnostic tracking
    static constexpr uint32_t kDiagIntervalFrames = 300;  // ~5 s @ 60 fps
    uint32_t lastDiagDropped_  = 0;
    uint32_t lastDiagOverflow_ = 0;
};
