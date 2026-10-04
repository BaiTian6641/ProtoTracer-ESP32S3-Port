#pragma once

#include <M5GFX.h>
#include <atomic>
#include "OledCarePolicy.h"

// All panel operations run on the same application task as the display/gesture
// I2C traffic. BLE callbacks only set an atomic wake request; no extra task/timer.
namespace OledCare
{
    inline M5GFX* screen = nullptr;
    inline oledcare::Policy policy;
    inline oledcare::Frame frame;
    inline std::atomic<bool> wakeRequested{false};
    inline bool enabled = false;
    inline bool applied = false;
    inline bool redraw = false;

    inline void RequestWake() { wakeRequested.store(true, std::memory_order_relaxed); }
    inline void Activity(uint32_t now) { policy.Activity(now); }

    inline void Configure(M5GFX* display, bool isOled, uint32_t now)
    {
        screen = display;
        enabled = display && isOled;
        applied = false;
        redraw = true;
        wakeRequested.store(false, std::memory_order_relaxed);
        frame = {};
        policy.Begin(now);
    }

    inline void SetRegistration(bool registration, uint32_t now)
    {
        policy.SetRegistration(registration, now);
        redraw = true;
    }

    inline void Poll(uint32_t now)
    {
        if (!enabled) return;
        if (wakeRequested.exchange(false, std::memory_order_relaxed)) policy.Activity(now);
        const auto next = policy.Evaluate(now);
        redraw = redraw || !applied || next.x != frame.x || next.y != frame.y
                 || (frame.asleep && !next.asleep);
        if (!applied || next.asleep != frame.asleep)
        {
            // Use display on/off without LGFX wakeup() changing drive voltages.
            screen->panel()->setSleep(next.asleep);
        }
        if (!next.asleep && (!applied || frame.asleep || next.contrast != frame.contrast))
        {
            // SSD1309/SH1107 share the contrast command. Avoid setBrightness()
            // on SSD1309: that also changes precharge and VCOM settings.
            screen->startWrite();
            screen->writeCommand(0x81);
            screen->writeCommand(next.contrast);
            screen->endWrite();
        }
#ifdef PRINTINFO
        if (!applied || next.asleep != frame.asleep || next.contrast != frame.contrast)
            Serial.printf("[OLED] registration=%u contrast=%u asleep=%u\n",
                          unsigned(policy.IsRegistration()), unsigned(next.contrast), unsigned(next.asleep));
#endif
        frame = next;
        applied = true;
    }

    inline bool TakeRedraw()
    {
        const bool pending = redraw;
        redraw = false;
        return pending;
    }

    inline int OffsetX() { return enabled ? frame.x : 0; }
    inline int OffsetY() { return enabled ? frame.y : 0; }
    inline bool IsAsleep() { return enabled && frame.asleep; }
}
