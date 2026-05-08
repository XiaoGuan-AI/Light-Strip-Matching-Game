#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>

// ============================================
// AI 元器件分拣箱 (Component Sorter) 配置
// 功能: WiFi + HTTP/WebSocket服务器 + LED灯带 + MAX98357A I2S功放
// ============================================

// 按钮 (用于进入 WiFi 配网模式)
#define BOOT_BUTTON_GPIO    GPIO_NUM_0

// LED 状态指示 (可选)
#define BUILTIN_LED_GPIO    GPIO_NUM_21

// ============================================
// 本项目不使用以下功能 (已禁用):
// - 显示屏 (LCD/OLED)
// - 热敏打印机
// - 语音音频 (麦克风/功放扬声器)
// ============================================

#endif // _BOARD_CONFIG_H_
