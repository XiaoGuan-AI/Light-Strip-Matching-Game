/**
 * @file component_sorter_board.cc
 * @brief Component Sorter 板级实现
 */
#include "wifi_board.h"

#include "application.h"
#include "font_awesome_symbols.h"
#include "settings.h"

#include <esp_log.h>

static const char *TAG = "ComponentSorterBoard";

/**
 * @brief Component Sorter 开发板类
 * 
 * 功能：
 * - WiFi 网络连接
 * - HTTP/WebSocket 服务器
 * - LED 指示灯
 * - MAX98357A I2S 功放提示音
 * - 不包含显示屏、音频、打印功能
 */
class ComponentSorterBoard : public WifiBoard {
public:
    // 公有构造函数
    ComponentSorterBoard();

    // 禁用拷贝构造函数和赋值运算符
    ComponentSorterBoard(const ComponentSorterBoard&) = delete;
    ComponentSorterBoard& operator=(const ComponentSorterBoard&) = delete;

    virtual ~ComponentSorterBoard() = default;

    virtual std::string GetBoardType() override;
    virtual AudioCodec* GetAudioCodec() override;
    virtual void SetPowerSaveMode(bool enabled) override;
    virtual void ResetWifiConfiguration() override;
};

// 在类定义外部声明板子
DECLARE_BOARD(ComponentSorterBoard)

// 构造函数实现
ComponentSorterBoard::ComponentSorterBoard() {
    ESP_LOGI(TAG, "Component Sorter Board initialized");
}

std::string ComponentSorterBoard::GetBoardType() {
    return "component-sorter";
}

AudioCodec* ComponentSorterBoard::GetAudioCodec() {
    // Component Sorter 板没有音频编解码器
    return nullptr;
}

void ComponentSorterBoard::SetPowerSaveMode(bool enabled) {
    // Component Sorter 板暂未实现省电模式
    (void)enabled;
}

void ComponentSorterBoard::ResetWifiConfiguration() {
    // 重置 WiFi 配置，进入配网模式
    Settings settings("wifi", true);
    settings.SetInt("force_ap", 1);
    esp_restart();
}
