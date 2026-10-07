#pragma once
#include <Arduino.h>
#include <M5UnitGLASS2.h>
#include <hub75.h>
#include <esp_heap_caps.h>
#include "Controller.h"
#include "../Render/Camera.h"

extern M5GFX* display;
uint16_t* gHudBuffer = nullptr;
ProtoRGBColor* volatile gHudColors = nullptr;
String gHub75ColorOrder = "RGB";

#ifndef S31_HUB75_CLOCK_HZ
#define S31_HUB75_CLOCK_HZ 10000000
#endif
#ifndef S31_HUB75_REFRESH_HZ
#define S31_HUB75_REFRESH_HZ 200
#endif

// Function-Core map: the first eight signals match the attached analyzer.
// Remaining RGB2/C/D lines use unprobed, exposed GPIOs; GPIO33/34 are USB-DBG.
class TasESP32S31KitV0 : public Controller {
    CameraLayout cameraLayout = CameraLayout(CameraLayout::ZForward, CameraLayout::YUp);
    Transform camTransform = Transform(Vector3D(), Vector3D(0, 0, -500), Vector3D(1, 1, 1));
    PixelGroup* pixels = new PixelGroup(64, 32, Vector2D(0, 0), Vector2D(3, 3));
    Camera* camera = new Camera(&camTransform, &cameraLayout, pixels);
    CameraBase* cameras[1] = {camera};
    Hub75Driver* panel = nullptr;
    uint8_t* physicalRgb = nullptr;
    uint32_t completedFrames = 0;
    uint32_t presentFailures = 0;

    static void MapColors(const String& order, int8_t& r, int8_t& g, int8_t& b) {
        const int8_t pins[3] = {r, g, b};
        int8_t mapped[3];
        String upper = order; upper.toUpperCase();
        if (upper.length() != 3) return;
        for (unsigned i = 0; i < 3; ++i) {
            const int position = upper.indexOf("RGB"[i]);
            if (position < 0) return;
            mapped[i] = pins[position];
        }
        r = mapped[0]; g = mapped[1]; b = mapped[2];
    }

public:
    TasESP32S31KitV0(uint8_t maxBrightness) : Controller(cameras, 1, maxBrightness, 0) {}

    void Initialize() override {
        if (panel) { delete panel; panel = nullptr; }
        if (!physicalRgb) physicalRgb = static_cast<uint8_t*>(heap_caps_malloc(128 * 32 * 3, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!gHudBuffer) gHudBuffer = static_cast<uint16_t*>(heap_caps_calloc(64 * 32, sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!physicalRgb) { Serial.println("[S31 HUB75] RGB allocation failed"); return; }
        Hub75Config config{};
        config.panel_width = 64; config.panel_height = 32;
        config.layout_cols = 2; config.double_buffer = true;
        config.output_clock_speed = static_cast<Hub75ClockSpeed>(S31_HUB75_CLOCK_HZ);
        config.min_refresh_rate = S31_HUB75_REFRESH_HZ;
        config.clk_phase_inverted = true;
        config.latch_blanking = 2;
        config.pins.clk = 35; config.pins.lat = 36; config.pins.oe = 37;
        config.pins.r1 = 38; config.pins.g1 = 39; config.pins.b1 = 40;
        config.pins.a = 42; config.pins.b = 43;
        config.pins.c = 44; config.pins.d = 45; config.pins.e = -1;
        config.pins.r2 = 46; config.pins.g2 = 47; config.pins.b2 = 48;
        MapColors(gHub75ColorOrder, config.pins.r1, config.pins.g1, config.pins.b1);
        MapColors(gHub75ColorOrder, config.pins.r2, config.pins.g2, config.pins.b2);
        panel = new Hub75Driver(config);
        if (!panel || !panel->begin()) { delete panel; panel = nullptr; Serial.println("[S31 HUB75] PARLIO initialization failed"); return; }
        Serial.printf("[S31 HUB75] initialized 128x32 clock=%u depth=8 intFree=%u psramFree=%u\n",
                      unsigned(S31_HUB75_CLOCK_HZ), unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                      unsigned(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
    }
    void ResetDisplayDriver() override { Initialize(); }

    void Display() override {
        if (!panel || !panel->wait_for_frame(100)) { ++presentFailures; return; }
        ProtoRGBColor* colors = pixels->GetColors();
        if (!colors) return;
        // Match the S3 chained packer's vertical flip and mirrored second panel.
        for (unsigned y = 0; y < 32; ++y) for (unsigned x = 0; x < 128; ++x) {
            const ProtoRGBColor& c = colors[(31 - y) * 64 + (x < 64 ? x : 127 - x)];
            const unsigned offset = (y * 128 + x) * 3;
            physicalRgb[offset] = c.R; physicalRgb[offset + 1] = c.G; physicalRgb[offset + 2] = c.B;
        }
        panel->set_brightness(brightness);
        panel->draw_pixels(0, 0, 128, 32, physicalRgb, Hub75PixelFormat::RGB888);
        if (panel->present_frame(100)) ++completedFrames;
        else ++presentFailures;
        gHudColors = colors;
    }
    uint32_t GetCompletedFrames() const { return completedFrames; }
    uint32_t GetPresentFailures() const { return presentFailures; }
};
