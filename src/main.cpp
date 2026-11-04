/* Front end (XIAO ESP32-S3, or with FPV_LCD the Waveshare ESP32-C5 touch
 * LCD board): 2.4 GHz access point + captive-portal web page showing a
 * 5 GHz sweep measured by an ESP-SDR XIAO ESP32-C5 over UART. Instead of
 * its own access point it can join a router or a phone's hotspot (client
 * mode), falling back to the access point when that fails. The LCD
 * build also draws the sweep on its own screen (lcd_ui.cpp).
 *
 * The C5 sees about 30 MHz at a time, so the sweep task retunes it in steps,
 * runs a short on-chip spectrum (SPEC) at each step and stitches the centre
 * part of every spectrum into one trace of SWEEP_POINTS display points. */
#include <Arduino.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <WebSocketsServer.h>
#include <WiFi.h>
#include "config.h"
#include "index_html.h"
#include "portal_http.h"
#include "sdr_link.h"
#include "shared.h"
#if FPV_LCD
#include "lcd_ui.h"
#endif

/* Each step keeps only +-half_khz around the LO. The capture rate is at
 * least four times that and the analog filter is narrowed to the step,
 * so strong signals outside the step are attenuated before they can fold
 * (alias) into it. With the widest filter at 40 MS/s, a strong VTX showed
 * up in neighbouring steps at the wrong frequency. */
struct StepPlan {
    unsigned half_khz, rate_code, fft_n, bandwidth_mhz;
};
static const StepPlan steps_plan[] = {
    /* 1024 bins: a display point (1000 per span) never needs finer bins,
     * and half the FFT size halves the C5's FFT time and UART transfer. */
    {20000, 0, 1024, 44}, /* 40 MHz steps, 80 MS/s, 78 kHz bins */
    {15000, 0, 1024, 32}, /* 30 MHz steps, 80 MS/s, 78 kHz bins */
    {7500, 1, 1024, 16},  /* 15 MHz steps, 40 MS/s, 39 kHz bins */
    {3750, 2, 1024, 11},  /* 7.5 MHz steps, 20 MS/s, 20 kHz bins */
};
static const unsigned capture_khz[] = {80000, 40000, 20000};
static double half_use_khz(unsigned plan) {
    return steps_plan[plan].half_khz;
}

/* The pages' WebSocket server, with a check that a page's socket takes
 * data without blocking: WiFiClient::write() waits up to 10 s for a page
 * that stopped reading (a phone switching networks, asleep or gone), which
 * held up every other page, and the library has no way around it. */
class PageServer : public WebSocketsServer {
  public:
    using WebSocketsServer::WebSocketsServer;
    /* lwIP reports a socket writable only with about 2.9 KB of its send
     * buffer free (TCP_SNDLOWAT): more than a frame or a status line. */
    bool writable(uint8_t i) {
        WiFiClient *t = _clients[i].tcp;
        int fd = t ? t->fd() : -1;
        if (fd < 0) return false;
        fd_set set;
        FD_ZERO(&set);
        FD_SET(fd, &set);
        timeval tv = {0, 0};
        return select(fd + 1, nullptr, &set, nullptr, &tv) > 0;
    }
    /* Closes the connection without writing to it (the library's disconnect
     * sends a close frame, which may block); the library then reports it. */
    void drop(uint8_t i) {
        if (_clients[i].tcp) _clients[i].tcp->stop();
    }
};
static PageServer ws(81);
static DNSServer dns;
static SdrLink sdr;
static SemaphoreHandle_t lock;
static SweepConfig config;
static bool config_changed = true;

/* Latest sweep, shared with the web task: 24-byte header + codes. */
static const size_t HEADER = FRAME_HEADER;
static uint8_t frame[HEADER + SWEEP_POINTS];
static uint32_t frame_seq, sent_seq;
static char status_text[1024];
static bool status_dirty = true;
static unsigned gain_max;
static char receiver_error[64];
static bool settings_dirty;
static int battery_percent = -1; /* LCD build only */

/* ---- Wi-Fi: host (own access point) or client of a router / hotspot ----
 * Client mode connects to the saved network at start; when that does not
 * succeed within wait seconds, or a connection is lost for as long, the
 * access point starts instead (until a restart or a new Wi-Fi setting).
 * A changed setting applies at once. The password is never reported. */
enum WifiState { WIFI_HOST, WIFI_CONNECTING, WIFI_CLIENT };
static struct {
    int mode = 0;  /* 0: host, 1: client first */
    int wait = 30; /* seconds: 15, 30 or 60 */
    char ssid[33] = "", pass[65] = "";
    WifiState state = WIFI_HOST;
    bool fallback;      /* host because the client connection failed */
    bool restart;       /* apply the settings (set by the web task) */
    bool lost;          /* client: connection lost since `since` */
    uint32_t since;     /* client: connecting / lost since */
    char ip[16] = "";
} wifi;

