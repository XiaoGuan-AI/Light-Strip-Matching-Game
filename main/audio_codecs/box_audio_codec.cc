#include "box_audio_codec.h"

#include <cmath>
#include <cstring>
#include <esp_log.h>
#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <driver/i2s_tdm.h>

static const char TAG[] = "BoxAudioCodec";

BoxAudioCodec::BoxAudioCodec(void* i2c_master_handle, int input_sample_rate, int output_sample_rate,
    gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout, gpio_num_t din,
    gpio_num_t pa_pin, uint8_t es8311_addr, uint8_t es7210_addr, bool input_reference) {
    duplex_ = true; // 是否双工
    input_reference_ = input_reference; // 是否使用参考输入，实现回声消除
    input_channels_ = input_reference_ ? 2 : 1; // 输入通道数
    input_sample_rate_ = input_sample_rate;
    output_sample_rate_ = output_sample_rate;
    pa_pin_ = pa_pin;  // 保存 PA 引脚，稍后手动控制
    
    // 先初始化 PA 引脚（在 ESP-ADF 初始化之前）
    if (pa_pin_ != GPIO_NUM_NC) {
        gpio_config_t io_conf = {
            .pin_bit_mask = (1ULL << pa_pin_),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&io_conf);
        gpio_set_level(pa_pin_, 0);  // 初始关闭
        ESP_LOGI(TAG, "PA GPIO%d configured as output", pa_pin_);
    }

    CreateDuplexChannels(mclk, bclk, ws, dout, din);

    // Do initialize of related interface: data_if, ctrl_if and gpio_if
    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = I2S_NUM_0,
        .rx_handle = rx_handle_,
        .tx_handle = tx_handle_,
    };
    data_if_ = audio_codec_new_i2s_data(&i2s_cfg);
    assert(data_if_ != NULL);

    // Output
    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = (i2c_port_t)1,
        .addr = es8311_addr,
        .bus_handle = i2c_master_handle,
    };
    out_ctrl_if_ = audio_codec_new_i2c_ctrl(&i2c_cfg);
    assert(out_ctrl_if_ != NULL);

    gpio_if_ = audio_codec_new_gpio();
    assert(gpio_if_ != NULL);

    es8311_codec_cfg_t es8311_cfg = {};
    es8311_cfg.ctrl_if = out_ctrl_if_;
    es8311_cfg.gpio_if = gpio_if_;
    es8311_cfg.codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC;
    es8311_cfg.pa_pin = pa_pin;
    es8311_cfg.use_mclk = true;
    es8311_cfg.hw_gain.pa_voltage = 5.0;
    es8311_cfg.hw_gain.codec_dac_voltage = 3.3;
    out_codec_if_ = es8311_codec_new(&es8311_cfg);
    assert(out_codec_if_ != NULL);

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = out_codec_if_,
        .data_if = data_if_,
    };
    output_dev_ = esp_codec_dev_new(&dev_cfg);
    assert(output_dev_ != NULL);

    // Input
    i2c_cfg.addr = es7210_addr;
    in_ctrl_if_ = audio_codec_new_i2c_ctrl(&i2c_cfg);
    assert(in_ctrl_if_ != NULL);

    es7210_codec_cfg_t es7210_cfg = {};
    es7210_cfg.ctrl_if = in_ctrl_if_;
    es7210_cfg.mic_selected = ES7120_SEL_MIC1 | ES7120_SEL_MIC2 | ES7120_SEL_MIC3 | ES7120_SEL_MIC4;
    in_codec_if_ = es7210_codec_new(&es7210_cfg);
    assert(in_codec_if_ != NULL);

    dev_cfg.dev_type = ESP_CODEC_DEV_TYPE_IN;
    dev_cfg.codec_if = in_codec_if_;
    input_dev_ = esp_codec_dev_new(&dev_cfg);
    assert(input_dev_ != NULL);

    ESP_LOGI(TAG, "BoxAudioDevice initialized");
}

