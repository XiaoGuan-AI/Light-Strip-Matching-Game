#include "st7306_driver.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char* TAG __attribute__((unused)) = "ST7306";

static esp_err_t st7306_write_command(st7306_device_t* dev, uint8_t cmd) {
    uint8_t data = cmd;
    spi_transaction_t trans = { .length = 8, .tx_buffer = &data };
    gpio_set_level(dev->dc_pin, 0);
    gpio_set_level(dev->cs_pin, 0);
    esp_err_t ret = spi_device_transmit(dev->spi_handle, &trans);
    gpio_set_level(dev->cs_pin, 1);
    return ret;
}
static esp_err_t st7306_write_data(st7306_device_t* dev, uint8_t data) {
    spi_transaction_t trans = { .length = 8, .tx_buffer = &data };
    gpio_set_level(dev->dc_pin, 1);
    gpio_set_level(dev->cs_pin, 0);
    esp_err_t ret = spi_device_transmit(dev->spi_handle, &trans);
    gpio_set_level(dev->cs_pin, 1);
    return ret;
}
static esp_err_t st7306_write_buffer(st7306_device_t* dev, const uint8_t* buffer, size_t len) {
    gpio_set_level(dev->dc_pin, 1);
    gpio_set_level(dev->cs_pin, 0);
    const size_t max_transfer = 4092; size_t offset = 0; esp_err_t ret = ESP_OK;
    while (offset < len && ret == ESP_OK) {
        size_t transfer_size = (len - offset) > max_transfer ? max_transfer : (len - offset);
        spi_transaction_t trans = { .length = transfer_size * 8, .tx_buffer = buffer + offset };
        ret = spi_device_transmit(dev->spi_handle, &trans);
        offset += transfer_size;
    }
    gpio_set_level(dev->cs_pin, 1);
    return ret;
}

