#pragma once

// #include <esp_now.h>
#include <WiFi.h>
#include <Adafruit_NeoPixel.h>
#include <Wire.h>

#include <BLE2902.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <ArduinoJson.h>
#include <esp_attr.h>
#include <esp_task_wdt.h>
#include <driver/rtc_io.h>
#include <esp_mac.h>
#include <esp_timer.h>
#include <algorithm>
#include <string>

#include <M5Unified.h>
#include <M5UnitGLASS2.h>

#ifdef LANG_CN
#define TXT(en, cn) cn
#else
#define TXT(en, cn) en
#endif

extern M5UnitGLASS2 display;

#ifdef NEW_GESTURE
#include "RevEng_PAJ7620.h"
#else
#include <SparkFun_APDS9960.h>
#endif

#include "../Filter/MinFilter.h"
#include "../Signals/TimeStep.h"
#include "../Flash/Icons/Icons.h"
#include "../Network/UserConfigManager.h"

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
extern UserConfig userConfig;

const char *BLE_SERIAL2_SERVICE_UUID = "73cf57c7-6797-46e8-8202-dc5e7f956b57";
extern std::string BLE_RX2_UUID;
extern std::string BLE_TX2_UUID;

static BLEServer *bleServer = nullptr;
static BLECharacteristic *bleTxCharacteristic = nullptr;
static bool bleDeviceConnected = false;
static bool bleOldDeviceConnected = false;
static String blePendingJsonPayload;
static bool bleJsonPayloadPending = false;
static String bleRxJsonBuffer;

uint32_t raw_data = 32000;

namespace
{
    constexpr size_t kBleJsonChunkBytes = 160;
    constexpr size_t kBleRxJsonBufferBytes = 1024;
    constexpr uint8_t kRemoteCommandQueueSize = 8;
    constexpr uint8_t kRemoteControllerExpressionCount = 17;
    constexpr const char *kRemoteControllerExpressionNames[kRemoteControllerExpressionCount] = {
        "Default",
        "Angry",
        "Doubt",
        "Frown",
        "Heart",
        "Sad",
        "Surprise",
        "Happy",
        "OwO",
        "Surprise",
        "Sleepy",
        "Curious",
        "Excited",
        "Wink",
        "Shy",
        "Focus",
        "Custom",
    };

    volatile uint32_t remoteCommandQueue[kRemoteCommandQueueSize] = {};
    volatile uint8_t remoteCommandQueueHead = 0;
    volatile uint8_t remoteCommandQueueTail = 0;

    String BlePreviewText(const std::string &value)
    {
        constexpr size_t kPreviewBytes = 96;
        const size_t previewLength = std::min(kPreviewBytes, value.size());
        String preview;
        preview.reserve(previewLength + 1);

        for (size_t index = 0; index < previewLength; ++index)
        {
            const unsigned char c = static_cast<unsigned char>(value[index]);
            preview += (c >= 32 && c <= 126) ? static_cast<char>(c) : '.';
        }

        if (value.size() > previewLength)
        {
            preview += "...";
        }

        return preview;
    }

    void LogBleWritePreview(const std::string &value)
    {
        Serial.printf(
            "BLE RX write received: bytes=%u buffered=%u preview='%s'\n",
            static_cast<unsigned>(value.size()),
            static_cast<unsigned>(bleRxJsonBuffer.length()),
            BlePreviewText(value).c_str());
    }

    String EffectiveBleName()
    {
        if (!userConfig.username.isEmpty())
        {
            return userConfig.username;
        }

        if (!userConfig.device_id.isEmpty())
        {
            return userConfig.device_id;
        }

        return String(user_name.c_str());
    }

