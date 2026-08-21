#pragma once
// Host-test shim for Arduino.h (Windows x64, MSVC).
//
// Mirrors the two transitive dependencies the firmware gets from the real
// Arduino-ESP32 Arduino.h before any render/math header is parsed:
//   - esp_attr.h  (IRAM_ATTR & friends; Triangle3D.h uses IRAM_ATTR without
//                  including esp_attr.h itself)
//   - WString.h   (String)
//
// Deliberately does NOT define ARDUINO so `#if defined(ARDUINO)` blocks in
// src/ (e.g. Serial.printf in Camera.h) stay compiled out.
//
// Kept in sync with: src/Render, src/Math, src/Materials headers.

#include "esp_attr.h"
#include "WString.h"