/* Noise reference saved from the page: same layout as a sweep frame. */
static uint8_t noise[HEADER + SWEEP_POINTS];
static bool has_noise;
static Preferences prefs;

static void load_settings() {
    prefs.begin("fpv", false);
    SweepConfig &c = config;
    c.start_mhz = prefs.getUInt("start", c.start_mhz);
    c.stop_mhz = prefs.getUInt("stop", c.stop_mhz);
    c.rate_code = prefs.getUInt("rate", c.rate_code);
    c.dwell_ms = prefs.getUInt("dwell", c.dwell_ms);
    c.max_hold = prefs.getBool("det", c.max_hold);
    c.gain = prefs.getInt("gain", c.gain);
    c.cal_tenths = prefs.getInt("cal", c.cal_tenths);
    c.warn_dbfs = prefs.getInt("warn", c.warn_dbfs);
    c.bands = prefs.getUInt("bands", c.bands);
    prefs.getBytes("markers", c.markers, sizeof(c.markers));
    c.hold = prefs.getBool("hold", c.hold);
    c.avg = prefs.getBool("avg", c.avg);
    c.ytop = prefs.getInt("ytop", c.ytop);
    c.yrange = prefs.getInt("yrange", c.yrange);
    c.lcd_bright = prefs.getInt("lbright", c.lcd_bright);
    c.lcd_sleep = prefs.getInt("lsleep", c.lcd_sleep);
    c.lcd_panel = prefs.getInt("lpanel", c.lcd_panel);
    c.lcd_land = prefs.getBool("lland", c.lcd_land);
    c.lcd_flip = prefs.getBool("lflip", c.lcd_flip);
    wifi.mode = prefs.getInt("wmode", wifi.mode);
    wifi.wait = prefs.getInt("wwait", wifi.wait);
    prefs.getString("wssid", wifi.ssid, sizeof(wifi.ssid));
    prefs.getString("wpass", wifi.pass, sizeof(wifi.pass));
    /* NSE2: references saved by earlier builds did not track the receiver
     * settings they were taken with, so they are discarded once. */
    has_noise = prefs.getBytes("noise", noise, sizeof(noise)) == sizeof(noise) && !memcmp(noise, "NSE2", 4);
    if (!has_noise && prefs.isKey("noise")) prefs.remove("noise");
    if (prefs.isKey("spur")) prefs.remove("spur"); /* setting of earlier builds */
}

static void save_settings() {
    xSemaphoreTake(lock, portMAX_DELAY);
    SweepConfig c = config;
    settings_dirty = false;
    xSemaphoreGive(lock);
    prefs.putUInt("start", c.start_mhz);
    prefs.putUInt("stop", c.stop_mhz);
    prefs.putUInt("rate", c.rate_code);
    prefs.putUInt("dwell", c.dwell_ms);
    prefs.putBool("det", c.max_hold);
    prefs.putInt("gain", c.gain);
    prefs.putInt("cal", c.cal_tenths);
    prefs.putInt("warn", c.warn_dbfs);
    prefs.putUInt("bands", c.bands);
    prefs.putBytes("markers", c.markers, sizeof(c.markers));
    prefs.putBool("hold", c.hold);
    prefs.putBool("avg", c.avg);
    prefs.putInt("ytop", c.ytop);
    prefs.putInt("yrange", c.yrange);
    prefs.putInt("lbright", c.lcd_bright);
    prefs.putInt("lsleep", c.lcd_sleep);
    prefs.putInt("lpanel", c.lcd_panel);
    prefs.putBool("lland", c.lcd_land);
    prefs.putBool("lflip", c.lcd_flip);
}

/* JSON string contents: quotes, backslashes and control characters escaped. */
static void json_escape(const char *in, char *out, size_t size) {
    size_t n = 0;
    for (; *in && n + 7 < size; in++) {
        unsigned char ch = *in;
        if (ch == '"' || ch == '\\') out[n++] = '\\', out[n++] = ch;
        else if (ch < 0x20) n += snprintf(out + n, size - n, "\\u%04x", ch);
        else out[n++] = ch;
    }
    out[n] = '\0';
}

static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static void put16(uint8_t *p, uint16_t v) { memcpy(p, &v, 2); }

/* The settings reported are those of the sweep that just finished (applied),
 * so the page can show whether a change has taken effect. */