BoxAudioCodec::~BoxAudioCodec() {
    ESP_ERROR_CHECK(esp_codec_dev_close(output_dev_));
    esp_codec_dev_delete(output_dev_);
    ESP_ERROR_CHECK(esp_codec_dev_close(input_dev_));
    esp_codec_dev_delete(input_dev_);

    audio_codec_delete_codec_if(in_codec_if_);
    audio_codec_delete_ctrl_if(in_ctrl_if_);
    audio_codec_delete_codec_if(out_codec_if_);
    audio_codec_delete_ctrl_if(out_ctrl_if_);
    audio_codec_delete_gpio_if(gpio_if_);
    audio_codec_delete_data_if(data_if_);
}

void BoxAudioCodec::CreateDuplexChannels(gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout, gpio_num_t din) {
    assert(input_sample_rate_ == output_sample_rate_);

    i2s_chan_config_t chan_cfg = {
        .id = I2S_NUM_0,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = AUDIO_CODEC_DMA_DESC_NUM,
        .dma_frame_num = AUDIO_CODEC_DMA_FRAME_NUM,
        .auto_clear_after_cb = true,
        .auto_clear_before_cb = false,
        .intr_priority = 0,
    };
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle_, &rx_handle_));

    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = (uint32_t)output_sample_rate_,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .ext_clk_freq_hz = 0,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_STEREO,
            .slot_mask = I2S_STD_SLOT_BOTH,
            .ws_width = I2S_DATA_BIT_WIDTH_16BIT,
            .ws_pol = false,
            .bit_shift = true,
            .left_align = true,
            .big_endian = false,
            .bit_order_lsb = false
        },
        .gpio_cfg = {
            .mclk = mclk,
            .bclk = bclk,
            .ws = ws,
            .dout = dout,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false
            }
        }
    };

    i2s_tdm_config_t tdm_cfg = {
        .clk_cfg = {
            .sample_rate_hz = (uint32_t)input_sample_rate_,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .ext_clk_freq_hz = 0,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
            .bclk_div = 8,
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_STEREO,
            .slot_mask = i2s_tdm_slot_mask_t(I2S_TDM_SLOT0 | I2S_TDM_SLOT1 | I2S_TDM_SLOT2 | I2S_TDM_SLOT3),
            .ws_width = I2S_TDM_AUTO_WS_WIDTH,
            .ws_pol = false,
            .bit_shift = true,
            .left_align = false,
            .big_endian = false,
            .bit_order_lsb = false,
            .skip_mask = false,
            .total_slot = I2S_TDM_AUTO_SLOT_NUM
        },
        .gpio_cfg = {
            .mclk = mclk,
            .bclk = bclk,
            .ws = ws,
            .dout = I2S_GPIO_UNUSED,
            .din = din,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false
            }
        }
    };

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle_, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_init_tdm_mode(rx_handle_, &tdm_cfg));
    ESP_LOGI(TAG, "Duplex channels created");
}

void BoxAudioCodec::SetOutputVolume(int volume) {
    ESP_ERROR_CHECK(esp_codec_dev_set_out_vol(output_dev_, volume));
    AudioCodec::SetOutputVolume(volume);
}

void BoxAudioCodec::EnableInput(bool enable) {
    if (enable == input_enabled_) {
        return;
    }
    if (enable) {
        esp_codec_dev_sample_info_t fs = {
            .bits_per_sample = 16,
            .channel = 4,
            .channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0),
            .sample_rate = (uint32_t)output_sample_rate_,
            .mclk_multiple = 0,
        };
        if (input_reference_) {
            fs.channel_mask |= ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1);
        }
        ESP_ERROR_CHECK(esp_codec_dev_open(input_dev_, &fs));
        ESP_ERROR_CHECK(esp_codec_dev_set_in_channel_gain(input_dev_, ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0), 40.0));
    } else {
        ESP_ERROR_CHECK(esp_codec_dev_close(input_dev_));
    }
    AudioCodec::EnableInput(enable);
}

