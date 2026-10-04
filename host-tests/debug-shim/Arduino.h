#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#define strtok_r strtok_s
inline uint32_t debugClock = 0;
inline uint32_t millis() { return debugClock; }
struct DebugSerial
{
    std::string rx, tx;
    size_t cursor = 0;
    int capacity = 4096;
    int available() { return int(rx.size() - cursor); }
    int read() { return uint8_t(rx[cursor++]); }
    int availableForWrite() { return capacity; }
    size_t write(const uint8_t* bytes, size_t count)
    {
        count = std::min(count, size_t(capacity));
        tx.append(reinterpret_cast<const char*>(bytes), count);
        capacity -= int(count);
        return count;
    }
};
inline DebugSerial Serial;
