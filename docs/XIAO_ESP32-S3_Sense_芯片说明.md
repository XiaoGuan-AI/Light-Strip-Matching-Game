# XIAO ESP32-S3 Sense 芯片/开发板说明

本文档整理本项目使用的 **Seeed Studio XIAO ESP32-S3 Sense** 的主要硬件规格、引脚定义、板载资源和使用注意事项。

> 严格来说，XIAO ESP32-S3 Sense 不是单颗“芯片”，而是一块基于 ESP32-S3R8 的开发板，并带有 Sense 扩展板（摄像头、数字麦克风、microSD 卡槽）。

## 1. 官方资料来源

- Seeed 官方入门文档：<https://wiki.seeedstudio.com/cn/xiao_esp32s3_getting_started/>
- Seeed 官方引脚复用文档：<https://wiki.seeedstudio.com/cn/xiao_esp32s3_pin_multiplexing/>
- Seeed Sense 麦克风文档：<https://wiki.seeedstudio.com/xiao_esp32s3_sense_mic/>
- Seeed Sense 文件系统 / microSD 文档：<https://wiki.seeedstudio.com/xiao_esp32s3_sense_filesystem/>
- Seeed Sense 摄像头文档：<https://wiki.seeedstudio.com/xiao_esp32s3_camera_usage/>

## 2. 核心规格

| 项目 | XIAO ESP32-S3 Sense 说明 |
|---|---|
| 主控芯片 | ESP32-S3R8 |
| CPU | Xtensa LX7 双核 32-bit，最高 240MHz |
| 无线 | 2.4GHz Wi-Fi，Bluetooth Low Energy 5.0 / Bluetooth Mesh |
| Flash | 8MB Flash |
| PSRAM | 8MB PSRAM |
| 板载传感器/模块 | 摄像头、数字麦克风、microSD 卡槽（在 Sense 扩展板上） |
| 摄像头 | 早期 OV2640；后续批次常见 OV3660；官方示例对 OV2640/OV3660/OV5640 兼容 |
| microSD | 官方文档建议最大 32GB，FAT32 格式 |
| USB | USB-C，支持供电、下载、调试 |
| 常规外侧排针 | D0~D10，共 11 个主要 GPIO 引出 |
| 扩展板额外 GPIO | D11/D12，对应 GPIO42/GPIO41，默认给麦克风使用 |
| 板载按钮 | BOOT 按钮、RESET 按钮 |
| 板载 LED | 用户 LED、充电指示 LED |
| 尺寸 | XIAO 本体约 21mm × 17.8mm；Sense 带扩展板约 21mm × 17.8mm × 15mm |

## 3. 供电说明

| 引脚/接口 | 说明 |
|---|---|
| USB-C | 最常用供电和烧录方式 |
| 5V / VBUS / VUSB | 来自 USB 5V；也可作为外部 5V 输入，但外部供电建议加入防反灌二极管；板子丝印/资料可能写作 `5V`、`VBUS` 或 `VUSB` |
| 3V3 | 板载稳压输出，官方文档说明可提供约 700mA |
| GND | 电源地、信号地，所有外设必须共地 |
| 电池焊盘/接口 | XIAO ESP32-S3 系列支持锂电池相关供电/充电能力，具体接线需参考官方原理图 |

### 3.1 本项目外设供电结论

| 外设/模块引脚 | 应该接到哪里 | 不建议接 | 原因 |
|---|---|---|---|
| WS2811 `5V` | 外部 5V 电源正极（推荐）；少量灯测试才可临时接 XIAO `5V / VUSB` | XIAO `3V3` | WS2811 是 5V 灯串；整条灯带不要从 XIAO `VUSB` 取电 |
| MAX98357A `VIN / VCC` | 外部 5V 电源正极，或 XIAO `5V / VUSB` | 不推荐 XIAO `3V3` | 3.3V 有些模块能工作，但喇叭输出功率小，还会增加 XIAO 3V3 稳压器负担 |
| WS2811 `GND` | XIAO `GND` / 外部电源 GND | - | 必须与控制信号共地 |
| MAX98357A `GND` | XIAO `GND` / 外部电源 GND | - | I2S 信号必须有共同参考地 |

