#pragma once
#include <cstdlib>
#include <cstdint>
using BaseType_t = int;
constexpr int pdFALSE = 0, pdTRUE = 1, ESP_OK = 0;
#define IRAM_ATTR
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_8BIT 2
#define portMUX_INITIALIZER_UNLOCKED 0
using portMUX_TYPE = int;
#define portENTER_CRITICAL(x) ((void)(x))
#define portEXIT_CRITICAL(x) ((void)(x))
#define portENTER_CRITICAL_ISR(x) ((void)(x))
#define portEXIT_CRITICAL_ISR(x) ((void)(x))
#define pdMS_TO_TICKS(x) (x)
struct StaticSemaphore_t { bool signaled = false; };
using SemaphoreHandle_t = StaticSemaphore_t*;
inline void* heap_caps_malloc(size_t n, int) { return std::malloc(n); }
inline void heap_caps_free(void* p) { std::free(p); }
inline SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t* s) { return s; }
inline void vSemaphoreDelete(SemaphoreHandle_t) {}
inline int xSemaphoreTake(SemaphoreHandle_t s, unsigned) {
    const bool value = s->signaled; s->signaled = false; return value;
}
inline void xSemaphoreGiveFromISR(SemaphoreHandle_t s, BaseType_t*) { s->signaled = true; }
struct FakeUnit;
using parlio_tx_unit_handle_t = FakeUnit*;
struct parlio_tx_buffer_switched_event_data_t { void* old_buffer_addr; void* new_buffer_addr; };
using Callback = bool (*)(parlio_tx_unit_handle_t, const parlio_tx_buffer_switched_event_data_t*, void*);
struct parlio_tx_event_callbacks_t { Callback on_buffer_switched = nullptr; };
struct parlio_transmit_config_t {};
struct FakeUnit {
    Callback callback = nullptr;
    void* user = nullptr;
    void* current = nullptr;
    void* requested = nullptr;
    bool fail = false;
    bool immediate = false;
    void Complete(void* oldBuffer, void* newBuffer) {
        parlio_tx_buffer_switched_event_data_t event{oldBuffer, newBuffer};
        callback(this, &event, user);
    }
};
inline int parlio_tx_unit_register_event_callbacks(FakeUnit* u, const parlio_tx_event_callbacks_t* c, void* p) {
    u->callback = c->on_buffer_switched; u->user = p; return ESP_OK;
}
inline int parlio_tx_unit_transmit(FakeUnit* u, void* p, size_t, const parlio_transmit_config_t*) {
    if (u->fail) return -1;
    u->requested = p;
    if (u->immediate) u->Complete(u->current, p);
    return ESP_OK;
}