esp_err_t st7306_init(st7306_device_t* dev, int dc_pin, int res_pin, int cs_pin, spi_device_handle_t spi_handle) {
    ESP_LOGI(TAG, "init start dc=%d res=%d cs=%d", dc_pin, res_pin, cs_pin);
    dev->spi_handle = spi_handle; dev->dc_pin = dc_pin; dev->res_pin = res_pin; dev->cs_pin = cs_pin;
    gpio_config_t io_conf = { .mode = GPIO_MODE_OUTPUT, .pin_bit_mask = ((1ULL<<dc_pin)|(1ULL<<res_pin)|(1ULL<<cs_pin)) };
    ESP_ERROR_CHECK(gpio_config(&io_conf));
    // Keep CS high, DC low, and perform a controlled reset pulse
    gpio_set_level(cs_pin, 1);
    gpio_set_level(dc_pin, 0);
    gpio_set_level(res_pin, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(res_pin, 1);
    vTaskDelay(pdMS_TO_TICKS(120));
    ESP_LOGI(TAG, "panel reset done");
    // Reference init sequence (port of Arduino driver):
    // Booster enable
    st7306_write_command(dev, 0xD6); st7306_write_data(dev, 0x17); st7306_write_data(dev, 0x02);
    st7306_write_command(dev, 0xD1); st7306_write_data(dev, 0x01);

    // Gate Voltage Setting VGH/VGL (17V / -10V)
    st7306_write_command(dev, 0xC0); st7306_write_data(dev, 0x12); st7306_write_data(dev, 0x0A);
    // VSHP/VSLP/VSHN/VSLN settings
    st7306_write_command(dev, 0xC1); st7306_write_data(dev, 115); st7306_write_data(dev, 0x3E); st7306_write_data(dev, 0x3C); st7306_write_data(dev, 0x3C);
    st7306_write_command(dev, 0xC2); st7306_write_data(dev, 0x00); st7306_write_data(dev, 0x21); st7306_write_data(dev, 0x23); st7306_write_data(dev, 0x23);
    st7306_write_command(dev, 0xC4); st7306_write_data(dev, 50);  st7306_write_data(dev, 0x5C); st7306_write_data(dev, 0x5A); st7306_write_data(dev, 0x5A);
    st7306_write_command(dev, 0xC5); st7306_write_data(dev, 50);  st7306_write_data(dev, 0x35); st7306_write_data(dev, 0x37); st7306_write_data(dev, 0x37);

    // OSC + Frame Rate: HPM=32Hz, LPM=1Hz
    st7306_write_command(dev, 0xD8); st7306_write_data(dev, 0xA6); st7306_write_data(dev, 0xE9);
    st7306_write_command(dev, 0xB2); st7306_write_data(dev, 0x12);

    // Gate timing
    st7306_write_command(dev, 0x62); st7306_write_data(dev, 0x32); st7306_write_data(dev, 0x03); st7306_write_data(dev, 0x1F);

    // Source EQ Enable
    st7306_write_command(dev, 0xB7); st7306_write_data(dev, 0x13);

    // Gate Line Setting (400 lines)
    st7306_write_command(dev, 0xB0); st7306_write_data(dev, 0x64);

    // Sleep out
    st7306_write_command(dev, 0x11); vTaskDelay(pdMS_TO_TICKS(120));
    ESP_LOGI(TAG, "sleep out");

    // Source Voltage Select
    st7306_write_command(dev, 0xC9); st7306_write_data(dev, 0x00);

    // Memory Data Access Control: MX=1, DO=1
    st7306_write_command(dev, 0x36); st7306_write_data(dev, 0x48);
    // Data Format Select 3-write for 24bit (we still use our packed format buffer)
    st7306_write_command(dev, 0x3A); st7306_write_data(dev, 0x11);

    // Gamma Mode: Mono
    st7306_write_command(dev, 0xB9); st7306_write_data(dev, 0x20);
    // Panel Setting: 1-dot inversion, frame inversion, one line interlace
    st7306_write_command(dev, 0xB8); st7306_write_data(dev, 0x29);

    // Column/Row Address window 300x400
    st7306_write_command(dev, 0x2A); st7306_write_data(dev, 0x05); st7306_write_data(dev, 0x36);
    st7306_write_command(dev, 0x2B); st7306_write_data(dev, 0x00); st7306_write_data(dev, 0xC7);

    // TE off (some panels can glitch early; keep TE disabled)
    st7306_write_command(dev, 0x35); st7306_write_data(dev, 0x00);
    // Auto power down ON
    st7306_write_command(dev, 0xD0); st7306_write_data(dev, 0xFF);
    // High Power Mode ON
    st7306_write_command(dev, 0x38); vTaskDelay(pdMS_TO_TICKS(300));
    // Display ON, inversion off
    st7306_write_command(dev, 0x29);
    st7306_write_command(dev, 0x20);
    ESP_LOGI(TAG, "display on");
    // Ensure RAM known state (all white) before first display
    st7306_write_command(dev, 0xBB);
    st7306_write_data(dev, 0x00);
    memset(dev->buffer, 0x00, ST7306_BUFFER_SIZE);
    ESP_LOGI(TAG, "ram cleared -> white (0x00)");
    return ESP_OK;
}
esp_err_t st7306_clear(st7306_device_t* dev) { memset(dev->buffer, 0x00, ST7306_BUFFER_SIZE); return ESP_OK; }
esp_err_t st7306_fill(st7306_device_t* dev, uint8_t data) { memset(dev->buffer, data, ST7306_BUFFER_SIZE); return ESP_OK; }
esp_err_t st7306_fill_half(st7306_device_t* dev) { for (int i=0;i<ST7306_BUFFER_SIZE;i++){ dev->buffer[i] = (i%ST7306_DATA_WIDTH<ST7306_DATA_WIDTH/2)?0x55:0x00;} return ESP_OK; }
esp_err_t st7306_draw_pixel(st7306_device_t* dev, int x, int y, uint8_t color) {
    if (x<0||y<0||x>=ST7306_WIDTH||y>=ST7306_HEIGHT) return ESP_ERR_INVALID_ARG;
    // Flip vertically and horizontally to correct orientation
    y = (ST7306_HEIGHT - 1) - y;
    x = (ST7306_WIDTH  - 1) - x;
    uint32_t real_x = x/2, real_y=y/2; uint32_t idx = real_y*ST7306_DATA_WIDTH+real_x; if (idx>=ST7306_BUFFER_SIZE) return ESP_ERR_INVALID_ARG;
    uint32_t one_two = (y%2==0)?0:1; uint32_t line_bit_1=(x%2)*4; uint32_t line_bit_0=(x%2)*4+2; uint8_t b1=7-(line_bit_1+one_two); uint8_t b0=7-(line_bit_0+one_two);
    bool d0 = (color & 0x01)!=0; bool d1=(color & 0x02)!=0;
    if (d1) dev->buffer[idx] |= (1<<b1); else dev->buffer[idx] &= ~(1<<b1);
    if (d0) dev->buffer[idx] |= (1<<b0); else dev->buffer[idx] &= ~(1<<b0);
    return ESP_OK;
}
esp_err_t st7306_display(st7306_device_t* dev) {
    // @@@fullframe-cell-mode - Use the same addressing scheme as window updates (cell rows + full byte width)
    // This avoids the visible noise seen when using pixel-addressed 0x2A/0x2B with packed 2-bit data.
    ESP_LOGI(TAG, "display full frame (stream rows)");
    // Column window: constant full-width byte addressing (matches init 0x2A 0x05 0x36)
    st7306_write_command(dev, 0x2A);
    st7306_write_data(dev, 0x05);
    st7306_write_data(dev, 0x36);
    // Row window: cell rows 0..199 (each cell row is 2 pixels)
    st7306_write_command(dev, 0x2B);
    st7306_write_data(dev, 0x00);
    st7306_write_data(dev, 0xC7);

    // Stream the entire buffer row-by-row: 200 cell-rows, each 150 bytes
    st7306_write_command(dev, 0x2C);
    gpio_set_level(dev->dc_pin, 1);
    gpio_set_level(dev->cs_pin, 0);
    const size_t max_transfer = 4092; esp_err_t ret = ESP_OK;
    for (int row = 0; row < ST7306_DATA_HEIGHT && ret == ESP_OK; ++row) {
        const uint8_t* row_ptr = &dev->buffer[row * ST7306_DATA_WIDTH];
        int remaining = ST7306_DATA_WIDTH;
        while (remaining > 0 && ret == ESP_OK) {
            int chunk = remaining > (int)max_transfer ? (int)max_transfer : remaining;
            spi_transaction_t trans = { .length = (size_t)chunk * 8, .tx_buffer = row_ptr };
            ret = spi_device_transmit(dev->spi_handle, &trans);
            row_ptr += chunk; remaining -= chunk;
        }
    }
    gpio_set_level(dev->cs_pin, 1);
    return ret;
}
esp_err_t st7306_display_window(st7306_device_t* dev, int x1, int y1, int x2, int y2) {
    // Clamp and normalize
    if (x1 < 0) { x1 = 0; }
    if (y1 < 0) { y1 = 0; }
    if (x2 >= ST7306_WIDTH) { x2 = ST7306_WIDTH - 1; }
    if (y2 >= ST7306_HEIGHT) { y2 = ST7306_HEIGHT - 1; }
    if (x2<x1 || y2<y1) return ESP_ERR_INVALID_ARG;

    // Due to internal packing (2x2 pixels per byte) and controller expectations,
    // align window to even coordinates so we can stream contiguous bytes.
    int wx1 = x1 & ~1; int wy1 = y1 & ~1; int wx2 = x2 | 1; int wy2 = y2 | 1;

    // Set window with the empirically working addressing mode:
    // - Column window: keep full width constants as in init (0x05..0x36)
    // - Row window: use cell rows (each cell is 2 pixels tall) and mirror vertically
    int ty1 = (ST7306_HEIGHT - 1) - wy2;
    int ty2 = (ST7306_HEIGHT - 1) - wy1;
    int row_start_cell = ty1 / 2;
    int row_end_cell = ty2 / 2;
    int bw = ST7306_DATA_WIDTH; // full width in packed bytes per row
    int bh = (wy2 - wy1 + 1 + 1) / 2; // number of cell rows
    static uint32_t s_win_log_cnt = 0;
    if ((s_win_log_cnt++ % 20) == 0) {
        ESP_LOGI(TAG, "display window req=(%d,%d)-(%d,%d) aligned=(%d,%d)-(%d,%d) rows=(%d..%d) bw=%d bh=%d",
                 x1, y1, x2, y2, wx1, wy1, wx2, wy2, row_start_cell, row_end_cell, bw, bh);
    }
    st7306_write_command(dev, 0x2A);
    st7306_write_data(dev, 0x05);
    st7306_write_data(dev, 0x36);
    st7306_write_command(dev, 0x2B);
    st7306_write_data(dev, (uint8_t)row_start_cell);
    st7306_write_data(dev, (uint8_t)row_end_cell);

    // Now stream only the bytes that map to this window from our packed buffer (full width rows)
    int start_col = 0;
    int start_row = row_start_cell;
    int bytes_per_row = ST7306_DATA_WIDTH;

    st7306_write_command(dev, 0x2C);
    gpio_set_level(dev->dc_pin, 1);
    gpio_set_level(dev->cs_pin, 0);
    const size_t max_transfer = 4092; esp_err_t ret = ESP_OK;
    for (int row = 0; row < bh && ret == ESP_OK; ++row) {
        const uint8_t* row_ptr = &dev->buffer[(start_row + row) * bytes_per_row + start_col];
        int remaining = bw;
        while (remaining > 0 && ret == ESP_OK) {
            int chunk = remaining > (int)max_transfer ? (int)max_transfer : remaining;
            spi_transaction_t trans = { .length = (size_t)chunk * 8, .tx_buffer = row_ptr };
            ret = spi_device_transmit(dev->spi_handle, &trans);
            row_ptr += chunk; remaining -= chunk;
        }
    }
    gpio_set_level(dev->cs_pin, 1);
    return ret;
}
esp_err_t st7306_set_high_power_mode(st7306_device_t* dev){ return ESP_OK; }
esp_err_t st7306_set_low_power_mode(st7306_device_t* dev){ return ESP_OK; }
esp_err_t st7306_set_contrast(st7306_device_t* dev, uint8_t contrast){ return ESP_OK; }
esp_err_t st7306_test_pattern(st7306_device_t* dev){ return ESP_OK; }

// 强力擦拭序列：先全黑，再全白，最大程度降低残影
esp_err_t st7306_wipe_sequence(st7306_device_t* dev) {
    // Full black frame
    memset(dev->buffer, 0xAA, ST7306_BUFFER_SIZE); // 10=BLACK per 2-bit pattern
    st7306_display(dev);
    vTaskDelay(pdMS_TO_TICKS(500));
    // Full white frame
    memset(dev->buffer, 0x00, ST7306_BUFFER_SIZE);
    st7306_display(dev);
    vTaskDelay(pdMS_TO_TICKS(500));
    return ESP_OK;
}


