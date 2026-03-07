/**
 * @file TasESP32S3KitV1_GPU.h
 * @brief GPU-offloaded controller for TasESP32S3 Kit V1 + RP2350 GPU daughter board.
 *
 * This controller replaces the local HUB75 I2S-DMA rasterizer with GPU offload:
 * the ESP32-S3 encodes the ProtoTracer scene into ProtoGL wire-format commands,
 * DMA-transfers them to the RP2350 GPU via Octal SPI, and the GPU handles
 * rasterisation + HUB75 output autonomously.
 *
 * M7 deliverable — GPU integration into animation system.
 *
 * Pin mapping (defaults, overridable via build flags):
 *   Octal SPI (ESP32-S3 LCD Parallel → RP2350 PIO1):
 *     D0..D7  = GPIO 3,4,5,6,7,8,15,16   (8 data lines)
 *     CLK     = GPIO 10                    (pixel clock)
 *     CS      = GPIO 11                    (chip select / frame sync)
 *     RDY     = GPIO 12                    (GPU ready, input)
 *
 *   I2C Control Bus (ESP32-S3 master → RP2350 I2C0 slave @ 0x3C):
 *     SDA     = GPIO 47
 *     SCL     = GPIO 48
 *
 * When TASESP32S3_GPU is defined at build time, main.cpp selects this
 * controller instead of TasESP32S3KitV1 (which uses local I2S-DMA HUB75).
 */

#pragma once

#include <Arduino.h>
#include "GPUDriverController.h"
#include "../Render/Camera.h"
#include "../Flash/PixelGroups/P3HUB75.h"

#include <M5UnitGLASS2.h>
extern M5UnitGLASS2 display;

// ─── Octal SPI Pin Configuration ────────────────────────────────────────────
// Override any of these in platformio.ini build_flags:
//   -D GPU_SPI_D0=3  etc.

#ifndef GPU_SPI_D0
#define GPU_SPI_D0 3
#endif
#ifndef GPU_SPI_D1
#define GPU_SPI_D1 4
#endif
#ifndef GPU_SPI_D2
#define GPU_SPI_D2 5
#endif
#ifndef GPU_SPI_D3
#define GPU_SPI_D3 6
#endif
#ifndef GPU_SPI_D4
#define GPU_SPI_D4 7
#endif
#ifndef GPU_SPI_D5
#define GPU_SPI_D5 8
#endif
#ifndef GPU_SPI_D6
#define GPU_SPI_D6 15
#endif
#ifndef GPU_SPI_D7
#define GPU_SPI_D7 16
#endif
#ifndef GPU_SPI_CLK
#define GPU_SPI_CLK 10
#endif
#ifndef GPU_SPI_CS
#define GPU_SPI_CS 11
#endif
#ifndef GPU_SPI_CLK_MHZ
#define GPU_SPI_CLK_MHZ 80
#endif

// ─── Bus Direction & Notification Pins ───────────────────────────────────────────
#ifndef GPU_DIR_PIN
#define GPU_DIR_PIN 10
#endif
#ifndef GPU_IRQ_PIN
#define GPU_IRQ_PIN 13
#endif

// ─── I2C Control Bus ────────────────────────────────────────────────────────
// These can share the same I2C bus as the M5Glass2 display if the GPU
// address (0x3C) doesn't conflict. The M5Glass2 uses a different address.
#ifndef GPU_I2C_SDA
#define GPU_I2C_SDA 47
#endif
#ifndef GPU_I2C_SCL
#define GPU_I2C_SCL 48
#endif
#ifndef GPU_I2C_ADDR
#define GPU_I2C_ADDR 0x3C
#endif

// ─── Panel Geometry (for camera setup — same as local HUB75 variant) ────────
#ifndef GPU_PANEL_WIDTH
#define GPU_PANEL_WIDTH 64
#endif
#ifndef GPU_PANEL_HEIGHT
#define GPU_PANEL_HEIGHT 64  // 2× 64×32 panels stacked = 64×64 virtual
#endif

// ─── DMA Buffer Size ────────────────────────────────────────────────────────
#ifndef GPU_CMD_BUFFER_SIZE
#define GPU_CMD_BUFFER_SIZE 32768
#endif

// ─── Controller ─────────────────────────────────────────────────────────────

class TasESP32S3KitV1_GPU : public GPUDriverController {
private:
    static const uint16_t kHalfPixels = 1024;

