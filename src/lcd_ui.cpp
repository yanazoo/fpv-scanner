#if FPV_LCD
/* Touch LCD screens (240x320 portrait).
 *
 * Main: status line, spectrum (dBm axis, FPV band lines, markers), marker
 * table (level, channel, frequency) and buttons. Tapping or dragging on
 * the graph moves the selected marker (snapping to a shown FPV channel);
 * tapping a table row selects the marker.
 * Settings: the page's display settings and the noise reference.
 *
 * Settings go through the same "cfg ..." lines as the web page, so the
 * screen and the page stay in step and both are saved. Peak hold and the
 * average are kept per screen (the LCD and every page have their own).
 *
 * Without a full frame buffer (150 KB) the screen is drawn in strips of
 * STRIP rows: the whole scene is drawn into each strip, clipped. */
#include "lcd_ui.h"
#include <Arduino.h>
#include "lcd_board.h"
#include "lcd_font.h"
#include "shared.h"

namespace {
constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}
const uint16_t BLACK = 0, WHITE = rgb(230, 237, 243), GRAY = rgb(139, 148, 158), GRID = rgb(40, 40, 40),
               PANEL = rgb(33, 38, 45), BORDER = rgb(70, 76, 84), GREEN = rgb(63, 185, 80), RED = rgb(248, 81, 73),
               YELLOW = rgb(242, 204, 96), BLUE = rgb(121, 192, 255), PEAK = rgb(255, 123, 114),
               NOISE = rgb(120, 128, 138), ACCENT = rgb(31, 111, 235);
const uint16_t MCOL[4] = {rgb(255, 255, 102), rgb(102, 255, 255), rgb(255, 102, 255), rgb(102, 255, 102)};
const char BAND_NAME[] = "ABEFR";
const uint16_t BCOL[5] = {rgb(255, 158, 100), rgb(158, 206, 106), rgb(187, 154, 247), rgb(125, 207, 255),
                          rgb(247, 118, 142)};
const uint16_t BANDS[5][8] = {
    {5865, 5845, 5825, 5805, 5785, 5765, 5745, 5725}, {5733, 5752, 5771, 5790, 5809, 5828, 5847, 5866},
    {5705, 5685, 5665, 5645, 5885, 5905, 5925, 5945}, {5740, 5760, 5780, 5800, 5820, 5840, 5860, 5880},
    {5658, 5695, 5732, 5769, 5806, 5843, 5880, 5917},
};
uint16_t dim(uint16_t c) { return (c >> 1) & 0x7BEF; }

/* ---- drawing into the current strip ---- */
const int STRIP = 40;
uint16_t *buf;
int strip_y;

void fill(int x, int y, int w, int h, uint16_t c) {
    int y0 = max(y, strip_y), y1 = min(y + h, strip_y + STRIP), x0 = max(x, 0), x1 = min(x + w, LCD_W);
    for (int yy = y0; yy < y1; yy++) {
        uint16_t *p = buf + (yy - strip_y) * LCD_W;
        for (int xx = x0; xx < x1; xx++) p[xx] = c;
    }
}
void pixel(int x, int y, uint16_t c) {
    if ((unsigned)x < (unsigned)LCD_W && y >= strip_y && y < strip_y + STRIP) buf[(y - strip_y) * LCD_W + x] = c;
}
void vline(int x, int y0, int y1, uint16_t c) {
    if (y0 > y1) std::swap(y0, y1);
    fill(x, y0, 1, y1 - y0 + 1, c);
}
void hline(int x0, int x1, int y, uint16_t c) { fill(x0, y, x1 - x0 + 1, 1, c); }

