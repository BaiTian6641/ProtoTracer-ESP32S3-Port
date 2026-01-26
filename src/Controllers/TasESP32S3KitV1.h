#include <Arduino.h>

#include "Controller.h"
#include "../Render/Camera.h"
#include "../Flash/PixelGroups/P3HUB75.h"

//HUB75
#include <ESP32-VirtualMatrixPanel-I2S-DMA.h>
//#include <FastLED.h>

#include <M5UnitGLASS2.h>
extern M5UnitGLASS2 display;

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


// placeholder for the matrix object
MatrixPanel_I2S_DMA *dma_display = nullptr;

// placeholder for the virtual display object
VirtualMatrixPanel  *virtualDisp = nullptr;

class TasESP32S3KitV1 : public Controller {
private:
    CameraLayout cameraLayout = CameraLayout(CameraLayout::ZForward, CameraLayout::YUp);
    Transform camTransform1 = Transform(Vector3D(), Vector3D(0.0f, 0.0f, -500.0f), Vector3D(1, 1, 1));
    Transform camTransform2 = Transform(Vector3D(), Vector3D(0.0f, 0.0f, -500.0f), Vector3D(1, 1, 1));

    PixelGroup* camPixels1 = new PixelGroup(2048,P3HUB75);
    PixelGroup* camPixels2 = new PixelGroup(4,P3HUB75);
    
    Camera* camMain1 = new Camera(&camTransform1, &cameraLayout, camPixels1);
    Camera* camMain2 = new Camera(&camTransform2, &cameraLayout, camPixels2);

    //CameraBase* cameras[1] = { &camMain1 };
    CameraBase* cameras[2] = { camMain1, camMain2 };
    struct RGB{
    uint8_t R;
    uint8_t G;
    uint8_t B;
    };

public:
    TasESP32S3KitV1(uint8_t maxBrightness) : Controller(cameras, 2, maxBrightness, 0){}

    void Initialize() override{
        #ifdef VERBOSE_STARTUP
        display.println("初始化HUB75驱动...");
        #else
        display.progressBar(14,50,100,8,50);
        #endif
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

        // OK, now we can create our matrix object
        dma_display = new MatrixPanel_I2S_DMA(mxconfig);
        #ifndef VERBOSE_STARTUP 
        display.progressBar(14,50,100,8,55);
        #endif

        // let's adjust default brightness to about 75%
        dma_display->setBrightness8(125);    // range is 0-255, 0 - 0%, 255 - 100%

        // Allocate memory and start DMA display
        if(!dma_display->begin()){
            display.clearDisplay();
            display.println("初始化HUB75驱动失败！");
            display.println("I2S 内存分配失败");
            Serial.println("****** I2S memory allocation failed ***********");
        }
        #ifdef VERBOSE_STARTUP
        display.println("初始化HUB75驱动完成");
        #else
        display.progressBar(14,50,100,8,60);
        #endif
        delay(200);
        //Serial1.begin(2048000, SERIAL_8N1, -1, 47);

        // create VirtualDisplay object based on our newly created dma_display object
        virtualDisp = new VirtualMatrixPanel((*dma_display), NUM_ROWS, NUM_COLS, PANEL_RES_X, PANEL_RES_Y, CHAIN_BOTTOM_LEFT_UP);
        #ifndef VERBOSE_STARTUP 
        display.progressBar(14,50,100,8,65);
        #endif

        dma_display->fillScreenRGB888(100,0,0);
        #ifdef VERBOSE_STARTUP
        display.println("HUB75测试：红色");
        #else
        display.progressBar(14,50,100,8,70);
        #endif
        delay(1000);
        dma_display->fillScreenRGB888(0,100,0);
        #ifdef VERBOSE_STARTUP
        display.println("HUB75测试：绿色");
        #else
        display.progressBar(14,50,100,8,75);
        #endif
        delay(1000);
        dma_display->fillScreenRGB888(0,0,100);
        #ifdef VERBOSE_STARTUP
        display.println("HUB75测试：蓝色");
        #else
        display.progressBar(14,50,100,8,80);
        #endif
        delay(1000);

        dma_display->clearScreen();
        Serial.println("Init OK!");
    }

    void Display() override {
        dma_display->setBrightness8(brightness);
        
        display.startWrite();
        display.drawRect(0,0,66,34,TFT_WHITE);
        for (uint16_t y = 0; y < 32; y++) {
            for (uint16_t x = 0; x < 64; x++){
                uint16_t pixelNum = y * 64 + x;
                virtualDisp->drawPixelRGB888(63 - x, (y) + 32, (uint16_t)camPixels1->GetColor(pixelNum)->R, (uint16_t)camPixels1->GetColor(pixelNum)->G, (uint16_t)camPixels1->GetColor(pixelNum)->B);
                virtualDisp->drawPixelRGB888(63 - x, (31 - y), (uint16_t)camPixels1->GetColor(pixelNum)->R, (uint16_t)camPixels1->GetColor(pixelNum)->G, (uint16_t)camPixels1->GetColor(pixelNum)->B);
                display.drawPixel(64 - x, (32 - y), display.color888((camPixels1->GetColor(pixelNum)->R ? 255 : 0), (camPixels1->GetColor(pixelNum)->G ? 255 : 0), (camPixels1->GetColor(pixelNum)->B ? 255 : 0)));
            }
        }
        display.display();
        display.endWrite();
    }
};
