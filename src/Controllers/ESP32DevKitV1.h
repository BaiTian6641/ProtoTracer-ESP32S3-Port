#include <Arduino.h>

#include "Controller.h"
#include "Render/Camera.h"
#include "Flash/PixelGroups/P3HUB75.h"

//HUB75
#include <ESP32-VirtualMatrixPanel-I2S-DMA.h>
#include <FastLED.h>

#define R1_PIN_DEFAULT 4
#define G1_PIN_DEFAULT 5
#define B1_PIN_DEFAULT 6
#define R2_PIN_DEFAULT 7
#define G2_PIN_DEFAULT 15
#define B2_PIN_DEFAULT 16
#define A_PIN_DEFAULT  18
#define B_PIN_DEFAULT  8
#define C_PIN_DEFAULT  3
#define D_PIN_DEFAULT  42
#define E_PIN_DEFAULT  -1 // required for 1/32 scan panels, like 64x64. Any available pin would do, i.e. IO32
#define LAT_PIN_DEFAULT 40
#define OE_PIN_DEFAULT  2
#define CLK_PIN_DEFAULT 41

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

class ESP32DevKitV1 : public Controller {
private:
    CameraLayout cameraLayout = CameraLayout(CameraLayout::ZForward, CameraLayout::YUp);
    Transform camTransform1 = Transform(Vector3D(), Vector3D(0.0f, 0.0f, -500.0f), Vector3D(1, 1, 1));
    Transform camTransform2 = Transform(Vector3D(), Vector3D(0.0f, 0.0f, -500.0f), Vector3D(1, 1, 1));

    PixelGroup<2048> camPixels1 = PixelGroup<2048>(P3HUB75);
    PixelGroup<2048> camPixels2 = PixelGroup<2048>(P3HUB75);
    
    Camera<2048> camMain1 = Camera<2048>(&camTransform1, &cameraLayout, &camPixels1);
    Camera<2048> camMain2 = Camera<2048>(&camTransform2, &cameraLayout, &camPixels2);

    //CameraBase* cameras[1] = { &camMain1 };
    CameraBase* cameras[2] = { &camMain1, &camMain2 };

public:
    ESP32DevKitV1(uint8_t maxBrightness) : Controller(cameras, 2, maxBrightness, 0){}

    void Initialize() override{

        HUB75_I2S_CFG mxconfig(
                PANEL_RES_X,   // module width
                PANEL_RES_Y,   // module height
                PANEL_CHAIN    // chain length
        );

        // OK, now we can create our matrix object
        dma_display = new MatrixPanel_I2S_DMA(mxconfig);

        // let's adjust default brightness to about 75%
        dma_display->setBrightness8(maxBrightness);    // range is 0-255, 0 - 0%, 255 - 100%

        // Allocate memory and start DMA display
        if(!dma_display->begin()){
            Serial.println("****** I2S memory allocation failed ***********");
        }

        // create VirtualDisplay object based on our newly created dma_display object
        virtualDisp = new VirtualMatrixPanel((*dma_display), NUM_ROWS, NUM_COLS, PANEL_RES_X, PANEL_RES_Y, CHAIN_BOTTOM_LEFT_UP);
        
        dma_display->fillScreenRGB888(128,0,0);
        delay(1500);

        dma_display->clearScreen();
        Serial0.println("Init OK!");
    }

    void Display() override {
        dma_display->setBrightness8(brightness);
        
        for (uint16_t y = 0; y < 32; y++) {
            for (uint16_t x = 0; x < 64; x++){
                uint16_t pixelNum = y * 64 + x;
                //Serial0.println("PIXEL COLOR R:"+camPixels1.GetColor(pixelNum)->ToString());

                virtualDisp->drawPixelRGB888(63 - x, (y) + 32, (uint16_t)camPixels1.GetColor(pixelNum)->R, (uint16_t)camPixels1.GetColor(pixelNum)->G, (uint16_t)camPixels1.GetColor(pixelNum)->B);
                virtualDisp->drawPixelRGB888(63 - x, (31 - y), (uint16_t)camPixels1.GetColor(pixelNum)->R, (uint16_t)camPixels1.GetColor(pixelNum)->G, (uint16_t)camPixels1.GetColor(pixelNum)->B);

            }
        }
        
        /*
        for (uint16_t y = 0; y < 32; y++) {
            for (uint16_t x = 0; x < 64; x++){
                uint16_t pixelNum = y * 64 + x;

                virtualDisp->drawPixelRGB888(63 - x, y + 32, (uint16_t)camPixels2.GetColor(pixelNum)->R, (uint16_t)camPixels2.GetColor(pixelNum)->G, (uint16_t)camPixels2.GetColor(pixelNum)->B);
            }
        }*/
    }
};
