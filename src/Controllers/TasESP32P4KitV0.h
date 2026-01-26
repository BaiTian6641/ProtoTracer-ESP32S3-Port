#pragma once

#include <Arduino.h>
#include <string.h>

#include "Controller.h"
#include "Render/Camera.h"
#include "Flash/PixelGroups/P3HUB75.h"

// Gamma LUT disabled for now to keep base driver stable; using simple brightness scaling + bit truncation.

#include "esp_err.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/parlio_tx.h"

#if defined(CONFIG_IDF_TARGET_ESP32P4)

class TasESP32P4KitV0 : public Controller {
private:
    static constexpr const char *LOG_TAG = "TasESP32P4KitV0";

    // HUB75 signal bit positions
    static constexpr uint8_t HUB75_WIDTH = 16;
    static constexpr uint8_t HUB75_R1_IDX = 7;
    static constexpr uint8_t HUB75_R2_IDX = 6;
    static constexpr uint8_t HUB75_LATCH_IDX = 5;
    static constexpr uint8_t HUB75_G1_IDX = 4;
    static constexpr uint8_t HUB75_G2_IDX = 3;
    static constexpr uint8_t HUB75_OE_IDX = 2;
    static constexpr uint8_t HUB75_B1_IDX = 1;
    static constexpr uint8_t HUB75_B2_IDX = 0;
    static constexpr uint8_t HUB75_NUM_A_IDX = 8;
    static constexpr uint8_t HUB75_NUM_B_IDX = 9;
    static constexpr uint8_t HUB75_NUM_C_IDX = 10;
    static constexpr uint8_t HUB75_NUM_D_IDX = 11;

    // Panel geometry/timing (64x32 physical, 1/16 scan) with two chained panels
    static constexpr uint16_t PANEL_RES_X = 64;
    static constexpr uint16_t PANEL_RES_Y = 32;
    static constexpr uint8_t PANEL_CHAIN = 2; // front + mirrored back
    static constexpr uint8_t BIT_DEPTH = 5;   // Align closer to RGB565 per-channel depth
    static constexpr uint16_t GAP_CYCLE_PER_LINE = 40;
    static constexpr uint32_t LED_MATRIX_PIXEL_CLOCK_HZ = 20 * 1000 * 1000;

    static constexpr size_t CHAINED_RES_X = PANEL_RES_X * PANEL_CHAIN;
    static constexpr size_t LINE_STRIDE = CHAINED_RES_X + GAP_CYCLE_PER_LINE;
    static constexpr size_t HALF_FRAME_WORDS = LINE_STRIDE * (PANEL_RES_Y / 2);
    static constexpr size_t WEIGHTED_WORDS = HALF_FRAME_WORDS; // single bitplane buffer, reused per weight

    // GPIO assignment
    static constexpr int R1_PIN = 52;
    static constexpr int G1_PIN = 51;
    static constexpr int B1_PIN = 31;
    static constexpr int R2_PIN = 30;
    static constexpr int G2_PIN = 29;
    static constexpr int B2_PIN = 28;
    static constexpr int LAT_PIN = 2;
    static constexpr int OE_PIN = 24;
    static constexpr int CLK_PIN = 3;
    static constexpr int A_PIN = 50;
    static constexpr int B_PIN = 49;
    static constexpr int C_PIN = 5;
    static constexpr int D_PIN = 4;

    CameraLayout cameraLayout = CameraLayout(CameraLayout::ZForward, CameraLayout::YUp);
    Transform camTransform1 = Transform(Vector3D(), Vector3D(0.0f, 0.0f, -500.0f), Vector3D(1, 1, 1));
    Transform camTransform2 = Transform(Vector3D(), Vector3D(0.0f, 0.0f, -500.0f), Vector3D(1, 1, 1));

    PixelGroup *camPixels1 = new PixelGroup(2048, P3HUB75);
    PixelGroup *camPixels2 = new PixelGroup(4, P3HUB75);

    Camera *camMain1 = new Camera(&camTransform1, &cameraLayout, camPixels1);
    Camera *camMain2 = new Camera(&camTransform2, &cameraLayout, camPixels2);

    CameraBase *cameras[2] = {camMain1, camMain2};

    parlio_tx_unit_handle_t txUnit = nullptr;
    parlio_transmit_config_t transmitConfig = {};

    // Double buffer the bitplane data so we never mutate a buffer that might still be in use by DMA.
    uint16_t txFrames[2][WEIGHTED_WORDS] = {};
    size_t txFrameIdx = 0;

    struct CompRGB {
        uint8_t r;
        uint8_t g;
        uint8_t b;
    };