const LcdGlyph *glyph(uint16_t code) {
    unsigned lo = 0, hi = lcd_font_count;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if (lcd_font_glyphs[mid].code < code) lo = mid + 1;
        else hi = mid;
    }
    if (lo < lcd_font_count && lcd_font_glyphs[lo].code == code) return &lcd_font_glyphs[lo];
    return code == '?' ? &lcd_font_glyphs[0] : glyph('?');
}
uint16_t next_code(const char *&s) {
    uint8_t c = *s++;
    if (c < 0x80) return c;
    if ((c & 0xE0) == 0xC0 && *s) return ((c & 0x1F) << 6) | (*s++ & 0x3F);
    if ((c & 0xF0) == 0xE0 && s[0] && s[1]) {
        uint16_t v = ((c & 0x0F) << 12) | ((s[0] & 0x3F) << 6) | (s[1] & 0x3F);
        s += 2;
        return v;
    }
    return '?';
}
int text_width(const char *s) {
    int w = 0;
    while (*s) w += glyph(next_code(s))->width;
    return w;
}
/* 16 px high text, transparent background; returns the x after it. */
int text(int x, int y, const char *s, uint16_t c) {
    bool visible = y + 16 > strip_y && y < strip_y + STRIP;
    while (*s) {
        const LcdGlyph *g = glyph(next_code(s));
        if (visible) {
            const uint8_t *bits = lcd_font_bits + g->offset;
            for (int r = 0; r < 16; r++) {
                unsigned row = g->width == 16 ? (bits[2 * r] << 8) | bits[2 * r + 1] : bits[r];
                for (int b = 0; b < g->width; b++)
                    if (row & (1u << (g->width - 1 - b))) pixel(x + b, y + r, c);
            }
        }
        x += g->width;
    }
    return x;
}
void text_right(int right, int y, const char *s, uint16_t c) { text(right - text_width(s), y, s, c); }
void text_center(int cx, int y, const char *s, uint16_t c) { text(cx - text_width(s) / 2, y, s, c); }

struct Rect {
    int x, y, w, h;
    bool in(int px, int py) const { return px >= x && px < x + w && py >= y && py < y + h; }
};
void button(const Rect &r, const char *label, bool on = false) {
    fill(r.x, r.y, r.w, r.h, on ? ACCENT : PANEL);
    hline(r.x, r.x + r.w - 1, r.y, BORDER);
    hline(r.x, r.x + r.w - 1, r.y + r.h - 1, BORDER);
    vline(r.x, r.y, r.y + r.h - 1, BORDER);
    vline(r.x + r.w - 1, r.y, r.y + r.h - 1, BORDER);
    text_center(r.x + r.w / 2, r.y + (r.h - 16) / 2, label, WHITE);
}

/* ---- state ---- */
const unsigned N = SWEEP_POINTS;
SweepConfig cfg;
SweepStatus st;
uint8_t frame[FRAME_SIZE], noise[FRAME_SIZE];
uint32_t seq;
bool has_noise, have, warn, reset_traces = true;
uint8_t data[N], peak[N], avg8[N];
float avgv[N];
double f0 = DEFAULT_START_MHZ, f1 = DEFAULT_STOP_MHZ;
bool shown_hold;
unsigned shown_rate = ~0u;
bool shown_det;
int shown_gain = -2;

enum Screen { MAIN, SETTINGS } screen = MAIN;
int active = 0;       /* selected marker */
int battery = -1;     /* percent, -1 = unknown */
bool dragging;
int drag_tenths = -1; /* marker position while dragging, not yet applied */

uint32_t get32(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}
float dbm(float code) { return code / 2 - 84.3f + cfg.cal_tenths / 10.0f; }
const uint8_t *trace() { return cfg.avg ? avg8 : data; }

void take_frame() {
    double a = get32(frame + 4) / 1000.0, b = get32(frame + 8) / 1000.0;
    if (a != f0 || b != f1) reset_traces = true;
    f0 = a;
    f1 = b;
    const uint8_t *t = frame + FRAME_HEADER;
    uint8_t top = 0;
    for (unsigned i = 0; i < N; i++) {
        uint8_t v = t[i];
        data[i] = v;
        if (v > top) top = v;
        if (reset_traces || !have) {
            peak[i] = v;
            avgv[i] = v;
        } else {
            if (v > peak[i]) peak[i] = v;
            if (v) avgv[i] = avgv[i] ? avgv[i] * 0.8f + v * 0.2f : v;
        }
        avg8[i] = (uint8_t)lroundf(avgv[i]);
    }
    reset_traces = false;
    have = true;
    /* Wide signals near -30 dBFS per bin already add up to ADC full scale. */
    warn = top / 2.0f - 84.3f > -30;
}

