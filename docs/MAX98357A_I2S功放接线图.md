# XIAO ESP32-S3 Sense + MAX98357A 完整接线图

主控板：**Seeed Studio XIAO ESP32-S3 Sense**。

本接线按 Seeed 官方 XIAO ESP32-S3 Sense 引脚表重新整理：XIAO 外侧排针使用 `D0~D10` 标号，固件里实际使用的是对应的 ESP32-S3 `GPIO` 编号。

本项目当前不使用 Sense 扩展板上的摄像头、麦克风和 SD 卡功能。为避免占用摄像头/麦克风/UART 调试脚，MAX98357A 使用 `D8/D9/D10` 这组 SPI 排针做 I2S；因此**不要同时使用板载 SD 卡**。

## 1. 最终引脚定义

| 功能 | XIAO 排针标号 | ESP32-S3 GPIO | 接到哪里 | 说明 |
|---|---|---:|---|---|
| WS2811 DIN | D0 | GPIO1 | LED 灯串 DIN | 中间串 220Ω 电阻 |
| 红色按钮模块 OUT | D1 | GPIO2 | 红色按钮 OUT | VCC 接 3V3，GND 共地；按下 OUT=HIGH |
| 黄色按钮模块 OUT | D3 | GPIO4 | 黄色按钮 OUT | VCC 接 3V3，GND 共地；按下 OUT=HIGH |
| 蓝色按钮模块 OUT | D4 | GPIO5 | 蓝色按钮 OUT | VCC 接 3V3，GND 共地；按下 OUT=HIGH |
| 绿色按钮模块 OUT | D5 | GPIO6 | 绿色按钮 OUT | VCC 接 3V3，GND 共地；按下 OUT=HIGH |
| MAX98357A BCLK | D8 | GPIO7 | MAX98357A BCLK | I2S 位时钟，占用 SPI/SD SCK |
| MAX98357A LRC/WS | D9 | GPIO8 | MAX98357A LRC/WS/LRCLK | I2S 声道时钟，占用 SPI/SD MISO |
| MAX98357A DIN | D10 | GPIO9 | MAX98357A DIN | I2S 音频数据，占用 SPI/SD MOSI |
| 板载用户 LED | - | GPIO21 | XIAO 板载 LED | 固件保留，不外接 |

保留/不建议占用：

| XIAO 排针/资源 | GPIO | 原因 |
|---|---:|---|
| D2 | GPIO3 | 官方文档中与启动/JTAG/SD 相关，当前留空 |
| D6 / TX | GPIO43 | UART TX，留给串口调试 |
| D7 / RX | GPIO44 | UART RX，留给串口调试 |
| D11 / D12 | GPIO42 / GPIO41 | Sense 麦克风默认使用，除非切断扩展板跳线否则不占用 |
| 摄像头相关 GPIO | GPIO10~18、38~40、47、48 | Sense 摄像头使用，不占用 |

## 2. 元器件清单

| 元器件 | 规格/型号 | 数量 | 用途 |
|---|---|---:|---|
| XIAO ESP32-S3 Sense | Seeed Studio XIAO ESP32-S3 Sense | 1 | 主控 |
| WS2811 LED 灯串 | 5V，灯珠数量按实物为准，DIN 输入 | 1 | 灯光显示 |
| MAX98357A 模块 | I2S 数字功放 | 1 | 驱动喇叭 |
| 两线喇叭 | 4Ω~8Ω，建议 3W 以内 | 1 | 声音输出 |
| 220Ω 电阻 | 1/4W | 1 | 串在 LED 数据线上 |
| 常开自复位按钮 | 普通按键 | 4 | 红/黄/蓝/绿游戏按钮 |
| 5V 电源 | 建议 5V/3A~5A | 1 | 给 XIAO、LED、MAX98357A 供电 |
| 杜邦线/面包板 | 若干 | - | 接线 |

不需要：NPN 三极管、1kΩ 基极电阻、10kΩ 下拉电阻、喇叭串联电阻。

## 3. 完整接线图

```text
                         5V 电源
                    +5V ──┬──────────────────────► XIAO 5V/VUSB
                           ├──────────────────────► WS2811 5V 线/端子
                           └──────────────────────► MAX98357A VIN / VCC

                    GND ───┬──────────────────────► XIAO GND
                           ├──────────────────────► WS2811 GND 线/端子
                           └──────────────────────► MAX98357A GND

 XIAO ESP32-S3 Sense
 ┌──────────────────────────────────────────────────────────────┐
 │                                                              │
 │ D0  / GPIO1 ──[220Ω]────────────────────────► WS2811 DIN 线/端子
 │                                                              │
 │ D8  / GPIO7 ────────────────────────────────► MAX98357A BCLK │
 │ D9  / GPIO8 ────────────────────────────────► MAX98357A LRC / WS / LRCLK
 │ D10 / GPIO9 ────────────────────────────────► MAX98357A DIN  │
 │                                                              │
 │ 3V3 ─────────► 四个按钮模块 VCC；GND ─────────► 四个按钮模块 GND │
 │ D1 / GPIO2 ◄──────────────────────── 红色按钮模块 OUT        │
 │ D3 / GPIO4 ◄──────────────────────── 黄色按钮模块 OUT        │
 │ D4 / GPIO5 ◄──────────────────────── 蓝色按钮模块 OUT        │
 │ D5 / GPIO6 ◄──────────────────────── 绿色按钮模块 OUT        │
 └──────────────────────────────────────────────────────────────┘

 MAX98357A 输出端
 ┌────────────────────────┐
 │ SPK+ / OUT+ ───────────► 喇叭正极(+)
 │ SPK- / OUT- ───────────► 喇叭负极(-)
 └────────────────────────┘
```

