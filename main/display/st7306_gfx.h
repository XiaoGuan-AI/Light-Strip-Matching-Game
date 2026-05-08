#ifndef ST7306_GFX_H
#define ST7306_GFX_H

#include "display/eink/Adafruit_GFX.h"
#include "st7306_driver.h"

class ST7306_GFX : public Adafruit_GFX {
public:
    ST7306_GFX(st7306_device_t* device);
    virtual ~ST7306_GFX() = default;
    virtual void drawPixel(int16_t x, int16_t y, uint16_t color) override;
    void clearDisplay();
    void display();
    void setHighPowerMode();
    void setLowPowerMode();
private:
    st7306_device_t* device_;
};

#endif // ST7306_GFX_H