    String BuildRemoteControllerManifestJson()
    {
        DynamicJsonDocument doc(1536);
        const String relayBaseUrl = (WiFi.status() == WL_CONNECTED)
                                        ? (String("http://") + WiFi.localIP().toString() + "/api/relay/esp32c6")
                                        : String("");

        JsonObject device = doc.createNestedObject("device");
        device["display_name"] = EffectiveBleName();
        device["hardware_revision"] = "esp32s3";

        JsonObject pairing = doc.createNestedObject("pairing");
        pairing["transport"] = "ble";
        pairing["service_uuid"] = BLE_SERIAL2_SERVICE_UUID;
        pairing["ble_rx_uuid"] = BLE_RX2_UUID.c_str();
        pairing["ble_tx_uuid"] = BLE_TX2_UUID.c_str();
        pairing["config_endpoint"] = "/api/remote/config";
        pairing["bound_peer_id"] = EffectiveBleName();

        JsonObject visual = doc.createNestedObject("visual");
        visual["animation_asset"] = userConfig.user_animation;
        visual["expression_count"] = kRemoteControllerExpressionCount;
        JsonArray expressionNames = visual.createNestedArray("expression_names");
        for (uint8_t index = 0; index < kRemoteControllerExpressionCount; ++index)
        {
            expressionNames.add(kRemoteControllerExpressionNames[index]);
        }
        visual["red"] = userConfig.user_r;
        visual["green"] = userConfig.user_g;
        visual["blue"] = userConfig.user_b;

        JsonObject repo = doc.createNestedObject("repo");
        repo["asset_base_url"] = relayBaseUrl;

        String payload;
        serializeJson(doc, payload);
        return payload;
    }

    void NotifyBleJsonPayload(const String &payload)
    {
        if (!bleDeviceConnected || bleTxCharacteristic == nullptr || payload.isEmpty())
        {
            return;
        }

        for (size_t offset = 0; offset < payload.length(); offset += kBleJsonChunkBytes)
        {
            const size_t chunkLength = std::min(kBleJsonChunkBytes, payload.length() - offset);
            bleTxCharacteristic->setValue(reinterpret_cast<const uint8_t *>(payload.c_str() + offset), chunkLength);
            bleTxCharacteristic->notify();
            delay(12);
        }
    }

    void QueueBleJsonPayload(const String &payload)
    {
        if (payload.isEmpty())
        {
            return;
        }

        blePendingJsonPayload = payload;
        bleJsonPayloadPending = true;
    }

    void FlushQueuedBleJsonPayload()
    {
        if (!bleJsonPayloadPending)
        {
            return;
        }

        if (!bleDeviceConnected || bleTxCharacteristic == nullptr)
        {
            return;
        }

        NotifyBleJsonPayload(blePendingJsonPayload);
        blePendingJsonPayload = String("");
        bleJsonPayloadPending = false;
    }

    String BuildRemoteControllerStateJson(const JsonDocument &doc)
    {
        DynamicJsonDocument response(384);
        response["op"] = "control.state";
        response["accepted"] = true;

        if (!doc["expression"].isNull())
        {
            response["expression"] = doc["expression"].as<int>();
        }
        if (!doc["brightness"].isNull())
        {
            response["brightness"] = doc["brightness"].as<int>();
        }
        if (!doc["voice_enabled"].isNull())
        {
            response["voice_enabled"] = doc["voice_enabled"].as<bool>();
        }
        if (!doc["display_mode"].isNull())
        {
            response["display_mode"] = doc["display_mode"].as<int>();
        }
        if (!doc["hue_shift"].isNull())
        {
            response["hue_shift"] = doc["hue_shift"].as<float>();
        }

        String payload;
        serializeJson(response, payload);
        return payload;
    }

    uint32_t EncodeLegacyCommand(const uint8_t type, const uint16_t value)
    {
        switch (type)
        {
        case 1:
            return (static_cast<uint32_t>(type) << 24) | (0xFFUL << 16) | ((static_cast<uint32_t>(value) & 0xFFUL) << 8);
        case 4:
            return (static_cast<uint32_t>(type) << 24) | (0xFFUL << 16) | (static_cast<uint32_t>(value) & 0xFFFFUL);
        case 0:
        case 2:
        case 3:
        default:
            return (static_cast<uint32_t>(type) << 24) | (0xFFUL << 16) | (static_cast<uint32_t>(value) & 0xFFUL);
        }
    }

    bool EnqueueLegacyCommand(const uint32_t command)
    {
        const uint8_t nextHead = static_cast<uint8_t>((remoteCommandQueueHead + 1) % kRemoteCommandQueueSize);
        if (nextHead == remoteCommandQueueTail)
        {
            Serial.printf("BLE legacy command queue full, dropping raw=0x%08lx\n", static_cast<unsigned long>(command));
            return false;
        }

        remoteCommandQueue[remoteCommandQueueHead] = command;
        remoteCommandQueueHead = nextHead;
        Serial.printf(
            "BLE legacy command queued: raw=0x%08lx head=%u tail=%u\n",
            static_cast<unsigned long>(command),
            static_cast<unsigned>(remoteCommandQueueHead),
            static_cast<unsigned>(remoteCommandQueueTail));
        return true;
    }

