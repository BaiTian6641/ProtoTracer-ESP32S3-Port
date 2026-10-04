#pragma once

#include <Arduino.h>
#include <cstdarg>
#include <cstdio>

namespace BestEffortLog
{
// Ordinary telemetry may be dropped under backpressure. Protocol records use
// SerialDebug's retained output instead. This helper is called only on core 1.
inline void Printf(const char* format, ...)
{
    if (Serial.availableForWrite() <= 0) return;
    char line[224];
    va_list arguments;
    va_start(arguments, format);
    const int length = vsnprintf(line, sizeof(line), format, arguments);
    va_end(arguments);
    if (length <= 0 || length >= int(sizeof(line)) || Serial.availableForWrite() < length) return;
    Serial.write(reinterpret_cast<const uint8_t*>(line), size_t(length));
}
}
