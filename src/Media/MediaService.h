#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <ProtoGC.h>
#include <esp_heap_caps.h>
#include "../Materials/RGBColor.h"
#include "MediaCodec.h"
#include "BuiltinMedia.h"

// BLE callbacks only copy bounded packets/commands. All allocation, decode,
// flash I/O and mode changes run on the render/main-loop core.
namespace MediaService {
constexpr uint32_t MaxFile = 1024 * 1024;
constexpr unsigned Slots = 8;
constexpr size_t PacketCapacity = MediaCodec::MaxFrame + 4;
enum class Mode : uint8_t { Face, Stream, File, Builtin };
inline Mode mode = Mode::Face;
inline portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
inline char command[512] = {};
inline bool commandPending = false, disconnected = false;
inline uint8_t* packet = nullptr;
inline ProtoRGBColor* front = nullptr;
inline ProtoRGBColor* scratch = nullptr;
inline bool packetReady = false;
inline uint8_t acceptedKind = 0;
inline uint8_t packetKind = 0;
inline uint32_t packetId = 0, packetSize = 0, packetOffset = 0;
inline uint32_t lastRxMs = 0, lastStreamMs = 0, framesShown = 0;
inline uint16_t chunkBytes = 20;
inline File upload, playback;
inline uint32_t uploadSize = 0, uploadOffset = 0, uploadHash = 2166136261u, expectedHash = 0;
inline uint32_t uploadTouched = 0;
inline unsigned uploadSlot = 0;
inline char uploadName[97] = {};
inline const BuiltinMedia::Clip* builtin = nullptr;
inline uint32_t playOffset = 12, nextFrameMs = 0;
inline uint16_t frameCount = 0, playFrame = 0;
inline int selectedSlot = -1;
using Reply = void (*)(const String&);

inline bool Active() { return mode != Mode::Face; }
inline const char* ModeName() {
    switch (mode) { case Mode::Stream: return "stream"; case Mode::File: return "gif";
        case Mode::Builtin: return "builtin"; default: return "face"; }
}
inline String Path(unsigned slot) { return String("/media/gif") + slot + ".ptm"; }
inline String NamePath(unsigned slot) { return String("/media/gif") + slot + ".name"; }
inline bool EnsureBuffers() {
    if (packet) return true;
    auto* p = static_cast<uint8_t*>(heap_caps_malloc(PacketCapacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    auto* f = static_cast<ProtoRGBColor*>(heap_caps_malloc(MediaCodec::Pixels * sizeof(ProtoRGBColor), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    auto* s = static_cast<ProtoRGBColor*>(heap_caps_malloc(MediaCodec::Pixels * sizeof(ProtoRGBColor), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!p || !f || !s) { heap_caps_free(p); heap_caps_free(f); heap_caps_free(s); return false; }
    memset(f, 0, MediaCodec::Pixels * sizeof(ProtoRGBColor));
    front = f; scratch = s;
    portENTER_CRITICAL(&mux); packet = p; portEXIT_CRITICAL(&mux);
    return true;
}
inline void ResetPacket() {
    portENTER_CRITICAL(&mux);
    packetOffset = packetSize = 0; packetReady = false;
    portEXIT_CRITICAL(&mux);
}
inline void Stop() {
    playback.close(); builtin = nullptr; mode = Mode::Face; selectedSlot = -1;
    portENTER_CRITICAL(&mux); acceptedKind = 0; portEXIT_CRITICAL(&mux);
    ResetPacket();
}
inline void AbortUpload() {
    if (upload) { upload.close(); LittleFS.remove("/media/.upload"); }
    uploadSize = uploadOffset = 0;
}
inline void Disconnect() {
    portENTER_CRITICAL(&mux); disconnected = true; commandPending = false; portEXIT_CRITICAL(&mux);
}
inline bool Submit(JsonDocument& doc) {
    char text[512];
    const size_t n = measureJson(doc);
    if (n >= sizeof(text)) return false;
    serializeJson(doc, text, sizeof(text));
    portENTER_CRITICAL(&mux);
    const bool accepted = !commandPending;
    if (accepted) { memcpy(command, text, n + 1); commandPending = true; }
    portEXIT_CRITICAL(&mux);
    return accepted;
}
// 16-byte envelope: 'PM', version 1, kind (1 frame/2 file block),
// uint32 transfer id, uint32 fragment offset, uint32 total, then payload.
inline bool Receive(const uint8_t* data, size_t n) {
    if (n < 2 || data[0] != 'P' || data[1] != 'M') return false;
    if (n <= 16 || n > 244 || data[2] != 1 || (data[3] != 1 && data[3] != 2)) return true;
    const uint32_t id = MediaCodec::U32(data + 4), off = MediaCodec::U32(data + 8), total = MediaCodec::U32(data + 12);
    const size_t bytes = n - 16;
    if (!total || total > PacketCapacity || off > total || bytes > total - off) return true;
    portENTER_CRITICAL(&mux);
    if (packet && !packetReady && data[3] == acceptedKind) {
        if (off == 0) { packetId = id; packetKind = data[3]; packetSize = total; packetOffset = 0; }
        if (id == packetId && data[3] == packetKind && total == packetSize && off == packetOffset) {
            memcpy(packet + off, data + 16, bytes);
            packetOffset += bytes; lastRxMs = millis();
            packetReady = packetOffset == total;
        }
    }
    portEXIT_CRITICAL(&mux);
    return true;
}
inline size_t ReadAt(uint32_t offset, uint8_t* dest, size_t n) {
    if (builtin) {
        if (offset > builtin->size || n > builtin->size - offset) return 0;
        memcpy(dest, builtin->data + offset, n); return n;
    }
    if (!playback.seek(offset)) return 0;
    return playback.read(dest, n);
}
inline bool Validate(File& file) {
    uint8_t header[12];
    if (file.size() < 12 || file.size() > MaxFile || file.read(header, 12) != 12 || !MediaCodec::Header(header, 12)) return false;
    const uint16_t count = MediaCodec::U16(header + 6);
    for (unsigned i = 0; i < count; ++i) {
        uint8_t fh[4];
        if (file.read(fh, 4) != 4) return false;
        const size_t n = MediaCodec::U16(fh + 2);
        if (MediaCodec::U16(fh) < 20 || !n || n > MediaCodec::MaxFrame || file.read(packet, n) != n
            || !MediaCodec::Decode(packet, n, scratch)) return false;
        yield();
    }
    return file.position() == file.size();
}
inline bool BeginPlayback(unsigned slot, const char* name) {
    File candidate;
    const BuiltinMedia::Clip* clip = nullptr;
    if (name && *name) {
        for (const auto& c : BuiltinMedia::Clips) if (strcmp(name, c.name) == 0) clip = &c;
        if (!clip) return false;
    } else {
        if (slot >= Slots) return false;
        candidate = LittleFS.open(Path(slot), "r");
        if (!candidate || !Validate(candidate)) return false;
    }
    uint8_t h[12];
    if (clip) memcpy(h, clip->data, 12);
    else { candidate.seek(0); candidate.read(h, 12); }
    Stop();
    playback = candidate; builtin = clip; frameCount = MediaCodec::U16(h + 6);
    playFrame = 0; playOffset = 12; nextFrameMs = millis();
    mode = clip ? Mode::Builtin : Mode::File; selectedSlot = clip ? -1 : int(slot);
    return true;
}
inline void PublishFrame() { memcpy(front, scratch, MediaCodec::Pixels * sizeof(ProtoRGBColor)); ++framesShown; }
inline void CopyTo(ProtoRGBColor* colors) {
    if (front && colors) memcpy(colors, front, MediaCodec::Pixels * sizeof(ProtoRGBColor));
}
inline void TickPlayback(uint32_t now) {
    if ((mode != Mode::File && mode != Mode::Builtin) || int32_t(now - nextFrameMs) < 0) return;
    uint8_t fh[4];
    if (ReadAt(playOffset, fh, 4) != 4) { Stop(); return; }
    const uint16_t delayMs = MediaCodec::U16(fh), n = MediaCodec::U16(fh + 2);
    if (n > MediaCodec::MaxFrame || ReadAt(playOffset + 4, packet, n) != n || !MediaCodec::Decode(packet, n, scratch)) { Stop(); return; }
    PublishFrame();
    nextFrameMs += delayMs;
    // A long menu/flash stall slows playback; never spin to catch up unboundedly.
    if (int32_t(now - nextFrameMs) > 250) nextFrameMs = now + delayMs;
    if (++playFrame == frameCount) { playFrame = 0; playOffset = 12; }
    else playOffset += 4 + n;
}
inline void Poll(Reply reply) {
    const uint32_t now = millis();
    bool lost;
    char text[512] = {};
    portENTER_CRITICAL(&mux);
    lost = disconnected; disconnected = false;
    if (commandPending) { memcpy(text, command, sizeof(text)); commandPending = false; }
    portEXIT_CRITICAL(&mux);
    if (lost) { if (mode == Mode::Stream) Stop(); AbortUpload(); ResetPacket(); }
    if (upload && now - uploadTouched > 15000 && now - lastRxMs > 15000) { AbortUpload(); Stop(); }
    if (mode == Mode::Stream && now - lastStreamMs > 2500 && now - lastRxMs > 2500) Stop();
    if (text[0]) {
        BasicJsonDocument<protogc::ProtoJsonPsramAllocator> cmd(1024), response(2048);
        deserializeJson(cmd, text);
        const String op = cmd["op"] | "";
        response["op"] = "media.reply"; response["rid"] = cmd["rid"] | 0u;
        bool ok = EnsureBuffers();
        const unsigned slot = cmd["slot"] | Slots;
        const char* error = "invalid_request";
        if (ok && op == "media.stop") { AbortUpload(); Stop(); ok = true; }
        else if (ok && op == "media.stream") {
            AbortUpload(); Stop(); memset(front, 0, MediaCodec::Pixels * sizeof(ProtoRGBColor));
            mode = Mode::Stream; lastStreamMs = now;
            portENTER_CRITICAL(&mux); acceptedKind = 1; portEXIT_CRITICAL(&mux);
        } else if (ok && op == "media.play") {
            AbortUpload(); Stop(); ok = BeginPlayback(slot, cmd["builtin"] | ""); error = "clip_invalid";
        } else if (ok && op == "media.delete") {
            if (mode == Mode::File && selectedSlot == int(slot)) Stop();
            ok = slot < Slots && LittleFS.remove(Path(slot));
            if (ok) LittleFS.remove(NamePath(slot));
        } else if (ok && op == "media.upload.begin") {
            const uint32_t size = cmd["size"] | 0u;
            AbortUpload(); Stop();
            const size_t freeBytes = LittleFS.totalBytes() - LittleFS.usedBytes();
            ok = slot < Slots && size >= 17 && size <= MaxFile && freeBytes > size + 32768;
            error = "space_or_size";
            if (ok) {
                LittleFS.mkdir("/media"); upload = LittleFS.open("/media/.upload", "w"); ok = bool(upload);
                if (ok) { uploadSlot = slot; uploadSize = size; uploadOffset = 0; uploadHash = 2166136261u;
                    expectedHash = cmd["hash"] | 0u; uploadTouched = now; }
                if (ok) {
                    const char* name = cmd["name"] | "";
                    const size_t nameBytes = strlen(name);
                    uploadName[0] = 0;
                    if (nameBytes <= 96) memcpy(uploadName, name, nameBytes + 1);
                }
                if (ok) { portENTER_CRITICAL(&mux); acceptedKind = 2; portEXIT_CRITICAL(&mux); }
            }
        } else if (ok && op == "media.upload.end") {
            ok = upload && uploadOffset == uploadSize && uploadHash == expectedHash;
            if (upload) upload.close();
            File check = LittleFS.open("/media/.upload", "r");
            if (ok) ok = check && Validate(check);
            check.close(); error = "upload_invalid";
            if (ok) {
                if (mode == Mode::File && selectedSlot == int(uploadSlot)) Stop();
                // LittleFS rename replaces an existing file atomically.
                ok = LittleFS.rename("/media/.upload", Path(uploadSlot));
                if (ok) {
                    File label = LittleFS.open(NamePath(uploadSlot), "w");
                    if (label) label.write(reinterpret_cast<const uint8_t*>(uploadName), strlen(uploadName));
                }
            }
            if (!ok) LittleFS.remove("/media/.upload");
            uploadSize = uploadOffset = 0; Stop();
        } else if (ok && op == "media.upload.cancel") { AbortUpload(); Stop(); }
        else if (op != "media.list" && op != "media.status") ok = false;
        if (!ok) response["error"] = packet ? error : "psram_unavailable";
        response["ok"] = ok;
        response["mode"] = ModeName(); response["chunk_bytes"] = chunkBytes;
        response["frames"] = framesShown; response["free_bytes"] = LittleFS.totalBytes() - LittleFS.usedBytes();
        response["max_file_bytes"] = MaxFile; response["slots"] = Slots;
        if (op == "media.list") {
            JsonArray clips = response.createNestedArray("clips");
            for (unsigned i = 0; i < Slots; ++i) {
                File f = LittleFS.open(Path(i), "r");
                if (f) {
                    JsonObject c = clips.createNestedObject(); c["slot"] = i; c["bytes"] = f.size();
                    File label = LittleFS.open(NamePath(i), "r");
                    if (label && label.size() <= 96) c["name"] = label.readString();
                }
            }
            JsonArray defaults = response.createNestedArray("builtins");
            for (const auto& c : BuiltinMedia::Clips) defaults.add(c.name);
        }
        String result; serializeJson(response, result); reply(result);
    }
    bool ready;
    portENTER_CRITICAL(&mux); ready = packetReady; portEXIT_CRITICAL(&mux);
    if (ready) {
        const uint32_t id = packetId;
        bool ok = false;
        if (packetKind == 1 && mode == Mode::Stream && !upload && packetSize >= 6) {
            const size_t n = packetSize - 4;
            ok = MediaCodec::Hash(packet, n) == MediaCodec::U32(packet + n) && MediaCodec::Decode(packet, n, scratch);
            if (ok) { PublishFrame(); lastStreamMs = now; }
        } else if (packetKind == 2 && upload && packetSize <= 4096 && packetSize <= uploadSize - uploadOffset) {
            ok = upload.write(packet, packetSize) == packetSize;
            if (ok) { uploadHash = MediaCodec::Hash(packet, packetSize, uploadHash); uploadOffset += packetSize; uploadTouched = now; }
            else AbortUpload();
        }
        String result = String("{\"op\":\"media.ack\",\"id\":") + id + ",\"ok\":" + (ok ? "true" : "false") + ",\"offset\":" + uploadOffset + "}";
        ResetPacket(); reply(result);
    }
    // Playback never shares packet scratch with an upload/transfer.
    if (!upload) {
        portENTER_CRITICAL(&mux); ready = packetOffset != 0; portEXIT_CRITICAL(&mux);
        if (!ready) TickPlayback(now);
    }
}
}
