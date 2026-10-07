#pragma once

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <cstdlib>
#include <cstring>

#ifndef SERIAL_DEBUG_ENABLED
#define SERIAL_DEBUG_ENABLED 1
#endif

// Frame-boundary debug transport. Snapshots are read-only; optional PROFILE
// controls change only the live shader, never files or arbitrary memory.
// Resolver runs while the animation worker is waiting, never during mesh updates.
namespace SerialDebug
{
struct View
{
    const void* data = nullptr;
    size_t bytes = 0;
    const char* format = "u8";
    unsigned width = 0;
    unsigned height = 0;
};

#if SERIAL_DEBUG_ENABLED
using Resolver = bool (*)(const char*, View&);
using Stats = void (*)(char*, size_t);
using MaterialProfile = bool (*)(const char*);
inline char input[96] = {};
inline size_t inputLength = 0;
inline bool overflow = false;
#if defined(TASESP32S31)
inline char output[320] = {};
#else
inline char output[224] = {};
#endif
inline uint8_t* snapshot = nullptr;
inline size_t total = 0, sent = 0;
inline uint32_t sequence = 0, hash = 0, progressMs = 0;

inline void Release()
{
    heap_caps_free(snapshot);
    snapshot = nullptr;
    total = sent = 0;
}

inline bool Number(const char* text, size_t& value)
{
    if (!text || !*text || *text == '-') return false;
    char* end = nullptr;
    value = strtoul(text, &end, 10);
    return end && !*end;
}

inline void Command(char* line, Resolver resolve, Stats stats, uint32_t frame,
                    MaterialProfile profile = nullptr)
{
    char* context = nullptr;
    char* verb = strtok_r(line, " \t", &context);
    if (!verb) return;
    if (strcmp(verb, "dbg") != 0) return;
    char* op = strtok_r(nullptr, " \t", &context);
    if (!op || strcmp(op, "help") == 0) {
        snprintf(output, sizeof(output), "@DBG HELP stats|check|list|dump NAME [OFFSET [BYTES]]|cancel%s\n",
            profile ? "|profile normal/spiral/gradient/solid/verify" : "");
    } else if (strcmp(op, "cancel") == 0) {
        Release();
        snprintf(output, sizeof(output), "@DBG CANCEL\n");
    } else if (snapshot) {
        snprintf(output, sizeof(output), "@DBG ERROR busy; use dbg cancel\n");
    } else if (strcmp(op, "profile") == 0) {
        char* mode = strtok_r(nullptr, " \t", &context);
        if (!mode || strtok_r(nullptr, " \t", &context) || !profile || !profile(mode))
            snprintf(output, sizeof(output), "@DBG ERROR invalid or disabled material profile\n");
        else
            snprintf(output, sizeof(output), "@DBG PROFILE material=%s\n", mode);
    } else if (strcmp(op, "stats") == 0) {
        stats(output, sizeof(output));
    } else if (strcmp(op, "check") == 0) {
        const bool healthy = heap_caps_check_integrity_all(false);
        snprintf(output, sizeof(output), "@DBG CHECK heap=%s\n", healthy ? "ok" : "corrupt");
    } else if (strcmp(op, "list") == 0) {
        snprintf(output, sizeof(output), "@DBG LIST rgbN scratchN verticesN indicesN hud samples fft filtered dmafrontR dmabackR\n");
    } else if (strcmp(op, "dump") == 0) {
        char* name = strtok_r(nullptr, " \t", &context);
        char* offsetText = strtok_r(nullptr, " \t", &context);
        char* bytesText = strtok_r(nullptr, " \t", &context);
        size_t offset = 0, count = 0;
        View view;
        if (!name || !resolve(name, view) || !view.data || !view.bytes) {
            snprintf(output, sizeof(output), "@DBG ERROR unavailable target\n");
            return;
        }
        if ((offsetText && !Number(offsetText, offset)) || offset >= view.bytes ||
            (bytesText && !Number(bytesText, count)) || strtok_r(nullptr, " \t", &context)) {
            snprintf(output, sizeof(output), "@DBG ERROR invalid range\n");
            return;
        }
        if (!bytesText) count = view.bytes - offset;
        if (!count || count > view.bytes - offset || count > 65536) {
            snprintf(output, sizeof(output), "@DBG ERROR range exceeds target or 65536-byte limit\n");
            return;
        }
        snapshot = static_cast<uint8_t*>(heap_caps_malloc(count, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!snapshot) {
            snprintf(output, sizeof(output), "@DBG ERROR PSRAM allocation failed\n");
            return;
        }
        memcpy(snapshot, static_cast<const uint8_t*>(view.data) + offset, count);
        total = count;
        sent = 0;
        hash = 2166136261u;
        for (size_t i = 0; i < count; ++i) hash = (hash ^ snapshot[i]) * 16777619u;
        ++sequence;
        progressMs = millis();
        snprintf(output, sizeof(output), "@DBG BEGIN %lu %s %u %u %08lx %s %u %u %lu\n",
            static_cast<unsigned long>(sequence), name, unsigned(offset), unsigned(count),
            static_cast<unsigned long>(hash), view.format, view.width, view.height,
            static_cast<unsigned long>(frame));
    } else {
        snprintf(output, sizeof(output), "@DBG ERROR unknown command\n");
    }
}

// At most 64 RX bytes and four short TX records per frame. USB backpressure
// never pauses rendering. Lost clients release their snapshot after five seconds.
inline void Poll(Resolver resolve, Stats stats, uint32_t frame, MaterialProfile profile = nullptr)
{
    if (snapshot && millis() - progressMs > 5000) {
        Release();
        snprintf(output, sizeof(output), "@DBG ERROR transfer timeout\n");
    }
    for (unsigned n = 0; n < 64 && Serial.available() && !output[0]; ++n) {
        const char ch = char(Serial.read());
        if (ch == '\r') continue;
        if (ch == '\n') {
            input[inputLength] = 0;
            if (overflow) snprintf(output, sizeof(output), "@DBG ERROR line too long\n");
            else Command(input, resolve, stats, frame, profile);
            inputLength = 0;
            overflow = false;
        } else if (inputLength + 1 < sizeof(input)) input[inputLength++] = ch;
        else overflow = true;
    }
    for (unsigned n = 0; n < 4; ++n) {
        if (!output[0]) {
            if (!snapshot) break;
            if (sent == total) {
                snprintf(output, sizeof(output), "@DBG END %lu %u %08lx\n",
                    static_cast<unsigned long>(sequence), unsigned(total), static_cast<unsigned long>(hash));
                Release();
            } else {
                const size_t count = total - sent < 32 ? total - sent : 32;
                size_t pos = snprintf(output, sizeof(output), "@DBG DATA %lu %u ",
                    static_cast<unsigned long>(sequence), unsigned(sent));
                constexpr char hex[] = "0123456789abcdef";
                for (size_t i = 0; i < count; ++i) {
                    output[pos++] = hex[snapshot[sent + i] >> 4];
                    output[pos++] = hex[snapshot[sent + i] & 15];
                }
                output[pos++] = '\n';
                output[pos] = 0;
                sent += count;
            }
        }
        const char* newline = strchr(output, '\n');
        const size_t length = newline ? size_t(newline - output) + 1 : strlen(output);
        if (Serial.availableForWrite() < int(length)) break;
        // This loop is the only task writing debug records; other logs are ignored
        // by the host parser. Retain the remainder even on a partial USB write.
        const size_t written = Serial.write(reinterpret_cast<const uint8_t*>(output), length);
        if (!written) break;
        memmove(output, output + written, strlen(output) - written + 1);
        progressMs = millis();
        if (output[0]) break;
    }
}
#endif
}