/* ---- main screen geometry ---- */
const int PX = 32, PY = 18, PW = LCD_W - PX, PH = 150;
const int TY = PY + PH + 20; /* marker table */
const int ROW = 20;
const Rect BTN_PEAK{0, 288, 78, 32}, BTN_RESET{81, 288, 78, 32}, BTN_SETTINGS{162, 288, 78, 32};

int fx(double f) { return PX + (int)lround((f - f0) / (f1 - f0) * PW); }
int dy(float d) {
    int y = PY + (int)lroundf((cfg.ytop - d) / cfg.yrange * PH);
    return constrain(y, PY, PY + PH - 1);
}
int marker_tenths(int m) { return m == active && drag_tenths >= 0 ? drag_tenths : cfg.markers[m]; }
bool level(double f, float *out) {
    if (!have || f < f0 || f >= f1) return false;
    unsigned i = (unsigned)((f - f0) / (f1 - f0) * N);
    uint8_t v = trace()[i < N ? i : N - 1];
    if (!v) return false;
    *out = dbm(v);
    return true;
}
void nearest_channel(double f, char *out) {
    double best = 1e9;
    strcpy(out, "-");
    for (int b = 0; b < 5; b++)
        for (int k = 0; k < 8; k++) {
            double d = fabs(BANDS[b][k] - f);
            if (d <= 10 && d < best) {
                best = d;
                snprintf(out, 4, "%c%d", BAND_NAME[b], k + 1);
            }
        }
}

/* Each trace is reduced to one y per graph column (maximum of its points),
 * computed once per frame and drawn into every strip. */
int16_t col_noise[PW], col_peak[PW], col_main[PW];
void columns(int16_t *out, const uint8_t *t, unsigned n, double a, double b) {
    for (int x = 0; x < PW; x++) {
        double fa = f0 + (f1 - f0) * x / PW, fb = f0 + (f1 - f0) * (x + 1) / PW;
        int i0 = (int)floor((fa - a) / (b - a) * n), i1 = (int)ceil((fb - a) / (b - a) * n) - 1;
        out[x] = -1;
        if (i1 < 0 || i0 >= (int)n) continue;
        i0 = max(i0, 0);
        i1 = min(max(i1, i0), (int)n - 1);
        uint8_t v = 0;
        for (int i = i0; i <= i1; i++) v = max(v, t[i]);
        if (v) out[x] = dy(dbm(v));
    }
}
void plot(const int16_t *c, uint16_t color) {
    for (int x = 0; x < PW; x++) {
        if (c[x] < 0) continue;
        if (x && c[x - 1] >= 0) vline(PX + x, c[x - 1], c[x], color);
        else pixel(PX + x, c[x], color);
    }
}

