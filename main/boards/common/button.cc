#include "button.h"

#include <esp_log.h>
#include <esp_timer.h>

static const char* TAG = "Button";

// Software debounce: minimum time (ms) between triggering the same event
// Increased from 100 to 500 to filter Dupont wire vibration noise
static const int64_t DEBOUNCE_INTERVAL_MS = 500;
#if CONFIG_SOC_ADC_SUPPORTED
Button::Button(const button_adc_config_t& adc_cfg) {
    button_config_t button_config = {
        .type = BUTTON_TYPE_ADC,
        .long_press_time = 700,
        .short_press_time = 200,
        .adc_button_config = adc_cfg
    };
    button_handle_ = iot_button_create(&button_config);
    if (button_handle_ == NULL) {
        ESP_LOGE(TAG, "Failed to create button handle");
        return;
    }
}
#endif

Button::Button(gpio_num_t gpio_num, bool active_high) : gpio_num_(gpio_num), active_high_(active_high) {
    if (gpio_num == GPIO_NUM_NC) {
        return;
    }
    button_config_t button_config = {
        .type = BUTTON_TYPE_GPIO,
        .long_press_time = 700,
        .short_press_time = 200,
        .gpio_button_config = {
            .gpio_num = gpio_num,
            .active_level = static_cast<uint8_t>(active_high ? 1 : 0)
        }
    };
    button_handle_ = iot_button_create(&button_config);
    if (button_handle_ == NULL) {
        ESP_LOGE(TAG, "Failed to create button handle");
        return;
    }
}

Button::~Button() {
    if (button_handle_ != NULL) {
        iot_button_delete(button_handle_);
    }
}

void Button::OnPressDown(std::function<void()> callback) {
    if (button_handle_ == nullptr) {
        return;
    }
    on_press_down_ = callback;
    iot_button_register_cb(button_handle_, BUTTON_PRESS_DOWN, [](void* handle, void* usr_data) {
        Button* button = static_cast<Button*>(usr_data);
        int64_t now = esp_timer_get_time();
        // Software debounce: ignore if triggered too soon after last event
        if ((now - button->last_press_down_time_) < DEBOUNCE_INTERVAL_MS * 1000) {
            return;
        }
        button->last_press_down_time_ = now;
        if (button->on_press_down_) {
            button->on_press_down_();
        }
    }, this);
}

void Button::OnPressUp(std::function<void()> callback) {
    if (button_handle_ == nullptr) {
        return;
    }
    on_press_up_ = callback;
    iot_button_register_cb(button_handle_, BUTTON_PRESS_UP, [](void* handle, void* usr_data) {
        Button* button = static_cast<Button*>(usr_data);
        int64_t now = esp_timer_get_time();
        // Software debounce: ignore if triggered too soon after last event
        if ((now - button->last_press_up_time_) < DEBOUNCE_INTERVAL_MS * 1000) {
            return;
        }
        button->last_press_up_time_ = now;
        if (button->on_press_up_) {
            button->on_press_up_();
        }
    }, this);
}

void Button::OnLongPress(std::function<void()> callback) {
    if (button_handle_ == nullptr) {
        return;
    }
    on_long_press_ = callback;
    iot_button_register_cb(button_handle_, BUTTON_LONG_PRESS_START, [](void* handle, void* usr_data) {
        Button* button = static_cast<Button*>(usr_data);
        int64_t now = esp_timer_get_time();
        // Software debounce: ignore if triggered too soon after last event
        if ((now - button->last_long_press_time_) < DEBOUNCE_INTERVAL_MS * 1000) {
            return;
        }
        button->last_long_press_time_ = now;
        if (button->on_long_press_) {
            button->on_long_press_();
        }
    }, this);
}

void Button::OnClick(std::function<void()> callback) {
    if (button_handle_ == nullptr) {
        return;
    }
    on_click_ = callback;
    iot_button_register_cb(button_handle_, BUTTON_SINGLE_CLICK, [](void* handle, void* usr_data) {
        Button* button = static_cast<Button*>(usr_data);
        int64_t now = esp_timer_get_time();
        // Software debounce: ignore if triggered too soon after last event
        if ((now - button->last_click_time_) < DEBOUNCE_INTERVAL_MS * 1000) {
            return;
        }
        // GPIO level verification: reject false clicks from wire noise
        // A valid click means the button has been released (back to inactive level)
        if (button->gpio_num_ != GPIO_NUM_NC) {
            int level = gpio_get_level(button->gpio_num_);
            int inactive_level = button->active_high_ ? 0 : 1;
            if (level != inactive_level) {
                ESP_LOGW(TAG, "Click rejected: GPIO%d level=%d (expected %d, noise?)",
                         button->gpio_num_, level, inactive_level);
                return;
            }
        }
        button->last_click_time_ = now;
        if (button->on_click_) {
            button->on_click_();
        }
    }, this);
}

void Button::OnDoubleClick(std::function<void()> callback) {
    if (button_handle_ == nullptr) {
        return;
    }
    on_double_click_ = callback;
    iot_button_register_cb(button_handle_, BUTTON_DOUBLE_CLICK, [](void* handle, void* usr_data) {
        Button* button = static_cast<Button*>(usr_data);
        int64_t now = esp_timer_get_time();
        // Software debounce: ignore if triggered too soon after last event
        if ((now - button->last_double_click_time_) < DEBOUNCE_INTERVAL_MS * 1000) {
            return;
        }
        button->last_double_click_time_ = now;
        if (button->on_double_click_) {
            button->on_double_click_();
        }
    }, this);
}