    static inline CompRGB compressGamma(const ProtoRGBColor *src) {
        // Map 8-bit channel to BIT_DEPTH with rounding (matches DMA lib NO_CIE path behavior)
        constexpr uint8_t targetMax = (1U << BIT_DEPTH) - 1U;

        const auto scaleToDepth = [](uint8_t v) -> uint8_t {
            // round((v / 255) * targetMax)
            constexpr uint16_t numerator = targetMax;
            return static_cast<uint8_t>((static_cast<uint16_t>(v) * numerator + 127) / 255);
        };

        return {
            scaleToDepth(src->R),
            scaleToDepth(src->G),
            scaleToDepth(src->B)
        };
    }

    static inline uint16_t encodeShiftWord(const CompRGB &upper, const CompRGB &lower, uint8_t rowAddr, bool latch, uint8_t bitPlane) {
        const uint8_t bitPos = (BIT_DEPTH - 1U) - bitPlane; // bit position within 3-bit packed values
        uint16_t word = 0;

        word |= ((upper.r >> bitPos) & 0x01) << HUB75_R1_IDX;
        word |= ((upper.g >> bitPos) & 0x01) << HUB75_G1_IDX;
        word |= ((upper.b >> bitPos) & 0x01) << HUB75_B1_IDX;

        word |= ((lower.r >> bitPos) & 0x01) << HUB75_R2_IDX;
        word |= ((lower.g >> bitPos) & 0x01) << HUB75_G2_IDX;
        word |= ((lower.b >> bitPos) & 0x01) << HUB75_B2_IDX;

        word |= (rowAddr & 0x01) << HUB75_NUM_A_IDX;
        word |= ((rowAddr >> 1) & 0x01) << HUB75_NUM_B_IDX;
        word |= ((rowAddr >> 2) & 0x01) << HUB75_NUM_C_IDX;
        word |= ((rowAddr >> 3) & 0x01) << HUB75_NUM_D_IDX;

        word |= 1U << HUB75_OE_IDX; // keep output disabled while shifting
        if (latch) {
            word |= 1U << HUB75_LATCH_IDX;
        }
        return word;
    }

    void buildBitplane(uint8_t bit, uint8_t brt, uint16_t *dest) {
        const ProtoRGBColor *colors = camPixels1->GetColors();
        const uint16_t enable_cycles = (brt == 0) ? 0 : static_cast<uint16_t>((static_cast<uint32_t>(GAP_CYCLE_PER_LINE) * brt + 254) / 255);

        size_t pixelIdx = 0;

        for (uint8_t line = 0; line < (PANEL_RES_Y / 2); ++line) {
            for (uint8_t panel = 0; panel < PANEL_CHAIN; ++panel) {
                const bool mirrorBackSide = (panel == 0);

                for (uint16_t col = 0; col < PANEL_RES_X; ++col) {
                    const uint16_t srcCol = (PANEL_RES_X - 1U) - col;

                    const uint16_t upperRow = mirrorBackSide ? (PANEL_RES_Y - 1U - line) : line;
                    const uint16_t lowerRow = mirrorBackSide ? (PANEL_RES_Y - 1U - (line + PANEL_RES_Y / 2)) : (line + PANEL_RES_Y / 2);

                    const ProtoRGBColor *upper = colors + (upperRow * PANEL_RES_X) + srcCol;
                    const ProtoRGBColor *lower = colors + (lowerRow * PANEL_RES_X) + srcCol;

                    const CompRGB upperC = compressGamma(upper);
                    const CompRGB lowerC = compressGamma(lower);

                    const bool latch = (panel == (PANEL_CHAIN - 1)) && (col == (PANEL_RES_X - 1));
                    dest[pixelIdx++] = encodeShiftWord(upperC, lowerC, line, latch, bit);
                }
            }

            for (uint16_t gap = 0; gap < GAP_CYCLE_PER_LINE; ++gap) {
                uint16_t word = 0;
                word |= (line & 0x01) << HUB75_NUM_A_IDX;
                word |= ((line >> 1) & 0x01) << HUB75_NUM_B_IDX;
                word |= ((line >> 2) & 0x01) << HUB75_NUM_C_IDX;
                word |= ((line >> 3) & 0x01) << HUB75_NUM_D_IDX;

                const uint8_t outputDisable = (gap >= enable_cycles) ? 1U : 0U;
                word |= outputDisable << HUB75_OE_IDX;
                dest[pixelIdx++] = word;
            }
        }
    }