void draw_main() {
    char s[48];
    /* status line */
    int x = text(0, 0, st.linked ? "C5" : "C5未接続", st.linked ? GREEN : RED) + 8;
    if (st.linked) snprintf(s, sizeof(s), "%u-%u %ust %ums", cfg.start_mhz, cfg.stop_mhz, st.steps, st.sweep_ms);
    else snprintf(s, sizeof(s), "%u-%uMHz", cfg.start_mhz, cfg.stop_mhz);
    text(x, 0, s, WHITE);
    /* battery, top right; below BATTERY_LOW_PERCENT it blinks in red */
    if (battery >= 0) {
        snprintf(s, sizeof(s), "%d%%", battery);
        if (battery < BATTERY_LOW_PERCENT && millis() / 500 % 2) {
            fill(LCD_W - text_width(s) - 2, 0, text_width(s) + 2, 16, RED);
            text_right(LCD_W - 1, 0, s, WHITE);
        } else
            text_right(LCD_W - 1, 0, s, battery < BATTERY_LOW_PERCENT ? RED : battery < 50 ? YELLOW : GREEN);
    }

    /* dBm grid and axis */
    int step = 10 * PH / cfg.yrange >= 18 ? 10 : 20;
    for (int d = (int)ceil((cfg.ytop - cfg.yrange) / (double)step) * step; d <= cfg.ytop; d += step) {
        int y = dy(d);
        hline(PX, LCD_W - 1, y, GRID);
        snprintf(s, sizeof(s), "%d", d);
        text_right(PX - 2, constrain(y - 8, PY, PY + PH - 16), s, GRAY);
    }
    /* frequency grid: labels at least 44 px apart */
    static const int fsteps[] = {1, 2, 5, 10, 20, 25, 50, 100, 200, 500};
    int fstep = 500;
    for (int c : fsteps)
        if (c / (f1 - f0) * PW >= 44) {
            fstep = c;
            break;
        }
    for (int f = (int)ceil(f0 / fstep) * fstep; f <= f1; f += fstep) {
        int fxp = fx(f);
        vline(min(fxp, LCD_W - 1), PY, PY + PH - 1, GRID);
        snprintf(s, sizeof(s), "%d", f);
        int w = text_width(s);
        text(constrain(fxp - w / 2, PX - 8, LCD_W - w), PY + PH + 2, s, GRAY);
    }
    hline(PX, LCD_W - 1, PY + PH, BORDER);

    /* FPV channels of the shown bands, one label lane per band */
    int lane = 0;
    for (int b = 0; b < 5; b++) {
        if (!(cfg.bands >> b & 1)) continue;
        int placed[8], count = 0; /* label positions, to skip overlaps */
        for (int k = 0; k < 8; k++) {
            double f = BANDS[b][k];
            if (f < f0 || f > f1) continue;
            int cx = min(fx(f), LCD_W - 1);
            for (int y = PY; y < PY + PH; y += 2) pixel(cx, y, dim(BCOL[b]));
            snprintf(s, sizeof(s), "%c%d", BAND_NAME[b], k + 1);
            int lx = constrain(cx - 8, PX, LCD_W - 16);
            bool overlaps = false;
            for (int i = 0; i < count; i++) overlaps |= abs(placed[i] - lx) < 18;
            if (!overlaps) {
                text(lx, PY + lane * 16, s, BCOL[b]);
                placed[count++] = lx;
            }
        }
        lane++;
    }

    if (has_noise) plot(col_noise, NOISE);
    if (cfg.hold) plot(col_peak, PEAK);
    plot(col_main, cfg.avg ? BLUE : YELLOW);

    for (int m = 0; m < 4; m++) {
        int t = marker_tenths(m);
        float lv;
        if (t < 0 || !level(t / 10.0, &lv)) continue;
        int mx = fx(t / 10.0), my = dy(lv);
        for (int r = 0; r < 6; r++) hline(mx - r, mx + r, my - 2 - r, MCOL[m]);
        snprintf(s, sizeof(s), "%d", m + 1);
        text(mx - 4, max(my - 24, PY), s, MCOL[m]);
    }

    if (warn) {
        fill(LCD_W - 98, PY, 98, 16, BLACK);
        text_right(LCD_W - 1, PY, "ゲイン下げて", RED);
    }
    if (st.error[0]) {
        fill(PX, PY + PH - 16, 152, 16, BLACK);
        text(PX, PY + PH - 16, "ゲイン範囲外→自動", YELLOW);
    }

    /* marker table */
    text(0, TY, "マーカー", GRAY);
    text_right(112, TY, "dBm", GRAY);
    text(128, TY, "ch", GRAY);
    text_right(236, TY, "MHz", GRAY);
    for (int m = 0; m < 4; m++) {
        int y = TY + 16 + m * ROW;
        if (m == active) fill(0, y, LCD_W, ROW, PANEL);
        y += (ROW - 16) / 2;
        snprintf(s, sizeof(s), "M%d%s", m + 1, m == active ? "◀" : "");
        text(4, y, s, MCOL[m]);
        int t = marker_tenths(m);
        float lv;
        if (t >= 0 && level(t / 10.0, &lv)) snprintf(s, sizeof(s), "%.1f", lv);
        else strcpy(s, "-");
        text_right(112, y, s, MCOL[m]);
        if (t >= 0) nearest_channel(t / 10.0, s);
        else strcpy(s, "-");
        text(128, y, s, MCOL[m]);
        if (t >= 0) snprintf(s, sizeof(s), "%.1f", t / 10.0);
        else strcpy(s, "-");
        text_right(236, y, s, MCOL[m]);
    }

    button(BTN_PEAK, "Mピーク");
    button(BTN_RESET, "リセット");
    button(BTN_SETTINGS, "設定");
}

