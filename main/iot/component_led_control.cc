/**
 * @file component_led_control.cc
 * @brief 元器件分拣LED控制实现
 */
#include "component_led_control.h"
#include "component_config.h"
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_timer.h>
#include <array>

#define TAG "ComponentLedControl"

namespace {

constexpr uint8_t kMatrixRows = 5;
constexpr uint8_t kMatrixCols = 10;
constexpr uint8_t kDigitWidth = 5;
constexpr uint8_t kDigitHorizontalOffset = 2;

using DigitPattern = std::array<std::array<uint8_t, kDigitWidth>, kMatrixRows>;

constexpr std::array<DigitPattern, 6> kCountdownDigits = {{
    // 0 (5x5)
    {{{0, 1, 1, 1, 0},
      {1, 0, 0, 0, 1},
      {1, 0, 0, 0, 1},
      {1, 0, 0, 0, 1},
      {0, 1, 1, 1, 0}}},
    // 1 (5x5)
    {{{0, 0, 1, 0, 0},
      {0, 1, 1, 0, 0},
      {0, 0, 1, 0, 0},
      {0, 0, 1, 0, 0},
      {0, 1, 1, 1, 0}}},
    // 2 (5x5)
    {{{0, 1, 1, 1, 0},
      {1, 0, 0, 0, 1},
      {0, 0, 1, 1, 0},
      {0, 1, 0, 0, 0},
      {1, 1, 1, 1, 1}}},
    // 3 (5x5)
    {{{1, 1, 1, 1, 0},
      {0, 0, 0, 0, 1},
      {0, 0, 1, 1, 0},
      {0, 0, 0, 0, 1},
      {1, 1, 1, 1, 0}}},
    // 4 (5x5)
    {{{1, 0, 0, 1, 0},
      {1, 0, 0, 1, 0},
      {1, 1, 1, 1, 1},
      {0, 0, 0, 1, 0},
      {0, 0, 0, 1, 0}}},
    // 5 (5x5)
    {{{1, 1, 1, 1, 1},
      {1, 0, 0, 0, 0},
      {1, 1, 1, 1, 0},
      {0, 0, 0, 0, 1},
      {1, 1, 1, 1, 0}}},
}};

uint8_t MatrixIndex(uint8_t row, uint8_t col) {
    if ((row % 2) == 0) {
        return row * kMatrixCols + col;
    }
    return row * kMatrixCols + (kMatrixCols - 1 - col);
}

}  // namespace

ComponentLedControl::ComponentLedControl() : strip_(nullptr), initialized_(false) {
}

ComponentLedControl::~ComponentLedControl() {
    StopAllBlink();
    if (strip_) {
        delete strip_;
        strip_ = nullptr;
    }
}

bool ComponentLedControl::Initialize() {
    if (initialized_) {
        return true;
    }

    // 创建CircularStrip实例
    strip_ = new CircularStrip(LED_STRIP_PIN, LED_STRIP_LENGTH);
    if (!strip_) {
        ESP_LOGE(TAG, "Failed to create LED strip");
        return false;
    }

    // 设置亮度
    strip_->SetBrightness(LED_BRIGHTNESS, 10);

    // 清除所有LED
    strip_->SetAllColor({0, 0, 0});

    initialized_ = true;
    ESP_LOGI(TAG, "LED control initialized, %d LEDs", LED_STRIP_LENGTH);
    return true;
}

void ComponentLedControl::HighlightLed(uint8_t led_index, uint8_t r, uint8_t g, uint8_t b, 
                                       uint8_t blink_count, uint8_t group) {
    if (!initialized_ || !strip_) {
        ESP_LOGW(TAG, "LED not initialized");
        return;
    }

    // 限制LED索引
    if (led_index >= LED_STRIP_LENGTH) {
        led_index = LED_STRIP_LENGTH - 1;
    }

    ESP_LOGI(TAG, "Highlight LED%d (Group %d), Color(%d,%d,%d), Blink %d times", 
             led_index, group, r, g, b, blink_count);

    // 闪烁效果
    for (uint8_t i = 0; i < blink_count; i++) {
        // 点亮指定LED
        StripColor color = {r, g, b};
        strip_->SetSingleColor(led_index, color);
        vTaskDelay(pdMS_TO_TICKS(LED_BLINK_ON_MS));

        // 熄灭
        strip_->SetSingleColor(led_index, {0, 0, 0});
        
        // 溢出组额外闪烁
        if (group > 1) {
            // 快速闪烁表示溢出组
            vTaskDelay(pdMS_TO_TICKS(LED_BLINK_OFF_MS / 2));
        } else {
            vTaskDelay(pdMS_TO_TICKS(LED_BLINK_OFF_MS));
        }
    }

    // 最后保持点亮一段时间
    StripColor final_color = {r, g, b};
    strip_->SetSingleColor(led_index, final_color);
    vTaskDelay(pdMS_TO_TICKS(LED_HOLD_MS));

    // 熄灭
    strip_->SetSingleColor(led_index, {0, 0, 0});
}

