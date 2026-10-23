#pragma once
/* Waveshare ESP32-C5-Touch-LCD-2.8: ST7789 240x320 LCD (SPI), CST3530 touch
 * and the CH32V003 I/O expander (resets, backlight), both on I2C. */
#include <Arduino.h>

static const int LCD_W = 240, LCD_H = 320;

bool lcd_board_begin();
/* Sends rows y .. y + rows - 1, full width, RGB565 in native byte order. */
void lcd_push_rows(int y, int rows, const uint16_t *pixels);
/* First touch point in screen coordinates; false when not touched. */
bool lcd_touch(int *x, int *y);
/* Battery ADC count from the I/O expander; -1 when it cannot be read. */
int lcd_battery_raw();