/* ---- settings screen ---- */
const int SROW = 44, SY = 40, BH = 36;
const Rect BTN_BACK{168, 0, 72, 34};
const Rect BTN_HOLD{116, SY, 52, BH}, BTN_HOLD_RESET{172, SY, 68, BH};
const Rect BTN_AVG{116, SY + SROW, 52, BH};
const Rect BTN_TOP_DOWN{56, SY + 2 * SROW, 48, BH}, BTN_TOP_UP{192, SY + 2 * SROW, 48, BH};
const Rect BTN_RANGE_DOWN{56, SY + 3 * SROW, 48, BH}, BTN_RANGE_UP{192, SY + 3 * SROW, 48, BH};
Rect band_button(int b) { return {60 + b * 36, SY + 4 * SROW, 34, BH}; }
const Rect BTN_NOISE_SAVE{96, SY + 5 * SROW, 68, BH}, BTN_NOISE_CLEAR{172, SY + 5 * SROW, 68, BH};

void draw_settings() {
    char s[48];
    const int ty = (BH - 16) / 2;
    text(4, 9, "表示設定", WHITE);
    button(BTN_BACK, "戻る");
    text(0, SY + ty, "ピークホールド", WHITE);
    button(BTN_HOLD, cfg.hold ? "ON" : "OFF", cfg.hold);
    button(BTN_HOLD_RESET, "リセット");
    text(0, SY + SROW + ty, "平均", WHITE);
    button(BTN_AVG, cfg.avg ? "ON" : "OFF", cfg.avg);
    text(0, SY + 2 * SROW + ty, "上限", WHITE);
    button(BTN_TOP_DOWN, "-5");
    snprintf(s, sizeof(s), "%ddBm", cfg.ytop);
    text_center(148, SY + 2 * SROW + ty, s, WHITE);
    button(BTN_TOP_UP, "+5");
    text(0, SY + 3 * SROW + ty, "範囲", WHITE);
    button(BTN_RANGE_DOWN, "-5");
    snprintf(s, sizeof(s), "%ddB", cfg.yrange);
    text_center(148, SY + 3 * SROW + ty, s, WHITE);
    button(BTN_RANGE_UP, "+5");
    text(0, SY + 4 * SROW + ty, "バンド", WHITE);
    for (int b = 0; b < 5; b++) {
        char name[2] = {BAND_NAME[b], 0};
        bool on = cfg.bands >> b & 1;
        button(band_button(b), name, on);
    }
    text(0, SY + 5 * SROW + ty, "ノイズ基準", WHITE);
    button(BTN_NOISE_SAVE, "保存");
    button(BTN_NOISE_CLEAR, "消去");
    if (has_noise)
        snprintf(s, sizeof(s), "保存済み %u-%uMHz", (unsigned)(get32(noise + 4) / 1000),
                 (unsigned)(get32(noise + 8) / 1000));
    else
        strcpy(s, "未保存（保存=今の平均線）");
    text(0, SY + 6 * SROW - 2, s, has_noise ? GREEN : GRAY);
}

/* ---- touch ---- */
void apply(const char *fmt, int value) {
    char line[32];
    snprintf(line, sizeof(line), fmt, value);
    shared_apply(line);
}

