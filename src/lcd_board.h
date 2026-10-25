#pragma once
/* Waveshare ESP32-C5-Touch-LCD-2.8: ST7789 240x320 LCD (SPI), CST3530 touch
 * and the CH32V003 I/O expander (resets, backlight), both on I2C. */
#include <Arduino.h>

static const int LCD_W = 240, LCD_H = 320;

/* Expander, LCD and touch controller (the original order); the display and
 * backlight stay off until lcd_display_on() and lcd_backlight(), after the
 * first screen is drawn. False when the I/O expander did not answer within
 * 3 s (call again). */
bool lcd_board_begin();
void lcd_display_on();
/* How long the last lcd_board_begin() waited for the expander, in ms. */
extern uint32_t lcd_expander_wait_ms;
/* I2C transfers to the I/O expander that failed (not acknowledged). */
extern unsigned lcd_expander_errors;
bool lcd_backlight(unsigned percent);
/* Writes the expander's pin setup and backlight again (an expander that
 * restarted would have dropped them); false when it does not answer. */
bool lcd_board_refresh(unsigned backlight_percent);
/* Sends rows y .. y + rows - 1, full width, RGB565 in native byte order. */
void lcd_push_rows(int y, int rows, const uint16_t *pixels);
/* First touch point in screen coordinates; false when not touched. */
bool lcd_touch(int *x, int *y);
/* Battery ADC count from the I/O expander; -1 when it cannot be read. */
int lcd_battery_raw();