    void ensureParlio() {
        if (txUnit) {
            return;
        }

        parlio_tx_unit_config_t config = {};
        config.clk_src = PARLIO_CLK_SRC_DEFAULT;
        config.data_width = HUB75_WIDTH;
        config.clk_in_gpio_num = GPIO_NUM_NC;
        config.valid_gpio_num = GPIO_NUM_NC;
        config.clk_out_gpio_num = static_cast<gpio_num_t>(CLK_PIN);
        config.output_clk_freq_hz = LED_MATRIX_PIXEL_CLOCK_HZ;
        config.trans_queue_depth = 8;
        config.max_transfer_size = sizeof(txFrames[0]);
        config.sample_edge = PARLIO_SAMPLE_EDGE_POS;

        config.data_gpio_nums[HUB75_R1_IDX] = static_cast<gpio_num_t>(R1_PIN);
        config.data_gpio_nums[HUB75_R2_IDX] = static_cast<gpio_num_t>(R2_PIN);
        config.data_gpio_nums[HUB75_G1_IDX] = static_cast<gpio_num_t>(G1_PIN);
        config.data_gpio_nums[HUB75_G2_IDX] = static_cast<gpio_num_t>(G2_PIN);
        config.data_gpio_nums[HUB75_B1_IDX] = static_cast<gpio_num_t>(B1_PIN);
        config.data_gpio_nums[HUB75_B2_IDX] = static_cast<gpio_num_t>(B2_PIN);
        config.data_gpio_nums[HUB75_OE_IDX] = static_cast<gpio_num_t>(OE_PIN);
        config.data_gpio_nums[HUB75_LATCH_IDX] = static_cast<gpio_num_t>(LAT_PIN);
        config.data_gpio_nums[HUB75_NUM_A_IDX] = static_cast<gpio_num_t>(A_PIN);
        config.data_gpio_nums[HUB75_NUM_B_IDX] = static_cast<gpio_num_t>(B_PIN);
        config.data_gpio_nums[HUB75_NUM_C_IDX] = static_cast<gpio_num_t>(C_PIN);
        config.data_gpio_nums[HUB75_NUM_D_IDX] = static_cast<gpio_num_t>(D_PIN);
        config.data_gpio_nums[12] = GPIO_NUM_NC;
        config.data_gpio_nums[13] = GPIO_NUM_NC;
        config.data_gpio_nums[14] = GPIO_NUM_NC;
        config.data_gpio_nums[15] = GPIO_NUM_NC;

        esp_err_t err = parlio_new_tx_unit(&config, &txUnit);
        if (err != ESP_OK) {
            ESP_LOGE(LOG_TAG, "parlio_new_tx_unit failed: %d", static_cast<int>(err));
            return;
        }

        transmitConfig.idle_value = 0x00;
        transmitConfig.flags.loop_transmission = true; // block until each plane finishes to avoid buffer races
        transmitConfig.flags.queue_nonblocking = true;

        err = parlio_tx_unit_enable(txUnit);
        if (err != ESP_OK) {
            ESP_LOGE(LOG_TAG, "parlio_tx_unit_enable failed: %d", static_cast<int>(err));
            txUnit = nullptr;
        }
    }

public:
    TasESP32P4KitV0(uint8_t maxBrightness) : Controller(cameras, 2, maxBrightness, 0) {}

    void Initialize() override {
        ensureParlio();
    }

    void Display() override {
        if (!txUnit) {
            return;
        }

        const size_t payloadBits = HALF_FRAME_WORDS * sizeof(uint16_t) * 8;

        for (uint8_t bit = 0; bit < BIT_DEPTH; ++bit) {
            uint16_t *dest = txFrames[txFrameIdx];
            txFrameIdx ^= 1; // flip for next plane to avoid touching a buffer still in flight

            const uint16_t repeats = 1U << (BIT_DEPTH - 1 - bit);
            buildBitplane(bit, brightness, dest);

            for (uint16_t rep = 0; rep < repeats; ++rep) {
                esp_err_t err = parlio_tx_unit_transmit(txUnit, dest, payloadBits, &transmitConfig);
                if (err != ESP_OK) {
                    ESP_LOGE(LOG_TAG, "parlio_tx_unit_transmit failed: %d", static_cast<int>(err));
                    return;
                }
            }
        }
    }
};

#else
// Fallback stub for non-ESP32P4 targets
class TasESP32P4KitV0 : public Controller {
public:
    TasESP32P4KitV0(uint8_t maxBrightness) : Controller(nullptr, 0, maxBrightness, 0) {}
    void Initialize() override {}
    void Display() override {}
};

#endif