结论：WS2811 的 `5V` 线优先接外部 5V 主电源，MAX98357A 的 `VIN / VCC` 接同一个外部 5V 或 XIAO `5V / VUSB`；`3V3` 只建议给小电流 3.3V 逻辑模块或传感器使用，不给灯串和功放主电源使用。

本项目使用外部 5V 电源时，建议：

```text
5V 电源 +  ──► WS2811 5V / MAX98357A VIN / XIAO 5V-VUSB（可选，注意防反灌）
5V 电源 GND ─► XIAO GND / WS2811 GND / MAX98357A GND
```

如果只用 USB-C 给 XIAO 供电，可以从 XIAO 的 `5V / VUSB` 给 MAX98357A 供电；但 WS2811 整条灯带电流较大，建议直接使用外部 `5V/3A~5A` 或更大电源给灯带供电。外部 5V 与 USB-C 不要随意同时反向供电；如果需要同时插 USB 调试和外部 5V，建议加入防反灌二极管或电源 ORing 保护。

所有模块必须共地。

## 4. 外侧排针定义 D0~D10

XIAO 板子丝印通常写 `D0~D10`，在 ESP-IDF 固件中要使用对应的 `GPIO_NUM_x`。

| XIAO 标号 | ESP32-S3 GPIO | 常见复用功能 | ADC | 本项目建议 |
|---|---:|---|---|---|
| D0 / A0 | GPIO1 | GPIO / ADC / PWM | 支持 | WS2811 DIN，串 220Ω |
| D1 / A1 | GPIO2 | GPIO / ADC / PWM | 支持 | 红色按钮 |
| D2 / A2 | GPIO3 | GPIO / ADC / PWM | 支持 | 保留，避免启动/调试风险 |
| D3 / A3 | GPIO4 | GPIO / ADC / PWM | 支持 | 黄色按钮 |
| D4 / A4 | GPIO5 | GPIO / ADC / PWM | 支持 | 蓝色按钮 |
| D5 / A5 | GPIO6 | GPIO / ADC / PWM | 支持 | 绿色按钮 |
| D6 / TX | GPIO43 | UART TX | 不建议作 ADC | 建议留给串口 TX |
| D7 / RX | GPIO44 | UART RX | 不建议作 ADC | 建议留给串口 RX |
| D8 / A8 / SCK | GPIO7 | SPI SCK / GPIO / PWM | 支持 | MAX98357A BCLK |
| D9 / A9 / MISO | GPIO8 | SPI MISO / GPIO / PWM | 支持 | MAX98357A LRC/WS |
| D10 / A10 / MOSI | GPIO9 | SPI MOSI / GPIO / PWM | 支持 | MAX98357A DIN |

> 注意：D8/D9/D10 在 Sense 扩展板上也和 microSD/SPI 相关。如果本项目用它们接 MAX98357A，就不要同时使用板载 microSD 卡。

## 5. Sense 扩展板资源

### 5.1 摄像头

Sense 扩展板带摄像头。官方说明中，早期为 OV2640，后续批次可能为 OV3660，相关示例也兼容 OV5640。

| 摄像头功能 | GPIO |
|---|---:|
| XMCLK | GPIO10 |
| DVP_Y8 | GPIO11 |
| DVP_Y7 | GPIO12 |
| DVP_PCLK | GPIO13 |
| DVP_Y6 | GPIO14 |
| DVP_Y2 | GPIO15 |
| DVP_Y5 | GPIO16 |
| DVP_Y3 | GPIO17 |
| DVP_Y4 | GPIO18 |
| DVP_VSYNC | GPIO38 |
| Camera SCL | GPIO39 |
| Camera SDA | GPIO40 |
| DVP_HREF | GPIO47 |
| DVP_Y9 | GPIO48 |

