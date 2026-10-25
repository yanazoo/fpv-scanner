#pragma once
/* State shared between the sweep / web tasks (main.cpp) and the touch LCD
 * (lcd_ui.cpp). Every accessor takes the state lock, so any task may call it. */
#include <Arduino.h>
#include "config.h"

struct SweepConfig {
    unsigned start_mhz = DEFAULT_START_MHZ, stop_mhz = DEFAULT_STOP_MHZ;
    unsigned rate_code = 0; /* step plan, see steps[]: 0: 40, 1: 30, 2: 15, 3: 7.5 MHz */
    unsigned dwell_ms = 25;
    bool max_hold = true;
    int gain = 10; /* -1: hardware AGC, otherwise a PHY gain index */
    /* Display only; changing them does not restart the sweep. */
    int cal_tenths = 0;      /* dBm = dBFS + cal_tenths / 10 */
    unsigned bands = 0x11;   /* FPV band lines shown: bit 0 A, 1 B, 2 E, 3 F, 4 R */
    int markers[4] = {-1, -1, -1, -1}; /* tenths of MHz, -1 = unset */
    bool hold = false, avg = false;    /* peak-hold / averaged trace */
    int ytop = -17, yrange = 75;       /* graph top (dBm) and height (dB) */
    /* LCD build: backlight (%) and seconds without a touch until the screen
     * dims (it goes dark after as long again); 0 = never. */
    int lcd_bright = 100, lcd_sleep = 0;
    bool lcd_land = false; /* main screen in landscape (settings stay portrait) */
};

/* Sweep frames and noise references: 24-byte header + SWEEP_POINTS codes.
 * Header: magic "FPV1" / "NSE2", start kHz (u32), stop kHz (u32),
 * points (u16), ... (little endian). */
static const size_t FRAME_HEADER = 24;
static const size_t FRAME_SIZE = FRAME_HEADER + SWEEP_POINTS;

struct SweepStatus {
    bool linked;
    unsigned steps, sweep_ms, tune_ms, measure_ms;
    char error[64];
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
/* Battery level in percent from the LCD board, -1 when unknown. */
void shared_set_battery(int percent);
