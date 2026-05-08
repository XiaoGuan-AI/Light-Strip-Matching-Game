#include "display/st7306_gfx.h"
#include <esp_log.h>

static const char* TAG = "ST7306_GFX";

ST7306_GFX::ST7306_GFX(st7306_device_t* device)
    : Adafruit_GFX(ST7306_WIDTH, ST7306_HEIGHT), device_(device) {
    ESP_LOGI(TAG, "ST7306_GFX initialized with %dx%d display", ST7306_WIDTH, ST7306_HEIGHT);
}

void ST7306_GFX::drawPixel(int16_t x, int16_t y, uint16_t color) {
    if (x < 0 || x >= ST7306_WIDTH || y < 0 || y >= ST7306_HEIGHT) {
        return;
    }
    uint8_t st_color;
    if (color == 0xF800) st_color = ST7306_COLOR_RED; // pure red
    else if (color == 0x0000) st_color = ST7306_COLOR_BLACK; // black
    else st_color = ST7306_COLOR_WHITE; // default white for others
    st7306_draw_pixel(device_, x, y, st_color);
}

void ST7306_GFX::clearDisplay() { st7306_clear(device_); }
void ST7306_GFX::display() { st7306_display(device_); }
void ST7306_GFX::setHighPowerMode() { st7306_set_high_power_mode(device_); }
void ST7306_GFX::setLowPowerMode() { st7306_set_low_power_mode(device_); }


