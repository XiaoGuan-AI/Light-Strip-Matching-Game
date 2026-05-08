#include "iot/thing.h"
#include "board.h"
#include "audio_codec.h"

#include <driver/gpio.h>
#include <esp_log.h>

#define TAG "Lamp"

namespace iot {

// 这里仅定义 Lamp 的属性和方法，不包含具体的实现
class Lamp : public Thing {
private:
#ifdef CONFIG_IDF_TARGET_ESP32
    gpio_num_t gpio_num_ = GPIO_NUM_12;
#else
    // 注意：GPIO18 在 ESP32-S3-AI名片 PCB 上用于电源开关控制电路
    // 不能使用 GPIO18，否则会绕过硬件开关直接导通电源！
    // 改用其他空闲 GPIO
    gpio_num_t gpio_num_ = GPIO_NUM_38;  // 使用 GPIO38 代替
#endif
    bool power_ = false;

    void InitializeGpio() {
        gpio_config_t config = {
            .pin_bit_mask = (1ULL << gpio_num_),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_ERROR_CHECK(gpio_config(&config));
        gpio_set_level(gpio_num_, 0);
    }

public:
    Lamp() : Thing("Lamp", "一个测试用的灯"), power_(false) {
        InitializeGpio();

        // 定义设备的属性
        properties_.AddBooleanProperty("power", "灯是否打开", [this]() -> bool {
            return power_;
        });

        // 定义设备可以被远程执行的指令
        methods_.AddMethod("TurnOn", "打开灯", ParameterList(), [this](const ParameterList& parameters) {
            power_ = true;
            gpio_set_level(gpio_num_, 1);
        });

        methods_.AddMethod("TurnOff", "关闭灯", ParameterList(), [this](const ParameterList& parameters) {
            power_ = false;
            gpio_set_level(gpio_num_, 0);
        });
    }
};

} // namespace iot

DECLARE_THING(Lamp);