void drag_to(int x) {
    double f = f0 + (x - PX + 0.5) * (f1 - f0) / PW;
    f = constrain(f, f0, f1);
    /* snap to a shown FPV channel within 3 px */
    for (int b = 0; b < 5; b++)
        if (cfg.bands >> b & 1)
            for (int k = 0; k < 8; k++)
                if (BANDS[b][k] >= f0 && BANDS[b][k] <= f1 && abs(fx(BANDS[b][k]) - x) <= 3) f = BANDS[b][k];
    drag_tenths = (int)lround(f * 10);
}

void set_marker(int tenths) {
    char line[24];
    snprintf(line, sizeof(line), "cfg m%d=%d", active + 1, tenths);
    shared_apply(line);
}

void press_main(int x, int y) {
    if (y >= PY && y < PY + PH && x >= PX) {
        dragging = true;
        drag_to(x);
    } else if (y >= TY + 16 && y < TY + 16 + 4 * ROW) {
        /* tapping the selected row again clears its marker */
        int m = (y - TY - 16) / ROW;
        if (m == active && cfg.markers[m] >= 0) set_marker(-1);
        active = m;
    } else if (BTN_PEAK.in(x, y) && have) {
        const uint8_t *t = trace();
        unsigned k = 0;
        for (unsigned i = 1; i < N; i++)
            if (t[i] > t[k]) k = i;
        set_marker((int)lround((f0 + (f1 - f0) * (k + 0.5) / N) * 10));
    } else if (BTN_RESET.in(x, y)) {
        memcpy(peak, data, N);
    } else if (BTN_SETTINGS.in(x, y)) {
        screen = SETTINGS;
    }
}

void press_settings(int x, int y) {
    char line[32];
    if (BTN_BACK.in(x, y)) screen = MAIN;
    else if (BTN_HOLD.in(x, y)) apply("cfg hold=%d", !cfg.hold);
    else if (BTN_HOLD_RESET.in(x, y)) memcpy(peak, data, N);
    else if (BTN_AVG.in(x, y)) apply("cfg avg=%d", !cfg.avg);
    else if (BTN_TOP_DOWN.in(x, y)) apply("cfg ytop=%d", cfg.ytop - 5);
    else if (BTN_TOP_UP.in(x, y)) apply("cfg ytop=%d", cfg.ytop + 5);
    else if (BTN_RANGE_DOWN.in(x, y)) apply("cfg yrange=%d", max(cfg.yrange - 5, 10));
    else if (BTN_RANGE_UP.in(x, y)) apply("cfg yrange=%d", cfg.yrange + 5);
    else if (BTN_NOISE_CLEAR.in(x, y)) shared_clear_noise();
    else if (BTN_NOISE_SAVE.in(x, y) && have) {
        /* the running average of the current span, as the page saves it */
        static uint8_t out[FRAME_SIZE];
        memset(out, 0, FRAME_HEADER);
        memcpy(out, "NSE2", 4);
        uint32_t a = (uint32_t)lround(f0 * 1000), b = (uint32_t)lround(f1 * 1000);
        uint16_t n = N;
        memcpy(out + 4, &a, 4);
        memcpy(out + 8, &b, 4);
        memcpy(out + 12, &n, 2);
        memcpy(out + FRAME_HEADER, avg8, N);
        shared_store_noise(out);
    } else
        for (int b = 0; b < 5; b++)
            if (band_button(b).in(x, y)) {
                snprintf(line, sizeof(line), "cfg bands=%u", cfg.bands ^ (1u << b));
                shared_apply(line);
            }
}

void render() {
    cfg = shared_config();
    st = shared_status();
    has_noise = shared_noise(noise);
    /* Peak and average restart when the receiver settings change (as on the
     * page) and the peak restarts when peak hold is switched on. */
    if (cfg.rate_code != shown_rate || cfg.max_hold != shown_det || cfg.gain != shown_gain) reset_traces = true;
    if (cfg.hold && !shown_hold) memcpy(peak, data, N);
    shown_rate = cfg.rate_code;
    shown_det = cfg.max_hold;
    shown_gain = cfg.gain;
    shown_hold = cfg.hold;
    if (screen == MAIN) {
        for (int x = 0; x < PW; x++) col_noise[x] = col_peak[x] = col_main[x] = -1;
        if (have) {
            columns(col_main, trace(), N, f0, f1);
            if (cfg.hold) columns(col_peak, peak, N, f0, f1);
            uint16_t nn;
            memcpy(&nn, noise + 12, 2);
            if (has_noise && nn == N)
                columns(col_noise, noise + FRAME_HEADER, N, get32(noise + 4) / 1000.0, get32(noise + 8) / 1000.0);
        }
    }
    for (strip_y = 0; strip_y < LCD_H; strip_y += STRIP) {
        memset(buf, 0, LCD_W * STRIP * sizeof(uint16_t));
        if (screen == MAIN) draw_main();
        else draw_settings();
        lcd_push_rows(strip_y, STRIP, buf);
    }
}

