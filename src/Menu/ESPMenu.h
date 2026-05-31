#pragma once

// #include <esp_now.h>
#include <WiFi.h>
#include <Adafruit_NeoPixel.h>
#include <Wire.h>

#include <esp_bt.h>
#include <BLE2902.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <esp_attr.h>
#include <esp_task_wdt.h>
#include <esp_heap_caps.h>
#include <driver/rtc_io.h>
#include <esp_mac.h>
#include <esp_timer.h>
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include <M5Unified.h>
#include <M5UnitGLASS2.h>

#ifdef LANG_CN
#define TXT(en, cn) cn
#else
#define TXT(en, cn) en
#endif

#ifndef ESPMENU_REMOTE_DEBUG_LOG
#define ESPMENU_REMOTE_DEBUG_LOG 0
#endif

#if ESPMENU_REMOTE_DEBUG_LOG
#define ESPMENU_LOG_PRINTF(...) Serial.printf(__VA_ARGS__)
#else
#define ESPMENU_LOG_PRINTF(...) do { } while (0)
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

// Cached manifest built once during init — avoids file I/O + JSON parsing in BLE callbacks.
static String bleCachedManifestJson;
static bool bleManifestRequested = false;
static portMUX_TYPE gBleFlagMux = portMUX_INITIALIZER_UNLOCKED; // guards bleManifestRequested

// Lightweight spinlock for the legacy command queue shared between BLE ISR and main loop.
static portMUX_TYPE gCommandQueueMux = portMUX_INITIALIZER_UNLOCKED;

uint32_t raw_data = 32000;

namespace
{
    constexpr size_t kBleJsonChunkBytes = 160;
    constexpr size_t kBleRxJsonBufferBytes = 1024;
    constexpr uint8_t kRemoteCommandQueueSize = 8;
    constexpr uint8_t kBoopThresholdFloor = 6;
    constexpr uint8_t kBoopThresholdScalePercent = 60;
    constexpr uint8_t kBoopReleaseScalePercent = 40;
    constexpr uint8_t kRemoteControllerExpressionMax = 64;
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

    struct AnimationManifestMetadata
    {
        String animation_asset;
        String animation_name;
        std::vector<String> expression_names;
    };

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

