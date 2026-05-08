#include "display/eink/Adafruit_GFX.h"

Adafruit_GFX::Adafruit_GFX(int16_t w, int16_t h)
    : WIDTH(w), HEIGHT(h) {
  _width = w;
  _height = h;
  cursor_x = 0;
  cursor_y = 0;
  textcolor = 0x0000;
  textbgcolor = 0xFFFF;
  textsize_x = 1;
  textsize_y = 1;
  rotation = 0;
  wrap = true;
  _cp437 = false;
  gfxFont = nullptr;
}

// Minimal text output: advance cursor; drawing can be implemented later if needed.
size_t Adafruit_GFX::write(uint8_t c) {
  if (c == '\n') {
    cursor_y += 8 * textsize_y;
    cursor_x = 0;
    return 1;
  }
  if (c == '\r') {
    return 1;
  }
  // Placeholder: advance cursor as if drawing a 6x8 glyph
  cursor_x += 6 * textsize_x;
  return 1;
}

void Adafruit_GFX::startWrite(void) {}

void Adafruit_GFX::writePixel(int16_t x, int16_t y, uint16_t color) {
  drawPixel(x, y, color);
}

void Adafruit_GFX::writeFillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
  for (int16_t yy = 0; yy < h; ++yy) {
    for (int16_t xx = 0; xx < w; ++xx) {
      drawPixel(x + xx, y + yy, color);
    }
  }
}

void Adafruit_GFX::writeFastVLine(int16_t x, int16_t y, int16_t h, uint16_t color) {
  for (int16_t i = 0; i < h; ++i) {
    drawPixel(x, y + i, color);
  }
}

void Adafruit_GFX::writeFastHLine(int16_t x, int16_t y, int16_t w, uint16_t color) {
  for (int16_t i = 0; i < w; ++i) {
    drawPixel(x + i, y, color);
  }
}

void Adafruit_GFX::writeLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color) {
  // Simple Bresenham
  int16_t dx = (x1 > x0) ? (x1 - x0) : (x0 - x1);
  int16_t sx = (x0 < x1) ? 1 : -1;
  int16_t dy = (y1 > y0) ? (y0 - y1) : (y1 - y0); // -abs(y1 - y0)
  int16_t sy = (y0 < y1) ? 1 : -1;
  int16_t err = dx + dy;
  while (true) {
    drawPixel(x0, y0, color);
    if (x0 == x1 && y0 == y1) break;
    int16_t e2 = 2 * err;
    if (e2 >= dy) { err += dy; x0 += sx; }
    if (e2 <= dx) { err += dx; y0 += sy; }
  }
}

void Adafruit_GFX::endWrite(void) {}

void Adafruit_GFX::setRotation(uint8_t r) { rotation = r; }

void Adafruit_GFX::invertDisplay(bool i) { (void)i; }

void Adafruit_GFX::drawFastVLine(int16_t x, int16_t y, int16_t h, uint16_t color) {
  writeFastVLine(x, y, h, color);
}

void Adafruit_GFX::drawFastHLine(int16_t x, int16_t y, int16_t w, uint16_t color) {
  writeFastHLine(x, y, w, color);
}

void Adafruit_GFX::fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
  writeFillRect(x, y, w, h, color);
}

void Adafruit_GFX::fillScreen(uint16_t color) {
  fillRect(0, 0, _width, _height, color);
}

void Adafruit_GFX::drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color) {
  writeLine(x0, y0, x1, y1, color);
}

void Adafruit_GFX::setTextSize(uint8_t s) {
  if (s == 0) s = 1;
  textsize_x = s;
  textsize_y = s;
}

void Adafruit_GFX::setTextSize(uint8_t sx, uint8_t sy) {
  if (sx == 0) sx = 1;
  if (sy == 0) sy = 1;
  textsize_x = sx;
  textsize_y = sy;
}

void Adafruit_GFX::setFont(const GFXfont *f) { gfxFont = const_cast<GFXfont*>(f); }


