#ifndef ST7306_DRIVER_H
#define ST7306_DRIVER_H

#include <stdint.h>
#include <stdbool.h>
#include "driver/spi_master.h"
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ST7306_WIDTH  300
#define ST7306_HEIGHT 400
#define ST7306_DATA_WIDTH  150
#define ST7306_DATA_HEIGHT 200
#define ST7306_BUFFER_SIZE 30000

// Match reference driver encoding: 2-bit per pixel D1:D0
// 00=WHITE, 01=RED, 10=BLACK, 11=RED+BLACK
#define ST7306_COLOR_WHITE        0x00
#define ST7306_COLOR_RED          0x01
#define ST7306_COLOR_BLACK        0x02
#define ST7306_COLOR_RED_BLACK    0x03

typedef struct {
    spi_device_handle_t spi_handle;
    int dc_pin;
    int res_pin; 
    int cs_pin;
    uint8_t buffer[ST7306_BUFFER_SIZE];
} st7306_device_t;

esp_err_t st7306_init(st7306_device_t* dev, int dc_pin, int res_pin, int cs_pin, spi_device_handle_t spi_handle);
esp_err_t st7306_clear(st7306_device_t* dev);
esp_err_t st7306_draw_pixel(st7306_device_t* dev, int x, int y, uint8_t color);
esp_err_t st7306_display(st7306_device_t* dev);
// Display only a window region. Coordinates are in panel pixel space (0..W-1/0..H-1)
// and follow the same orientation expectations as st7306_draw_pixel (i.e., caller
// can pass LVGL coordinates; the driver will handle internal mirroring to match
// buffer layout).
esp_err_t st7306_display_window(st7306_device_t* dev, int x1, int y1, int x2, int y2);
esp_err_t st7306_test_pattern(st7306_device_t* dev);
esp_err_t st7306_fill(st7306_device_t* dev, uint8_t data);
esp_err_t st7306_fill_half(st7306_device_t* dev);
esp_err_t st7306_set_high_power_mode(st7306_device_t* dev);
esp_err_t st7306_set_low_power_mode(st7306_device_t* dev);
esp_err_t st7306_set_contrast(st7306_device_t* dev, uint8_t contrast);
// Strong optical erase to remove ghosting: drive full black then full white
esp_err_t st7306_wipe_sequence(st7306_device_t* dev);

#ifdef __cplusplus
}
#endif

#endif // ST7306_DRIVER_H

