#pragma once

#include <M5GFX.h>
#include <lgfx/utility/lgfx_qrcode.h>
#include <cstring>
#include "OledCare.h"

// Registration payload is the complete chip ID, without a URL or decoration.
// Crisp binary pixels; use the full height while preserving the quiet zone.
inline void ShowDeviceRegistrationScreen(M5GFX* screen, const String& deviceId,
                                         const String& setupPassword = String())
{
    if (!screen || deviceId.isEmpty()) return;
    const int width = screen->width();
    const int height = screen->height();
    const int areaWidth = width < 120 ? width : 120;
    const int areaHeight = height < 42 ? height : 42;
    const int side = areaWidth < areaHeight ? areaWidth : areaHeight;
    // A 32-character hexadecimal ID fits QR version 2 (25 modules + margins).
    if (side < 33) return;
    QRCode qr;
    uint8_t modules[128];
    if (lgfx_qrcode_initText(&qr, modules, 2, ECC_LOW, deviceId.c_str()) != 0) return;
    // Full ID needs 25 modules: 2x with four-module margins needs 66 pixels.
    // At the required 42px height, 1x is the largest uniform integer scale.
    // Fractional 30px patterns failed decoding; preserve scan reliability.
    const int scale = side / (qr.size + 8);
    const int pattern = qr.size * scale;
    const int quiet = (side - pattern) / 2;
    const int driftLimit = quiet - scale * 4;
    const int dx = driftLimit >= 1 ? (OledCare::OffsetX() > 1 ? 1 : OledCare::OffsetX()) : 0;
    const int dy = driftLimit >= 1 ? (OledCare::OffsetY() > 1 ? 1 : OledCare::OffsetY()) : 0;
    const int areaX = (width - areaWidth) / 2;
    const int areaY = (height - areaHeight) / 2;
    const bool showPassword = !setupPassword.isEmpty() && areaWidth >= side + 28;
    const int tileX = showPassword ? areaX : (width - side) / 2;
    const int tileY = (height - side) / 2;
    screen->startWrite();
    screen->fillScreen(TFT_BLACK);
    // No illuminated side bars: less OLED exposure, with a white QR quiet zone.
    screen->fillRect(tileX, tileY, side, side, TFT_WHITE);
    for (int y = 0; y < qr.size; ++y)
    {
        const int top = tileY + quiet + dy + y * pattern / qr.size;
        const int bottom = tileY + quiet + dy + (y + 1) * pattern / qr.size;
        for (int x = 0; x < qr.size; ++x)
        {
            if (!lgfx_qrcode_getModule(&qr, x, y)) continue;
            const int left = tileX + quiet + dx + x * pattern / qr.size;
            const int right = tileX + quiet + dx + (x + 1) * pattern / qr.size;
            screen->fillRect(left, top, right - left, bottom - top, TFT_BLACK);
        }
    }
    if (showPassword)
    {
        // Show only the device's setup AP password, never saved home WiFi.
        // Keep text outside the QR tile/quiet zone and within the 120x42 area.
        const int paneX = tileX + side + 4;
        const int paneWidth = areaX + areaWidth - paneX - 2;
        const int bodyHeight = areaHeight - 12;
        const int normalColumns = paneWidth / 6;
        const bool compact = setupPassword.length() > unsigned(normalColumns * (bodyHeight / 8));
        const int columns = paneWidth / (compact ? 4 : 6);
        const int pitch = compact ? 6 : 8;
        const auto savedStyle = screen->getTextStyle();
        const auto savedFont = screen->getFont();
        screen->setTextSize(1);
        screen->setTextDatum(textdatum_t::top_left);
        screen->setTextPadding(0);
        screen->setTextColor(TFT_WHITE, TFT_BLACK);
        screen->drawString("WiFi PW", paneX + 1 + dx, areaY + 1 + dy, &fonts::Font0);
        // Up to 20 compact-font columns on a shorter supported QR tile.
        char line[21];
        for (size_t offset = 0, row = 0; offset < setupPassword.length() && row < unsigned(bodyHeight / pitch); ++row)
        {
            const size_t count = setupPassword.length() - offset < unsigned(columns)
                                 ? setupPassword.length() - offset : unsigned(columns);
            std::memcpy(line, setupPassword.c_str() + offset, count);
            line[count] = '\0';
            screen->drawString(line, paneX + 1 + dx, areaY + 11 + dy + row * pitch,
                               compact ? static_cast<const lgfx::IFont*>(&fonts::TomThumb)
                                       : static_cast<const lgfx::IFont*>(&fonts::Font0));
            offset += count;
        }
        screen->setFont(savedFont);
        screen->setTextStyle(savedStyle);
    }
    screen->display();
    screen->endWrite();
}
