#include <Arduino.h>

#include "Controller.h"
#include "../Render/Camera.h"
#include "../Flash/PixelGroups/P3HUB75.h"
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>

//HUB75
#include <ESP32-VirtualMatrixPanel-I2S-DMA.h>
//#include <FastLED.h>

#include <M5UnitGLASS2.h>
extern M5GFX *display;
extern bool gColoredPreview;

#ifdef NEW_HUB75
#define R1_PIN   6
#define G1_PIN   4
#define B1_PIN   5
#define R2_PIN   16
#define G2_PIN  7
#define B2_PIN  15
#define A_PIN   18
#define B_PIN    8
#define C_PIN    3
#define D_PIN    9
#define E_PIN   -1 // required for 1/32 scan panels, like 64x64. Any available pin would do, i.e. IO32
#define LAT_PIN 11
#define OE_PIN  12
#define CLK_PIN 10
#else
#define R1_PIN   4
#define G1_PIN   5
#define B1_PIN   6
#define R2_PIN   7
#define G2_PIN  15
#define B2_PIN  16
#define A_PIN   18
#define B_PIN    8
#define C_PIN    3
#define D_PIN    9
#define E_PIN   -1 // required for 1/32 scan panels, like 64x64. Any available pin would do, i.e. IO32
#define LAT_PIN 11
#define OE_PIN  12
#define CLK_PIN 10
#endif

// Configure for your panel(s) as appropriate!
#define PANEL_RES_X 64 // Number of pixels wide of each INDIVIDUAL panel module. 
#define PANEL_RES_Y 32 // Number of pixels tall of each INDIVIDUAL panel module.

#define NUM_ROWS 2 // Number of rows of chained INDIVIDUAL PANELS
#define NUM_COLS 1 // Number of INDIVIDUAL PANELS per ROW
#define PANEL_CHAIN NUM_ROWS*NUM_COLS    // total number of panels chained one to another

// Change this to your needs, for details on VirtualPanel pls read the PDF!
#define SERPENT true

#ifndef HUB75_PIXEL_COLOR_DEPTH_BITS
#define HUB75_PIXEL_COLOR_DEPTH_BITS 8
#endif

#ifndef HUB75_PIXEL_COLOR_DEPTH_RETRY_BITS
#define HUB75_PIXEL_COLOR_DEPTH_RETRY_BITS HUB75_PIXEL_COLOR_DEPTH_BITS
#endif

// ── HUD preview buffer: placed in PSRAM.  ESPMenu::Update() reads
//     gHudColors (set below), converts to RGB565 into gHudBuffer, and
//     pushes it to the secondary M5 display — all inside its existing
//     I2C transaction on the main-loop core.  No FreeRTOS task needed.
EXT_RAM_BSS_ATTR uint16_t gHudBuffer[64 * 32];
ProtoRGBColor* volatile gHudColors = nullptr;

// placeholder for the matrix object
MatrixPanel_I2S_DMA *dma_display = nullptr;

// placeholder for the virtual display object
VirtualMatrixPanel  *virtualDisp = nullptr;

class TasESP32S3KitV1 : public Controller {
private:
    CameraLayout cameraLayout = CameraLayout(CameraLayout::ZForward, CameraLayout::YUp);
    Transform camTransform1 = Transform(Vector3D(0.0f,0.0f,0.0f), Vector3D(0.0f, 0.0f, -500.0f), Vector3D(1, 1, 1));
    uint8_t lastDmaBrightness = 0xFF;

    PixelGroup* camPixels1 = new PixelGroup(2048,P3HUB75);
    
    Camera* camMain1 = new Camera(&camTransform1, &cameraLayout, camPixels1);

    CameraBase* cameras[1] = { camMain1 };
    struct RGB{
    uint8_t R;
    uint8_t G;
    uint8_t B;
    };

