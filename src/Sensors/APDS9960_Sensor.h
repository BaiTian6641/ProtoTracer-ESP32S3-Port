#pragma once

#include <Arduino.h>
//#include <Adafruit_SPIDevice.h>
#include <Arduino_APDS9960.h>
#include "../Filter/MinFilter.h"
#include "../Signals/TimeStep.h"

class APDS9960_Sensor{
private:
    static uint16_t proximity;
    static uint16_t threshold;
    static MinFilter<10> minF;
    static TimeStep timeStep;
    static float minimum;
    static bool didBegin;
    static bool isBright;
    static bool isProx;

public:
    void Initialize(uint8_t threshold) {//timeout in milliseconds and threshold is minimum for detection (0 is far away, 255 is touching)
        APDS9960_Sensor::threshold = threshold;
        Serial0.begin(115200);
        if(Wire1.setPins(47,48)) Serial0.print("IIC Successful.");
        if (!APDS.begin())
        {
            Serial0.println("failed to initialize device! Please check your wiring.");
        }
        else
            Serial0.println("Device initialized!");

        //apds.setLED(APDS9960_LEDDRIVE_12MA, APDS9960_LEDBOOST_100PCNT);
        //apds.setProxGain(APDS9960_PGAIN_1X);
    }

    static bool isBooped(){
        GetValue();
        
        if(timeStep.IsReady()){
            minimum = minF.Filter(proximity);
        }

        return proximity > minimum + threshold;
    }

    static uint8_t GetValue(){
        if (didBegin && APDS.proximityAvailable()){
            proximity = APDS.readProximity();
        }

        return proximity;
    }
    
    static uint16_t GetBrightness(){

        uint16_t brightness;
        int r, g, b;

        if (didBegin && APDS.colorAvailable()){
            APDS.readColor(r, g, b);

            brightness = r + g + b;
        }

        return (uint16_t)brightness;
    }
};

uint16_t APDS9960_Sensor::proximity;
uint16_t APDS9960_Sensor::threshold;

MinFilter<10> APDS9960_Sensor::minF = MinFilter<10>(false);
TimeStep APDS9960_Sensor::timeStep = TimeStep(5);
float APDS9960_Sensor::minimum = 0.0f;
bool APDS9960_Sensor::didBegin = false;
bool APDS9960_Sensor::isBright = false;
bool APDS9960_Sensor::isProx = false;
