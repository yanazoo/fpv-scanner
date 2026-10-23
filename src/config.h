#pragma once
/* ---- Wi-Fi access point (2.4 GHz) -----------------------------------
 * These values are public (committed to the repository). WPA2 needs a
 * password of at least 8 characters. */
#define AP_SSID     "FPV-Scanner"
#define AP_PASSWORD "fpv-sa00"
#define AP_CHANNEL  1

/* ---- UART link to the XIAO ESP32-C5 (ESP-SDR firmware) --------------- */
#if FPV_LCD
/* Waveshare ESP32-C5-Touch-LCD-2.8, header J8:
 * J8 pin 8 (GPIO3, RX) <- XIAO C5 D6 (GPIO11, TX)
 * J8 pin 7 (GPIO2, TX) -> XIAO C5 D7 (GPIO12, RX), plus a common GND. */
#define SDR_RX_PIN 3
#define SDR_TX_PIN 2
/* Touch panel orientation: set to 1 if taps land mirrored or swapped. */
#define TOUCH_SWAP_XY  0
#define TOUCH_MIRROR_X 0
#define TOUCH_MIRROR_Y 0
#define LCD_BACKLIGHT  100 /* percent */
#else
/* XIAO S3 D7 (GPIO44, RX) <- C5 D6 (GPIO11, TX)
 * XIAO S3 D6 (GPIO43, TX) -> C5 D7 (GPIO12, RX), plus a common GND. */
#define SDR_RX_PIN 44
#define SDR_TX_PIN 43
#endif
#define SDR_BAUD   2000000

/* ---- Default sweep ------------------------------------------------- */
#define DEFAULT_START_MHZ 5650
#define DEFAULT_STOP_MHZ  5850
#define SWEEP_POINTS      1000  /* display points sent to the browser */