    AnimationManifestMetadata LoadAnimationManifestMetadata()
    {
        AnimationManifestMetadata metadata;

        metadata.animation_asset = userConfig.device_id.length() > 0
                                       ? (userConfig.device_id + String("_animation.json"))
                                       : String("example_animation.json");

        metadata.expression_names.reserve(kRemoteControllerExpressionCount);
        for (uint8_t index = 0; index < kRemoteControllerExpressionCount; ++index)
        {
            metadata.expression_names.push_back(String(kRemoteControllerExpressionNames[index]));
        }

        if (!LittleFS.begin(false))
        {
            return metadata;
        }

        const String preferredPath = String("/") + metadata.animation_asset;
        const String fallbackPath = String("/example_animation.json");
        String selectedPath;
        if (LittleFS.exists(preferredPath))
        {
            selectedPath = preferredPath;
        }
        else if (LittleFS.exists(fallbackPath))
        {
            selectedPath = fallbackPath;
        }
        else
        {
            return metadata;
        }

        File animationFile = LittleFS.open(selectedPath, "r");
        if (!animationFile)
        {
            return metadata;
        }

        // Allocate JSON doc in PSRAM to avoid 65KB internal DRAM pressure.
        // heap_caps_malloc_extmem_enable(0) prevents automatic fallback, so use explicit SPIRAM alloc.
        struct PsramAlloc {
            void* allocate(size_t s) { return heap_caps_malloc(s, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
            void  deallocate(void* p) { heap_caps_free(p); }
        };
        BasicJsonDocument<PsramAlloc> doc(65536);
        const DeserializationError err = deserializeJson(doc, animationFile);
        animationFile.close();
        if (err)
        {
            Serial.printf("BLE manifest JSON parse failed: %s\n", err.c_str());
            return metadata;
        }

        metadata.animation_asset = selectedPath.startsWith("/") ? selectedPath.substring(1) : selectedPath;
        metadata.animation_name = doc["animation_name"] | doc["display_name"] | doc["name"] | doc["user"] | String("");
        if (metadata.animation_name.isEmpty())
        {
            metadata.animation_name = metadata.animation_asset;
            const int dot = metadata.animation_name.lastIndexOf('.');
            if (dot > 0)
            {
                metadata.animation_name = metadata.animation_name.substring(0, dot);
            }
        }

        std::vector<String> parsed_names;
        parsed_names.reserve(kRemoteControllerExpressionCount);
        JsonObject expressions = doc["expressions"].as<JsonObject>();
        if (!expressions.isNull())
        {
            for (JsonPair kv : expressions)
            {
                const char *name = kv.key().c_str();
                if (name == nullptr || name[0] == '\0' || std::strcmp(name, "reset_state") == 0)
                {
                    continue;
                }

                parsed_names.push_back(String(name));
                if (parsed_names.size() >= kRemoteControllerExpressionMax)
                {
                    break;
                }
            }
        }

        size_t configured_count = 0;
        if (doc["animation_num"].is<int>())
        {
            configured_count = static_cast<size_t>(std::max(0, doc["animation_num"].as<int>()));
        }
        else if (doc["animatiuon_num"].is<int>())
        {
            configured_count = static_cast<size_t>(std::max(0, doc["animatiuon_num"].as<int>()));
        }
        if (configured_count > 0 && configured_count < parsed_names.size())
        {
            parsed_names.resize(configured_count);
        }

        if (!parsed_names.empty())
        {
            metadata.expression_names = std::move(parsed_names);
        }

        return metadata;
    }

    String BuildRemoteControllerManifestJson()
    {
        // Manifest JSON - PSRAM-enabled by default
        DynamicJsonDocument doc(6144);
        const AnimationManifestMetadata animation_metadata = LoadAnimationManifestMetadata();
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
        visual["animation_name"] = animation_metadata.animation_name;
        visual["animation_asset"] = animation_metadata.animation_asset;
        visual["expression_count"] = static_cast<uint8_t>(std::min<size_t>(kRemoteControllerExpressionMax, animation_metadata.expression_names.size()));
        JsonArray expressionNames = visual.createNestedArray("expression_names");
        for (size_t index = 0; index < animation_metadata.expression_names.size(); ++index)
        {
            expressionNames.add(animation_metadata.expression_names[index]);
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

    // Non-blocking BLE notification state: sends one chunk per main-loop iteration
    static String sBleNotifyPayload;
    static size_t sBleNotifyOffset;

    void NotifyBleJsonPayloadChunk()
    {
        if (bleTxCharacteristic == nullptr || sBleNotifyPayload.isEmpty())
        {
            sBleNotifyPayload = String("");
            sBleNotifyOffset = 0;
            return;
        }

        bool connected = false;
        portENTER_CRITICAL(&gBleFlagMux);
        connected = bleDeviceConnected;
        portEXIT_CRITICAL(&gBleFlagMux);

        if (!connected)
        {
            sBleNotifyPayload = String("");
            sBleNotifyOffset = 0;
            return;
        }

        if (sBleNotifyOffset >= sBleNotifyPayload.length())
        {
            Serial.printf("BLE notify complete: %u bytes\n", static_cast<unsigned>(sBleNotifyPayload.length()));
            sBleNotifyPayload = String("");
            sBleNotifyOffset = 0;
            return;
        }

        const size_t chunkLength = std::min(kBleJsonChunkBytes, sBleNotifyPayload.length() - sBleNotifyOffset);
        bleTxCharacteristic->setValue(reinterpret_cast<const uint8_t *>(sBleNotifyPayload.c_str() + sBleNotifyOffset), chunkLength);
        bleTxCharacteristic->notify();
        sBleNotifyOffset += chunkLength;
        // No delay — caller (Menu::Update) returns after one chunk, next chunk on next loop iteration
    }

    void QueueBleJsonPayload(const String &payload)
    {
        if (payload.isEmpty())
        {
            return;
        }

        portENTER_CRITICAL(&gCommandQueueMux);
        blePendingJsonPayload = payload;
        bleJsonPayloadPending = true;
        portEXIT_CRITICAL(&gCommandQueueMux);
    }

    void FlushQueuedBleJsonPayload()
    {
        bool pending = false;
        String payload;

        portENTER_CRITICAL(&gCommandQueueMux);
        pending = bleJsonPayloadPending;
        if (pending)
        {
            payload = blePendingJsonPayload;
            blePendingJsonPayload = String("");
            bleJsonPayloadPending = false;
        }
        portEXIT_CRITICAL(&gCommandQueueMux);

        // If a new payload is queued, start sending it
        if (pending)
        {
            if (bleTxCharacteristic == nullptr)
            {
                Serial.println("BLE flush failed: TX characteristic is null");
                return;
            }
            bool connected = false;
            portENTER_CRITICAL(&gBleFlagMux);
            connected = bleDeviceConnected;
            portEXIT_CRITICAL(&gBleFlagMux);
            if (!connected)
            {
                Serial.println("BLE flush deferred: re-queuing payload (client not connected yet)");
                QueueBleJsonPayload(payload);
                return;
            }
            sBleNotifyPayload = payload;
            sBleNotifyOffset = 0;
        }

        // Continue sending chunks from any in-progress payload
        if (!sBleNotifyPayload.isEmpty())
        {
            NotifyBleJsonPayloadChunk();
        }
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
        portENTER_CRITICAL(&gCommandQueueMux);
        const uint8_t nextHead = static_cast<uint8_t>((remoteCommandQueueHead + 1) % kRemoteCommandQueueSize);
        if (nextHead == remoteCommandQueueTail)
        {
            portEXIT_CRITICAL(&gCommandQueueMux);
            Serial.printf("BLE legacy command queue full, dropping raw=0x%08lx\n", static_cast<unsigned long>(command));
            return false;
        }

        remoteCommandQueue[remoteCommandQueueHead] = command;
        remoteCommandQueueHead = nextHead;
        portEXIT_CRITICAL(&gCommandQueueMux);
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
        if (command == nullptr)
        {
            return false;
        }

        portENTER_CRITICAL(&gCommandQueueMux);
        if (remoteCommandQueueTail == remoteCommandQueueHead)
        {
            portEXIT_CRITICAL(&gCommandQueueMux);
            return false;
        }

        *command = remoteCommandQueue[remoteCommandQueueTail];
        remoteCommandQueueTail = static_cast<uint8_t>((remoteCommandQueueTail + 1) % kRemoteCommandQueueSize);
        portEXIT_CRITICAL(&gCommandQueueMux);
        return true;
    }

    void ClearQueuedLegacyCommands()
    {
        portENTER_CRITICAL(&gCommandQueueMux);
        remoteCommandQueueHead = 0;
        remoteCommandQueueTail = 0;
        portEXIT_CRITICAL(&gCommandQueueMux);
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

        // BLE JSON - PSRAM-enabled by default
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
            // DO NOT build the manifest here — this runs in the BLE RX callback.
            // Defer to the main loop via a flag to avoid file I/O / JSON parsing
            // on the BLE task, which would block further BLE RX and trigger the WDT.
            Serial.println("BLE JSON config request accepted (deferred to main loop)");
            portENTER_CRITICAL(&gBleFlagMux);
            bleManifestRequested = true;
            portEXIT_CRITICAL(&gBleFlagMux);
            return true;
        }

        if (op == "control.set" || op == "control.patch")
        {
            bool handledControl = false;
            uint32_t lastCommand = 0;
            if (!doc["expression"].isNull())
            {
                const uint8_t value = static_cast<uint8_t>(constrain(doc["expression"].as<int>(), 0, kRemoteControllerExpressionMax - 1));
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
            portENTER_CRITICAL(&gBleFlagMux);
            bleDeviceConnected = true;
            portEXIT_CRITICAL(&gBleFlagMux);
            bleRxJsonBuffer = String("");
            ClearQueuedLegacyCommands();
            Serial.println("BLE client connected — bleDeviceConnected=true");
        }

        void onDisconnect(BLEServer *server) override
        {
            portENTER_CRITICAL(&gBleFlagMux);
            bleDeviceConnected = false;
            portEXIT_CRITICAL(&gBleFlagMux);
            bleRxJsonBuffer = String("");
            ClearQueuedLegacyCommands();
            Serial.println("BLE client disconnected — bleDeviceConnected=false");
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
    static bool boopCurrentHigh;
    static bool boopPulsePending;
    static uint32_t boopLastPulseMs;
    static uint16_t boopRearmMs;
    static uint8_t boopReleaseHysteresis;
    static uint32_t bleReAdvertiseAtMs;
    static bool didBegin;
    static bool isBright;
    static bool isProx;
    static bool mouth;

    static void UpdateBoopSensorState()
    {
#ifdef NEW_GESTURE
        proximity = PAJ7620_sensor.getProximityDistance();
#else
        apds.readProximity(proximity);
#endif

        // Keep baseline stable while booped so proximity deltas do not self-cancel.
        if (timeStep.IsReady() && !boopCurrentHigh)
        {
            minimum = minF.Filter(proximity);
        }

        // Dark acrylic attenuates the proximity delta, so trigger earlier and release with a smaller margin.
        const uint8_t effectiveThreshold = std::max<uint8_t>(
            kBoopThresholdFloor,
            static_cast<uint8_t>((static_cast<uint16_t>(threshold) * kBoopThresholdScalePercent) / 100));
        const uint8_t releaseMargin = std::max<uint8_t>(
            boopReleaseHysteresis,
            static_cast<uint8_t>(std::max<uint8_t>(2, static_cast<uint8_t>((static_cast<uint16_t>(effectiveThreshold) * kBoopReleaseScalePercent) / 100))));
        const float onThreshold = minimum + effectiveThreshold;
        const float offThreshold = minimum + releaseMargin;
        const uint32_t now = millis();

        if (!boopCurrentHigh)
        {
            if (proximity > onThreshold && (now - boopLastPulseMs) >= boopRearmMs)
            {
                boopCurrentHigh = true;
                boopPulsePending = true;
                boopLastPulseMs = now;
            }
        }
        else if (proximity < offThreshold)
        {
            boopCurrentHigh = false;
        }
    }

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
        // Release ~50-70KB of classic BT (BR/EDR) memory before BLE init.
        // The pre-compiled BLE controller reserves this by default; we only
        // use BLE so the classic memory is wasted internal DRAM.
        const esp_err_t releaseResult = esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
        Serial.printf("BLE classic BT memory release result=%d intFree=%u largestBlk=%u psramFree=%u\n",
                  static_cast<int>(releaseResult),
                  static_cast<unsigned int>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                  static_cast<unsigned int>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
                  static_cast<unsigned int>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));

        constexpr size_t kBleControllerMinLargestBlock = 0x7800;
        const size_t largestInternalBlock = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
        if (largestInternalBlock < kBleControllerMinLargestBlock)
        {
            Serial.printf("[BLE] Skipping BLE init: largest internal block=%u, need at least %u\n",
                          static_cast<unsigned int>(largestInternalBlock),
                          static_cast<unsigned int>(kBleControllerMinLargestBlock));
            return;
        }

        BLEDevice::init(user_name.c_str());
        bleServer = BLEDevice::createServer();
        if (!bleServer)
        {
            Serial.println("[BLE] createServer failed; BLE disabled");
            return;
        }
        bleServer->setCallbacks(new MenuBleServerCallbacks());

        Serial.printf(
            "BLE UART start: name='%s' service=%s rx=%s tx=%s\n",
            user_name.c_str(),
            BLE_SERIAL2_SERVICE_UUID,
            BLE_RX2_UUID.c_str(),
            BLE_TX2_UUID.c_str());

        BLEService *service = bleServer->createService(BLE_SERIAL2_SERVICE_UUID);
        if (!service)
        {
            Serial.println("[BLE] createService failed; BLE disabled");
            return;
        }

        bleTxCharacteristic = service->createCharacteristic(BLE_TX2_UUID.c_str(), BLECharacteristic::PROPERTY_NOTIFY);
        if (!bleTxCharacteristic)
        {
            Serial.println("[BLE] TX characteristic create failed; BLE disabled");
            return;
        }
        // BLE2902 descriptor auto-added by NimBLE when notifications are enabled — no manual add needed
        // bleTxCharacteristic->addDescriptor(new BLE2902());

        BLECharacteristic *rxCharacteristic = service->createCharacteristic(BLE_RX2_UUID.c_str(), BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
        if (!rxCharacteristic)
        {
            Serial.println("[BLE] RX characteristic create failed; BLE disabled");
            bleTxCharacteristic = nullptr;
            return;
        }
        rxCharacteristic->setCallbacks(new MenuBleRxCallbacks());

        service->start();
        BLEAdvertising *advertising = bleServer->getAdvertising();
        advertising->addServiceUUID(BLE_SERIAL2_SERVICE_UUID);
        advertising->setScanResponse(true);
        advertising->start();
        Serial.println("BLE UART ready, waiting for client...");
    }

public:
    static void SetThreshold(uint8_t threshold)
    {
        Menu::threshold = threshold;
    }

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

        // Pre-build the remote controller manifest once during init so that
        // BLE config/discover requests do not trigger file I/O or JSON parsing
        // in the BLE RX callback (which would block the BLE task and cause WDT).
        bleCachedManifestJson = BuildRemoteControllerManifestJson();
        Serial.printf("BLE manifest cached: %u bytes\n", static_cast<unsigned>(bleCachedManifestJson.length()));

        display.display();
        delay(500);
    }

    static void Update()
    {
        if (bleServer != nullptr)
        {
            if (!bleDeviceConnected && bleOldDeviceConnected)
            {
                if (bleReAdvertiseAtMs == 0)
                {
                    bleReAdvertiseAtMs = millis() + 500;
                }
                else if (millis() >= bleReAdvertiseAtMs)
                {
                    bleServer->startAdvertising();
                    Serial.println("Restarted BLE advertising...");
                    bleOldDeviceConnected = false;
                    bleReAdvertiseAtMs = 0;
                }
            }
            else if (bleDeviceConnected && !bleOldDeviceConnected)
            {
                bleOldDeviceConnected = true;
                bleReAdvertiseAtMs = 0;
            }
        }

        // Handle deferred manifest request (set by BLE RX callback).
        // We build/send here in the main loop, NOT in the BLE callback, so that
        // file I/O and JSON parsing never block the BLE task.
        bool manifestReq = false;
        portENTER_CRITICAL(&gBleFlagMux);
        manifestReq = bleManifestRequested;
        bleManifestRequested = false;
        portEXIT_CRITICAL(&gBleFlagMux);
        if (manifestReq)
        {
            if (bleCachedManifestJson.isEmpty())
            {
                bleCachedManifestJson = BuildRemoteControllerManifestJson();
                Serial.printf("BLE manifest rebuilt on demand: %u bytes\n",
                              static_cast<unsigned>(bleCachedManifestJson.length()));
            }
            QueueBleJsonPayload(bleCachedManifestJson);
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
            ESPMENU_LOG_PRINTF(
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
        return didBegin;
    }

    static bool ConsumeBoopPulse()
    {
        if (!UseBoopSensor())
        {
            return false;
        }

        UpdateBoopSensorState();
        if (!boopPulsePending)
        {
            return false;
        }

        boopPulsePending = false;
        return true;
    }

    static bool isBooped()
    {
        if (!UseBoopSensor())
        {
            return false;
        }

        UpdateBoopSensorState();
        return boopCurrentHigh;
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
bool Menu::boopCurrentHigh = false;
bool Menu::boopPulsePending = false;
uint32_t Menu::boopLastPulseMs = 0;
uint16_t Menu::boopRearmMs = 35;
uint8_t Menu::boopReleaseHysteresis = 4;
uint32_t Menu::bleReAdvertiseAtMs = 0;
bool Menu::didBegin = false;
bool Menu::isBright = false;
bool Menu::isProx = false;
bool Menu::mouth = false;
float Menu::tempHue = 0.0;