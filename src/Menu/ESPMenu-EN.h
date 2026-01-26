// #include <esp_now.h>
#include <WiFi.h>
#include <Adafruit_NeoPixel.h>
#include <Wire.h>

#include <BLE2902.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <esp_attr.h>
#include <esp_task_wdt.h>
#include <driver/rtc_io.h>
#include <esp_mac.h>
#include <esp_timer.h>
#include <string>

#include <M5Unified.h>
#include <M5UnitGLASS2.h>

extern M5UnitGLASS2 display;

#ifdef NEW_GESTURE
#include "RevEng_PAJ7620.h"
#else
#include <SparkFun_APDS9960.h>
#endif

#include "../Filter/MinFilter.h"
#include "../Signals/TimeStep.h"
#include "../Flash/Icons/Icons.h"

#define DEMO_MODE 0

// 73cf57c7-6797-46e8-8202-dc5e7f956b57 Bluetooth UUID

#define WIFI_CHANNEL 1
// M5UnitOLED display(48, 47, 400000, 2); // SDA, SCL, FREQ
extern Adafruit_NeoPixel nowpixels;

#ifdef NEW_GESTURE
RevEng_PAJ7620 PAJ7620_sensor = RevEng_PAJ7620();
#else
SparkFun_APDS9960 apds = SparkFun_APDS9960();
#endif

extern std::string user_name;

const char *BLE_SERIAL2_SERVICE_UUID = "73cf57c7-6797-46e8-8202-dc5e7f956b57";
extern std::string BLE_RX2_UUID;
extern std::string BLE_TX2_UUID;

static BLEServer *bleServer = nullptr;
static BLECharacteristic *bleTxCharacteristic = nullptr;
static bool bleDeviceConnected = false;
static bool bleOldDeviceConnected = false;

uint32_t raw_data = 32000;

namespace
{
    class MenuBleServerCallbacks : public BLEServerCallbacks
    {
        void onConnect(BLEServer *server) override
        {
            bleDeviceConnected = true;
            Serial.println("BLE client connected");
        }

        void onDisconnect(BLEServer *server) override
        {
            bleDeviceConnected = false;
            Serial.println("BLE client disconnected");
        }
    };

    class MenuBleRxCallbacks : public BLECharacteristicCallbacks
    {
        void onWrite(BLECharacteristic *characteristic) override
        {
            const std::string rxValue = characteristic->getValue().c_str();
            if (rxValue.empty())
            {
                return;
            }

            raw_data = 0;
            for (size_t i = 0; i < rxValue.size() && i < 8; ++i)
            {
                const char c = rxValue[i];
                if (c >= '0' && c <= '9')
                {
                    raw_data = raw_data * 10 + static_cast<uint32_t>(c - '0');
                }
            }
            Serial.printf("BLE RX raw_data: %lu\n", static_cast<unsigned long>(raw_data));
        }
    };
}

class Menu
{
private:
    static esp_timer_handle_t demoTimer;
    static uint8_t faceCount;
    static uint8_t currentMenu;
    static float wiggleRatio;
    static bool isSecondary;

    static float rotation;
    static float showMenuRatio;

    static float GyroX;
    static float GyroY;
    static float GyroZ;

    static float AccelerometerX;
    static float AccelerometerY;
    static float AccelerometerZ;

    static uint8_t faceState;
    static uint8_t bright;
    static uint8_t accentBright;
    static uint8_t microphone;
    static uint8_t micLevel;
    static uint8_t boopSensor;
    static uint8_t spectrumMirror;
    static uint8_t faceSize;
    static uint8_t confirm;
    static uint8_t color;
    static uint8_t voiceenable;

    static uint8_t facialexpression;
    static uint8_t tempvalue;

    static uint8_t data_type;

    static uint8_t onetime;
    static uint8_t dmode;

    static uint8_t proximity;
    static uint8_t threshold;
    static float tempHue;
    static MinFilter<10> minF;
    static TimeStep timeStep;
    static float minimum;
    static bool didBegin;
    static bool isBright;
    static bool isProx;
    static bool mouth;

    // static void InitESPNow()
    //{
    //     WiFi.disconnect();
    //     if (esp_now_init() == ESP_OK)
    //     {
    //         Serial.println("ESPNow Init Success");
    //     }
    //     else
    //     {
    //         Serial.println("ESPNow Init Failed");
    //         // Retry InitESPNow, add a counte and then restart?
    //  InitESPNow();
    //   or Simply Restart
    //        ESP.restart();
    //    }
    //}