本项目目前不使用摄像头，因此应避免把外设接到这些 GPIO 上。尤其是旧版本里曾用过 `GPIO18` 接 LED，迁移到 XIAO ESP32-S3 Sense 后已改为 `D0/GPIO1`。

### 5.2 数字麦克风

Sense 扩展板上的 PDM 数字麦克风默认占用：

| 麦克风功能 | GPIO | 说明 |
|---|---:|---|
| PDM Microphone DATA | GPIO41 | 也可作为扩展板 D12，但默认给麦克风 |
| PDM Microphone CLK | GPIO42 | 也可作为扩展板 D11，但默认给麦克风 |

如需把 GPIO41/GPIO42 当普通 GPIO 使用，需要按官方文档处理扩展板焊盘/跳线；这样会影响麦克风功能。GPIO41/GPIO42 不支持 ADC。

### 5.3 microSD 卡槽

Sense 扩展板上的 microSD 卡槽使用 SPI。官方资料中相关占用如下：

| microSD SPI 功能 | GPIO / XIAO 标号 |
|---|---|
| CS | GPIO21 |
| SCK | D8 / GPIO7 |
| MISO | D9 / GPIO8 |
| MOSI | D10 / GPIO9 |

本项目为了接 MAX98357A，把 D8/D9/D10 用作 I2S，因此不建议同时使用板载 microSD 卡。若后续必须使用 SD 卡，需要重新分配 MAX98357A 的 I2S 引脚，并确认与摄像头、麦克风、串口不冲突。

## 6. 板载按钮与 LED

| 资源 | GPIO/功能 | 说明 |
|---|---|---|
| BOOT 按钮 | GPIO0 | 启动/下载模式相关 |
| RESET 按钮 | EN/Reset | 复位开发板 |
| 用户 LED | GPIO21 | XIAO 板载用户 LED；本项目不外接使用 |
| 充电 LED | 硬件充电状态指示 | 通常由充电电路控制，不作为普通 GPIO 使用 |

## 7. 本项目最终占用引脚

| 功能 | XIAO 标号 | GPIO | 接线 |
|---|---|---:|---|
| WS2811 DIN | D0 | GPIO1 | 串 220Ω 后接 LED DIN |
| 红色按钮模块 OUT | D1 | GPIO2 | VCC 接 3V3，GND 共地；按下 OUT=HIGH |
| 黄色按钮模块 OUT | D3 | GPIO4 | VCC 接 3V3，GND 共地；按下 OUT=HIGH |
| 蓝色按钮模块 OUT | D4 | GPIO5 | VCC 接 3V3，GND 共地；按下 OUT=HIGH |
| 绿色按钮模块 OUT | D5 | GPIO6 | VCC 接 3V3，GND 共地；按下 OUT=HIGH |
| MAX98357A BCLK | D8 | GPIO7 | 接 MAX98357A BCLK |
| MAX98357A LRC/WS | D9 | GPIO8 | 接 MAX98357A LRC/WS/LRCLK |
| MAX98357A DIN | D10 | GPIO9 | 接 MAX98357A DIN |

对应固件文件：`main/iot/component_config.h`

```c
#define LED_STRIP_PIN            GPIO_NUM_1
#define SPEAKER_I2S_BCLK         GPIO_NUM_7
#define SPEAKER_I2S_LRCLK        GPIO_NUM_8
#define SPEAKER_I2S_DOUT         GPIO_NUM_9
#define BTN_RED_GPIO             GPIO_NUM_2
#define BTN_YELLOW_GPIO          GPIO_NUM_4
#define BTN_BLUE_GPIO            GPIO_NUM_5
#define BTN_GREEN_GPIO           GPIO_NUM_6
#define BTN_RED_ACTIVE_LEVEL     1  // 三线按钮模块：按下 OUT=HIGH
#define BTN_YELLOW_ACTIVE_LEVEL  1
#define BTN_BLUE_ACTIVE_LEVEL    1
#define BTN_GREEN_ACTIVE_LEVEL   1
```


