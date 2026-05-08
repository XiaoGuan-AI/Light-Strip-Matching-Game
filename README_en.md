# Light-Strip-Matching-Game

English | [中文](./README.md)

Light-Strip-Matching-Game is a physical reaction and matching game for ESP32-S3. It treats an addressable light strip as a one-dimensional battlefield: colored light enemies move from the far end toward the origin, and the player fires matching colors with four physical buttons or a browser UI. The firmware combines LED animation, scoring, combos, levels, WiFi provisioning, a web dashboard, and I2S sound effects.

> Note: Some source files still use `component_sorter` names because this firmware evolved from an earlier component-sorting project. The active application path now starts `MatchGame` from `main/application.cc` and `main/iot/match_game.*`.

## Table of Contents

- [Features](#features)
- [Gameplay](#gameplay)
- [Hardware](#hardware)
- [Pinout](#pinout)
- [Software Layout](#software-layout)
- [Quick Start](#quick-start)
- [Web Control and API](#web-control-and-api)
- [Configuration](#configuration)
- [Troubleshooting](#troubleshooting)
- [License](#license)

## Features

- **Linear LED battlefield**: 100 LEDs by default, mapped from `LED0` to `LED99`.
- **Four-color physical controls**: red, yellow, blue, and green buttons fire matching colors.
- **Browser dashboard**: view the live strip state, score, combo, level, and high score; fire colors from a phone or desktop browser.
- **Realtime sync**: HTTP endpoints control the game, while WebSocket broadcasts live state updates.
- **Combos and levels**: fast consecutive hits increase score; every 100 points raises the level and speeds up the game.
- **I2S audio feedback**: MAX98357A output for hit, combo, level-up, and game-over sounds.
- **WiFi provisioning**: on first boot or when no saved network exists, the device creates a `LightStripGame-XXXX` access point and serves configuration at `http://192.168.4.1`.

## Gameplay

1. After boot, the strip shows an idle breathing animation.
2. Press any color button, or click Start in the web UI, to start a round.
3. Enemy lights spawn near the far end of the strip and move toward `LED0`.
4. Press the matching color to fire from `LED0`; the nearest enemy with that color is cleared.
5. A hit scores `10 x current combo`. Quick consecutive hits increase the combo.
6. Every 100 points increases the level, making movement and spawning faster.
7. The game ends when any enemy reaches `LED0`; the firmware keeps the high score for the current runtime.

Color indexes:

| Index | Color | Used by |
|------:|-------|---------|
| `0` | Red | Red physical button / red web button |
| `1` | Yellow | Yellow physical button / yellow web button |
| `2` | Blue | Blue physical button / blue web button |
| `3` | Green | Green physical button / green web button |

## Hardware

| Module | Recommended spec | Qty | Notes |
|--------|------------------|-----|-------|
| MCU board | XIAO ESP32-S3 / ESP32-S3 dev board | 1 | The current target is `esp32s3` |
| Light strip | 5V WS2811/WS2812-compatible addressable strip | 1 | Default is 100 LEDs; change config to match your strip |
| Data resistor | 220-470 Ohm | 1 | Put in series before LED `DIN` |
| Button modules | VCC/OUT/GND button modules or equivalent circuits | 4 | Default trigger level is active-high |
| I2S amplifier | MAX98357A | 1 | For game sound effects |
| Speaker | 4-8 Ohm, 1-3 W | 1 | Connect to MAX98357A `SPK+` / `SPK-` |
| Power supply | 5V with enough current for the strip | 1 | For 100 LEDs, use at least 5V/3A as a practical starting point |
| Wires/connectors | As needed | - | Keep MCU, amplifier, and strip grounds connected |

Do not power the whole light strip from the ESP32 `3V3` pin. If the strip uses a separate 5V supply, connect all grounds together.

## Pinout

Defaults are defined in `main/iot/component_config.h`.

| Function | XIAO pin | ESP32-S3 GPIO | Config symbol |
|----------|----------|---------------|---------------|
| LED data | D0 | GPIO1 | `LED_STRIP_PIN` |
| Red button | D1 | GPIO2 | `BTN_RED_GPIO` |
| Yellow button | D3 | GPIO4 | `BTN_YELLOW_GPIO` |
| Blue button | D4 | GPIO5 | `BTN_BLUE_GPIO` |
| Green button | D5 | GPIO6 | `BTN_GREEN_GPIO` |
| MAX98357A BCLK | D8 | GPIO7 | `SPEAKER_I2S_BCLK` |
| MAX98357A LRC/WS | D9 | GPIO8 | `SPEAKER_I2S_LRCLK` |
| MAX98357A DIN | D10 | GPIO9 | `SPEAKER_I2S_DOUT` |

Buttons are active-high by default (`BTN_*_ACTIVE_LEVEL = 1`). If your wiring is active-low, update the corresponding active-level definitions and external pull-up/pull-down circuit.

## Software Layout

| Path | Purpose |
|------|---------|
| `main/main.cc` | ESP-IDF `app_main`, event loop, NVS init, application start |
| `main/application.cc` | Application startup; initializes `MatchGame`, networking, and protocol layer |
| `main/iot/match_game.h` / `main/iot/match_game.cc` | Game state machine, button scanning, enemy spawning, hit logic, scoring, rendering |
| `main/iot/component_http_server.*` | Embedded web page and HTTP API |
| `main/iot/component_ws_server.*` | WebSocket server, default `/ws` |
| `main/led/circular_strip.*` | Addressable LED strip abstraction |
| `main/iot/buzzer_control.*` | MAX98357A I2S sound output |
| `components/esp-wifi-connect/` | Local WiFi provisioning component |
| `main/boards/component-sorter/` | Current ESP32-S3 board adapter; the directory name is historical |

## Quick Start

### 1. Install Requirements

- ESP-IDF `>= 5.3`
- Python 3.x and the ESP-IDF toolchain
- ESP32-S3 board, preferably a XIAO ESP32-S3 variant

### 2. Select the Target

```bash
idf.py set-target esp32s3
```

If you need to reconfigure the project:

```bash
idf.py menuconfig
```

In `Board Type`, select the board option that maps to `BOARD_TYPE_COMPONENT_SORTER`. The menu text may still contain legacy Component Sorter wording. If you are using the checked-in `sdkconfig`, this is usually already configured.

### 3. Build and Flash

```bash
idf.py build
idf.py -p COM3 flash monitor
```

Replace `COM3` with your real serial port. On Linux/macOS it is commonly `/dev/ttyUSB0` or `/dev/ttyACM0`.

### 4. Provision WiFi and Open the Game

1. On first boot, or when no WiFi credential is saved, the device creates a `LightStripGame-XXXX` access point.
2. Connect your phone or computer to that access point.
3. Open `http://192.168.4.1`, choose a 2.4 GHz WiFi network, and enter the password.
4. After the device connects to your network, read its IP address from the serial monitor.
5. Open `http://<device-ip>:32323` in a browser.

## Web Control and API

Default services:

| Service | URL |
|---------|-----|
| Web page | `http://<device-ip>:32323/` |
| HTTP API | `http://<device-ip>:32323/api/...` |
| WebSocket | `ws://<device-ip>:32324/ws` |

### HTTP API

| Method | Path | Body | Description |
|--------|------|------|-------------|
| `GET` | `/api` | - | List API endpoints |
| `GET` | `/api/status` | - | Return the current game state |
| `POST` | `/api/start` | - | Start or restart a round |
| `POST` | `/api/fire` | `{"color":0}` | Fire a color; `0..3` means red/yellow/blue/green |

Example `GET /api/status` fields:

```json
{
  "state": "playing",
  "score": 120,
  "level": 2,
  "combo": 3,
  "high_score": 240,
  "rows": 1,
  "cols": 100,
  "led_count": 100,
  "grid": [
    {"row": 0, "col": 72, "color": 1}
  ],
  "projectile": {"active": false, "row": 0, "col": 0, "color": 0},
  "explosions": []
}
```

### WebSocket

The web page connects to `ws://<device-ip>:32324/ws`. The device broadcasts JSON similar to `/api/status` whenever state changes. Use the HTTP `/api/start` and `/api/fire` endpoints for game control; WebSocket is mainly for live state display.

## Configuration

Edit `main/iot/component_config.h`, then rebuild and flash.

| Symbol | Default | Description |
|--------|---------|-------------|
| `LED_STRIP_PIN` | `GPIO_NUM_1` | LED data pin |
| `LED_STRIP_LENGTH` | `100` | LED count and game column count |
| `LED_BRIGHTNESS` | `150` | Brightness, `0..255` |
| `HTTP_SERVER_PORT` | `32323` | Web page and HTTP API port |
| `WS_SERVER_PORT` | `32324` | WebSocket port |
| `GAME_TICK_BASE_MS` | `800` | Base enemy movement timing |
| `GAME_TICK_MIN_MS` | `200` | Fastest timing limit |
| `PROJECTILE_SPEED_MS` | `60` | Shot animation speed |
| `LEVEL_UP_SCORE` | `100` | Points per level-up |
| `BTN_*_GPIO` | `GPIO2/4/5/6` | Color button GPIOs |
| `BTN_*_ACTIVE_LEVEL` | `1` | Button trigger level |

## Troubleshooting

### LEDs do not light

- Check strip direction: ESP32 data must go to `DIN`, not `DOUT`.
- Check 5V power and common ground between ESP32, strip, and power supply.
- Make sure `LED_STRIP_LENGTH` matches your actual strip length.
- Confirm the serial log shows `Match game initialized` and `Match game started`.

### Buttons do not respond

- The default wiring is active-high: button `OUT` to GPIO, `VCC` to 3.3V, and shared `GND`.
- For active-low wiring, change the corresponding `BTN_*_ACTIVE_LEVEL` to `0`.
- The firmware periodically logs raw button levels, which helps verify wiring.

### No sound

- Check MAX98357A `BCLK/LRC/DIN` wiring to GPIO7/8/9.
- Connect the speaker to `SPK+` / `SPK-`; do not drive a speaker directly from ESP32 GPIO.
- If amplifier initialization fails, the game continues without sound.

### Cannot open the web UI

- Confirm the device is connected to a 2.4 GHz WiFi network and read the actual IP from serial logs.
- Include the port in the URL: `http://<device-ip>:32323/`.
- If an incorrect WiFi credential was saved, the device may stay in station-only mode instead of opening AP mode. Erase NVS and flash again, or use the firmware WiFi reset path to set `force_ap=1` before rebooting.

## License

This project is released under the MIT License. See [LICENSE](./LICENSE).

---

Last updated: 2026-05-08
