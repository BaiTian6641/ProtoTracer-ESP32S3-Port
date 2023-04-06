#include <esp_now.h>
#include <WiFi.h>
#include <Adafruit_NeoPixel.h>

#define WIFI_CHANNEL 1
Adafruit_NeoPixel nowpixels(1, 45, NEO_GRB + NEO_KHZ800);
//uint8_t localCustomMac[] = {0xE3, 0xFA, 0x12, 0x33, 0x33, 0x33};E3FA12F4
const byte maxDataFrameSize = 200;

class Menu{
private:
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
    static uint8_t color;

    static uint8_t facialexpression;
    static uint8_t tempvalue;

    static uint8_t data_type;

    static uint32_t raw_data;

    static void InitESPNow(){
        WiFi.disconnect();
        if (esp_now_init() == ESP_OK) {
            Serial0.println("ESPNow Init Success");
        }else{
            Serial0.println("ESPNow Init Failed");
                // Retry InitESPNow, add a counte and then restart?
            //InitESPNow();
                // or Simply Restart
            ESP.restart();
        }
    }

       // const char *SSID = "TestE3FA12F4";

    static void configDeviceAP() {
  const char *SSID = "Slave_1";
  bool result = WiFi.softAP(SSID, "Slave_1_Password", 2, 0);
  if (!result) {
    Serial.println("AP Config failed.");
  } else {
    Serial.println("AP Config Success. Broadcasting with AP: " + String(SSID));
    Serial.print("AP CHANNEL "); Serial.println(WiFi.channel());
  }
}

