#pragma once
#include <driver/parlio_tx.h>
#include <esp_heap_caps.h>
#include <esp_attr.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <new>

// ISR context is explicitly internal RAM. DMA source ownership persists across
// a timeout; only the matching hardware switch releases the previous frame.
class S31ParlioFrameSync {
    struct Context {
        StaticSemaphore_t storage{};
        SemaphoreHandle_t semaphore = nullptr;
        portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
        void* oldBuffer = nullptr;
        void* newBuffer = nullptr;
        bool pending = false;
        bool confirmed = false;
    };
    Context* context = nullptr;

    static bool IRAM_ATTR Switched(parlio_tx_unit_handle_t,
                                  const parlio_tx_buffer_switched_event_data_t* event,
                                  void* user) {
        auto* ctx = static_cast<Context*>(user);
        bool matched;
        portENTER_CRITICAL_ISR(&ctx->mux);
        matched = ctx->pending && event->old_buffer_addr == ctx->oldBuffer &&
                  event->new_buffer_addr == ctx->newBuffer;
        if (matched) ctx->confirmed = true;
        portEXIT_CRITICAL_ISR(&ctx->mux);
        BaseType_t wake = pdFALSE;
        if (matched) xSemaphoreGiveFromISR(ctx->semaphore, &wake);
        return wake == pdTRUE;
    }

public:
    S31ParlioFrameSync() = default;
    S31ParlioFrameSync(const S31ParlioFrameSync&) = delete;
    S31ParlioFrameSync& operator=(const S31ParlioFrameSync&) = delete;
    ~S31ParlioFrameSync() {
        // The enclosing driver must disable/delete its TX unit before this.
        if (context) {
            if (context->semaphore) vSemaphoreDelete(context->semaphore);
            context->~Context(); heap_caps_free(context);
        }
    }
    bool Initialize(parlio_tx_unit_handle_t unit) {
        if (!context) {
            void* memory = heap_caps_malloc(sizeof(Context), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            if (!memory) return false;
            context = new (memory) Context();
            context->semaphore = xSemaphoreCreateBinaryStatic(&context->storage);
            if (!context->semaphore) return false;
        }
        parlio_tx_event_callbacks_t callbacks{};
        callbacks.on_buffer_switched = Switched;
        return parlio_tx_unit_register_event_callbacks(unit, &callbacks, context) == ESP_OK;
    }
    void Abandon() { context = nullptr; } // Quarantine if hardware cannot be stopped.
    bool Pending() const {
        if (!context) return false;
        portENTER_CRITICAL(&context->mux);
        const bool value = context->pending;
        portEXIT_CRITICAL(&context->mux);
        return value;
    }
    bool Queue(parlio_tx_unit_handle_t unit, void* oldBuffer, void* newBuffer,
               size_t bits, const parlio_transmit_config_t* config) {
        if (!context || Pending()) return false;
        while (xSemaphoreTake(context->semaphore, 0) == pdTRUE) {}
        portENTER_CRITICAL(&context->mux);
        context->oldBuffer = oldBuffer;
        context->newBuffer = newBuffer;
        context->confirmed = false;
        context->pending = true;
        portEXIT_CRITICAL(&context->mux);
        if (parlio_tx_unit_transmit(unit, newBuffer, bits, config) == ESP_OK) return true;
        portENTER_CRITICAL(&context->mux);
        context->pending = false;
        portEXIT_CRITICAL(&context->mux);
        return false;
    }
    bool Wait(uint32_t timeoutMs) {
        if (!context) return false;
        if (!Pending()) return true;
        xSemaphoreTake(context->semaphore, pdMS_TO_TICKS(timeoutMs));
        portENTER_CRITICAL(&context->mux);
        const bool complete = context->confirmed;
        if (complete) context->pending = false;
        portEXIT_CRITICAL(&context->mux);
        return complete;
    }
};
