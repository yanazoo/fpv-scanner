#pragma once
#ifndef FPV_LCD
#define FPV_LCD 0 /* 1: an LCD board (FPV_LCD147 for the 1.47 inch one) */
#endif
/* ---- Wi-Fi access point (2.4 GHz) -----------------------------------
 * These values are public (committed to the repository). WPA2 needs a
 * password of at least 8 characters. */
#define AP_SSID     "FPV-Scanner"
#define AP_PASSWORD "fpv-sa00"
#define AP_CHANNEL  1

/* ---- UART link to the XIAO ESP32-C5 (ESP-SDR firmware) --------------- */
#if FPV_LCD147
/* ESP32-C5-LCD-1.47 (KLAYERS / Waveshare), pin header:
 * "1" (GPIO1, RX) <- XIAO C5 D6 (GPIO11, TX)
 * "0" (GPIO0, TX) -> XIAO C5 D7 (GPIO12, RX), "5V" -> XIAO 5V, "GND" -> GND.
 * GPIO0/1 are no strapping pins, so the XIAO cannot change how this board
 * starts. */
#define SDR_RX_PIN 1
#define SDR_TX_PIN 0
/* Landscape screen (320x172): 0 = the USB connector on the left; the Web
 * page's "上下反転" turns it over. */
#define LCD_LANDSCAPE_FLIP 0
#define LCD_BUTTON_LONG_MS 1000 /* BOOT button: shorter = reset, longer = peak hold */
/* No battery circuit on this board. */
#define BATTERY_MV_PER_COUNT 0.0f
#define BATTERY_READ_MS  30000
#define BATTERY_GAUGE    0
#define EXPANDER_REFRESH 0
#elif FPV_LCD
/* Waveshare ESP32-C5-Touch-LCD-2.8, header J8:
 * J8 pin 8 (GPIO3, RX) <- XIAO C5 D6 (GPIO11, TX)
 * J8 pin 7 (GPIO2, TX) -> XIAO C5 D7 (GPIO12, RX), plus a common GND. */
#define SDR_RX_PIN 3
#define SDR_TX_PIN 2
/* Touch panel orientation: set to 1 if taps land mirrored or swapped. */
#define TOUCH_SWAP_XY  0
#define TOUCH_MIRROR_X 0
#define TOUCH_MIRROR_Y 0
/* Landscape main screen: 0 = the portrait screen turned 90 degrees clockwise
 * (its top edge on the right), 1 = turned the other way. */
#define LCD_LANDSCAPE_FLIP 0
/* Battery gauge: VBAT reaches the I/O expander's ADC through a 200k/100k
 * divider (1/3). mV = ADC count * BATTERY_MV_PER_COUNT; 3300 mV * 3 / 1023
 * assumes a 10-bit ADC referred to 3.3 V. If the voltage in the serial log
 * ("battery raw=... mv=...") differs from a meter on the battery, scale this
 * by meter / shown. */
#define BATTERY_MV_PER_COUNT 9.677f
/* Test switches for the I/O expander traffic added after the first
 * version (screen start-up investigation):
 * BATTERY_GAUGE 0    no battery reads, no battery level shown
 * EXPANDER_REFRESH 1 rewrite the expander's pins and backlight every 2 s and
 *                    restart the LCD when the expander was lost (off: every
 *                    transfer is a chance for the expander to garble it) */
#define BATTERY_READ_MS  30000 /* battery read interval */
#define BATTERY_GAUGE    1
#define EXPANDER_REFRESH 0
#elif FPV_XIAO_C5
/* A second XIAO ESP32-C5 as the front end (web page only):
 * front D7 (GPIO12, RX) <- receiver C5 D6 (GPIO11, TX)
 * front D6 (GPIO11, TX) -> receiver C5 D7 (GPIO12, RX), plus a common GND.
 * The same pins and wiring as the S3; on the C5 they are UART0's, used here
 * as Serial1 (Serial is the USB port). */
#define SDR_RX_PIN 12
#define SDR_TX_PIN 11
#else
/* XIAO S3 D7 (GPIO44, RX) <- C5 D6 (GPIO11, TX)
 * XIAO S3 D6 (GPIO43, TX) -> C5 D7 (GPIO12, RX), plus a common GND. */
#define SDR_RX_PIN 44
#define SDR_TX_PIN 43
#endif
#define SDR_BAUD   2000000

/* Battery level (LCD build) below which screen and page blink it in red. */
#define BATTERY_LOW_PERCENT 10

/* ---- Default sweep ------------------------------------------------- */
#define DEFAULT_START_MHZ 5650
#define DEFAULT_STOP_MHZ  5850
#define SWEEP_POINTS      1000  /* display points sent to the browser */