    static void OnDataRecv(const uint8_t *mac_addr, const uint8_t *data, int data_len) {
        char macStr[18];
        snprintf(macStr, sizeof(macStr), "%02x:%02x:%02x:%02x:%02x:%02x",
            mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);
            memcpy(&raw_data, data, sizeof(data));
        Serial0.print("Last Packet Recv from: "); Serial0.println(macStr);
        Serial0.print("Last Packet Length: "); Serial0.println(data_len);
        Serial0.print("Last Packet Recv Data: "); Serial0.println(raw_data);
        Serial0.println("");
}

public:
static void Initialize(uint8_t faceCount){
    Menu::faceCount = faceCount;

    Serial0.begin(115200);
    Serial0.println("ESPNow Receive init.");
    WiFi.mode(WIFI_AP);
    Serial0.print("AP MAC: "); Serial0.println(WiFi.softAPmacAddress());
        // Init ESPNow with a fallback logic
    InitESPNow();
    nowpixels.begin();
    esp_now_register_recv_cb(OnDataRecv);
    nowpixels.clear();
}

static void Update(float ratio){
        InitESPNow();
        esp_now_register_recv_cb(OnDataRecv);

        data_type = (uint8_t)(raw_data >> 24);
        /*
        data_type 0 -> color, brightness, and facialexpression data
        data_type 1 -> Gyro X with 6 radix
        data_type 2 -> Gyro Y with 6 radix
        data_type 3 -> Gyro Z with 6 radix
        data_type 4 -> Accelerometer X with 6 radix
        data_type 5 -> Accelerometer Y with 6 radix
        data_type 6 -> Accelerometer Z with 6 radix
        data_type 7 -> Reverse
        data_type 8 -> Device's Battery status
        data_type 9-15 -> Resistance bending sensor value
        */

        if(data_type == 0){
            color = (uint8_t)((raw_data & 0x00FF0000) >> 16);
            bright = (uint8_t)((raw_data & 0x0000FF00) >> 8);
            facialexpression = (uint8_t)(raw_data & 0x000000FF);
            if (facialexpression == 0)      nowpixels.setPixelColor(0, nowpixels.Color(0, 100, 0));   //Green
                else if (facialexpression == 1) nowpixels.setPixelColor(0, nowpixels.Color(100, 0, 0));   //Red
                else if (facialexpression == 2) nowpixels.setPixelColor(0, nowpixels.Color(120, 60, 0));   //Orange
                else if (facialexpression == 3) nowpixels.setPixelColor(0, nowpixels.Color(5, 140, 120));   //Cyan
                else if (facialexpression == 4) nowpixels.setPixelColor(0, nowpixels.Color(100, 0, 100));   //Purple
                else if (facialexpression == 5) nowpixels.setPixelColor(0, nowpixels.Color(0, 0, 100));   //Blue
                else                     nowpixels.setPixelColor(0, nowpixels.Color(100, 100, 2));   //Yellow
        }else if(data_type == 1){
            GyroX = (uint32_t)(raw_data & 0x00FFFFFF) / 1000000;
        }else if(data_type == 2){
            GyroY = (uint32_t)(raw_data & 0x00FFFFFF) / 1000000;
        }else if(data_type == 3){
            GyroZ = (uint32_t)(raw_data & 0x00FFFFFF) / 1000000;
        }else if(data_type == 4){
            AccelerometerX = (uint32_t)(raw_data & 0x00FFFFFF) / 1000000;
        }else if(data_type == 5){
            AccelerometerY = (uint32_t)(raw_data & 0x00FFFFFF) / 1000000;
        }else if(data_type == 6){
            AccelerometerZ = (uint32_t)(raw_data & 0x00FFFFFF) / 1000000;
        }

        /*
        if(raw_data == 2){
            currentMenu++;
            tempvalue = 0;
            if(currentMenu >= 2){
                currentMenu = 0;
            }
        }

        if(currentMenu == 0){
            tempvalue = facialexperssion;
            InitESPNow();
            esp_now_register_recv_cb(OnDataRecv);
            if(raw_data == 8){
                tempvalue++;

            }else if (raw_data == 4){
                tempvalue--;
            }
            InitESPNow();
            esp_now_register_recv_cb(OnDataRecv);
            if(tempvalue > faceCount || tempvalue < 0){
                tempvalue = 0;
            }
                if (tempvalue == 0)      nowpixels.setPixelColor(0, nowpixels.Color(0, 100, 0));   //Green
                else if (tempvalue == 1) nowpixels.setPixelColor(0, nowpixels.Color(100, 0, 0));   //Red
                else if (tempvalue == 2) nowpixels.setPixelColor(0, nowpixels.Color(120, 60, 0));   //Orange
                else if (tempvalue == 3) nowpixels.setPixelColor(0, nowpixels.Color(5, 140, 120));   //Cyan
                else if (tempvalue == 4) nowpixels.setPixelColor(0, nowpixels.Color(100, 0, 100));   //Purple
                else if (tempvalue == 5) nowpixels.setPixelColor(0, nowpixels.Color(0, 0, 100));   //Blue
                else                     nowpixels.setPixelColor(0, nowpixels.Color(100, 100, 2));   //Yellow
            if(raw_data == 1){
                facialexperssion = tempvalue;
            }
        }

        if(currentMenu == 1){
            tempvalue = bright;
            InitESPNow();
            esp_now_register_recv_cb(OnDataRecv);
            if(raw_data == 8){
                tempvalue+=7;

            }else if (raw_data == 4){
                tempvalue-=7;
            }
            if(tempvalue > 254 || tempvalue < 0){
                tempvalue = 0;
            }
            nowpixels.setPixelColor(0, nowpixels.Color(bright, bright, bright));
            InitESPNow();
            esp_now_register_recv_cb(OnDataRecv);
            if(raw_data == 1){
                bright = tempvalue;
            }
        }*/
        nowpixels.show();
        


    }


static uint8_t GetMicLevel(){
        return micLevel;
    }

static uint8_t GetFaceState(){
    return facialexpression;
    }

static uint8_t GetBrightness(){
        return bright;
    }

static uint8_t GetAccentBrightness(){
        return accentBright;
    }

static uint8_t MirrorSpectrumAnalyzer(){
        return spectrumMirror;
    }
    static uint8_t GetFaceColor(){
        return color;
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
uint8_t Menu::color = 0;
uint8_t Menu::facialexpression = 0;
uint8_t Menu::tempvalue = 0;
uint32_t Menu::raw_data = 0;
uint8_t Menu::data_type = 0;
float Menu::GyroX = 0;
float Menu::GyroY = 0;
float Menu::GyroZ = 0;

float Menu::AccelerometerX = 0;
float Menu::AccelerometerY = 0;
float Menu::AccelerometerZ = 0;