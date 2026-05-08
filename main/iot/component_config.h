/**
 * @file component_config.h
 * @brief 元器件分拣系统配置文件
 */
#ifndef _COMPONENT_CONFIG_H_
#define _COMPONENT_CONFIG_H_

#include <driver/gpio.h>
#include <driver/i2s_types.h>

// ============================================================================
// LED 配置
// ============================================================================
#define LED_STRIP_PIN            GPIO_NUM_1        // XIAO D0 / GPIO1: WS2811 DIN 数据引脚
#define LED_STRIP_LENGTH        100               // LED数量；一整条灯带按实际灯珠数修改
#define LED_BRIGHTNESS          150               // 默认亮度 0-255

// ============================================================================
// 存储配置
// ============================================================================
#define COMPONENTS_FILE_PATH     "/spiffs/components.json"  // 元器件数据文件路径

// ============================================================================
// MAX98357A I2S 功放配置
// ============================================================================
#define SPEAKER_I2S_PORT        I2S_NUM_1         // 避免占用常见语音音频 I2S_NUM_0
#define SPEAKER_I2S_BCLK        GPIO_NUM_7        // XIAO D8 / GPIO7: MAX98357A BCLK
#define SPEAKER_I2S_LRCLK       GPIO_NUM_8        // XIAO D9 / GPIO8: MAX98357A LRC/WS
#define SPEAKER_I2S_DOUT        GPIO_NUM_9        // XIAO D10 / GPIO9: MAX98357A DIN
#define SPEAKER_SAMPLE_RATE_HZ  16000             // 提示音采样率
#define SPEAKER_TONE_AMPLITUDE  6000              // 16-bit PCM 音量
#define SPEAKER_DEFAULT_FREQ_HZ 2000              // 默认提示音频率
#define BUZZER_PIN              SPEAKER_I2S_DOUT  // 兼容旧代码命名

// ============================================================================
// 网络配置
// ============================================================================
#define STATIC_IP               "192.168.3.238"
#define GATEWAY_IP              "192.168.3.1"
#define SUBNET_MASK             "255.255.255.0"

// ============================================================================
// 服务器配置
// ============================================================================
#define HTTP_SERVER_PORT        32323              // HTTP端口
#define WS_SERVER_PORT          32324              // WebSocket端口

// ============================================================================
// 数据库配置
// ============================================================================
#define MAX_COMPONENTS          500               // 最大支持元器件数量
#define NVS_NAMESPACE           "comp_db"         // NVS命名空间
#define NVS_COMPONENTS_KEY      "components"      // 元器件数据键
#define COMPONENT_DB_VERSION    4                 // 数据库版本号（更改后强制刷新数据）

// ============================================================================
// 溢出处理配置
// ============================================================================
#define OVERFLOW_BEEP_DELAY_MS  150              // 多次提示音间隔
#define OVERFLOW_LED_DELAY_MS   200              // LED闪烁间隔

// ============================================================================
// 游戏灯带配置 (灯带消消乐)
// ============================================================================
#define GAME_ROWS              1                 // 线性灯带：只有1行
#define GAME_COLS              LED_STRIP_LENGTH  // 一整条正常顺序：LED0 -> LED_STRIP_LENGTH-1
#define GAME_COLORS            4                 // 红/黄/蓝/绿 4种颜色

// ============================================================================
// 游戏按钮GPIO
// ============================================================================
#define BTN_RED_GPIO           GPIO_NUM_2        // XIAO D1 / GPIO2: 红色按钮
#define BTN_YELLOW_GPIO        GPIO_NUM_4        // XIAO D3 / GPIO4: 黄色按钮
#define BTN_BLUE_GPIO          GPIO_NUM_5        // XIAO D4 / GPIO5: 蓝色按钮
#define BTN_GREEN_GPIO         GPIO_NUM_6        // XIAO D5 / GPIO6: 绿色按钮
#define BTN_RED_ACTIVE_LEVEL   1                 // VCC/OUT/GND 按钮模块：按下 OUT=HIGH
#define BTN_YELLOW_ACTIVE_LEVEL 1                // VCC 接 3V3，GND 共地，OUT 接对应 GPIO
#define BTN_BLUE_ACTIVE_LEVEL  1
#define BTN_GREEN_ACTIVE_LEVEL 1

// ============================================================================
// 游戏速度配置
// ============================================================================
#define GAME_TICK_BASE_MS      800               // 基础tick间隔(ms)
#define GAME_TICK_MIN_MS       200               // 最快tick间隔(ms)
#define PROJECTILE_SPEED_MS    60                // 兼容旧版飞行动画；当前玩法为快速碰撞消除
#define SPAWN_INTERVAL_BASE    3                 // 基础：每N个tick生成一个敌人
#define LEVEL_UP_SCORE         100               // 每100分升一级

// ============================================================================
// LED效果配置 (保留兼容)
// ============================================================================
#define LED_BLINK_COUNT         3
#define LED_BLINK_ON_MS         150
#define LED_BLINK_OFF_MS        150
#define LED_HOLD_MS             60000
#define LED_BLINK_INTERVAL_MS   1000

#endif // _COMPONENT_CONFIG_H_