## 4. 分模块接线表

### 4.1 WS2811 LED 灯串

WS2811 只有 3 根线/端子，按标识接，不按颜色猜：

| WS2811 标识 | 接到哪里 | 说明 |
|---|---|---|
| `5V` | 外部 5V 电源正极（推荐）；少量灯测试才可临时接 XIAO `5V/VUSB` | 不要接 XIAO `3V3`；整条灯带不要从 XIAO `VUSB` 取电 |
| `DIN` | XIAO D0 / GPIO1 | 中间串联 220Ω 电阻；必须接输入端 DIN，不是 DO |
| `GND` | 外部电源 GND / XIAO GND | 必须与 XIAO 和 MAX98357A 共地 |


| 从哪里 | 接到哪里 | 中间器件 | 说明 |
|---|---|---|---|
| XIAO D0 / GPIO1 | WS2811 DIN 线/端子 | 串联 220Ω 电阻 | 接灯串输入端 |
| 5V 电源正极 | WS2811 5V 线/端子 | 无 | LED 主供电 |
| 5V 电源 GND | WS2811 GND 线/端子 | 无 | 必须与 XIAO 共地 |

### 4.2 MAX98357A 功放模块

| MAX98357A 引脚 | 接到哪里 | 说明 |
|---|---|---|
| VIN / VCC | 外部 5V 电源正极，或 XIAO `5V/VUSB` | 推荐 5V；和 XIAO、WS2811 必须共地；不推荐接 XIAO `3V3` 做功放主电源 |
| GND | 公共 GND | 必须与 XIAO、LED 共地 |
| BCLK | XIAO D8 / GPIO7 | I2S 位时钟 |
| LRC / LRCLK / WS | XIAO D9 / GPIO8 | I2S 左右声道时钟 |
| DIN | XIAO D10 / GPIO9 | I2S 音频数据 |
| SD / EN | 推荐接 VIN 或 3V3 | 拉高启用功放；不要接 GND，接 GND 会关断 |
| GAIN | 先悬空 | 默认增益约 9dB；如果声音太小/太大再按模块说明调整 |
| SPK+ / OUT+ | 喇叭正极 `+` | 功放输出，不能接 XIAO GPIO |
| SPK- / OUT- | 喇叭负极 `-` | 功放输出，不能接 GND |

本项目固件左右声道输出相同，所以 `SD / EN` 接高电平后即使模块选择左声道输出，也不影响提示音。

### 4.3 四个按钮

按钮使用三线 `VCC/OUT/GND` 模块，当前固件按“按下 OUT 输出高电平”处理。

| 按钮模块 | XIAO 排针 | ESP32-S3 GPIO | 接法 |
|---|---|---:|---|
| 红色按钮 | D1 | GPIO2 | `OUT` 接 GPIO2，`VCC` 接 3V3，`GND` 共地 |
| 黄色按钮 | D3 | GPIO4 | `OUT` 接 GPIO4，`VCC` 接 3V3，`GND` 共地 |
| 蓝色按钮 | D4 | GPIO5 | `OUT` 接 GPIO5，`VCC` 接 3V3，`GND` 共地 |
| 绿色按钮 | D5 | GPIO6 | `OUT` 接 GPIO6，`VCC` 接 3V3，`GND` 共地 |

## 5. 上电前检查

```text
□ XIAO D0/GPIO1 到 LED DIN 之间已经串联 220Ω 电阻
□ WS2811 5V 接外部 +5V 主电源，不接 XIAO 3V3；整条灯带不要从 XIAO VUSB 取电；GND 接公共 GND，DIN 接 XIAO D0/GPIO1（中间串 220Ω）
□ MAX98357A VIN/VCC 接外部 +5V 或 XIAO 5V/VUSB，不推荐接 XIAO 3V3
□ MAX98357A GND 接公共 GND
□ MAX98357A BCLK 接 XIAO D8/GPIO7
□ MAX98357A LRC/WS 接 XIAO D9/GPIO8
□ MAX98357A DIN 接 XIAO D10/GPIO9
□ 喇叭正极接 MAX98357A SPK+/OUT+
□ 喇叭负极接 MAX98357A SPK-/OUT-
□ MAX98357A GAIN 先悬空
□ 如果 MAX98357A 有 SD/EN 引脚，已接到 VIN 或 3V3 高电平启用
□ XIAO、5V 电源、LED 灯串、MAX98357A 全部共地
□ 四个按钮模块的 VCC 接 XIAO 3V3，GND 接公共 GND；按下时 OUT 分别输出 3.3V 到 GPIO2/GPIO4/GPIO5/GPIO6
□ 不使用/不插入板载 SD 卡，避免与 D8/D9/D10 冲突
```

## 6. 重要注意事项

- XIAO ESP32-S3 Sense 的外侧排针标的是 `D0~D10`，固件里必须写对应 `GPIO` 编号。
- 本项目将 WS2811 从原来的 GPIO18 改到 `D0/GPIO1`，因为 GPIO18 是 Sense 摄像头相关引脚。
- MAX98357A 的 `SPK- / OUT-` 不是 GND，不能把喇叭负极接到系统 GND。
- 喇叭两根线只接 MAX98357A 的输出端，不直接接 XIAO。
- 如果要使用 Sense 的 SD 卡，请不要使用 D8/D9/D10 接 MAX98357A；需要重新分配 I2S 引脚。
- 如果要使用 Sense 的摄像头，请继续避开 GPIO10~18、GPIO38~40、GPIO47、GPIO48。