static struct {
    bool linked;
    const char *info = "";
    SweepConfig c;
    unsigned steps, sweep_ms;
} last;
/* Per-sweep totals of SdrLink::tune_ms / measure_ms, shown on the page. */
static uint32_t sweep_tune_ms, sweep_measure_ms;
static void set_status(bool linked, const char *info, const SweepConfig &c, unsigned steps, unsigned sweep_ms) {
    xSemaphoreTake(lock, portMAX_DELAY);
    last.linked = linked;
    last.info = info;
    last.c = c;
    last.steps = steps;
    last.sweep_ms = sweep_ms;
    char ssid[sizeof(wifi.ssid) * 6];
    json_escape(wifi.ssid, ssid, sizeof(ssid));
    snprintf(status_text, sizeof(status_text),
             "{\"type\":\"status\",\"linked\":%s,\"info\":\"%s\",\"start\":%u,\"stop\":%u,\"rate\":%u,"
             "\"dwell\":%u,\"det\":%u,\"gain\":%d,\"gain_max\":%u,\"steps\":%u,\"step_khz\":%u,"
             "\"sweep_ms\":%u,\"error\":\"%s\",\"cal\":%d,\"bands\":%u,\"noise\":%s,"
             "\"markers\":[%d,%d,%d,%d],\"hold\":%u,\"avg\":%u,\"ytop\":%d,\"yrange\":%d,"
             "\"tune_ms\":%u,\"measure_ms\":%u,\"bat\":%d,\"bat_low\":%d,"
             "\"lcd\":%d,\"lbright\":%d,\"lsleep\":%d,\"lflip\":%d,\"warn\":%d,"
             "\"wmode\":%d,\"wwait\":%d,\"wssid\":\"%s\",\"wpass\":%d,\"wstate\":%d,\"wfb\":%d,"
             "\"wip\":\"%s\",\"ap\":\"%s\",\"mdns\":\"%s\"}",
             linked ? "true" : "false", info, c.start_mhz, c.stop_mhz, c.rate_code, c.dwell_ms,
             c.max_hold ? 1 : 0, c.gain, gain_max, steps, (unsigned)(2 * half_use_khz(c.rate_code)), sweep_ms,
             receiver_error, config.cal_tenths, config.bands, has_noise ? "true" : "false", config.markers[0],
             config.markers[1], config.markers[2], config.markers[3], config.hold, config.avg, config.ytop,
             config.yrange, (unsigned)sweep_tune_ms, (unsigned)sweep_measure_ms, battery_percent, BATTERY_LOW_PERCENT,
             FPV_LCD ? 1 : 0, config.lcd_bright, config.lcd_sleep, config.lcd_flip ? 1 : 0, config.warn_dbfs,
             wifi.mode, wifi.wait, ssid, wifi.pass[0] ? 1 : 0, (int)wifi.state, wifi.fallback ? 1 : 0, wifi.ip,
             AP_SSID, MDNS_NAME);
    status_dirty = true;
    xSemaphoreGive(lock);
}
/* Display-only settings and the noise reference take effect at once. */
static void refresh_status() {
    xSemaphoreTake(lock, portMAX_DELAY);
    auto l = last;
    xSemaphoreGive(lock);
    set_status(l.linked, l.info, l.c, l.steps, l.sweep_ms);
}

/* Returns false only when the link broke. A rejected gain falls back to
 * the hardware AGC and is reported on the page. */
static bool apply_receiver(SweepConfig &c) {
    char reply[96], cmd[32];
    receiver_error[0] = '\0';
    if (sdr.command("LIMITS?", reply, sizeof(reply))) {
        const char *g = strstr(reply, "\"gain\":[");
        unsigned lo, hi;
        if (g && sscanf(g, "\"gain\":[%u,%u", &lo, &hi) == 2) gain_max = hi;
    }
    snprintf(cmd, sizeof(cmd), "BANDWIDTH %u", steps_plan[c.rate_code].bandwidth_mhz);
    if (!sdr.command(cmd)) return false;
    if (c.gain >= 0) {
        snprintf(cmd, sizeof(cmd), "GAIN MANUAL %d", c.gain);
        reply[0] = '\0';
        if (sdr.command(cmd, reply, sizeof(reply)) && !strcmp(reply, "OK")) return true;
        if (strncmp(reply, "ERR", 3)) return false;
        snprintf(receiver_error, sizeof(receiver_error), "ゲイン%dは範囲外(2〜%u)のため自動に変更", c.gain, gain_max);
        c.gain = -1;
        xSemaphoreTake(lock, portMAX_DELAY);
        config.gain = -1;
        settings_dirty = true;
        xSemaphoreGive(lock);
    }
    return sdr.command("GAIN HARDWARE");
}

static bool changed_since_start(void) {
    xSemaphoreTake(lock, portMAX_DELAY);
    bool changed = config_changed;
    xSemaphoreGive(lock);
    return changed;
}

