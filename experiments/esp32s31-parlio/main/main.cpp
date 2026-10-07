#include <Arduino.h>
#include <driver/gpio.h>
#include <esp_heap_caps.h>
#include <esp_psram.h>
#include <soc/soc_caps.h>
#include <hub75.h>
#include "Math/Vector2D.h"
#include "Math/Vector3D.h"

static_assert(SOC_PARLIO_TX_UNIT_MAX_DATA_WIDTH == 16, "Expected 16-bit PARLIO");
static_assert(SOC_PARLIO_TX_CLK_SUPPORT_GATING, "Clock gating required");
static_assert(SOC_PARLIO_TX_SUPPORT_LOOP_TRANSMISSION, "Continuous refresh required");

static bool ValidatePanelPins(const Hub75Pins& pins)
{
    const int outputs[] = {pins.r1, pins.g1, pins.b1, pins.r2, pins.g2, pins.b2,
                           pins.a, pins.b, pins.c, pins.d, pins.lat, pins.oe, pins.clk};
    for (size_t i = 0; i < sizeof(outputs) / sizeof(outputs[0]); ++i) {
        if (!GPIO_IS_VALID_OUTPUT_GPIO(outputs[i])) return false;
        // Function-Core USB-DBG, USB-UART, onboard RGB LED and BOOT button.
        if (outputs[i] == 33 || outputs[i] == 34 || (outputs[i] >= 58 && outputs[i] <= 61)) return false;
        for (size_t j = 0; j < i; ++j) if (outputs[i] == outputs[j]) return false;
    }
    return true;
}

static void StartPanel()
{
#ifndef CONFIG_PROTOTRACER_HUB75_ENABLE
    Serial.println("[S31] HUB75 disabled: set board wiring in menuconfig first");
    return;
#endif
    // Keep the complete panel path type-checked even in the default idle build.
    Hub75Config config{};
    config.panel_width = 64;
    config.panel_height = 32;
    config.layout_cols = 2;
    config.double_buffer = true;
    config.output_clock_speed = Hub75ClockSpeed::HZ_10M;
    config.pins.r1 = CONFIG_PROTOTRACER_HUB75_R1;
    config.pins.g1 = CONFIG_PROTOTRACER_HUB75_G1;
    config.pins.b1 = CONFIG_PROTOTRACER_HUB75_B1;
    config.pins.r2 = CONFIG_PROTOTRACER_HUB75_R2;
    config.pins.g2 = CONFIG_PROTOTRACER_HUB75_G2;
    config.pins.b2 = CONFIG_PROTOTRACER_HUB75_B2;
    config.pins.a = CONFIG_PROTOTRACER_HUB75_A;
    config.pins.b = CONFIG_PROTOTRACER_HUB75_B;
    config.pins.c = CONFIG_PROTOTRACER_HUB75_C;
    config.pins.d = CONFIG_PROTOTRACER_HUB75_D;
    config.pins.e = -1;
    config.pins.lat = CONFIG_PROTOTRACER_HUB75_LAT;
    config.pins.oe = CONFIG_PROTOTRACER_HUB75_OE;
    config.pins.clk = CONFIG_PROTOTRACER_HUB75_CLK;
    if (!ValidatePanelPins(config.pins)) {
        Serial.println("[S31] HUB75 wiring incomplete or duplicate; output not started");
        return;
    }
    // Persistent lifetime: continuous DMA owns its buffers until shutdown.
    static Hub75Driver panel(config);
    if (!panel.begin()) {
        Serial.println("[S31] HUB75 initialization failed");
        return;
    }
    panel.fill(0, 0, 64, 32, 32, 0, 0);
    panel.fill(64, 0, 64, 32, 0, 32, 0);
    panel.flip_buffer();
    Serial.println("[S31] Static two-panel PARLIO pattern submitted");
}

void setup()
{
    Serial.begin(115200);
    Serial.setTxTimeoutMs(0);
    Serial.printf("[S31] IDF=%s flash=%u PSRAM=%u CPU=%u MHz\n", ESP.getSdkVersion(),
                   unsigned(ESP.getFlashChipSize()), unsigned(esp_psram_get_size()),
                   unsigned(ESP.getCpuFreqMHz()));
    Vector2D a2(2, 3);
    Vector3D a3(2, 3, 4);
    Serial.printf("[S31] Portable DSP dot2=%.1f dot3=%.1f (expected 23/47)\n",
                   a2.DotProduct(Vector2D(4, 5)), a3.DotProduct(Vector3D(4, 5, 6)));
    StartPanel();
}

void loop()
{
    Serial.printf("[S31] intFree=%u largest=%u psramFree=%u\n",
                   unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                   unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                   unsigned(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
    delay(2000);
}
