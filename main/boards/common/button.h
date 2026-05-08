#ifndef BUTTON_H_
#define BUTTON_H_

#include <driver/gpio.h>
#include <iot_button.h>
#include <functional>

class Button {
public:
#if CONFIG_SOC_ADC_SUPPORTED
    Button(const button_adc_config_t& cfg);
#endif
    Button(gpio_num_t gpio_num, bool active_high = false);
    ~Button();

    void OnPressDown(std::function<void()> callback);
    void OnPressUp(std::function<void()> callback);
    void OnLongPress(std::function<void()> callback);
    void OnClick(std::function<void()> callback);
    void OnDoubleClick(std::function<void()> callback);
private:
    gpio_num_t gpio_num_ = GPIO_NUM_NC;
    bool active_high_ = false;
    button_handle_t button_handle_ = nullptr;

    // Software debounce timestamps (microseconds)
    int64_t last_press_down_time_ = 0;
    int64_t last_press_up_time_ = 0;
    int64_t last_long_press_time_ = 0;
    int64_t last_click_time_ = 0;
    int64_t last_double_click_time_ = 0;

    std::function<void()> on_press_down_;
    std::function<void()> on_press_up_;
    std::function<void()> on_long_press_;
    std::function<void()> on_click_;
    std::function<void()> on_double_click_;
};

#endif // BUTTON_H_