    static void DemoUpdateCallback(void * /*arg*/)
    {
        facialexpression++;
        if (facialexpression == faceCount)
            facialexpression = 0;
    }

    static void OnDataRecv(const uint8_t *mac_addr, const uint8_t *data, int data_len)
    {

        memcpy(&raw_data, data, sizeof(data));
        data_type = (uint8_t)(raw_data >> 24);
        if (data_type == 0)
        {
            tempvalue = (uint8_t)(raw_data & 0x000000FF);
            confirm = (uint8_t)((raw_data & 0x00FF0000) >> 16);
            if (confirm == 255)
            {
                facialexpression = tempvalue;
                confirm = 0;
            }
        }
        else if (data_type == 1)
        {
            tempvalue = (uint8_t)((raw_data & 0x0000FF00) >> 8);
            confirm = (uint8_t)((raw_data & 0x00FF0000) >> 16);
            if (confirm == 255)
            {
                bright = tempvalue;
                confirm = 0;
            }
        }
        else if (data_type == 2)
        {
            tempvalue = (uint8_t)(raw_data & 0x000000FF);
            confirm = (uint8_t)((raw_data & 0x00FF0000) >> 16);
            if (confirm == 255)
            {
                voiceenable = tempvalue;
                confirm = 0;
            }
        }
        else if (data_type == 3)
        {
            tempvalue = (uint8_t)(raw_data & 0x000000FF);
            confirm = (uint8_t)((raw_data & 0x00FF0000) >> 16);
            if (confirm == 255)
            {
                dmode = tempvalue;
                confirm = 0;
            }
        }
    }

    void startBluetooth()
    {
        BLEDevice::init(user_name.c_str());
        bleServer = BLEDevice::createServer();
        bleServer->setCallbacks(new MenuBleServerCallbacks());

        BLEService *service = bleServer->createService(BLE_SERIAL2_SERVICE_UUID);

        bleTxCharacteristic = service->createCharacteristic(BLE_TX2_UUID.c_str(), BLECharacteristic::PROPERTY_NOTIFY);

        BLECharacteristic *rxCharacteristic = service->createCharacteristic(BLE_RX2_UUID.c_str(), BLECharacteristic::PROPERTY_WRITE);
        rxCharacteristic->setCallbacks(new MenuBleRxCallbacks());

        service->start();
        bleServer->getAdvertising()->start();
        Serial.println("BLE UART ready, waiting for client...");
    }

public:
    void Initialize(uint8_t faceCount, uint8_t threshold)
    {
        #ifdef VERBOSE_STARTUP
        display.println("Initialize Bluetooth...");
        #else
        display.progressBar(14,50,100,8,18);
        #endif
        display.display();
        delay(200);
        Menu::faceCount = faceCount;
        Menu::threshold = threshold;

        //Serial.begin(115200);
        //Serial.println("ESPNow Receive init.");
        //WiFi.mode(WIFI_AP);
        //Serial.println("WiFi OK.");
        //Serial.print("AP MAC: ");
        //Serial.println(WiFi.softAPmacAddress());
        Wire1.setPins(38, 39);
        //Wire.begin();
        #ifndef VERBOSE_STARTUP 
        display.progressBar(14,50,100,8,20);
        #endif
        //Wire.setPins(38, 39);
        //Wire.begin(38, 39);
        // Init ESPNow with a fallback logic
        Serial.println("IIC OK.");
        pinMode(21,OUTPUT);
        #ifndef VERBOSE_STARTUP 
        display.progressBar(14,50,100,8,22);
        #endif

        // InitESPNow();
        nowpixels.begin();
        // esp_now_register_recv_cb(OnDataRecv);
        nowpixels.clear();
        nowpixels.setPixelColor(0, nowpixels.Color(50, 50, 50));
        #ifndef VERBOSE_STARTUP 
        display.progressBar(14,50,100,8,25);
        #endif

        startBluetooth();
        #ifndef VERBOSE_STARTUP 
        display.progressBar(14,50,100,8,30);
        #endif

        #ifdef VERBOSE_STARTUP
        display.println("Initialize Gesture sensor...");
        #else
        display.progressBar(14,50,100,8,35);
        #endif

        delay(200);
        display.display();
        if (
        #ifdef NEW_GESTURE
            !PAJ7620_sensor.begin(&Wire)
        #else
            !apds.init()
        #endif
    )
        {
            #ifdef VERBOSE_STARTUP
            display.println("Failed to Initialize Gesture sensor");
            display.println("Please check connection");
            display.display();
            #endif
            Serial.println("failed to initialize device! Please check your wiring.");
            didBegin = false;
            delay(2000);
        }
        else
        {
            #ifdef VERBOSE_STARTUP
            display.println("Gesture sensor Initialized");
            #else
            display.progressBar(14,50,100,8,40);
            #endif
            Serial.println("Device initialized!");
            didBegin = true;
        }

        #ifdef NEW_GESTURE
        PAJ7620_sensor.setProximityMode();
        #else
        if(apds.setProximityGain(PGAIN_2X)){
            #ifdef VERBOSE_STARTUP
            display.println("Gesture sensor Gain set X2");
            #else
            display.progressBar(14,50,100,8,42);
            #endif
        }
        apds.enableProximitySensor(false);
        #endif

        #ifndef VERBOSE_STARTUP 
        display.progressBar(14,50,100,8,45);
        #endif

        if (DEMO_MODE && demoTimer == nullptr)
        {
            esp_timer_create_args_t args = {};
            args.callback = &DemoUpdateCallback;
            args.arg = nullptr;
            args.dispatch_method = ESP_TIMER_TASK;
            args.name = "demo_timer";
            args.skip_unhandled_events = false;

            if (esp_timer_create(&args, &demoTimer) == ESP_OK)
            {
                esp_timer_start_periodic(demoTimer, 5000000); // 5s
            }
        }
        #ifdef VERBOSE_STARTUP
        display.println("Sensors Initialized");
        #else
        display.progressBar(14,50,100,8,47);
        #endif
        display.display();
        delay(500);
    }

