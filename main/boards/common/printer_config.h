/**
 * @file printer_config.h
 * @brief 通用打印机配置 - 根据板子类型选择正确的引脚
 */

#ifndef _PRINTER_CONFIG_H_
#define _PRINTER_CONFIG_H_

#include <driver/gpio.h>
#include <driver/uart.h>

// ============================================
// 根据板子类型选择打印机引脚配置
// ============================================

#if defined(CONFIG_BOARD_TYPE_AI_PRINTER_LCD) || defined(CONFIG_BOARD_TYPE_AI_PRINTER_VOICE)
    // AI-Printer-LCD / AI-Printer-Voice 板子
    // 打印机引脚: GPIO42(TX)->打印机RXD, GPIO41(RX)<-打印机TXD
    #define PRINTER_UART_NUM    UART_NUM_1
    #define PRINTER_TX_PIN      GPIO_NUM_42
    #define PRINTER_RX_PIN      GPIO_NUM_41
#else
    // AI-Printer 原始板子 (面包板测试等)
    #define PRINTER_UART_NUM    UART_NUM_1
    #define PRINTER_TX_PIN      GPIO_NUM_1
    #define PRINTER_RX_PIN      GPIO_NUM_2
#endif

// 打印机通用配置
#define PRINTER_DPI         203

// 打印机型号配置 (由 Kconfig 选择)
#if defined(CONFIG_PRINTER_MODEL_EM20L)
    #define PRINTER_MODEL_NAME      "EM20L"
    #define PRINTER_BAUD_RATE       115200
    #define PRINTER_MAX_WIDTH_PX    386
    #define PRINTER_HAS_CUTTER      0
    #define PRINTER_LABEL_WIDTH_MM  54
    #define PRINTER_LABEL_HEIGHT_MM 30
    #define PRINTER_LABEL_WIDTH_PX  384
    #define PRINTER_LABEL_HEIGHT_PX 240
#else
    #define PRINTER_MODEL_NAME      "EM30L"
    #define PRINTER_BAUD_RATE       115200
    #define PRINTER_MAX_WIDTH_PX    576
    #define PRINTER_HAS_CUTTER      1
    #define PRINTER_LABEL_WIDTH_MM  75
    #define PRINTER_LABEL_HEIGHT_MM 45
    #define PRINTER_LABEL_WIDTH_PX  600
    #define PRINTER_LABEL_HEIGHT_PX 360
#endif

// HTTP 下载配置
#define PRINTER_HTTP_TIMEOUT_MS      30000
#define PRINTER_HTTP_BUFFER_SIZE     1024
#define PRINTER_MAX_BITMAP_SIZE      65536
#define PRINTER_MAX_RETRIES          3

#endif // _PRINTER_CONFIG_H_