    static void LogHub75Heap(const char* phase, uint8_t depth) {
        Serial.printf("[HUB75] %s depth=%u intFree=%u largestBlk=%u psramFree=%u\n",
                      phase,
                      depth,
                      static_cast<unsigned int>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                      static_cast<unsigned int>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
                      static_cast<unsigned int>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
    }

public:
    TasESP32S3KitV1(uint8_t maxBrightness) : Controller(cameras, 1, maxBrightness, 0){}

    void Initialize() override{
        lastDmaBrightness = 0xFF;

        if (display) {
        #ifdef VERBOSE_STARTUP
        display->println("初始化HUB75驱动...");
        #else
        display->progressBar(14,50,100,8,50);
        #endif
        }
        delay(400);
        HUB75_I2S_CFG mxconfig(
                PANEL_RES_X,   // module width
                PANEL_RES_Y,   // module height
                PANEL_CHAIN    // chain length
        );

        mxconfig.gpio.r1 = R1_PIN;
        mxconfig.gpio.g1 = G1_PIN;
        mxconfig.gpio.b1 = B1_PIN;
        mxconfig.gpio.r2 = R2_PIN;
        mxconfig.gpio.g2 = G2_PIN;
        mxconfig.gpio.b2 = B2_PIN;

        mxconfig.gpio.a = A_PIN;
        mxconfig.gpio.b = B_PIN;
        mxconfig.gpio.c = C_PIN;
        mxconfig.gpio.d = D_PIN;
        mxconfig.gpio.e = E_PIN;
        mxconfig.gpio.clk = CLK_PIN;
        mxconfig.gpio.lat = LAT_PIN;
        mxconfig.gpio.oe = OE_PIN;
        mxconfig.clkphase = false;
        mxconfig.double_buff = true;  // DMA-safe: write back buf, flip atomically
        mxconfig.setPixelColorDepthBits(HUB75_PIXEL_COLOR_DEPTH_BITS);

        // OK, now we can create our matrix object
        dma_display = new MatrixPanel_I2S_DMA(mxconfig);
        #ifndef VERBOSE_STARTUP 
        if (display) display->progressBar(14,50,100,8,55);
        #endif

        // let's adjust default brightness to about 75%
        if (dma_display) {
            dma_display->setBrightness8(125);    // range is 0-255, 0 - 0%, 255 - 100%
        }

        LogHub75Heap("begin", mxconfig.getPixelColorDepthBits());

        // Allocate memory and start DMA display
        bool dmaBeginOk = dma_display && dma_display->begin();
        if (dmaBeginOk) {
            LogHub75Heap("begin OK", mxconfig.getPixelColorDepthBits());
        } else {
            LogHub75Heap("begin failed", mxconfig.getPixelColorDepthBits());
        }

        if (!dmaBeginOk && mxconfig.getPixelColorDepthBits() > HUB75_PIXEL_COLOR_DEPTH_RETRY_BITS) {
            Serial.printf("[HUB75] DMA allocation failed at %u-bit depth, retrying at %u-bit depth\n",
                          mxconfig.getPixelColorDepthBits(), HUB75_PIXEL_COLOR_DEPTH_RETRY_BITS);
            delete dma_display;
            dma_display = nullptr;
            mxconfig.setPixelColorDepthBits(HUB75_PIXEL_COLOR_DEPTH_RETRY_BITS);
            dma_display = new MatrixPanel_I2S_DMA(mxconfig);
            LogHub75Heap("retry begin", mxconfig.getPixelColorDepthBits());
            dmaBeginOk = dma_display && dma_display->begin();
            if (dmaBeginOk) {
                LogHub75Heap("retry OK", mxconfig.getPixelColorDepthBits());
            } else {
                LogHub75Heap("retry failed", mxconfig.getPixelColorDepthBits());
            }
        }

        if(!dmaBeginOk){
            if (display) {
            display->clearDisplay();
            display->println("初始化HUB75驱动失败！");
            display->println("I2S 内存分配失败");
            }
            Serial.println("****** I2S memory allocation failed ***********");
            return;
        }
        if (display) {
        #ifdef VERBOSE_STARTUP
        display->println("初始化HUB75驱动完成");
        #else
        display->progressBar(14,50,100,8,60);
        #endif
        }
        delay(200);
        //Serial1.begin(2048000, SERIAL_8N1, -1, 47);

        // create VirtualDisplay object based on our newly created dma_display object
    #if PANEL_CHAIN > 1
        virtualDisp = new VirtualMatrixPanel((*dma_display), NUM_ROWS, NUM_COLS, PANEL_RES_X, PANEL_RES_Y, CHAIN_BOTTOM_LEFT_UP);
    #else
        virtualDisp = nullptr;
    #endif
        #ifndef VERBOSE_STARTUP 
        if (display) display->progressBar(14,50,100,8,65);
        #endif

        dma_display->fillScreenRGB888(100,0,0);
        dma_display->flipDMABuffer();  // push to both buffers
        if (display) {
        #ifdef VERBOSE_STARTUP
        display->println("HUB75测试：红色");
        #else
        display->progressBar(14,50,100,8,70);
        #endif
        }
        delay(1000);
        dma_display->fillScreenRGB888(0,100,0);
        dma_display->flipDMABuffer();
        if (display) {
        #ifdef VERBOSE_STARTUP
        display->println("HUB75测试：绿色");
        #else
        display->progressBar(14,50,100,8,75);
        #endif
        }
        delay(1000);
        dma_display->fillScreenRGB888(0,0,100);
        dma_display->flipDMABuffer();
        if (display) {
        #ifdef VERBOSE_STARTUP
        display->println("HUB75测试：蓝色");
        #else
        display->progressBar(14,50,100,8,80);
        #endif
        }
        delay(1000);

        dma_display->clearScreen();
        dma_display->flipDMABuffer();  // clear both buffers
        Serial.println("Init OK!");
    }

    void ResetDisplayDriver() override {
        Serial.println("[HUB75] Resetting driver after background downloads");

        lastDmaBrightness = 0xFF;

        if (dma_display) {
            dma_display->clearScreen();
            dma_display->flipDMABuffer();
            dma_display->stopDMAoutput();
        }

#if PANEL_CHAIN > 1
        delete virtualDisp;
        virtualDisp = nullptr;
#endif

        delete dma_display;
        dma_display = nullptr;

        Initialize();
    }

    void Display() override {
        if (!dma_display) return;
#if PANEL_CHAIN > 1
        if (!virtualDisp) return;
#endif

        // Cache brightness — only push to DMA when it actually changes.
        // 0xFF sentinel: skip first frame (Initialize() already set DMA
        // brightness to 125; soft-start begins at 0 and must not override
        // the init value with black).
        if (brightness > 0 && brightness != lastDmaBrightness) {
            dma_display->setBrightness8(brightness);
            lastDmaBrightness = brightness;
        } else if (lastDmaBrightness == 0xFF) {
            lastDmaBrightness = brightness;
        }
        
        ProtoRGBColor* colors = camPixels1->GetColors();
        if (!colors) return;
        
        // ── HUB75 rendering: write to back buffer, flip atomically ──
        // With double_buff=true the DMA scans the front buffer while we
        // fill the back buffer — no partial-frame scan, no color-depth loss.
#if PANEL_CHAIN > 1
        dma_display->fillBufferRgb888Chained((const uint8_t*)colors);
#else
        dma_display->fillBufferRgb888((const uint8_t*)colors, 64, 32);
#endif
        dma_display->flipDMABuffer();
        
        // ── Export colors pointer for ESPMenu HUD preview ──
        // ESPMenu::Update() reads gHudColors, builds the RGB565 buffer,
        // and pushes it to the secondary display inside its own I2C
        // transaction — all on the main-loop core, zero render-path cost.
        gHudColors = colors;
    }
};