/* Lithium cell voltage (mV) to remaining charge (%), at light load. */
int battery_percent(float mv) {
    static const uint16_t curve[][2] = {
        {4200, 100}, {4150, 95}, {4110, 90}, {4080, 85}, {4020, 80}, {3980, 75}, {3950, 70},
        {3910, 65},  {3870, 60}, {3850, 55}, {3840, 50}, {3820, 45}, {3800, 40}, {3790, 35},
        {3770, 30},  {3750, 25}, {3730, 20}, {3710, 15}, {3690, 10}, {3610, 5},  {3300, 0},
    };
    if (mv >= curve[0][0]) return 100;
    for (size_t i = 1; i < sizeof(curve) / sizeof(curve[0]); i++)
        if (mv >= curve[i][0])
            return lroundf(curve[i][1] + (curve[i - 1][1] - curve[i][1]) * (mv - curve[i][0]) /
                                             (curve[i - 1][0] - curve[i][0]));
    return 0;
}

/* Every 2 s; the voltage is smoothed because radio bursts pull it down. */
void read_battery() {
    static float mv_avg;
    static unsigned reads;
    int raw = lcd_battery_raw();
    float mv = raw * BATTERY_MV_PER_COUNT;
    if (raw < 0 || mv < 2500 || mv > 4600) { /* no battery or no reading */
        battery = -1;
        mv_avg = 0;
    } else {
        mv_avg = mv_avg ? mv_avg * 0.8f + mv * 0.2f : mv;
        battery = battery_percent(mv_avg);
    }
    if (reads++ % 5 == 0) Serial.printf("battery raw=%d mv=%.0f %d%%\n", raw, mv_avg, battery);
    shared_set_battery(battery);
}

void lcd_task(void *) {
    lcd_board_begin();
    buf = (uint16_t *)malloc(LCD_W * STRIP * sizeof(uint16_t));
    uint32_t last_draw = 0, last_battery = 0;
    bool dirty = true, down = false;
    read_battery();
    int up_polls = 0;
    for (;;) {
        int x, y;
        if (lcd_touch(&x, &y)) {
            up_polls = 0;
            if (!down) {
                down = true;
                cfg = shared_config();
                if (screen == MAIN) press_main(x, y);
                else press_settings(x, y);
            } else if (dragging) {
                drag_to(x);
            }
            dirty = true;
        } else if (down && ++up_polls >= 3) { /* the panel skips reports now and then */
            down = false;
            if (dragging) {
                set_marker(drag_tenths);
                dragging = false;
                drag_tenths = -1;
            }
            dirty = true;
        }
        uint32_t s = shared_frame(frame, seq);
        if (s != seq) {
            seq = s;
            if (seq) take_frame();
            dirty = true;
        }
        uint32_t now = millis();
        if (now - last_battery >= 2000) {
            last_battery = now;
            read_battery();
        }
        /* a low battery blinks: redraw every half second */
        if (battery >= 0 && battery < BATTERY_LOW_PERCENT && now / 500 != last_draw / 500) dirty = true;
        if ((dirty && now - last_draw >= 80) || now - last_draw >= 1000) {
            render();
            last_draw = now;
            dirty = false;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
} // namespace

void lcd_ui_begin() { xTaskCreatePinnedToCore(lcd_task, "lcd", 8192, nullptr, 1, nullptr, 0); }
#endif