    static void Update()
    {
        if (bleServer != nullptr)
        {
            if (!bleDeviceConnected && bleOldDeviceConnected)
            {
                delay(500);
                bleServer->startAdvertising();
                Serial.println("Restarted BLE advertising...");
                bleOldDeviceConnected = false;
            }
            else if (bleDeviceConnected && !bleOldDeviceConnected)
            {
                bleOldDeviceConnected = true;
            }
        }

        //free_mem = (int)((float)((float)ESP.getFreeHeap() / (float)ESP.getHeapSize())*100.0f);
        display.startWrite();
        // display.clearDisplay();
        // display.drawString("Current: ", 0, 12);
        // display.drawString("Brightness: ", 0, 24);
        // display.drawString("Lip Sync: ", 0, 36);
        data_type = (uint8_t)(raw_data >> 24);
        if (data_type == 0)
        {
            tempvalue = (uint8_t)(raw_data & 0x000000FF);
            confirm = (uint8_t)((raw_data & 0x00FF0000) >> 16);
            if (confirm == 255 && facialexpression != tempvalue)
            {
                facialexpression = tempvalue;
                confirm = 0;
                display.fillRect(65, 37, 14, 12, TFT_BLACK);
                display.display();
            }
        }
        else if (data_type == 1)
        {
            tempvalue = (uint8_t)((raw_data & 0x0000FF00) >> 8);
            confirm = (uint8_t)((raw_data & 0x00FF0000) >> 16);
            if (confirm == 255 && bright != tempvalue)
            {
                bright = tempvalue;
                confirm = 0;
                display.fillRect(40, 50, 20, 12, TFT_BLACK);
                display.display();
            }
        }
        else if (data_type == 2)
        {
            tempvalue = (uint8_t)(raw_data & 0x000000FF);
            confirm = (uint8_t)((raw_data & 0x00FF0000) >> 16);
            if (confirm == 255  && voiceenable != tempvalue)
            {
                voiceenable = tempvalue;
                display.fillRect(65, 50, 60, 12, TFT_BLACK);
                display.display();
                confirm = 0;
            }
        }
        else if (data_type == 3)
        {
            tempvalue = (uint8_t)(raw_data & 0x000000FF);
            confirm = (uint8_t)((raw_data & 0x00FF0000) >> 16);
            if (confirm == 255)
            {
                dmode = tempvalue;
                confirm = 0;
            }
        }else if (data_type == 4)
        {
            confirm = (uint8_t)((raw_data & 0x00FF0000) >> 16);
            if (confirm == 255)
            {
                tempHue = (uint16_t)(raw_data & 0x0000FFFF) / 8;
                confirm = 0;
            }
        }

        /*
        data_type 0 -> facialexpression data
        data_type 1 -> brightness
        data_type 2 -> Gyro X with 6 radix
        data_type 3 -> Gyro Y with 6 radix
        data_type 4 -> Gyro Z with 6 radix
        data_type 5 -> Accelerometer X with 6 radix
        data_type 6 -> Accelerometer Y with 6 radix
        data_type 7 -> Accelerometer Z with 6 radix
        data_type 8 -> Reverse
        data_type 9 -> Device's Battery status
        data_type 10-15 -> Resistance bending sensor value
        data_type 16 voiceDetection Enable/Disable
        */

        if (data_type == 0)
        {
            // display.drawString("Set facial expression", 0, 0);
            if (tempvalue == 0)
            {
                nowpixels.setPixelColor(0, nowpixels.Color(0, 100, 0)); // Green
                // display.drawString("Default", 55, 12);
            }
            else if (tempvalue == 1)
            {
                nowpixels.setPixelColor(0, nowpixels.Color(100, 0, 0)); // Red
                // display.drawString("Angry", 55, 12);
            }
            else if (tempvalue == 2)
            {
                nowpixels.setPixelColor(0, nowpixels.Color(120, 60, 0)); // Orange
                // display.drawString("Doubt", 55, 12);
            }
            else if (tempvalue == 3)
            {
                nowpixels.setPixelColor(0, nowpixels.Color(5, 140, 120)); // Cyan
                // display.drawString("Frown", 55, 12);
            }
            else if (tempvalue == 4)
            {
                nowpixels.setPixelColor(0, nowpixels.Color(100, 0, 100)); // Purple
                // display.drawString("Heart", 55, 12);
            }
            else if (tempvalue == 5)
            {
                nowpixels.setPixelColor(0, nowpixels.Color(0, 0, 100)); // Blue
                // display.drawString("Sad", 55, 12);
            }
            else if (tempvalue == 6)
            {
                nowpixels.setPixelColor(0, nowpixels.Color(226, 213, 70)); // Blue
                // display.drawString("Surprised", 55, 12);
            }
            else if (tempvalue == 7)
            {
                nowpixels.setPixelColor(0, nowpixels.Color(0, 213, 70)); // Blue
                // display.drawString("Happy", 55, 12);
            }
            else
            {
                nowpixels.setPixelColor(0, nowpixels.Color(100, 100, 2)); // Yellow
                // display.drawString("OwO", 55, 12);
            }
        }
        else if (data_type == 1)
        {
            // display.drawString("Set brightness", 0, 0);
            nowpixels.setPixelColor(0, nowpixels.Color(tempvalue, tempvalue, tempvalue));
            // display.drawNumber(tempvalue, 72, 24);
        }
        else if (data_type == 16)
        {
            GyroX = (uint32_t)(raw_data & 0x00FFFFFF) / 1000000;
        }
        else if (data_type == 8)
        {
            GyroY = (uint32_t)(raw_data & 0x00FFFFFF) / 1000000;
        }
        //else if (data_type == 4)
        //{
        //    GyroZ = (uint32_t)(raw_data & 0x00FFFFFF) / 1000000;
        //}
        else if (data_type == 5)
        {
            AccelerometerX = (uint32_t)(raw_data & 0x00FFFFFF) / 1000000;
        }
        else if (data_type == 6)
        {
            AccelerometerY = (uint32_t)(raw_data & 0x00FFFFFF) / 1000000;
        }
        else if (data_type == 7)
        {
            AccelerometerZ = (uint32_t)(raw_data & 0x00FFFFFF) / 1000000;
        }
        else if (data_type == 2)
        {
            // display.drawString("Set Lipsync", 0, 0);
            if (tempvalue == 1)
            {
                nowpixels.setPixelColor(0, nowpixels.Color(20, 20, 40)); // Green
            }
            else
            {
                nowpixels.setPixelColor(0, nowpixels.Color(40, 20, 20)); // Yellow
            }
        }
        else if (data_type == 3)
        {
            // display.drawString("Enter WiFi Display", 0, 0);
            if (tempvalue == 1)
            {
                nowpixels.setPixelColor(0, nowpixels.Color(00, 120, 40));
                // display.drawString("Accept", 0, 48);
            }
            else
            {
                nowpixels.setPixelColor(0, nowpixels.Color(120, 40, 0));
                // display.drawString("Cancel", 0, 48);
            }
        }

        if (bright >= 120)
        {
            // display.drawString("Enable", 60, 36);
            digitalWrite(21,HIGH);
        }
        else
        {
            // display.drawString("Disable", 60, 36);
            digitalWrite(21,LOW);
        }

        // display.endWrite();
        //display.qrcode(BLE_RX2_UUID, 70, 2, 29, 3);
        display.drawString("Exp Number: ", 5, 37);
        display.drawNumber(facialexpression, 70, 37);

        display.drawString("bright：", 5, 50);
        display.drawNumber(bright, 55, 50);

        //display.drawNumber(free_mem, 80, 37);

        if(voiceenable == 1){
            display.pushImageDMA(66, 0, 24, 24, epd_bitmap_microphone);
        }else{
            display.pushImageDMA(66, 0, 24, 24, epd_bitmap_microphone_off);
        }
        if(bleDeviceConnected){
            display.pushImageDMA(90, 0, 24, 24, epd_bitmap_bluetooth);
        }else{
            display.fillRect(90,0,24,24,TFT_BLACK);
        }

        display.display();
        display.endWrite();
        nowpixels.show();
    }