    bool EnqueueLegacyCommand(const uint8_t type, const uint16_t value, uint32_t *encodedCommand)
    {
        const uint32_t command = EncodeLegacyCommand(type, value);
        if (encodedCommand != nullptr)
        {
            *encodedCommand = command;
        }
        return EnqueueLegacyCommand(command);
    }

    bool DequeueLegacyCommand(uint32_t *command)
    {
        if (command == nullptr || remoteCommandQueueTail == remoteCommandQueueHead)
        {
            return false;
        }

        *command = remoteCommandQueue[remoteCommandQueueTail];
        remoteCommandQueueTail = static_cast<uint8_t>((remoteCommandQueueTail + 1) % kRemoteCommandQueueSize);
        return true;
    }

    void ClearQueuedLegacyCommands()
    {
        remoteCommandQueueHead = 0;
        remoteCommandQueueTail = 0;
    }

    bool IsJsonWhitespace(const char value)
    {
        return value == ' ' || value == '\r' || value == '\n' || value == '\t';
    }

    void TrimLeadingBleJsonWhitespace()
    {
        while (bleRxJsonBuffer.length() > 0 && IsJsonWhitespace(bleRxJsonBuffer[0]))
        {
            bleRxJsonBuffer.remove(0, 1);
        }
    }

    bool ExtractCompleteJsonObject(const String &source, String *json, size_t *nextIndex)
    {
        size_t start = 0;
        while (start < source.length() && source[start] != '{')
        {
            if (!IsJsonWhitespace(source[start]))
            {
                return false;
            }
            ++start;
        }

        if (start >= source.length())
        {
            return false;
        }

        bool inString = false;
        bool escaped = false;
        int depth = 0;
        for (size_t index = start; index < source.length(); ++index)
        {
            const char current = source[index];
            if (escaped)
            {
                escaped = false;
                continue;
            }

            if (current == '\\')
            {
                escaped = inString;
                continue;
            }

            if (current == '"')
            {
                inString = !inString;
                continue;
            }

            if (inString)
            {
                continue;
            }

            if (current == '{')
            {
                ++depth;
            }
            else if (current == '}')
            {
                --depth;
                if (depth == 0)
                {
                    if (json != nullptr)
                    {
                        *json = source.substring(start, index + 1);
                    }
                    if (nextIndex != nullptr)
                    {
                        *nextIndex = index + 1;
                    }
                    return true;
                }
            }
        }

        return false;
    }

