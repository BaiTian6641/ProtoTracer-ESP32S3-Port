#pragma once
// Host-test shim for ESP-IDF esp_attr.h (Windows x64, MSVC).
// Memory-placement attributes are no-ops on the host.

#define IRAM_ATTR
#define DRAM_ATTR
#define EXT_RAM_ATTR
#define EXT_RAM_BSS_ATTR
#define RTC_DATA_ATTR
#define PORT_IRAM_ATTR
#define PORT_FORCE_INLINE