    static bool GetMouthState()
    {
        return (dmode == 1);
    }

    static uint8_t GetMicLevel()
    {
        return micLevel;
    }

    static uint8_t GetFaceState()
    {
        return facialexpression;
    }

    static uint8_t GetBrightness()
    {
        return bright;
    }

    static uint8_t GetAccentBrightness()
    {
        return accentBright;
    }

    static uint8_t MirrorSpectrumAnalyzer()
    {
        return spectrumMirror;
    }
    static uint8_t GetFaceColor()
    {
        return color;
    }

    static float GetHueShift()
    {
        return tempHue;
    }

    uint8_t displayMode()
    {
        return dmode;
    }

    static bool GetvoiceDetectionEnable()
    {
        return (voiceenable == 1);
    }

    static bool UseBoopSensor()
    {
        return true;
    }

    static bool isBooped()
    { 
        #ifdef NEW_GESTURE
        proximity = PAJ7620_sensor.getProximityDistance();
        #else
        apds.readProximity(proximity);
        #endif

        if (timeStep.IsReady())
        {
            minimum = minF.Filter(proximity);
        }

        return proximity > minimum + threshold;
    }
};

float Menu::rotation;
float Menu::showMenuRatio;

uint8_t Menu::faceCount;
uint8_t Menu::currentMenu = 0;
float Menu::wiggleRatio = 1.0f;
bool Menu::isSecondary = 0;
uint8_t Menu::faceState = 0;
uint8_t Menu::bright = 105;
uint8_t Menu::accentBright = 0;
uint8_t Menu::microphone = 0;
uint8_t Menu::micLevel = 0;
uint8_t Menu::boopSensor = 0;
uint8_t Menu::spectrumMirror = 0;
uint8_t Menu::faceSize = 0;
uint8_t Menu::confirm = 0;
uint8_t Menu::color = 0;
uint8_t Menu::facialexpression = 0;
uint8_t Menu::voiceenable = 1;
uint8_t Menu::tempvalue = 0;
uint8_t Menu::data_type = 0;
uint8_t Menu::onetime = 0;
uint8_t Menu::dmode = 1;
float Menu::GyroX = 0;
float Menu::GyroY = 0;
float Menu::GyroZ = 0;
float Menu::AccelerometerX = 0;
float Menu::AccelerometerY = 0;
float Menu::AccelerometerZ = 0;
esp_timer_handle_t Menu::demoTimer = nullptr;

uint8_t Menu::proximity;
uint8_t Menu::threshold;

MinFilter<10> Menu::minF = MinFilter<10>(false);
TimeStep Menu::timeStep = TimeStep(10);
float Menu::minimum = 0.0f;
bool Menu::didBegin = false;
bool Menu::isBright = false;
bool Menu::isProx = false;
bool Menu::mouth = false;
float Menu::tempHue = 0.0;