/**
 * @file buzzer_control.cc
 * @brief MAX98357A I2S 功放提示音控制实现
 */
#include "buzzer_control.h"
#include "component_config.h"

#include <algorithm>
#include <cstring>
#include <driver/i2s_std.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define TAG "SpeakerControl"

namespace {
constexpr size_t kFramesPerChunk = 256;
constexpr uint16_t kTailSilenceMs = 20;
}

BuzzerControl::BuzzerControl() : tx_handle_(nullptr), initialized_(false) {
}

BuzzerControl::~BuzzerControl() {
    if (tx_handle_ != nullptr) {
        i2s_channel_disable(tx_handle_);
        i2s_del_channel(tx_handle_);
        tx_handle_ = nullptr;
    }
}

bool BuzzerControl::Initialize() {
    if (initialized_) {
        return true;
    }

    i2s_chan_config_t chan_cfg = {
        .id = SPEAKER_I2S_PORT,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = 4,
        .dma_frame_num = 256,
        .auto_clear_after_cb = true,
        .auto_clear_before_cb = false,
        .intr_priority = 0,
    };

    esp_err_t err = i2s_new_channel(&chan_cfg, &tx_handle_, nullptr);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S channel create failed: %s", esp_err_to_name(err));
        tx_handle_ = nullptr;
        return false;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = SPEAKER_SAMPLE_RATE_HZ,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
#ifdef I2S_HW_VERSION_2
            .ext_clk_freq_hz = 0,
#endif
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_STEREO,
            .slot_mask = I2S_STD_SLOT_BOTH,
            .ws_width = I2S_DATA_BIT_WIDTH_16BIT,
            .ws_pol = false,
            .bit_shift = true,
#ifdef I2S_HW_VERSION_2
            .left_align = true,
            .big_endian = false,
            .bit_order_lsb = false,
#endif
        },
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = SPEAKER_I2S_BCLK,
            .ws = SPEAKER_I2S_LRCLK,
            .dout = SPEAKER_I2S_DOUT,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    err = i2s_channel_init_std_mode(tx_handle_, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S std mode init failed: %s", esp_err_to_name(err));
        i2s_del_channel(tx_handle_);
        tx_handle_ = nullptr;
        return false;
    }

    err = i2s_channel_enable(tx_handle_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S channel enable failed: %s", esp_err_to_name(err));
        i2s_del_channel(tx_handle_);
        tx_handle_ = nullptr;
        return false;
    }

    initialized_ = true;
    WriteSilence(kTailSilenceMs);
    ESP_LOGI(TAG, "MAX98357A I2S initialized: BCLK GPIO%d, LRC GPIO%d, DIN GPIO%d",
             SPEAKER_I2S_BCLK, SPEAKER_I2S_LRCLK, SPEAKER_I2S_DOUT);
    return true;
}

void BuzzerControl::Beep(uint16_t duration_ms) {
    Tone(SPEAKER_DEFAULT_FREQ_HZ, duration_ms);
}

void BuzzerControl::Tone(uint16_t frequency_hz, uint16_t duration_ms) {
    if (!initialized_ || tx_handle_ == nullptr) {
        return;
    }

    if (frequency_hz == 0 || duration_ms == 0) {
        WriteSilence(duration_ms);
        return;
    }

    const uint32_t total_frames =
        (static_cast<uint32_t>(SPEAKER_SAMPLE_RATE_HZ) * duration_ms) / 1000;
    const uint32_t half_period_frames =
        std::max<uint32_t>(1, SPEAKER_SAMPLE_RATE_HZ / (static_cast<uint32_t>(frequency_hz) * 2));

    int16_t samples[kFramesPerChunk * 2];
    uint32_t frames_left = total_frames;
    uint32_t phase_frames = 0;
    bool high = true;

    while (frames_left > 0) {
        const size_t frames = std::min<uint32_t>(frames_left, kFramesPerChunk);

        for (size_t i = 0; i < frames; ++i) {
            const int16_t sample = high ? SPEAKER_TONE_AMPLITUDE : -SPEAKER_TONE_AMPLITUDE;
            samples[i * 2] = sample;
            samples[i * 2 + 1] = sample;

            if (++phase_frames >= half_period_frames) {
                phase_frames = 0;
                high = !high;
            }
        }

        size_t bytes_written = 0;
        const esp_err_t err = i2s_channel_write(
            tx_handle_, samples, frames * 2 * sizeof(int16_t), &bytes_written, portMAX_DELAY);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "I2S tone write failed: %s", esp_err_to_name(err));
            break;
        }

        frames_left -= frames;
    }

    WriteSilence(kTailSilenceMs);
}

void BuzzerControl::WriteSilence(uint16_t duration_ms) {
    if (tx_handle_ == nullptr || duration_ms == 0) {
        return;
    }

    int16_t samples[kFramesPerChunk * 2];
    std::memset(samples, 0, sizeof(samples));

    uint32_t frames_left =
        (static_cast<uint32_t>(SPEAKER_SAMPLE_RATE_HZ) * duration_ms) / 1000;
    while (frames_left > 0) {
        const size_t frames = std::min<uint32_t>(frames_left, kFramesPerChunk);
        size_t bytes_written = 0;
        const esp_err_t err = i2s_channel_write(
            tx_handle_, samples, frames * 2 * sizeof(int16_t), &bytes_written, portMAX_DELAY);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "I2S silence write failed: %s", esp_err_to_name(err));
            break;
        }
        frames_left -= frames;
    }
}

void BuzzerControl::BeepMultiple(uint8_t count, uint16_t duration_ms, uint16_t interval_ms) {
    if (!initialized_ || count == 0) {
        return;
    }

    for (uint8_t i = 0; i < count; i++) {
        Tone(SPEAKER_DEFAULT_FREQ_HZ, duration_ms);

        if (i < count - 1) {
            vTaskDelay(pdMS_TO_TICKS(interval_ms));
        }
    }
}

void BuzzerControl::SuccessBeep() {
    Tone(1200, 100);
    vTaskDelay(pdMS_TO_TICKS(50));
    Tone(1800, 150);
    ESP_LOGI(TAG, "Success tone");
}

void BuzzerControl::ErrorBeep() {
    Tone(600, 150);
    vTaskDelay(pdMS_TO_TICKS(80));
    Tone(420, 200);
    vTaskDelay(pdMS_TO_TICKS(80));
    Tone(300, 150);
    ESP_LOGI(TAG, "Error tone");
}

void BuzzerControl::OverflowBeep(uint8_t group) {
    uint8_t beep_count = (group > 0) ? group : 1;
    BeepMultiple(beep_count, 80, OVERFLOW_BEEP_DELAY_MS);
    ESP_LOGI(TAG, "Overflow tone for group %d (%d times)", group, beep_count);
}