void BoxAudioCodec::EnableOutput(bool enable) {
    if (enable == output_enabled_) {
        return;
    }
    if (enable) {
        // Play 16bit 1 channel
        esp_codec_dev_sample_info_t fs = {
            .bits_per_sample = 16,
            .channel = 1,
            .channel_mask = 0,
            .sample_rate = (uint32_t)output_sample_rate_,
            .mclk_multiple = 0,
        };
        ESP_ERROR_CHECK(esp_codec_dev_open(output_dev_, &fs));
        // 设置最大音量进行测试
        ESP_ERROR_CHECK(esp_codec_dev_set_out_vol(output_dev_, 100));
        
        // 手动启用 PA（ESP-ADF 的 gpio_if 可能不工作）
        if (pa_pin_ != GPIO_NUM_NC) {
            gpio_set_level(pa_pin_, 1);
            ESP_LOGI(TAG, "PA GPIO%d enabled (manual)", pa_pin_);
        }
        
        // 延时确保 PA 稳定后再播放
        vTaskDelay(pdMS_TO_TICKS(100));
        
        // 播放测试音（1000Hz 正弦波，持续 500ms）
        static bool test_played = false;
        if (!test_played) {
            test_played = true;
            ESP_LOGW(TAG, "🔊 Playing 1000Hz test tone for 500ms...");
            const int test_freq = 1000;  // 1000Hz
            const int test_duration_ms = 500;
            const int samples_per_ms = output_sample_rate_ / 1000;
            const int total_samples = samples_per_ms * test_duration_ms;
            const int chunk_size = 480;  // 每次写入 480 个采样
            
            int16_t* tone_buf = (int16_t*)malloc(chunk_size * sizeof(int16_t));
            if (tone_buf) {
                for (int i = 0; i < total_samples; i += chunk_size) {
                    int samples_to_write = (i + chunk_size > total_samples) ? (total_samples - i) : chunk_size;
                    for (int j = 0; j < samples_to_write; j++) {
                        // 生成正弦波：sin(2*pi*f*t) * amplitude
                        float t = (float)(i + j) / output_sample_rate_;
                        tone_buf[j] = (int16_t)(sinf(2.0f * 3.14159f * test_freq * t) * 20000);
                    }
                    esp_codec_dev_write(output_dev_, tone_buf, samples_to_write * sizeof(int16_t));
                }
                free(tone_buf);
                ESP_LOGW(TAG, "🔊 Test tone finished!");
            }
        }
        
        ESP_LOGI(TAG, "Output enabled, volume=100");
    } else {
        // 先关闭 PA
        if (pa_pin_ != GPIO_NUM_NC) {
            gpio_set_level(pa_pin_, 0);
            ESP_LOGI(TAG, "PA GPIO%d disabled (manual)", pa_pin_);
        }
        ESP_ERROR_CHECK(esp_codec_dev_close(output_dev_));
        ESP_LOGI(TAG, "Output disabled");
    }
    AudioCodec::EnableOutput(enable);
}

int BoxAudioCodec::Read(int16_t* dest, int samples) {
    if (input_enabled_) {
        esp_err_t ret = esp_codec_dev_read(input_dev_, (void*)dest, samples * sizeof(int16_t));
        if (ret != ESP_OK) {
            // Throttle error logging to avoid flooding console
            static int error_count = 0;
            error_count++;
            if (error_count <= 3 || (error_count % 100) == 0) {
                ESP_LOGW(TAG, "Codec read error (count=%d): %s", error_count, esp_err_to_name(ret));
            }
            // Fill with silence on error
            memset(dest, 0, samples * sizeof(int16_t));
        }
    }
    return samples;
}

int BoxAudioCodec::Write(const int16_t* data, int samples) {
    if (output_enabled_) {
        // 调试：打印前几次写入信息
        static int write_count = 0;
        write_count++;
        if (write_count <= 5) {
            ESP_LOGI(TAG, "Write[%d]: samples=%d, vol=%d, data[0]=%d", 
                     write_count, samples, output_volume_, data[0]);
        }
        esp_err_t ret = esp_codec_dev_write(output_dev_, (void*)data, samples * sizeof(int16_t));
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Write failed: %s", esp_err_to_name(ret));
        }
    } else {
        ESP_LOGW(TAG, "Write: output not enabled! samples=%d", samples);
    }
    return samples;
}