#ifndef ESP_GFX_COMPAT_H
#define ESP_GFX_COMPAT_H

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <algorithm>
#include <cmath>

template<typename T>
inline T min(T a, T b) { return (a < b) ? a : b; }

template<typename T>
inline T max(T a, T b) { return (a > b) ? a : b; }
// Avoid clashing with <chrono>/<cmath> by not redefining std symbols as macros
template<typename T>
inline T constrain(T amt, T low, T high) { return (amt < low) ? low : ((amt > high) ? high : amt); }
#define radians(deg) ((deg)*DEG_TO_RAD)
#define degrees(rad) ((rad)*RAD_TO_DEG)
#define sq(x) ((x)*(x))

#define PI 3.1415926535897932384626433832795
#define HALF_PI 1.5707963267948966192313216916398
#define TWO_PI 6.283185307179586476925286766559
#define DEG_TO_RAD 0.017453292519943295769236907684886
#define RAD_TO_DEG 57.295779513082320876798154814105

class Print {
public:
    virtual ~Print() {}
    virtual size_t write(uint8_t) = 0;
    virtual size_t write(const uint8_t *buffer, size_t size) {
        size_t n = 0; while (size--) { n += write(*buffer++); } return n;
    }
    size_t write(const char *str) { if (str == NULL) return 0; return write((const uint8_t *)str, strlen(str)); }
    size_t write(const char *buffer, size_t size) { return write((const uint8_t *)buffer, size); }
    size_t print(const char* str) { return write(str); }
    size_t print(char c) { return write(c); }
    size_t print(int n) { char buf[16]; snprintf(buf, sizeof(buf), "%d", n); return write(buf); }
    size_t print(unsigned int n) { char buf[16]; snprintf(buf, sizeof(buf), "%u", n); return write(buf); }
    size_t print(long n) { char buf[16]; snprintf(buf, sizeof(buf), "%ld", n); return write(buf); }
    size_t print(unsigned long n) { char buf[16]; snprintf(buf, sizeof(buf), "%lu", n); return write(buf); }
    size_t println(const char* str = "") { size_t n = print(str); n += write('\n'); return n; }
};

#define pgm_read_byte(addr) (*(const unsigned char *)(addr))
#define pgm_read_word(addr) (*(const unsigned short *)(addr))
#define pgm_read_dword(addr) (*(const unsigned long *)(addr))
#define pgm_read_float(addr) (*(const float *)(addr))
#define pgm_read_ptr(addr) (*(const void **)(addr))

#define PROGMEM
#define PGM_P const char *

class __FlashStringHelper;
#define F(string_literal) (reinterpret_cast<const __FlashStringHelper *>(string_literal))

class String {
public:
    String() : buffer(nullptr), len(0), capacity(0) {}
    String(const char* str) {
        if (str) { len = strlen(str); capacity = len + 1; buffer = new char[capacity]; strcpy(buffer, str); }
        else { buffer = nullptr; len = 0; capacity = 0; }
    }
    ~String() { if (buffer) delete[] buffer; }
    const char* c_str() const { return buffer ? buffer : ""; }
    size_t length() const { return len; }
private:
    char* buffer; size_t len; size_t capacity;
};

class Adafruit_I2CDevice { public: Adafruit_I2CDevice(uint8_t addr) {} };
class Adafruit_SPIDevice { public: Adafruit_SPIDevice(int8_t cs) {} };

#endif // ESP_GFX_COMPAT_H