    CameraLayout cameraLayout = CameraLayout(CameraLayout::ZForward, CameraLayout::YUp);
    Transform camTransform1 = Transform(Vector3D(), Vector3D(0.0f, 0.0f, -500.0f), Vector3D(1, 1, 1));
    Transform camTransform2 = Transform(Vector3D(), Vector3D(0.0f, 0.0f, -500.0f), Vector3D(1, 1, 1));

    PixelGroup* camPixels1 = nullptr;
    PixelGroup* camPixels2 = nullptr;

    Camera* camMain1 = nullptr;
    Camera* camMain2 = nullptr;

    CameraBase* cameras_[2] = { nullptr, nullptr };

    // ── Build PglDeviceConfig from pin defines ──────────────────────────

    static PglDeviceConfig BuildGpuConfig() {
        PglDeviceConfig cfg{};
        cfg.spiDataPins[0] = GPU_SPI_D0;
        cfg.spiDataPins[1] = GPU_SPI_D1;
        cfg.spiDataPins[2] = GPU_SPI_D2;
        cfg.spiDataPins[3] = GPU_SPI_D3;
        cfg.spiDataPins[4] = GPU_SPI_D4;
        cfg.spiDataPins[5] = GPU_SPI_D5;
        cfg.spiDataPins[6] = GPU_SPI_D6;
        cfg.spiDataPins[7] = GPU_SPI_D7;
        cfg.spiClkPin      = GPU_SPI_CLK;
        cfg.spiCsPin       = GPU_SPI_CS;
        cfg.spiClockMHz    = GPU_SPI_CLK_MHZ;
        cfg.dirPin         = GPU_DIR_PIN;
        cfg.irqPin         = GPU_IRQ_PIN;
        cfg.i2cSdaPin      = GPU_I2C_SDA;
        cfg.i2cSclPin      = GPU_I2C_SCL;
        cfg.i2cAddress     = GPU_I2C_ADDR;
        cfg.commandBufferSize = GPU_CMD_BUFFER_SIZE;
        cfg.i2cPort        = 0;   // Wire (shared with M5Glass2 if compatible)
        cfg.dirTurnaroundCycles = 2;
        return cfg;
    }

public:
    TasESP32S3KitV1_GPU(uint8_t maxBrightness)
        : GPUDriverController(cameras_, 2, maxBrightness, 0, BuildGpuConfig())
    {}

    ~TasESP32S3KitV1_GPU() override {
        delete camMain1;
        delete camMain2;
        delete camPixels1;
        delete camPixels2;
    }

    void Initialize() override {
#ifdef VERBOSE_STARTUP
        display.println("Init GPU driver...");
#endif
        delay(200);

        // Allocate camera pixel groups (same layout as the local HUB75 variant)
        // The GPUDriverController sends pixel layout to the GPU on first frame;
        // it doesn't rasterize locally, but the Camera objects are still needed
        // for the GPUDriverController::EncodeCamera() calls (layout, transform).
        if (!camPixels1) camPixels1 = new PixelGroup(kHalfPixels, P3HUB75);
        if (!camPixels2) camPixels2 = new PixelGroup(kHalfPixels, P3HUB75 + kHalfPixels);
        if (!camMain1) camMain1 = new Camera(&camTransform1, &cameraLayout, camPixels1);
        if (!camMain2) camMain2 = new Camera(&camTransform2, &cameraLayout, camPixels2);

        cameras_[0] = camMain1;
        cameras_[1] = camMain2;

        // Initialize the ProtoGL device (SPI + I2C + DMA)
        GPUDriverController::Initialize();

#ifdef VERBOSE_STARTUP
        display.println("GPU driver OK");
#endif
        Serial.println("[TasS3_GPU] Init OK");
    }

    /**
     * @brief No-op for framebuffer output — the GPU drives HUB75 directly.
     *
     * The base GPUDriverController::Display() handles brightness sync via I2C.
     */
    void Display() override {
        GPUDriverController::Display();
    }

    // ── Material Registration Helpers ───────────────────────────────────
    //
    // These provide a convenient way for animations to batch-register
    // their materials at startup. Since ProtoTracer's Material base class
    // has no virtual GetType(), the animation must explicitly register
    // each material with its PglMaterialType mapping.
    //
    // Example (in setup or animation constructor):
    //   auto& gpu = static_cast<TasESP32S3KitV1_GPU&>(controller);
    //   gpu.RegisterSimpleMaterial(&redMaterial, 255, 0, 0);
    //   gpu.RegisterRainbowNoiseMaterial(&rainbowNoise, 0.02f, 1.0f);
    //   gpu.RegisterGradientMaterial(&gradientMat, stops, 2, 0, -175.0f, 175.0f);
    //
    // All RegisterXxx methods are inherited from GPUDriverController.
};
