#pragma once
/* State shared between the sweep / web tasks (main.cpp) and the touch LCD
 * (lcd_ui.cpp). Every accessor takes the state lock, so any task may call it. */
#include <Arduino.h>
#include "config.h"

struct SweepConfig {
    unsigned start_mhz = DEFAULT_START_MHZ, stop_mhz = DEFAULT_STOP_MHZ;
    unsigned rate_code = 0; /* step plan, see steps[]: 0: 40, 1: 30, 2: 15, 3: 7.5 MHz */
    unsigned dwell_ms = 5;
    bool max_hold = true;
    int gain = 40; /* -1: hardware AGC, otherwise a PHY gain index */
    /* Display only; changing them does not restart the sweep. */
    int cal_tenths = 0;      /* dBm = dBFS + cal_tenths / 10 */
    int warn_dbfs = -5;      /* "ゲイン下げて" when a point exceeds this (dBFS) */
    unsigned bands = 0x10;   /* FPV band lines shown: bit 0 A, 1 B, 2 E, 3 F, 4 R */
    int markers[4] = {56950, 57320, 57690, 58060}; /* tenths of MHz (R2..R5), -1 = unset */
    bool hold = true, avg = true;      /* peak-hold / averaged trace */
    int ytop = 0, yrange = 85;         /* graph top (dBm) and height (dB) */
    /* LCD build: backlight (%) and seconds without a touch until the screen
     * dims (it goes dark after as long again); 0 = never. */
    int lcd_bright = 100, lcd_sleep = 0;
    /* landscape button panel: seconds it stays out without a tap, 0 = until
     * a button (or the tab) is tapped */
    int lcd_panel = 5;
    bool lcd_land = true;  /* screens in landscape */
    bool lcd_flip = false; /* landscape turned by 180 degrees */
};

/* Sweep frames and noise references: 24-byte header + SWEEP_POINTS codes.
 * Header: magic "FPV1" / "NSE2", start kHz (u32), stop kHz (u32),
 * points (u16), ... (little endian). */
static const size_t FRAME_HEADER = 24;
static const size_t FRAME_SIZE = FRAME_HEADER + SWEEP_POINTS;

struct SweepStatus {
    bool linked;
    unsigned steps, sweep_ms, tune_ms, measure_ms;
    unsigned gain_max; /* highest manual gain the C5 accepts, 0 = not known yet */
    char error[64];
    int wifi_state;     /* 0: host (access point), 1: connecting, 2: client */
    bool wifi_fallback; /* host because the client connection failed */
    char wifi_ip[16];   /* the page's address ("" while connecting) */
};

SweepConfig shared_config();
SweepStatus shared_status();
/* Copies the latest sweep into out when its sequence number differs from
 * seen; returns the current sequence number (0: no sweep yet). */
uint32_t shared_frame(uint8_t *out, uint32_t seen);
/* Copies the noise reference into out; false when none is saved. */
bool shared_noise(uint8_t *out);
/* Applies a "cfg key=value ..." line, exactly as sent by the web page. */
void shared_apply(const char *text);
/* Saves (NSE2 frame) or clears the noise reference; done by the web task. */
void shared_store_noise(const uint8_t *frame);
void shared_clear_noise();
/* Wi-Fi settings and state, for the LCD's Wi-Fi screen (passwords included:
 * the screen shows them). */
struct WifiInfo {
    bool on;       /* Wi-Fi switched on */
    int state;     /* 0: host, 1: connecting, 2: client, 3: off */
    bool fallback; /* host because the client connection failed */
    unsigned reason; /* why the client connection failed (Wi-Fi reason code), 0 = none */
    int mode, wait; /* 0: host, 1: client first; seconds before falling back */
    char ip[16], ssid[33], pass[65], ap_ssid[33], ap_pass[65];
};
WifiInfo shared_wifi();
/* Switches Wi-Fi on or off (saved); used by the touch LCD. */
void shared_set_wifi_on(bool on);
/* Battery level in percent from the LCD board, -1 when unknown. */
void shared_set_battery(int percent);