    bool ApplyJsonCommand(const std::string &rxValue)
    {
        if (rxValue.empty() || rxValue.front() != '{')
        {
            return false;
        }

        DynamicJsonDocument doc(rxValue.size() + 512);
        const DeserializationError err = deserializeJson(doc, rxValue.c_str());
        if (err)
        {
            Serial.printf("BLE JSON parse failed: %s\n", err.c_str());
            return false;
        }

        const String op = doc["op"] | String("");
        Serial.printf("BLE JSON op received: %s bytes=%u\n", op.c_str(), static_cast<unsigned>(rxValue.size()));
        if (op == "config.get" || op == "pair.discover" || op == "pair.info")
        {
            Serial.println("BLE JSON config request accepted");
            QueueBleJsonPayload(BuildRemoteControllerManifestJson());
            return true;
        }

        if (op == "control.set" || op == "control.patch")
        {
            bool handledControl = false;
            uint32_t lastCommand = 0;
            if (!doc["expression"].isNull())
            {
                const uint8_t value = static_cast<uint8_t>(constrain(doc["expression"].as<int>(), 0, kRemoteControllerExpressionCount - 1));
                const bool queued = EnqueueLegacyCommand(0, value, &lastCommand);
                Serial.printf("BLE JSON control field expression=%u %s raw=0x%08lx\n", static_cast<unsigned>(value), queued ? "queued" : "failed", static_cast<unsigned long>(lastCommand));
                handledControl = queued || handledControl;
            }
            if (!doc["brightness"].isNull())
            {
                const uint8_t value = static_cast<uint8_t>(constrain(doc["brightness"].as<int>(), 0, 255));
                const bool queued = EnqueueLegacyCommand(1, value, &lastCommand);
                Serial.printf("BLE JSON control field brightness=%u %s raw=0x%08lx\n", static_cast<unsigned>(value), queued ? "queued" : "failed", static_cast<unsigned long>(lastCommand));
                handledControl = queued || handledControl;
            }
            if (!doc["voice_enabled"].isNull())
            {
                const uint8_t value = doc["voice_enabled"].as<bool>() ? 1 : 0;
                const bool queued = EnqueueLegacyCommand(2, value, &lastCommand);
                Serial.printf("BLE JSON control field voice_enabled=%u %s raw=0x%08lx\n", static_cast<unsigned>(value), queued ? "queued" : "failed", static_cast<unsigned long>(lastCommand));
                handledControl = queued || handledControl;
            }
            if (!doc["display_mode"].isNull())
            {
                const uint8_t value = static_cast<uint8_t>(constrain(doc["display_mode"].as<int>(), 0, 255));
                const bool queued = EnqueueLegacyCommand(3, value, &lastCommand);
                Serial.printf("BLE JSON control field display_mode=%u %s raw=0x%08lx\n", static_cast<unsigned>(value), queued ? "queued" : "failed", static_cast<unsigned long>(lastCommand));
                handledControl = queued || handledControl;
            }
            if (!doc["hue_shift"].isNull())
            {
                const float hue = doc["hue_shift"].as<float>();
                const int encoded = constrain(static_cast<int>(hue * 8.0f), 0, 65535);
                const bool queued = EnqueueLegacyCommand(4, static_cast<uint16_t>(encoded), &lastCommand);
                Serial.printf("BLE JSON control field hue_shift=%.2f encoded=%d %s raw=0x%08lx\n", hue, encoded, queued ? "queued" : "failed", static_cast<unsigned long>(lastCommand));
                handledControl = queued || handledControl;
            }

            Serial.printf("BLE JSON control op %s: %s raw=0x%08lx\n", handledControl ? "queued" : "ignored", op.c_str(), static_cast<unsigned long>(lastCommand));
            QueueBleJsonPayload(BuildRemoteControllerStateJson(doc));
            return true;
        }

        if (op == "ping")
        {
            Serial.println("BLE JSON ping accepted");
            DynamicJsonDocument pong(256);
            pong["op"] = "pong";
            pong["name"] = EffectiveBleName();
            pong["service_uuid"] = BLE_SERIAL2_SERVICE_UUID;

            String payload;
            serializeJson(pong, payload);
            QueueBleJsonPayload(payload);
            return true;
        }

        return false;
    }

    bool ApplyBleJsonWrite(const std::string &rxValue)
    {
        if (rxValue.empty())
        {
            return false;
        }

        size_t offset = 0;
        if (bleRxJsonBuffer.length() == 0)
        {
            while (offset < rxValue.size() && IsJsonWhitespace(rxValue[offset]))
            {
                ++offset;
            }
            if (offset >= rxValue.size() || rxValue[offset] != '{')
            {
                Serial.printf(
                    "BLE RX write is not JSON: bytes=%u first=0x%02x preview='%s'\n",
                    static_cast<unsigned>(rxValue.size()),
                    offset < rxValue.size() ? static_cast<unsigned>(static_cast<unsigned char>(rxValue[offset])) : 0,
                    BlePreviewText(rxValue).c_str());
                return false;
            }
        }

        const std::string chunk = rxValue.substr(offset);
        if (bleRxJsonBuffer.length() > 0 && !chunk.empty() && chunk.front() == '{')
        {
            Serial.printf(
                "BLE JSON new object started while %u bytes were buffered; dropping incomplete object\n",
                static_cast<unsigned>(bleRxJsonBuffer.length()));
            bleRxJsonBuffer = String("");
        }

        if ((bleRxJsonBuffer.length() + chunk.size()) > kBleRxJsonBufferBytes)
        {
            Serial.printf("BLE JSON RX buffer overflow, dropping %u buffered bytes\n", static_cast<unsigned>(bleRxJsonBuffer.length() + chunk.size()));
            bleRxJsonBuffer = String("");
            return true;
        }

        bleRxJsonBuffer += String(chunk.c_str());
        Serial.printf(
            "BLE JSON chunk buffered: chunk=%u total=%u\n",
            static_cast<unsigned>(chunk.size()),
            static_cast<unsigned>(bleRxJsonBuffer.length()));

        while (true)
        {
            TrimLeadingBleJsonWhitespace();

            String json;
            size_t nextIndex = 0;
            if (!ExtractCompleteJsonObject(bleRxJsonBuffer, &json, &nextIndex))
            {
                if (bleRxJsonBuffer.length() > 0 && bleRxJsonBuffer[0] != '{')
                {
                    Serial.printf("BLE JSON buffer lost object start, clearing %u bytes\n", static_cast<unsigned>(bleRxJsonBuffer.length()));
                    bleRxJsonBuffer = String("");
                }
                else
                {
                    Serial.printf("BLE JSON waiting for complete object: buffered=%u\n", static_cast<unsigned>(bleRxJsonBuffer.length()));
                }
                return true;
            }

            const std::string jsonValue(json.c_str(), json.length());
            Serial.printf("BLE JSON complete object: bytes=%u payload='%s'\n", static_cast<unsigned>(jsonValue.size()), BlePreviewText(jsonValue).c_str());
            if (!ApplyJsonCommand(jsonValue))
            {
                Serial.println("BLE JSON complete object ignored");
            }
            bleRxJsonBuffer.remove(0, nextIndex);

            TrimLeadingBleJsonWhitespace();
            if (bleRxJsonBuffer.length() == 0)
            {
                return true;
            }
        }
    }