static void publish(const SweepConfig &c, unsigned elapsed_ms, unsigned steps, const uint8_t *trace) {
    xSemaphoreTake(lock, portMAX_DELAY);
    memcpy(frame, "FPV1", 4);
    put32(frame + 4, c.start_mhz * 1000);
    put32(frame + 8, c.stop_mhz * 1000);
    put16(frame + 12, SWEEP_POINTS);
    put16(frame + 14, elapsed_ms > 65535 ? 65535 : elapsed_ms);
    put32(frame + 16, ++frame_seq);
    frame[20] = c.rate_code;
    frame[21] = c.max_hold;
    frame[22] = steps;
    frame[23] = 0;
    memcpy(frame + HEADER, trace, SWEEP_POINTS);
    xSemaphoreGive(lock);
}

static void sweep_task(void *) {
    static uint8_t bins[2048];
    static uint8_t trace[SWEEP_POINTS];
    static bool filled[SWEEP_POINTS];
    SweepConfig c;
    unsigned shown_start = 0, shown_stop = 0, shown_rate = ~0u;
    for (;;) {
        if (!sdr.connected()) {
            xSemaphoreTake(lock, portMAX_DELAY);
            c = config;
            xSemaphoreGive(lock);
            set_status(false, "C5 not found", c, 0, 0);
            if (!sdr.connect()) {
                delay(1000);
                continue;
            }
            xSemaphoreTake(lock, portMAX_DELAY);
            config_changed = true;
            xSemaphoreGive(lock);
        }
        xSemaphoreTake(lock, portMAX_DELAY);
        c = config;
        bool changed = config_changed;
        config_changed = false;
        xSemaphoreGive(lock);
        if (changed && !apply_receiver(c)) {
            sdr.connect();
            continue;
        }

        /* Skip the bins next to the LO, where DC correction leaves residue. */
        const StepPlan &plan = steps_plan[c.rate_code];
        const unsigned fft_n = plan.fft_n;
        const double fs = capture_khz[plan.rate_code], half_use = plan.half_khz;
        const double start = c.start_mhz * 1000.0, stop = c.stop_mhz * 1000.0, span = stop - start;
        const unsigned steps = (unsigned)ceil(span / (2 * half_use));
        /* Each step is published as soon as it is measured; points not yet
         * measured in this sweep keep the previous sweep's values. Code 0
         * marks "no data yet" after the span or step plan changes. */
        if (c.start_mhz != shown_start || c.stop_mhz != shown_stop || c.rate_code != shown_rate) {
            memset(trace, 0, sizeof(trace));
            shown_start = c.start_mhz;
            shown_stop = c.stop_mhz;
            shown_rate = c.rate_code;
        }
        memset(filled, 0, sizeof(filled));
        uint32_t t0 = millis(), tune_total = 0, measure_total = 0;
        bool ok = true, restarted = false;
        for (unsigned s = 0; s < steps && ok; s++) {
            /* A new setting restarts the sweep instead of waiting for it to end. */
            if (changed_since_start()) {
                restarted = true;
                break;
            }
            unsigned centre_mhz = (unsigned)lround((start + (2 * s + 1) * half_use) / 1000.0);
            ok = sdr.spectrum(centre_mhz, plan.rate_code, fft_n, c.dwell_ms, c.max_hold, bins);
            if (!ok) break;
            tune_total += sdr.tune_ms;
            measure_total += sdr.measure_ms;
            int first = SWEEP_POINTS, last_i = -1;
            for (unsigned k = 0; k < fft_n; k++) {
                int signed_k = k < fft_n / 2 ? (int)k : (int)k - (int)fft_n;
                if (abs(signed_k) <= 2) continue;
                /* On 5 GHz the C5 delivers the spectrum inverted: with the LO at
                 * 5710 MHz, R2 (5695) showed at 5725 and R3 (5732) at 5688,
                 * mirrored about the LO in every step for R2..R5. */
                double offset = (centre_mhz >= 4000 ? -signed_k : signed_k) * fs / fft_n;
                if (fabs(offset) > half_use) continue;
                double f = centre_mhz * 1000.0 + offset;
                if (f < start || f >= stop) continue;
                unsigned i = (unsigned)((f - start) / span * SWEEP_POINTS);
                if (i >= SWEEP_POINTS) continue;
                uint8_t v = bins[k] ? bins[k] : 1; /* 0 is reserved for "no data" */
                if (!filled[i] || v > trace[i]) trace[i] = v;
                filled[i] = true;
                if ((int)i < first) first = i;
                if ((int)i > last_i) last_i = i;
            }
            /* Points narrower than one FFT bin (small spans) are interpolated,
             * from the previous step's last point across the step join. */
            int prev = -1;
            for (int j = first - 1; j >= 0 && j >= first - 64; j--)
                if (filled[j]) {
                    prev = j;
                    break;
                }
            if (s == 0)
                for (int j = 0; j < first && first < SWEEP_POINTS; j++) trace[j] = trace[first];
            for (int i = first; i <= last_i; i++) {
                if (!filled[i]) continue;
                for (int j = prev + 1; prev >= 0 && j < i; j++) {
                    trace[j] = trace[prev] + (trace[i] - trace[prev]) * (j - prev) / (i - prev);
                    filled[j] = true;
                }
                prev = i;
            }
            /* The last step also fills the points past its final bin. */
            if (s + 1 == steps)
                for (int j = last_i + 1; j < SWEEP_POINTS && last_i >= 0; j++) trace[j] = trace[last_i];
            publish(c, millis() - t0, steps, trace);
        }
        if (!ok) {
            sdr.connect(); /* resynchronize after a broken transfer */
            continue;
        }
        if (restarted) continue;
        xSemaphoreTake(lock, portMAX_DELAY);
        sweep_tune_ms = tune_total;
        sweep_measure_ms = measure_total;
        xSemaphoreGive(lock);
        set_status(true, sdr.info(), c, steps, millis() - t0);
    }
}

