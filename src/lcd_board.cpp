#if FPV_LCD
/* Pin map, expander registers and the ST7789 / CST3530 set-up follow
 * Waveshare's ESP32-C5-Touch-LCD-2.8 Arduino examples (Apache-2.0). */
#include "lcd_board.h"
#include <SPI.h>
#include <Wire.h>
#include "config.h"

namespace {
const int PIN_SCLK = 6, PIN_MOSI = 7, PIN_LCD_CS = 10, PIN_LCD_DC = 9, PIN_SD_CS = 23;
const int PIN_SDA = 0, PIN_SCL = 1;
const uint32_t SPI_HZ = 80000000;

const uint8_t EXIO_ADDR = 0x24, EXIO_MODE = 0x02, EXIO_OUTPUT = 0x03, EXIO_PWM = 0x05, EXIO_ADC = 0x06;
const uint8_t EXIO_TP_RST = 0, EXIO_LCD_RST = 1;

const uint8_t TOUCH_ADDR = 0x58;
const uint32_t TOUCH_DATA = 0xD0070000, TOUCH_END = 0xD00002AB;

SPIClass spi(FSPI);
uint8_t exio_out;
bool touch_ok;

bool exio_write(uint8_t reg, uint8_t value) {
    Wire.beginTransmission(EXIO_ADDR);
    Wire.write(reg);
    Wire.write(value);
    return Wire.endTransmission() == 0;
}

void exio_set(uint8_t pin, bool level) {
    if (level) exio_out |= 1 << pin;
    else exio_out &= ~(1 << pin);
    exio_write(EXIO_OUTPUT, exio_out);
}

void lcd_write(bool data, const uint8_t *bytes, size_t size) {
    spi.beginTransaction(SPISettings(SPI_HZ, MSBFIRST, SPI_MODE0));
    digitalWrite(PIN_LCD_CS, LOW);
    digitalWrite(PIN_LCD_DC, data ? HIGH : LOW);
    spi.writeBytes(bytes, size);
    digitalWrite(PIN_LCD_CS, HIGH);
    spi.endTransaction();
}

void lcd_command(uint8_t cmd, std::initializer_list<uint8_t> data = {}) {
    lcd_write(false, &cmd, 1);
    if (data.size()) lcd_write(true, data.begin(), data.size());
}

void lcd_window(int x0, int y0, int x1, int y1) {
    lcd_command(0x2A, {(uint8_t)(x0 >> 8), (uint8_t)x0, (uint8_t)(x1 >> 8), (uint8_t)x1});
    lcd_command(0x2B, {(uint8_t)(y0 >> 8), (uint8_t)y0, (uint8_t)(y1 >> 8), (uint8_t)y1});
    lcd_command(0x2C);
}

bool touch_access(uint32_t reg, uint8_t *data, size_t size) {
    Wire.beginTransmission(TOUCH_ADDR);
    Wire.write((uint8_t)(reg >> 24));
    Wire.write((uint8_t)(reg >> 16));
    Wire.write((uint8_t)(reg >> 8));
    Wire.write((uint8_t)reg);
    if (!data) return Wire.endTransmission(true) == 0;
    if (Wire.endTransmission(false)) return false;
    if (Wire.requestFrom(TOUCH_ADDR, (uint8_t)size) != size) return false;
    for (size_t i = 0; i < size; i++) data[i] = Wire.read();
    return true;
}
} // namespace

uint32_t lcd_expander_wait_ms;
bool lcd_touch_begin();