### 7.1 WS2811 三线接法

WS2811 灯串只有 `5V`、`DIN`、`GND` 三根线/端子，按端子标识接线：

| WS2811 标识 | 接到哪里 | 说明 |
|---|---|---|
| `5V` | 外部 5V 电源正极（推荐）；少量灯测试才可临时接 XIAO `5V / VUSB` | 给灯串供电；不要接 XIAO `3V3`，整条灯带不要从 XIAO `VUSB` 取电 |
| `DIN` | XIAO D0 / GPIO1 | 中间串联 220Ω 电阻；必须接 DIN 输入端 |
| `GND` | 外部电源 GND / XIAO GND | 必须共地 |

### 7.2 MAX98357A 七针接法

你手上的 MAX98357A 若只有 `LRC`、`BCLK`、`DIN`、`GAIN`、`SD`、`GND`、`Vin` 这 7 个排针，按下表接：

| MAX98357A 引脚 | 接到哪里 | 说明 |
|---|---|---|
| `LRC` | XIAO `D9 / GPIO8` | I2S 左右声道时钟，也可能标成 `LRCLK`、`WS` |
| `BCLK` | XIAO `D8 / GPIO7` | I2S 位时钟 |
| `DIN` | XIAO `D10 / GPIO9` | I2S 音频数据输入；不要接 WS2811 的 `DIN` |
| `GAIN` | 先悬空 | 默认增益约 9dB；如果声音太小/太大再按模块说明调整 |
| `SD` | 推荐接 `Vin` 或 XIAO `3V3` | 拉高启用功放；不要接 GND，接 GND 会关断 |
| `GND` | 公共 GND | 必须与 XIAO、WS2811、电源 GND 共地 |
| `Vin` | 外部 5V 电源正极，或 XIAO `5V/VUSB` | 推荐 5V；和 XIAO、WS2811 必须共地；不推荐接 XIAO `3V3` 做功放主电源 |

本项目固件左右声道输出相同，所以 `SD` 接高电平后即使模块选择左声道输出，也不影响提示音。

喇叭不接这 7 个排针。喇叭两根线接 MAX98357A 模块上的输出端子：喇叭正极接 `SPK+ / OUT+`，喇叭负极接 `SPK- / OUT-`；`SPK- / OUT-` 不是系统 GND。

## 8. 推荐不要占用的引脚

| GPIO / XIAO 标号 | 原因 |
|---|---|
| GPIO0 | BOOT 启动模式相关 |
| D6/GPIO43、D7/GPIO44 | UART 调试脚，建议保留 |
| GPIO10~18、GPIO38~40、GPIO47、GPIO48 | Sense 摄像头相关 |
| GPIO41/GPIO42 | Sense 麦克风默认占用 |
| GPIO7/GPIO8/GPIO9 | 本项目已给 MAX98357A 使用；如果用 SD 卡会冲突 |
| GPIO21 | 板载用户 LED / microSD CS 相关，不建议再外接 |

## 9. 使用注意事项

1. XIAO 外侧排针丝印是 `D0~D10`，ESP-IDF 代码要写 `GPIO_NUM_x`。
2. Sense 版本的摄像头、麦克风、SD 卡会占用额外 GPIO，接线前要先确认是否启用这些功能。
3. MAX98357A 的 `SPK- / OUT-` 不是 GND，喇叭负极不能接系统地。
4. WS2811 `5V` 必须接外部 5V 主电源或少量灯测试用 XIAO `5V/VUSB`，不要接 `3V3`；整条灯带不要从 XIAO `VUSB` 取电；数据线建议串 220Ω 电阻。
5. MAX98357A `VIN/VCC` 推荐接外部 5V 或 XIAO `5V/VUSB`，不推荐接 `3V3` 做功放主电源。
6. WS2811 灯珠数量越多电流越大，建议使用外部 5V/3A~5A 或更大电源，并与 XIAO 共地。
7. 如果要启用摄像头或 SD 卡，需要重新评估本项目的引脚分配。