/* "cfg start=5650 stop=5850 rate=1 dwell=25 det=1 gain=-1 cal=0 bands=17
 *  m1=58000 m2=-1 m3=-1 m4=-1 hold=0 avg=0 ytop=-20 yrange=90",
 * sent by the page's apply buttons; every accepted change is saved. */
static void handle_text(const char *text) {
    if (strncmp(text, "cfg", 3)) return;
    xSemaphoreTake(lock, portMAX_DELAY);
    SweepConfig c = config;
    xSemaphoreGive(lock);
    const char *p = text + 3;
    char key[16];
    int value, used;
    while (sscanf(p, " %15[a-z0-9]=%d%n", key, &value, &used) == 2) {
        p += used;
        if (!strcmp(key, "start")) c.start_mhz = constrain(value, 2300, 5995);
        else if (!strcmp(key, "stop")) c.stop_mhz = constrain(value, 2305, 6000);
        else if (!strcmp(key, "rate")) c.rate_code = constrain(value, 0, 3);
        else if (!strcmp(key, "dwell")) c.dwell_ms = constrain(value, 5, 500);
        else if (!strcmp(key, "det")) c.max_hold = value != 0;
        else if (!strcmp(key, "gain")) c.gain = constrain(value, -1, 90);
        else if (!strcmp(key, "cal")) c.cal_tenths = constrain(value, -1000, 1000);
        else if (!strcmp(key, "warn")) c.warn_dbfs = constrain(value, -60, 0);
        else if (!strcmp(key, "bands")) c.bands = value & 0x1f;
        else if (key[0] == 'm' && key[1] >= '1' && key[1] <= '4' && !key[2])
            c.markers[key[1] - '1'] = value < 0 ? -1 : constrain(value, 23000, 60000);
        else if (!strcmp(key, "hold")) c.hold = value != 0;
        else if (!strcmp(key, "avg")) c.avg = value != 0;
        else if (!strcmp(key, "ytop")) c.ytop = constrain(value, -150, 50);
        else if (!strcmp(key, "yrange")) c.yrange = constrain(value, 10, 150);
        else if (!strcmp(key, "lbright")) c.lcd_bright = constrain(value, 10, 100);
        else if (!strcmp(key, "lsleep")) c.lcd_sleep = constrain(value, 0, 600);
        else if (!strcmp(key, "lpanel")) c.lcd_panel = constrain(value, 0, 30);
        else if (!strcmp(key, "lland")) c.lcd_land = value != 0;
        else if (!strcmp(key, "lflip")) c.lcd_flip = value != 0;
    }
    if (c.stop_mhz < c.start_mhz + 5) c.stop_mhz = c.start_mhz + 5;
    xSemaphoreTake(lock, portMAX_DELAY);
    const SweepConfig &o = config;
    if (c.start_mhz != o.start_mhz || c.stop_mhz != o.stop_mhz || c.rate_code != o.rate_code ||
        c.dwell_ms != o.dwell_ms || c.max_hold != o.max_hold || c.gain != o.gain)
        config_changed = true;
    /* A noise reference taken at another gain, rate or detector no longer
     * matches the trace, so it is dropped rather than shown misleadingly. */
    bool drop_noise = has_noise && (c.gain != o.gain || c.rate_code != o.rate_code || c.max_hold != o.max_hold);
    config = c;
    settings_dirty = true;
    xSemaphoreGive(lock);
    if (drop_noise) {
        has_noise = false;
        prefs.remove("noise");
    }
    refresh_status();
}

static void send_all(bool binary, const void *data, size_t size);