bool lcd_board_begin() {
    static bool wire_started;
    if (!wire_started) {
        Wire.begin(PIN_SDA, PIN_SCL);
        Wire.setClock(400000);
        wire_started = true;
    }
    /* The LCD reset and backlight go through the I/O expander, a CH32V003
     * that starts later than the ESP32-C5 at power-on: until it answers,
     * nothing below takes effect. Waits up to 3 s; the caller retries. */
    exio_out = (1 << EXIO_TP_RST) | (1 << EXIO_LCD_RST); /* resets released, amplifier off */
    uint32_t t0 = millis();
    bool io_ok;
    while (!(io_ok = exio_write(EXIO_MODE, 0xFF) && exio_write(EXIO_OUTPUT, exio_out)) && millis() - t0 < 3000)
        delay(20);
    if (!io_ok) {
        Serial.printf("[%lu ms] LCD: I/O expander not answering\n", millis());
        return false;
    }
    lcd_expander_wait_ms = millis() - t0;
    delay(20); /* let it settle */
    exio_write(EXIO_MODE, 0xFF);
    exio_write(EXIO_OUTPUT, exio_out);

    pinMode(PIN_LCD_CS, OUTPUT);
    pinMode(PIN_LCD_DC, OUTPUT);
    pinMode(PIN_SD_CS, OUTPUT);
    digitalWrite(PIN_LCD_CS, HIGH);
    digitalWrite(PIN_LCD_DC, HIGH);
    digitalWrite(PIN_SD_CS, HIGH); /* keep the microSD card off the bus */
    spi.begin(PIN_SCLK, -1, PIN_MOSI, PIN_LCD_CS);

    exio_set(EXIO_LCD_RST, false);
    delay(50);
    exio_set(EXIO_LCD_RST, true);
    delay(120);
    lcd_command(0x11); /* sleep out */
    delay(120);
    lcd_command(0x36, {0x00}); /* portrait, RGB */
    lcd_command(0x3A, {0x05}); /* 16 bits per pixel */
    lcd_command(0xB0, {0x00, 0xE8}); /* RAMCTRL: little-endian pixels */
    lcd_command(0xB2, {0x0C, 0x0C, 0x00, 0x33, 0x33});
    lcd_command(0xB7, {0x75});
    lcd_command(0xBB, {0x1A});
    lcd_command(0xC0, {0x2C});
    lcd_command(0xC2, {0x01, 0xFF});
    lcd_command(0xC3, {0x13});
    lcd_command(0xC4, {0x20});
    lcd_command(0xC6, {0x0F});
    lcd_command(0xD0, {0xA4, 0xA1});
    lcd_command(0xD6, {0xA1});
    lcd_command(0xE0, {0xD0, 0x0D, 0x14, 0x0D, 0x0D, 0x09, 0x38, 0x44, 0x4E, 0x3A, 0x17, 0x18, 0x2F, 0x30});
    lcd_command(0xE1, {0xD0, 0x09, 0x0F, 0x08, 0x07, 0x14, 0x37, 0x44, 0x4D, 0x38, 0x15, 0x16, 0x2C, 0x2E});
    lcd_command(0x21); /* inversion on (IPS panel) */
    lcd_command(0x29); /* display on */

    lcd_touch_begin();
    lcd_backlight(LCD_BACKLIGHT);
    Serial.printf("[%lu ms] LCD: ready (expander answered after %lu ms)\n", millis(),
                  (unsigned long)lcd_expander_wait_ms);
    return true;
}

bool lcd_backlight(unsigned percent) { return exio_write(EXIO_PWM, min(percent, 100u) * 255 / 100); }

bool lcd_board_refresh(unsigned backlight_percent) {
    return exio_write(EXIO_MODE, 0xFF) && exio_write(EXIO_OUTPUT, exio_out) && lcd_backlight(backlight_percent);
}

bool lcd_touch_begin() {
    exio_set(EXIO_TP_RST, false);
    delay(100);
    exio_set(EXIO_TP_RST, true);
    delay(300);
    Wire.beginTransmission(TOUCH_ADDR);
    touch_ok = Wire.endTransmission() == 0;
    Serial.printf("[%lu ms] LCD: touch %s\n", millis(), touch_ok ? "ok" : "not found");
    return touch_ok;
}

void lcd_push_rows(int y, int rows, const uint16_t *pixels) {
    lcd_window(0, y, LCD_W - 1, y + rows - 1);
    lcd_write(true, (const uint8_t *)pixels, (size_t)LCD_W * rows * 2);
}

int lcd_battery_raw() {
    Wire.beginTransmission(EXIO_ADDR);
    Wire.write(EXIO_ADC);
    if (Wire.endTransmission(false) || Wire.requestFrom(EXIO_ADDR, (uint8_t)2) != 2) return -1;
    int lo = Wire.read();
    return lo | Wire.read() << 8;
}

bool lcd_touch(int *x, int *y) {
    if (!touch_ok) return false;
    uint8_t b[9];
    bool read = touch_access(TOUCH_DATA, b, sizeof(b));
    touch_access(TOUCH_END, nullptr, 0);
    if (!read || !(b[3] & 0x0F) || !(b[8] & 0xF0)) return false;
    int tx = ((b[7] & 0x0F) << 8) + b[4], ty = ((b[7] & 0xF0) << 4) + b[5];
#if TOUCH_SWAP_XY
    int t = tx;
    tx = ty * LCD_W / LCD_H;
    ty = t * LCD_H / LCD_W;
#endif
#if TOUCH_MIRROR_X
    tx = LCD_W - 1 - tx;
#endif
#if TOUCH_MIRROR_Y
    ty = LCD_H - 1 - ty;
#endif
    *x = constrain(tx, 0, LCD_W - 1);
    *y = constrain(ty, 0, LCD_H - 1);
    return true;
}
#endif
