#if FPV_LCD147
/* ESP32-C5-LCD-1.47 (KLAYERS / Waveshare): ST7789 172x320 on SPI, backlight
 * on GPIO10 (active high), BOOT button on GPIO28, no touch. Pins, panel
 * offset and set-up follow Waveshare's ESP32-C5-LCD-1.47 examples and BSP
 * (Apache-2.0). Same interface as lcd_board.cpp (lcd_board.h). */
#include "lcd_board.h"
#include <SPI.h>
#include "config.h"

namespace {
const int PIN_SCLK = 7, PIN_MOSI = 6, PIN_LCD_CS = 23, PIN_LCD_DC = 24, PIN_LCD_RST = 26, PIN_BL = 10;
const int PIN_SD_CS = 4, PIN_BOOT = 28;
const uint32_t SPI_HZ = 40000000;
/* The 172 pixel wide panel sits in the middle of the controller's 240
 * columns (240 - 172 = 68, 34 on either side). */
const int PANEL_GAP = 34;

SPIClass spi(FSPI);
bool landscape;

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
    /* the gap is on the panel's short axis: x in portrait, y in landscape */
    if (landscape) y0 += PANEL_GAP, y1 += PANEL_GAP;
    else x0 += PANEL_GAP, x1 += PANEL_GAP;
    lcd_command(0x2A, {(uint8_t)(x0 >> 8), (uint8_t)x0, (uint8_t)(x1 >> 8), (uint8_t)x1});
    lcd_command(0x2B, {(uint8_t)(y0 >> 8), (uint8_t)y0, (uint8_t)(y1 >> 8), (uint8_t)y1});
    lcd_command(0x2C);
}
} // namespace

uint32_t lcd_expander_wait_ms; /* no expander on this board */
unsigned lcd_expander_errors;

bool lcd_board_begin() {
    static bool started;
    if (!started) {
        pinMode(PIN_LCD_CS, OUTPUT);
        pinMode(PIN_LCD_DC, OUTPUT);
        pinMode(PIN_LCD_RST, OUTPUT);
        pinMode(PIN_SD_CS, OUTPUT);
        pinMode(PIN_BOOT, INPUT_PULLUP);
        digitalWrite(PIN_LCD_CS, HIGH);
        digitalWrite(PIN_SD_CS, HIGH); /* keep the microSD card off the shared bus */
        ledcAttach(PIN_BL, 5000, 8);
        spi.begin(PIN_SCLK, -1, PIN_MOSI, PIN_LCD_CS);
        started = true;
    }
    lcd_backlight(0);
    digitalWrite(PIN_LCD_RST, LOW);
    delay(20);
    digitalWrite(PIN_LCD_RST, HIGH);
    delay(120);
    lcd_command(0x11); /* sleep out */
    delay(120);
    landscape = false;
    lcd_command(0x36, {0x00});       /* portrait, RGB */
    lcd_command(0x3A, {0x05});       /* 16 bits per pixel */
    lcd_command(0xB0, {0x00, 0xE8}); /* RAMCTRL: little-endian pixels */
    lcd_command(0x21);               /* inversion on (IPS panel) */
    lcd_command(0x13);               /* normal display mode */
    /* display on only after the first screen (lcd_display_on()) */
    Serial.printf("[%lu ms] LCD: ready (1.47)\n", millis());
    return true;
}

void lcd_display_on() { lcd_command(0x29); }

bool lcd_backlight(unsigned percent) {
    ledcWrite(PIN_BL, min(percent, 100u) * 255 / 100);
    return true;
}

bool lcd_board_refresh(unsigned backlight_percent) { return lcd_backlight(backlight_percent); }

void lcd_set_landscape(bool on, bool flip) {
    landscape = on;
    /* MADCTL: MV swaps rows and columns, MX / MY mirror them */
    lcd_command(0x36, {(uint8_t)(!on ? 0x00 : flip ? 0x60 : 0xA0)});
}

void lcd_push_rows(int y, int rows, int width, const uint16_t *pixels) {
    lcd_window(0, y, width - 1, y + rows - 1);
    lcd_write(true, (const uint8_t *)pixels, (size_t)width * rows * 2);
}

bool lcd_touch(int *, int *) { return false; }
int lcd_battery_raw() { return -1; }
bool lcd_user_button() { return digitalRead(PIN_BOOT) == LOW; }
#endif