    class MenuBleServerCallbacks : public BLEServerCallbacks
    {
        void onConnect(BLEServer *server) override
        {
            bleDeviceConnected = true;
            bleRxJsonBuffer = String("");
            ClearQueuedLegacyCommands();
            Serial.println("BLE client connected");
        }

        void onDisconnect(BLEServer *server) override
        {
            bleDeviceConnected = false;
            bleRxJsonBuffer = String("");
            ClearQueuedLegacyCommands();
            Serial.println("BLE client disconnected");
        }
    };

    class MenuBleRxCallbacks : public BLECharacteristicCallbacks
    {
        void onWrite(BLECharacteristic *characteristic) override
        {
            const uint8_t *data = characteristic->getData();
            const size_t length = characteristic->getLength();
            if (data == nullptr || length == 0)
            {
                return;
            }

            std::string rxValue(reinterpret_cast<const char *>(data), length);
            LogBleWritePreview(rxValue);

            if (ApplyBleJsonWrite(rxValue))
            {
                return;
            }

            uint32_t legacyCommand = 0;
            bool hasDigits = false;
            for (size_t i = 0; i < rxValue.size() && i < 10; ++i)
            {
                const char c = rxValue[i];
                if (c >= '0' && c <= '9')
                {
                    hasDigits = true;
                    legacyCommand = legacyCommand * 10 + static_cast<uint32_t>(c - '0');
                }
                else if (hasDigits)
                {
                    break;
                }
            }
            if (!hasDigits)
            {
                return;
            }
            if (EnqueueLegacyCommand(legacyCommand))
            {
                Serial.printf("BLE RX legacy raw_data queued: %lu\n", static_cast<unsigned long>(legacyCommand));
            }
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

    static void ApplyLegacyCommand(const uint32_t command)
    {
        raw_data = command;
        data_type = (uint8_t)(command >> 24);
        if (data_type == 0)
        {
            tempvalue = (uint8_t)(command & 0x000000FF);
            confirm = (uint8_t)((command & 0x00FF0000) >> 16);
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
            tempvalue = (uint8_t)((command & 0x0000FF00) >> 8);
            confirm = (uint8_t)((command & 0x00FF0000) >> 16);
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
            tempvalue = (uint8_t)(command & 0x000000FF);
            confirm = (uint8_t)((command & 0x00FF0000) >> 16);
            if (confirm == 255 && voiceenable != tempvalue)
            {
                voiceenable = tempvalue;
                display.fillRect(65, 50, 60, 12, TFT_BLACK);
                display.display();
                confirm = 0;
            }
        }
        else if (data_type == 3)
        {
            tempvalue = (uint8_t)(command & 0x000000FF);
            confirm = (uint8_t)((command & 0x00FF0000) >> 16);
            if (confirm == 255)
            {
                dmode = tempvalue;
                confirm = 0;
            }
        }
        else if (data_type == 4)
        {
            confirm = (uint8_t)((command & 0x00FF0000) >> 16);
            if (confirm == 255)
            {
                tempHue = static_cast<float>(command & 0x0000FFFF) / 8.0f;
                confirm = 0;
            }
        }
    }

    void startBluetooth()
    {
        BLEDevice::init(user_name.c_str());
        bleServer = BLEDevice::createServer();
        bleServer->setCallbacks(new MenuBleServerCallbacks());

        Serial.printf(
            "BLE UART start: name='%s' service=%s rx=%s tx=%s\n",
            user_name.c_str(),
            BLE_SERIAL2_SERVICE_UUID,
            BLE_RX2_UUID.c_str(),
            BLE_TX2_UUID.c_str());

        BLEService *service = bleServer->createService(BLE_SERIAL2_SERVICE_UUID);

        bleTxCharacteristic = service->createCharacteristic(BLE_TX2_UUID.c_str(), BLECharacteristic::PROPERTY_NOTIFY);
        bleTxCharacteristic->addDescriptor(new BLE2902());

        BLECharacteristic *rxCharacteristic = service->createCharacteristic(BLE_RX2_UUID.c_str(), BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
        rxCharacteristic->setCallbacks(new MenuBleRxCallbacks());

        service->start();
        BLEAdvertising *advertising = bleServer->getAdvertising();
        advertising->addServiceUUID(BLE_SERIAL2_SERVICE_UUID);
        advertising->setScanResponse(true);
        advertising->start();
        Serial.println("BLE UART ready, waiting for client...");
    }

public:
    void Initialize(uint8_t faceCount, uint8_t threshold)
    {
        #ifdef VERBOSE_STARTUP
        display.println(TXT("Initialize Bluetooth...", "初始化蓝牙驱动..."));
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
        //Wire1.setPins(38, 39);
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
        //nowpixels.begin();
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
        display.println(TXT("Initialize gesture sensor...", "初始化手势传感器驱动..."));
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
            display.println(TXT("Failed to initialize gesture sensor", "手势传感器驱动初始化失败"));
            display.println(TXT("Please check connection", "请检查连接"));
            display.display();
            #endif
            Serial.println("failed to initialize device! Please check your wiring.");
            didBegin = false;
            delay(2000);
        }
        else
        {
            #ifdef VERBOSE_STARTUP
            display.println(TXT("Gesture sensor initialized", "手势传感器驱动初始化成功"));
            #else
            display.progressBar(14,50,100,8,40);
            #endif
            Serial.println("Device initialized!");
            didBegin = true;
        }
        
        #ifndef NEW_GESTURE
        if(apds.setProximityGain(PGAIN_2X)){
            #ifdef VERBOSE_STARTUP
            display.println(TXT("Gesture sensor gain set", "手势传感器驱动增益设置成功"));
            #else
            display.progressBar(14,50,100,8,42);
            #endif
        }
        #endif

        #ifdef NEW_GESTURE
        PAJ7620_sensor.setProximityMode();
        #else
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
        display.println(TXT("Peripherals initialized", "外设初始化完成"));
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

        FlushQueuedBleJsonPayload();

        //free_mem = (int)((float)((float)ESP.getFreeHeap() / (float)ESP.getHeapSize())*100.0f);
        display.startWrite();
        // display.clearDisplay();
        // display.drawString("Current: ", 0, 12);
        // display.drawString("Brightness: ", 0, 24);
        // display.drawString("Lip Sync: ", 0, 36);

        uint32_t queuedCommand = 0;
        bool consumedQueuedCommand = false;
        while (DequeueLegacyCommand(&queuedCommand))
        {
            ApplyLegacyCommand(queuedCommand);
            Serial.printf(
                "ESPMenu applied remote raw=0x%08lx type=%u face=%u bright=%u hue=%u voice=%u\n",
                static_cast<unsigned long>(queuedCommand),
                static_cast<unsigned>(data_type),
                static_cast<unsigned>(facialexpression),
                static_cast<unsigned>(bright),
                static_cast<unsigned>(tempHue),
                static_cast<unsigned>(voiceenable));
            consumedQueuedCommand = true;
        }
        if (!consumedQueuedCommand)
        {
            ApplyLegacyCommand(raw_data);
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
        display.drawString(TXT("Exp Number:", "表情编号："), 5, 37);
        display.drawNumber(facialexpression, 70, 37);

        display.drawString(TXT("Bright:", "亮度："), 5, 50);
        display.drawNumber(bright, TXT(55, 40), 50);

        if(WiFi.isConnected()){
            display.pushImageDMA(100, 38, 24, 24, epd_bitmap_cloud);
        }else{
            display.fillRect(100,38,24,24,TFT_BLACK);
        }

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
TimeStep Menu::timeStep = TimeStep(5);
float Menu::minimum = 0.0f;
bool Menu::didBegin = false;
bool Menu::isBright = false;
bool Menu::isProx = false;
bool Menu::mouth = false;
float Menu::tempHue = 0.0;