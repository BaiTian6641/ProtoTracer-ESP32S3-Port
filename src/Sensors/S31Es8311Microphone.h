#pragma once

#include <Arduino.h>
#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <driver/i2s_std.h>
#include <esp_codec_dev.h>
#include <esp_codec_dev_defaults.h>
#include <esp_attr.h>

// Function-Core onboard electret microphone. Audio uses I2C0; external HUD
// and gesture devices use I2C1. The PA remains off for input-only operation.
class S31Es8311Microphone {
    inline static i2c_master_bus_handle_t bus = nullptr;
    inline static i2s_chan_handle_t rx = nullptr;
    inline static const audio_codec_ctrl_if_t* control = nullptr;
    inline static const audio_codec_data_if_t* data = nullptr;
    inline static const audio_codec_gpio_if_t* gpio = nullptr;
    inline static const audio_codec_if_t* codec = nullptr;
    inline static esp_codec_dev_handle_t device = nullptr;
    inline static bool rxStarted = false;
    inline static bool deviceOpened = false;
    inline static volatile uint32_t overflowCount = 0;

    static bool IRAM_ATTR Overflow(i2s_chan_handle_t, i2s_event_data_t*, void*) {
        overflowCount = overflowCount + 1;
        return false;
    }

    static bool Failed(const char* stage, int error) {
        Serial.printf("[MIC ES8311] %s failed: %d\n", stage, error);
        Close();
        return false;
    }

public:
    static void Close() {
        if (device) {
            esp_codec_dev_close(device);
            if (deviceOpened) rxStarted = false; // Codec close releases its channel.
            esp_codec_dev_delete(device); device = nullptr; deviceOpened = false;
        }
        if (codec) { audio_codec_delete_codec_if(codec); codec = nullptr; }
        if (data) { audio_codec_delete_data_if(data); data = nullptr; }
        if (control) { audio_codec_delete_ctrl_if(control); control = nullptr; }
        if (gpio) { audio_codec_delete_gpio_if(gpio); gpio = nullptr; }
        if (rx) {
            if (rxStarted) i2s_channel_disable(rx);
            i2s_del_channel(rx); rx = nullptr;
            rxStarted = false;
        }
        if (bus) { i2c_del_master_bus(bus); bus = nullptr; }
    }

    static bool Begin(uint32_t sampleRate) {
        if (device) return true;
        gpio_set_direction(GPIO_NUM_57, GPIO_MODE_OUTPUT);
        gpio_set_level(GPIO_NUM_57, 0);
        i2c_master_bus_config_t i2c{};
        i2c.i2c_port = I2C_NUM_0;
        i2c.sda_io_num = GPIO_NUM_51; i2c.scl_io_num = GPIO_NUM_50;
        i2c.clk_source = I2C_CLK_SRC_DEFAULT; i2c.glitch_ignore_cnt = 7;
        i2c.flags.enable_internal_pullup = true;
        esp_err_t error = i2c_new_master_bus(&i2c, &bus);
        if (error != ESP_OK) return Failed("I2C bus", error);
        error = i2c_master_probe(bus, 0x18, 100);
        if (error != ESP_OK) return Failed("codec probe", error);

        i2s_chan_config_t channel = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
        channel.dma_desc_num = 4; channel.dma_frame_num = 256;
        error = i2s_new_channel(&channel, nullptr, &rx);
        if (error != ESP_OK) return Failed("I2S channel", error);
        i2s_std_config_t standard{};
        standard.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sampleRate);
        standard.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
        standard.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO);
        standard.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
        standard.gpio_cfg.mclk = GPIO_NUM_52; standard.gpio_cfg.bclk = GPIO_NUM_53;
        standard.gpio_cfg.ws = GPIO_NUM_55; standard.gpio_cfg.din = GPIO_NUM_54;
        standard.gpio_cfg.dout = I2S_GPIO_UNUSED;
        error = i2s_channel_init_std_mode(rx, &standard);
        if (error != ESP_OK) return Failed("I2S mode", error);
        overflowCount = 0;
        i2s_event_callbacks_t callbacks{};
        callbacks.on_recv_q_ovf = Overflow;
        error = i2s_channel_register_event_callback(rx, &callbacks, nullptr);
        if (error != ESP_OK) return Failed("I2S callbacks", error);
        error = i2s_channel_enable(rx);
        if (error != ESP_OK) return Failed("I2S enable", error);
        rxStarted = true;
        audio_codec_i2c_cfg_t ctrl{};
        ctrl.port = I2C_NUM_0; ctrl.addr = ES8311_CODEC_DEFAULT_ADDR; ctrl.bus_handle = bus;
        ctrl.clock_speed_hz = 400000;
        control = audio_codec_new_i2c_ctrl(&ctrl);
        audio_codec_i2s_cfg_t stream{};
        stream.port = I2S_NUM_0; stream.rx_handle = rx;
        data = audio_codec_new_i2s_data(&stream);
        gpio = audio_codec_new_gpio();
        if (!control || !data || !gpio) return Failed("interfaces", ESP_ERR_NO_MEM);
        es8311_codec_cfg_t config{};
        config.ctrl_if = control; config.gpio_if = gpio;
        config.codec_mode = ESP_CODEC_DEV_WORK_MODE_ADC;
        config.master_mode = false; config.use_mclk = true;
        config.pa_pin = -1; config.mclk_div = 256;
        codec = es8311_codec_new(&config);
        if (!codec) return Failed("codec", ESP_ERR_NO_MEM);
        esp_codec_dev_cfg_t dev{};
        dev.dev_type = ESP_CODEC_DEV_TYPE_IN; dev.codec_if = codec; dev.data_if = data;
        device = esp_codec_dev_new(&dev);
        if (!device) return Failed("device", ESP_ERR_NO_MEM);
        esp_codec_dev_sample_info_t format{};
        format.sample_rate = sampleRate; format.bits_per_sample = 16;
        // The codec interface describes the two physical I2S slots; the left
        // slot mask selects mono DMA data. It owns channel enable/disable.
        format.channel = 2; format.channel_mask = 1; format.mclk_multiple = 256;
        int result = esp_codec_dev_open(device, &format);
        if (result != ESP_CODEC_DEV_OK) return Failed("open", result);
        deviceOpened = true;
        result = esp_codec_dev_set_in_gain(device, 30.0f);
        if (result != ESP_CODEC_DEV_OK) return Failed("gain", result);
        Serial.printf("[MIC ES8311] ready rate=%u mono=16bit gain=30dB\n", unsigned(sampleRate));
        return true;
    }

    static bool ReadFrame(uint16_t* samples, size_t count) {
        if (!device || !samples || count != 256) return false;
        int16_t pcm[256];
        size_t received = 0;
        const esp_err_t result = i2s_channel_read(rx, pcm, sizeof(pcm), &received, 100);
        if (result != ESP_OK || received != sizeof(pcm)) return false;
        // Preserve the existing FFT/viseme input scale: centered signed PCM
        // becomes the same 0..4095 range as the external 12-bit ADC path.
        for (size_t i = 0; i < count; ++i) samples[i] = uint16_t((int32_t(pcm[i]) + 32768) / 16);
        return true;
    }
    static uint32_t GetOverflowCount() { return overflowCount; }
};
