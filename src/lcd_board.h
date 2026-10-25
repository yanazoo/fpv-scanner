#pragma once
/* LCD boards of the FPV_LCD build:
 * - Waveshare ESP32-C5-Touch-LCD-2.8 (lcd_board.cpp): ST7789 240x320 LCD
 *   (SPI), CST3530 touch and the CH32V003 I/O expander (resets, backlight),
 *   both on I2C.
 * - ESP32-C5-LCD-1.47 (FPV_LCD147, lcd147_board.cpp): ST7789 172x320 LCD,
 *   backlight on a GPIO, no touch, the BOOT button as the user button. */
#include <Arduino.h>

#if FPV_LCD147
static const int LCD_W = 172, LCD_H = 320;
static const bool LCD_HAS_TOUCH = false;
#else
static const int LCD_W = 240, LCD_H = 320;
static const bool LCD_HAS_TOUCH = true;
#endif
/* The board's user button (1.47: BOOT) is pressed; false without one. */
bool lcd_user_button();

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
/* Sends rows y .. y + rows - 1 of the given width (the full screen width),
 * RGB565 in native byte order. */
void lcd_push_rows(int y, int rows, int width, const uint16_t *pixels);
/* Landscape (LCD_H x LCD_W; flip turns it by 180 degrees) or portrait
 * (LCD_W x LCD_H) addressing; lcd_touch() keeps reporting portrait
 * coordinates. */
void lcd_set_landscape(bool on, bool flip);
/* First touch point in screen coordinates; false when not touched. */
bool lcd_touch(int *x, int *y);
/* Battery ADC count from the I/O expander; -1 when it cannot be read. */
int lcd_battery_raw();