static void store_noise(const uint8_t *data, size_t length) {
    if (length != sizeof(noise) || memcmp(data, "NSE2", 4)) return;
    xSemaphoreTake(lock, portMAX_DELAY);
    memcpy(noise, data, sizeof(noise));
    has_noise = true;
    xSemaphoreGive(lock);
    prefs.putBytes("noise", noise, sizeof(noise));
    send_all(true, noise, sizeof(noise));
    refresh_status();
}

static void clear_noise() {
    if (has_noise) prefs.remove("noise");
    xSemaphoreTake(lock, portMAX_DELAY);
    has_noise = false;
    xSemaphoreGive(lock);
    refresh_status();
}

/* "wifi\n<mode>\n<wait>\n<ssid>\n<password>\n<keep>" from the page (lines,
 * since names may contain spaces); keep=1 keeps the saved password. The
 * settings are saved and applied at once. */
static void handle_wifi(const char *text) {
    char field[6][65] = {};
    int n = 0;
    for (const char *p = text; n < 6; n++) {
        const char *e = strchr(p, '\n');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len >= sizeof(field[0])) return;
        memcpy(field[n], p, len);
        if (!e) {
            n++;
            break;
        }
        p = e + 1;
    }
    if (n != 6 || strcmp(field[0], "wifi")) return;
    int mode = atoi(field[1]) ? 1 : 0, wait = atoi(field[2]);
    bool keep = atoi(field[5]) != 0;
    size_t pass_len = strlen(field[4]);
    if ((wait != 15 && wait != 30 && wait != 60) || strlen(field[3]) > 32 || (pass_len && pass_len < 8)) return;
    xSemaphoreTake(lock, portMAX_DELAY);
    const char *pass = keep ? wifi.pass : field[4];
    bool restart = mode != wifi.mode || strcmp(field[3], wifi.ssid) || strcmp(pass, wifi.pass) ||
                   (wifi.fallback && mode == 1); /* try the network again */
    wifi.mode = mode;
    wifi.wait = wait;
    strlcpy(wifi.ssid, field[3], sizeof(wifi.ssid));
    if (!keep) strlcpy(wifi.pass, field[4], sizeof(wifi.pass));
    if (restart) wifi.restart = true;
    xSemaphoreGive(lock);
    prefs.putInt("wmode", wifi.mode);
    prefs.putInt("wwait", wifi.wait);
    prefs.putString("wssid", wifi.ssid);
    prefs.putString("wpass", wifi.pass);
    refresh_status();
}

/* Noise reference requests from the LCD, carried out by the web task
 * because they broadcast to the pages. */
static uint8_t noise_request[FRAME_SIZE];
static bool noise_store_requested, noise_clear_requested;

static void on_ws(uint8_t client, WStype_t type, uint8_t *payload, size_t length) {
    if (type == WStype_DISCONNECTED) {
        Serial.printf("[%lu ms] page %u disconnected\n", millis(), client);
    } else if (type == WStype_CONNECTED) {
        Serial.printf("[%lu ms] page %u connected\n", millis(), client);
        /* A (re)connecting page gets the latest sweep at once. */
        static uint8_t latest[sizeof(frame)];
        xSemaphoreTake(lock, portMAX_DELAY);
        bool any = frame_seq != 0;
        if (any) memcpy(latest, frame, sizeof(latest));
        status_dirty = true;
        xSemaphoreGive(lock);
        if (has_noise) ws.sendBIN(client, noise, sizeof(noise));
        if (any) ws.sendBIN(client, latest, sizeof(latest));
    } else if (type == WStype_TEXT) {
        char text[200];
        size_t n = length < sizeof(text) - 1 ? length : sizeof(text) - 1;
        memcpy(text, payload, n);
        text[n] = '\0';
        if (!strcmp(text, "noise clear"))
            clear_noise();
        else if (!strncmp(text, "wifi\n", 5))
            handle_wifi(text);
        else
            handle_text(text);
    } else if (type == WStype_BIN) {
        store_noise(payload, length);
    }
}

/* Sends to every page whose socket takes it now; a page that is not ready
 * misses this frame. A page that stays stuck for 2 s is dropped without
 * waiting for it (it reconnects by itself), so one slow phone or PC never
 * holds up the others, the LCD or the sweep. */