void ComponentLedControl::StartAsyncBlink(uint8_t led_index, uint8_t r, uint8_t g, uint8_t b, uint32_t duration_ms) {
    if (!initialized_ || !strip_) {
        ESP_LOGW(TAG, "LED not initialized");
        return;
    }
    
    // 限制LED索引
    if (led_index >= LED_STRIP_LENGTH) {
        led_index = LED_STRIP_LENGTH - 1;
    }
    
    // 先停止该LED之前的闪烁任务（如果有）
    StopBlink(led_index);
    
    // 创建新的闪烁任务（每次查找都独立计时）
    auto task = std::make_unique<LedBlinkTask>(led_index, r, g, b, duration_ms);
    task->start_time = esp_timer_get_time() / 1000;  // 转换为毫秒
    
    ESP_LOGI(TAG, "Start async blink: LED%d, Color(%d,%d,%d), Duration=%lu ms",
             led_index, r, g, b, (unsigned long)duration_ms);
    
    // 添加到任务列表
    blink_tasks_[led_index] = std::move(task);
}

void ComponentLedControl::StopBlink(uint8_t led_index) {
    auto it = blink_tasks_.find(led_index);
    if (it != blink_tasks_.end()) {
        // 熄灭LED
        if (strip_) {
            strip_->SetSingleColor(led_index, {0, 0, 0});
        }
        blink_tasks_.erase(it);
        ESP_LOGI(TAG, "Stopped blink: LED%d", led_index);
    }
}

void ComponentLedControl::StopAllBlink() {
    for (auto& pair : blink_tasks_) {
        if (strip_) {
            strip_->SetSingleColor(pair.first, {0, 0, 0});
        }
    }
    blink_tasks_.clear();
    ESP_LOGI(TAG, "Stopped all blinks");
}

void ComponentLedControl::Update() {
    if (!initialized_ || !strip_ || blink_tasks_.empty()) {
        return;
    }
    
    uint32_t now = esp_timer_get_time() / 1000;  // 毫秒
    std::vector<uint8_t> expired_leds;
    
    for (auto& pair : blink_tasks_) {
        LedBlinkTask& task = *pair.second;
        if (!task.active) continue;
        
        uint32_t elapsed = now - task.start_time;
        
        // 检查是否超时
        if (elapsed >= task.duration) {
            expired_leds.push_back(task.led_index);
            strip_->SetSingleColor(task.led_index, {0, 0, 0});
            continue;
        }
        
        // 计算闪烁（亮-灭-亮-灭...）
        // 闪烁周期：亮500ms + 灭500ms = 1000ms
        uint32_t cycle_time = 1000;  // 1秒一个周期
        uint32_t position = elapsed % cycle_time;
        
        if (position < 500) {
            // 亮
            StripColor color = {task.r, task.g, task.b};
            strip_->SetSingleColor(task.led_index, color);
        } else {
            // 灭
            strip_->SetSingleColor(task.led_index, {0, 0, 0});
        }
    }
    
    // 移除超时的任务
    for (uint8_t led_index : expired_leds) {
        blink_tasks_.erase(led_index);
        ESP_LOGI(TAG, "Blink expired: LED%d", led_index);
    }
}

void ComponentLedControl::SetAllColor(uint8_t r, uint8_t g, uint8_t b) {
    if (!initialized_ || !strip_) return;
    strip_->SetAllColor({r, g, b});
}

void ComponentLedControl::Clear() {
    if (!initialized_ || !strip_) return;
    strip_->SetAllColor({0, 0, 0});
}

void ComponentLedControl::RunWaterfall() {
    if (!initialized_ || !strip_) return;

    // 简单流水灯效果
    for (int round = 0; round < 2; round++) {
        for (int i = 0; i < LED_STRIP_LENGTH; i++) {
            StripColor color = {0, 100, 255};  // 蓝色流水
            strip_->SetSingleColor(i, color);
            vTaskDelay(pdMS_TO_TICKS(30));
            strip_->SetSingleColor(i, {0, 0, 0});
        }
    }
    strip_->SetAllColor({0, 0, 0});
}

void ComponentLedControl::RunStartupCountdown() {
    if (!initialized_ || !strip_) {
        return;
    }

    if (LED_STRIP_LENGTH < (kMatrixRows * kMatrixCols)) {
        ESP_LOGW(TAG, "Startup countdown requires at least %d LEDs, got %d",
                 kMatrixRows * kMatrixCols, LED_STRIP_LENGTH);
        RunWaterfall();
        return;
    }

    constexpr StripColor kDigitColor = {0, 100, 255};
    constexpr uint32_t kDigitHoldMs = 700;
    constexpr uint32_t kDigitGapMs = 180;
    constexpr uint32_t kFinalFlashMs = 220;

    auto draw_digit = [this](uint8_t digit, StripColor color) {
        std::vector<StripColor> frame(LED_STRIP_LENGTH, {0, 0, 0});

        const DigitPattern& pattern = kCountdownDigits[digit];
        for (uint8_t row = 0; row < kMatrixRows; ++row) {
            for (uint8_t col = 0; col < kDigitWidth; ++col) {
                if (pattern[row][col] == 0) {
                    continue;
                }

                const uint8_t matrix_col = kDigitHorizontalOffset + col;
                const uint8_t led_index = MatrixIndex(row, matrix_col);
                frame[led_index] = color;
            }
        }

        strip_->SetColors(frame);
    };

    for (int digit = 5; digit >= 0; --digit) {
        draw_digit(static_cast<uint8_t>(digit), kDigitColor);
        vTaskDelay(pdMS_TO_TICKS(kDigitHoldMs));

        strip_->SetAllColor({0, 0, 0});
        vTaskDelay(pdMS_TO_TICKS(kDigitGapMs));
    }

    strip_->SetAllColor({kDigitColor.red / 2, kDigitColor.green / 2, kDigitColor.blue / 2});
    vTaskDelay(pdMS_TO_TICKS(kFinalFlashMs));
    strip_->SetAllColor({0, 0, 0});
}