static uint32_t stuck_since[WEBSOCKETS_SERVER_CLIENT_MAX];
static void send_all(bool binary, const void *data, size_t size) {
    uint32_t now = millis();
    for (uint8_t i = 0; i < WEBSOCKETS_SERVER_CLIENT_MAX; i++) {
        if (!ws.clientIsConnected(i)) {
            stuck_since[i] = 0;
            continue;
        }
        if (!ws.writable(i)) {
            if (!stuck_since[i]) stuck_since[i] = now | 1;
            else if (now - stuck_since[i] > 2000) {
                Serial.printf("[%lu ms] page %u not reading, dropped\n", now, i);
                ws.drop(i);
                stuck_since[i] = 0;
            }
            continue;
        }
        stuck_since[i] = 0;
        if (binary) ws.sendBIN(i, (const uint8_t *)data, size);
        else ws.sendTXT(i, (const char *)data, size);
    }
}

/* WebSocket traffic and settings storage run in their own task, so page
 * requests never delay spectrum frames. */
static void ws_task(void *) {
    static uint8_t out[HEADER + SWEEP_POINTS];
    static char status_out[sizeof(status_text)];
    uint32_t last_send = 0;
    for (;;) {
        ws.loop();
        /* Status at least every 2 s, so pages can tell a long sweep from a
         * dead connection (they reconnect after 5 s of silence). */
        if (millis() - last_send > 2000) {
            xSemaphoreTake(lock, portMAX_DELAY);
            status_dirty = true;
            xSemaphoreGive(lock);
        }
        bool send_frame = false, send_status = false;
        xSemaphoreTake(lock, portMAX_DELAY);
        if (frame_seq != sent_seq) {
            memcpy(out, frame, sizeof(out));
            sent_seq = frame_seq;
            send_frame = true;
        }
        if (status_dirty) {
            strlcpy(status_out, status_text, sizeof(status_out));
            status_dirty = false;
            send_status = true;
        }
        bool save = settings_dirty;
        bool store = noise_store_requested, clear = noise_clear_requested;
        noise_store_requested = noise_clear_requested = false;
        xSemaphoreGive(lock);
        if (save) save_settings();
        if (clear) clear_noise();
        if (store) store_noise(noise_request, sizeof(noise_request));
        if (send_frame) send_all(true, out, sizeof(out));
        if (send_status) {
            send_all(false, status_out, strlen(status_out));
            last_send = millis();
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

/* DNS answers run in their own task, so phones joining the access point
 * get them at once whatever the page server is doing. Every name resolves
 * to the access point (captive portal). */
static void dns_task(void *) {
    for (;;) {
        dns.processNextRequest();
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

SweepConfig shared_config() {
    xSemaphoreTake(lock, portMAX_DELAY);
    SweepConfig c = config;
    xSemaphoreGive(lock);
    return c;
}

SweepStatus shared_status() {
    SweepStatus s;
    xSemaphoreTake(lock, portMAX_DELAY);
    s.linked = last.linked;
    s.steps = last.steps;
    s.sweep_ms = last.sweep_ms;
    s.tune_ms = sweep_tune_ms;
    s.measure_ms = sweep_measure_ms;
    s.gain_max = gain_max;
    strlcpy(s.error, receiver_error, sizeof(s.error));
    s.wifi_state = wifi.state;
    s.wifi_fallback = wifi.fallback;
    strlcpy(s.wifi_ip, wifi.ip, sizeof(s.wifi_ip));
    xSemaphoreGive(lock);
    return s;
}

uint32_t shared_frame(uint8_t *out, uint32_t seen) {
    xSemaphoreTake(lock, portMAX_DELAY);
    uint32_t seq = frame_seq;
    if (seq != seen) memcpy(out, frame, sizeof(frame));
    xSemaphoreGive(lock);
    return seq;
}

bool shared_noise(uint8_t *out) {
    xSemaphoreTake(lock, portMAX_DELAY);
    bool any = has_noise;
    if (any) memcpy(out, noise, sizeof(noise));
    xSemaphoreGive(lock);
    return any;
}

void shared_apply(const char *text) { handle_text(text); }

void shared_store_noise(const uint8_t *data) {
    xSemaphoreTake(lock, portMAX_DELAY);
    memcpy(noise_request, data, sizeof(noise_request));
    noise_store_requested = true;
    xSemaphoreGive(lock);
}

void shared_clear_noise() {
    xSemaphoreTake(lock, portMAX_DELAY);
    noise_clear_requested = true;
    xSemaphoreGive(lock);
}

void shared_set_battery(int percent) {
    xSemaphoreTake(lock, portMAX_DELAY);
    bool changed = percent != battery_percent;
    battery_percent = percent;
    xSemaphoreGive(lock);
    if (changed) refresh_status();
}

/* ---- Wi-Fi modes (run from loop()) ---- */
static void set_wifi_state(WifiState state, bool fallback, const char *ip) {
    xSemaphoreTake(lock, portMAX_DELAY);
    wifi.state = state;
    wifi.fallback = fallback;
    strlcpy(wifi.ip, ip, sizeof(wifi.ip));
    xSemaphoreGive(lock);
    refresh_status();
}

/* The radio stays on 2.4 GHz (the dual-band ESP32-C5), away from the
 * 5 GHz band being measured; needs Wi-Fi started. */
static void radio_2g_only() {
#if SOC_WIFI_SUPPORT_5G
    WiFi.setBandMode(WIFI_BAND_MODE_2G_ONLY);
#endif
}

static void start_host(bool fallback) {
    MDNS.end();
    WiFi.mode(WIFI_AP);
    radio_2g_only();
    WiFi.softAP(AP_SSID, AP_PASSWORD, AP_CHANNEL, 0, AP_MAX_CLIENTS);
    WiFi.setSleep(false);
    set_wifi_state(WIFI_HOST, fallback, WiFi.softAPIP().toString().c_str());
    Serial.printf("[%lu ms] Wi-Fi host%s: %s  http://%s/\n", millis(), fallback ? " (client failed)" : "", AP_SSID,
                  wifi.ip);
}

static void start_client() {
    MDNS.end();
    WiFi.mode(WIFI_STA);
    radio_2g_only();
    WiFi.setSleep(false);
    WiFi.setAutoReconnect(true);
    WiFi.begin(wifi.ssid, wifi.pass[0] ? wifi.pass : nullptr);
    wifi.since = millis();
    wifi.lost = false;
    set_wifi_state(WIFI_CONNECTING, false, "");
    Serial.printf("[%lu ms] Wi-Fi client: connecting to %s\n", millis(), wifi.ssid);
}

static void start_wifi() {
    if (wifi.mode == 1 && wifi.ssid[0]) start_client();
    else start_host(false);
}

static void poll_wifi() {
    xSemaphoreTake(lock, portMAX_DELAY);
    bool restart = wifi.restart;
    wifi.restart = false;
    xSemaphoreGive(lock);
    if (restart) {
        start_wifi();
        return;
    }
    uint32_t now = millis(), limit = (uint32_t)wifi.wait * 1000;
    bool up = WiFi.status() == WL_CONNECTED;
    if (wifi.state == WIFI_CONNECTING) {
        if (up) {
            MDNS.begin(MDNS_NAME);
            MDNS.addService("http", "tcp", 80);
            set_wifi_state(WIFI_CLIENT, false, WiFi.localIP().toString().c_str());
            Serial.printf("[%lu ms] Wi-Fi client: %s  http://%s/  http://%s.local/\n", now, wifi.ssid, wifi.ip,
                          MDNS_NAME);
        } else if (now - wifi.since >= limit) {
            start_host(true);
        }
    } else if (wifi.state == WIFI_CLIENT) {
        if (!up && !wifi.lost) {
            wifi.lost = true;
            wifi.since = now;
            Serial.printf("[%lu ms] Wi-Fi client: connection lost\n", now);
        } else if (!up && now - wifi.since >= limit) {
            start_host(true);
        } else if (up && wifi.lost) {
            wifi.lost = false;
            set_wifi_state(WIFI_CLIENT, false, WiFi.localIP().toString().c_str());
        }
    }
}

void setup() {
    Serial.begin(115200);
    lock = xSemaphoreCreateMutex();
    load_settings();
    last.c = config;
    /* The screen comes first; Wi-Fi start-up (and the band setting) takes a
     * moment and never holds it back. */
#if FPV_LCD
    lcd_ui_begin();
#endif
    start_wifi();
    /* Captive portal on the access point: every name resolves to it (the
     * access point's address, also when it starts later as the fallback). */
    dns.setErrorReplyCode(DNSReplyCode::NoError);
    dns.start(53, "*", IPAddress(192, 168, 4, 1));
    portal_http_begin(INDEX_HTML);
    ws.begin();
    ws.onEvent(on_ws);
    /* No library heartbeat: its pings could block on a stuck page. Pages
     * that stop reading are dropped by send_all(); a page reconnects after
     * 5 s without data. */
    sdr.begin(SDR_RX_PIN, SDR_TX_PIN, SDR_BAUD);
    /* The ESP32-C5 (LCD build) has a single core. */
    const BaseType_t web_core = portNUM_PROCESSORS > 1 ? 1 : 0;
    xTaskCreatePinnedToCore(sweep_task, "sweep", 8192, nullptr, 2, nullptr, 0);
    /* Priority 1, as the LCD and loop(): the WebSockets library waits for a
     * slow page in a busy loop that would otherwise starve them. */
    xTaskCreatePinnedToCore(ws_task, "ws", 8192, nullptr, 1, nullptr, web_core);
    xTaskCreatePinnedToCore(dns_task, "dns", 4096, nullptr, 3, nullptr, web_core);
}

void loop() {
    poll_wifi();
    portal_http_poll();
    delay(2);
}
