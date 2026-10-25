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
#include <esp_system.h>
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
               NOISE = rgb(120, 128, 138), ACCENT = rgb(20, 39, 64); /* "on" buttons: a dim, calm blue */
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
uint16_t *buf; /* STRIP rows of the widest screen (320) */
int strip_y;
/* Logical screen size: 240x320 portrait, 320x240 landscape (main screen). */
int scr_w = LCD_W, scr_h = LCD_H;
bool landscape;

void fill(int x, int y, int w, int h, uint16_t c) {
    int y0 = max(y, strip_y), y1 = min(y + h, strip_y + STRIP), x0 = max(x, 0), x1 = min(x + w, scr_w);
    for (int yy = y0; yy < y1; yy++) {
        uint16_t *p = buf + (yy - strip_y) * scr_w;
        for (int xx = x0; xx < x1; xx++) p[xx] = c;
    }
}
void pixel(int x, int y, uint16_t c) {
    if ((unsigned)x < (unsigned)scr_w && y >= strip_y && y < strip_y + STRIP) buf[(y - strip_y) * scr_w + x] = c;
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

/* Settings icon: a gear of 8 teeth, about 20 px across, centred on cx, cy. */
void gear(int cx, int cy, uint16_t c) {
    for (int dy = -10; dy <= 10; dy++)
        for (int dx = -10; dx <= 10; dx++) {
            float r = sqrtf(dx * dx + dy * dy);
            if (r < 3.5f) continue; /* hole */
            bool tooth = cosf(8 * atan2f(dy, dx)) > 0.2f;
            if (r <= (tooth ? 10.0f : 7.5f)) pixel(cx + dx, cy + dy, c);
        }
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

enum Screen { MAIN, SETTINGS, ADVANCED } screen = MAIN;
int active = -1;      /* selected marker, -1 = none (as at start-up) */
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

/* ---- main screen geometry ----
 * Portrait: graph, marker table (4 rows) and a row of buttons below.
 * Landscape: graph almost the full width, the markers in one row of four
 * cells (level, whole MHz) below; the buttons are on a panel that slides in
 * from the right when the tab on the right edge (or, with no marker
 * selected, the graph) is tapped, and slides out after PANEL_MS without a
 * tap on it. */
const int PX = 32, PY = 18;
const int MAX_PW = 288;
int PW, PH; /* graph width and height */
int TY;     /* marker table (portrait: its header line) */
const int ROW = 20;          /* portrait table rows */
const int LROW = 18;         /* landscape marker row */
const int TAB_W = 16;        /* landscape: tab that opens the button panel */
const int PANEL_W = 72;      /* landscape: button panel */
const uint32_t PANEL_MS = 5000;
bool panel_open;
int panel_px;                /* how far the panel has slid in, 0 .. PANEL_W */
uint32_t panel_until;
void open_panel() {
    panel_open = true;
    panel_until = millis() + PANEL_MS;
}
/* peak hold and average (toggles), peak hold reset, settings */
Rect BTN_HOLD, BTN_AVG, BTN_RESET, BTN_SETTINGS;

void set_layout(bool land) {
    landscape = land;
    if (!land) {
        scr_w = LCD_W, scr_h = LCD_H;
        PW = LCD_W - PX, PH = 150;
        TY = PY + PH + 20;
        BTN_HOLD = {0, 288, 57, 32}, BTN_AVG = {61, 288, 57, 32}, BTN_RESET = {122, 288, 57, 32},
        BTN_SETTINGS = {183, 288, 57, 32};
    } else {
        scr_w = LCD_H, scr_h = LCD_W;
        PW = scr_w - TAB_W - PX, PH = 184;
        TY = PY + PH + 20;
        panel_open = false, panel_px = 0;
    }
}

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
int16_t col_noise[MAX_PW], col_peak[MAX_PW], col_main[MAX_PW];
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
            fill(scr_w - text_width(s) - 2, 0, text_width(s) + 2, 16, RED);
            text_right(scr_w - 1, 0, s, WHITE);
        } else
            text_right(scr_w - 1, 0, s, battery < BATTERY_LOW_PERCENT ? RED : battery < 50 ? YELLOW : GREEN);
    }

    const int GR = PX + PW; /* graph right edge */
    /* dBm grid and axis */
    int step = 10 * PH / cfg.yrange >= 18 ? 10 : 20;
    for (int d = (int)ceil((cfg.ytop - cfg.yrange) / (double)step) * step; d <= cfg.ytop; d += step) {
        int y = dy(d);
        hline(PX, GR - 1, y, GRID);
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
        vline(min(fxp, GR - 1), PY, PY + PH - 1, GRID);
        snprintf(s, sizeof(s), "%d", f);
        int w = text_width(s);
        text(constrain(fxp - w / 2, PX - 8, GR - w), PY + PH + 2, s, GRAY);
    }
    hline(PX, GR - 1, PY + PH, BORDER);

    /* FPV channels of the shown bands, one label lane per band */
    int lane = 0;
    for (int b = 0; b < 5; b++) {
        if (!(cfg.bands >> b & 1)) continue;
        int placed[8], count = 0; /* label positions, to skip overlaps */
        for (int k = 0; k < 8; k++) {
            double f = BANDS[b][k];
            if (f < f0 || f > f1) continue;
            int cx = min(fx(f), GR - 1);
            for (int y = PY; y < PY + PH; y += 2) pixel(cx, y, dim(BCOL[b]));
            snprintf(s, sizeof(s), "%c%d", BAND_NAME[b], k + 1);
            int lx = constrain(cx - 8, PX, GR - 16);
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
        fill(GR - 98, PY, 98, 16, BLACK);
        text_right(GR - 1, PY, "ゲイン下げて", RED);
    }
    if (st.error[0]) {
        fill(PX, PY + PH - 16, 152, 16, BLACK);
        text(PX, PY + PH - 16, "ゲイン範囲外→自動", YELLOW);
    }

    if (landscape) {
        /* markers in one row of four cells: a bar in the marker's colour
         * (as its number on the graph), level and whole MHz */
        const int CW = scr_w / 4;
        for (int m = 0; m < 4; m++) {
            int x0 = m * CW, y = TY;
            if (m == active) fill(x0, y, CW, LROW, PANEL);
            fill(x0, y + 2, 2, LROW - 4, MCOL[m]);
            y += (LROW - 16) / 2;
            int t = marker_tenths(m);
            float lv;
            if (t >= 0 && level(t / 10.0, &lv)) snprintf(s, sizeof(s), "%.1f", lv);
            else strcpy(s, "-");
            text_right(x0 + 43, y, s, MCOL[m]);
            /* the frequency dimmer, so that it reads apart from the level */
            if (t >= 0) snprintf(s, sizeof(s), "%ld", lround(t / 10.0));
            else strcpy(s, "-");
            text_right(x0 + CW - 1, y, s, dim(MCOL[m]));
        }
        /* tab, and the sliding button panel over the graph */
        if (panel_px < PANEL_W) {
            fill(scr_w - TAB_W, PY, TAB_W, PH, PANEL);
            text(scr_w - TAB_W + 4, PY + PH / 2 - 8, "◀", GRAY);
        }
        if (panel_px > 0) {
            int px = scr_w - panel_px;
            fill(px, PY, PANEL_W, PH + 2, BLACK);
            vline(px, PY, PY + PH + 1, BORDER);
            const int bh = (PH - 3 * 4) / 4;
            BTN_HOLD = {px + 4, PY, PANEL_W - 4, bh}, BTN_AVG = {px + 4, PY + bh + 4, PANEL_W - 4, bh},
            BTN_RESET = {px + 4, PY + 2 * (bh + 4), PANEL_W - 4, bh},
            BTN_SETTINGS = {px + 4, PY + 3 * (bh + 4), PANEL_W - 4, bh};
        } else {
            return; /* buttons hidden */
        }
    } else {
    /* marker table */
    text(0, TY, "マーカー", GRAY);
    text_right(112, TY, "dBm", GRAY);
    text(128, TY, "ch", GRAY);
    text_right(236, TY, "MHz", GRAY);
    for (int m = 0; m < 4; m++) {
        int y = TY + 16 + m * ROW;
        if (m == active) fill(0, y, scr_w, ROW, PANEL);
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
    }

    button(BTN_HOLD, "Hold", cfg.hold);
    button(BTN_AVG, "Ave", cfg.avg);
    button(BTN_RESET, "Reset");
    button(BTN_SETTINGS, "");
    gear(BTN_SETTINGS.x + BTN_SETTINGS.w / 2, BTN_SETTINGS.y + BTN_SETTINGS.h / 2, WHITE);
}

/* ---- settings screen ---- */
const int SROW = 44, SY = 40, BH = 36;
const Rect BTN_BACK{168, 0, 72, 34}, BTN_ADVANCED{92, 0, 72, 34};
const Rect BTN_PEAK{96, SY, 144, BH};
const Rect BTN_TOP_DOWN{56, SY + SROW, 48, BH}, BTN_TOP_UP{192, SY + SROW, 48, BH};
const Rect BTN_RANGE_DOWN{56, SY + 2 * SROW, 48, BH}, BTN_RANGE_UP{192, SY + 2 * SROW, 48, BH};
Rect band_button(int b) { return {60 + b * 36, SY + 3 * SROW, 34, BH}; }
const Rect BTN_NOISE_SAVE{96, SY + 4 * SROW, 68, BH}, BTN_NOISE_CLEAR{172, SY + 4 * SROW, 68, BH};
const Rect BTN_PORTRAIT{96, SY + 5 * SROW + 16, 68, BH}, BTN_LANDSCAPE{172, SY + 5 * SROW + 16, 68, BH};

void draw_settings() {
    char s[48];
    const int ty = (BH - 16) / 2;
    text(4, 9, "表示設定", WHITE);
    button(BTN_ADVANCED, "詳細");
    button(BTN_BACK, "戻る");
    /* moves the selected marker to the strongest point */
    text(0, SY + ty, "マーカー", WHITE);
    if (active >= 0) snprintf(s, sizeof(s), "M%dをピークへ", active + 1);
    else strcpy(s, "未選択");
    button(BTN_PEAK, s);
    text(0, SY + SROW + ty, "上限", WHITE);
    button(BTN_TOP_DOWN, "-5");
    snprintf(s, sizeof(s), "%ddBm", cfg.ytop);
    text_center(148, SY + SROW + ty, s, WHITE);
    button(BTN_TOP_UP, "+5");
    text(0, SY + 2 * SROW + ty, "範囲", WHITE);
    button(BTN_RANGE_DOWN, "-5");
    snprintf(s, sizeof(s), "%ddB", cfg.yrange);
    text_center(148, SY + 2 * SROW + ty, s, WHITE);
    button(BTN_RANGE_UP, "+5");
    text(0, SY + 3 * SROW + ty, "バンド", WHITE);
    for (int b = 0; b < 5; b++) {
        char name[2] = {BAND_NAME[b], 0};
        bool on = cfg.bands >> b & 1;
        button(band_button(b), name, on);
    }
    text(0, SY + 4 * SROW + ty, "ノイズ基準", WHITE);
    button(BTN_NOISE_SAVE, "保存");
    button(BTN_NOISE_CLEAR, "消去");
    if (has_noise)
        snprintf(s, sizeof(s), "保存済み %u-%uMHz", (unsigned)(get32(noise + 4) / 1000),
                 (unsigned)(get32(noise + 8) / 1000));
    else
        strcpy(s, "未保存（保存=今の平均線）");
    text(0, SY + 5 * SROW - 2, s, has_noise ? GREEN : GRAY);
    /* main screen orientation (this screen stays portrait) */
    text(0, SY + 5 * SROW + 16 + ty, "画面の向き", WHITE);
    button(BTN_PORTRAIT, "縦", !cfg.lcd_land);
    button(BTN_LANDSCAPE, "横", cfg.lcd_land);
}

/* ---- advanced settings screen: brightness and power saving ---- */
const Rect BTN_BRIGHT_DOWN{56, SY, 48, BH}, BTN_BRIGHT_UP{192, SY, 48, BH};
const Rect BTN_SLEEP_DOWN{56, SY + SROW, 48, BH}, BTN_SLEEP_UP{192, SY + SROW, 48, BH};
const Rect BTN_SLEEP_DOWN_MIN{56, SY + 2 * SROW, 48, BH}, BTN_SLEEP_UP_MIN{192, SY + 2 * SROW, 48, BH};

void sleep_text(int seconds, char *out, size_t size) {
    if (seconds <= 0) snprintf(out, size, "しない");
    else if (seconds < 60) snprintf(out, size, "%d秒", seconds);
    else if (seconds % 60 == 0) snprintf(out, size, "%d分", seconds / 60);
    else snprintf(out, size, "%d分%d秒", seconds / 60, seconds % 60);
}

void draw_advanced() {
    char s[32];
    const int ty = (BH - 16) / 2;
    text(4, 9, "詳細設定", WHITE);
    button(BTN_BACK, "戻る");
    text(0, SY + ty, "明るさ", WHITE);
    button(BTN_BRIGHT_DOWN, "-10");
    snprintf(s, sizeof(s), "%d%%", cfg.lcd_bright);
    text_center(148, SY + ty, s, WHITE);
    button(BTN_BRIGHT_UP, "+10");
    text(0, SY + SROW + ty, "省電力", WHITE);
    button(BTN_SLEEP_DOWN, "-10秒");
    sleep_text(cfg.lcd_sleep, s, sizeof(s));
    text_center(148, SY + SROW + ty, s, WHITE);
    button(BTN_SLEEP_UP, "+10秒");
    button(BTN_SLEEP_DOWN_MIN, "-1分");
    button(BTN_SLEEP_UP_MIN, "+1分");
    int y = SY + 3 * SROW;
    text(0, y, "省電力: 操作しない時間が", GRAY);
    text(0, y + 18, "設定時間たつと暗くなり、", GRAY);
    text(0, y + 36, "同じ時間でさらに消灯。", GRAY);
    text(0, y + 54, "画面のタップで明るさが戻る", GRAY);
    text(0, y + 72, "（そのタップは操作しない）", GRAY);
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

bool on_panel(int x) { return landscape && panel_px > 0 && x >= scr_w - panel_px; }
bool in_graph(int x, int y) { return y >= PY && y < PY + PH && x >= PX && x < PX + PW && !on_panel(x); }

/* A tap on the main screen, on release (the graph is handled on press, by
 * dragging the selected marker). */
void press_main(int x, int y) {
    if (in_graph(x, y)) {
        if (landscape && active < 0) open_panel(); /* the graph has nothing else to do */
        return;
    } else if (landscape) {
        if (on_panel(x) && y < TY - 2) {
            /* panel buttons: four rows, gaps included; each tap keeps the
             * panel out for another PANEL_MS */
            open_panel();
            switch (constrain((y - PY) * 4 / PH, 0, 3)) {
            case 0: apply("cfg hold=%d", !cfg.hold); break;
            case 1: apply("cfg avg=%d", !cfg.avg); break;
            case 2: memcpy(peak, data, N); break;
            default:
                panel_open = false, panel_px = 0;
                screen = SETTINGS;
            }
        } else if (x >= scr_w - TAB_W - 4 && y < TY - 2) {
            open_panel();
        } else if (y >= TY - 2) {
            /* marker cells: tapping the selected one again releases it */
            int m = min(x * 4 / scr_w, 3);
            active = m == active ? -1 : m;
        }
    } else if (y >= TY + 16 && y < TY + 16 + 4 * ROW) {
        /* Tapping the selected row again releases the selection; the marker
         * keeps its frequency and level. */
        int m = (y - TY - 16) / ROW;
        active = m == active ? -1 : m;
    } else if (y >= TY + 16 + 4 * ROW) {
        /* Bottom buttons: anything below the table counts, split into four
         * columns, so taps near the edges and corners (where the panel is
         * least accurate) and in the gaps still hit a button. */
        switch (min(x * 4 / scr_w, 3)) {
        case 0: apply("cfg hold=%d", !cfg.hold); break;
        case 1: apply("cfg avg=%d", !cfg.avg); break;
        case 2: memcpy(peak, data, N); break;
        default: screen = SETTINGS;
        }
    }
}

void press_advanced(int x, int y) {
    if (BTN_BACK.in(x, y)) screen = SETTINGS;
    else if (BTN_BRIGHT_DOWN.in(x, y)) apply("cfg lbright=%d", max(cfg.lcd_bright - 10, 10));
    else if (BTN_BRIGHT_UP.in(x, y)) apply("cfg lbright=%d", min(cfg.lcd_bright + 10, 100));
    else if (BTN_SLEEP_DOWN.in(x, y)) apply("cfg lsleep=%d", max(cfg.lcd_sleep - 10, 0));
    else if (BTN_SLEEP_UP.in(x, y)) apply("cfg lsleep=%d", min(cfg.lcd_sleep + 10, 600));
    else if (BTN_SLEEP_DOWN_MIN.in(x, y)) apply("cfg lsleep=%d", max(cfg.lcd_sleep - 60, 0));
    else if (BTN_SLEEP_UP_MIN.in(x, y)) apply("cfg lsleep=%d", min(cfg.lcd_sleep + 60, 600));
}

void press_settings(int x, int y) {
    char line[32];
    if (BTN_BACK.in(x, y)) screen = MAIN;
    else if (BTN_ADVANCED.in(x, y)) screen = ADVANCED;
    else if (BTN_PORTRAIT.in(x, y)) apply("cfg lland=%d", 0);
    else if (BTN_LANDSCAPE.in(x, y)) apply("cfg lland=%d", 1);
    else if (BTN_PEAK.in(x, y) && have && active >= 0) {
        const uint8_t *t = trace();
        unsigned k = 0;
        for (unsigned i = 1; i < N; i++)
            if (t[i] > t[k]) k = i;
        set_marker((int)lround((f0 + (f1 - f0) * (k + 0.5) / N) * 10));
    }
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

/* Touch state, sampled by poll_touch() from the main loop and between the
 * strips of a redraw, so that short taps are not lost while drawing. Taps
 * act on release at the last reported position: the first contact point is
 * the least accurate, worst at the screen edges. */
struct {
    bool down;              /* finger on the panel */
    int x, y;               /* last reported position */
    uint32_t last_report;
    bool pressed, released; /* events for the main loop */
} touch;
/* No report for this long means the finger was lifted (the panel skips a
 * report now and then while touched). */
const uint32_t RELEASE_MS = 80;

void poll_touch() {
    int x, y;
    uint32_t now = millis();
    if (lcd_touch(&x, &y)) {
        if (landscape) { /* portrait panel coordinates to the rotated screen */
            int px = x;
            x = LCD_LANDSCAPE_FLIP ? LCD_H - 1 - y : y;
            y = LCD_LANDSCAPE_FLIP ? px : LCD_W - 1 - px;
        }
        if (!touch.down) {
            touch.down = true;
            touch.pressed = true;
        }
        touch.x = x;
        touch.y = y;
        touch.last_report = now;
    } else if (touch.down && now - touch.last_report >= RELEASE_MS) {
        touch.down = false;
        touch.released = true;
    }
}

void render() {
    cfg = shared_config();
    /* only the main screen turns to landscape */
    bool land = screen == MAIN && cfg.lcd_land;
    if (land != landscape) {
        lcd_set_landscape(land);
        set_layout(land);
    }
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
    for (strip_y = 0; strip_y < scr_h; strip_y += STRIP) {
        memset(buf, 0, scr_w * STRIP * sizeof(uint16_t));
        if (screen == MAIN) draw_main();
        else if (screen == SETTINGS) draw_settings();
        else draw_advanced();
        lcd_push_rows(strip_y, STRIP, scr_w, buf);
        poll_touch();
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
/* Why the chip last started, to tell a power-on or the reset button from
 * a crash, a watchdog or a supply dip (brownout). */
const char *reset_reason() {
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "power-on";
    case ESP_RST_EXT: return "reset pin";
    case ESP_RST_SW: return "software restart";
    case ESP_RST_PANIC: return "CRASH (panic)";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT: return "WATCHDOG";
    case ESP_RST_BROWNOUT: return "BROWNOUT (supply dip)";
    case ESP_RST_USB: return "USB (download tool)";
    case ESP_RST_JTAG: return "JTAG";
    default: return "other";
    }
}

int backlight = -1;       /* percent currently set, -1 = not yet */
unsigned bring_ups;       /* LCD bring-ups since start */
uint32_t first_screen_ms; /* time of the last bring-up's first screen */

int last_raw = -1;
float battery_mv;
unsigned battery_bad_reads;

/* Every BATTERY_READ_MS. Each read is a transfer the expander may garble
 * (seen: about 1 in 10 at a 2 s interval), so it reads seldom. */
void read_battery() {
    if (!BATTERY_GAUGE) return;
    static float mv_avg;
    static unsigned reads, misses, bad_reads;
    int raw = lcd_battery_raw();
    float mv = raw * BATTERY_MV_PER_COUNT;
    /* A reading far from the average is a garbled transfer (seen: 67
     * instead of about 435), not the battery: it is dropped. */
    bool garbled = raw >= 0 && mv_avg && fabsf(mv - mv_avg) > 300;
    if (garbled) bad_reads++;
    if (garbled) {
    } else if (raw < 0 || mv < 2500 || mv > 4600) {
        /* No battery, or a failed read: hide the level only when it lasts,
         * so a single bad read does not make it flicker. */
        if (++misses >= 3) {
            battery = -1;
            mv_avg = 0;
        }
    } else {
        misses = 0;
        mv_avg = mv_avg ? mv_avg * 0.8f + mv * 0.2f : mv;
        battery = battery_percent(mv_avg);
    }
    last_raw = raw;
    battery_mv = mv_avg;
    battery_bad_reads = bad_reads;
    reads++;
    shared_set_battery(battery);
}

/* About every 10 s, with the start-up timings: the USB serial port
 * reconnects only after the start, so the log lines printed then are
 * usually missed. */
void log_status() {
    Serial.printf("battery raw=%d mv=%.0f %d%% | LCD up %u time(s), last: expander answered after %lu ms, "
                  "first screen at %lu ms | uptime %lu s, started by %s | gauge %d refresh %d | "
                  "I2C errors %u, bad battery reads %u, backlight %d%%\n",
                  last_raw, battery_mv, battery, bring_ups, (unsigned long)lcd_expander_wait_ms,
                  (unsigned long)first_screen_ms, millis() / 1000, reset_reason(), BATTERY_GAUGE, EXPANDER_REFRESH,
                  lcd_expander_errors, battery_bad_reads, backlight);
}

/* ---- backlight and power saving ----
 * After lcd_sleep seconds without a touch the backlight dims to 10 %, after
 * as long again it goes off; a touch brings it back (and does nothing else).
 * The backlight is set only here, from the setting and the idle time. */
uint32_t last_touch;

int wanted_backlight(uint32_t now) {
    if (cfg.lcd_sleep <= 0) return cfg.lcd_bright;
    uint32_t idle = now - last_touch, limit = (uint32_t)cfg.lcd_sleep * 1000;
    if (idle >= 2 * limit) return 0;
    if (idle >= limit) return min(cfg.lcd_bright, 10);
    return cfg.lcd_bright;
}

void update_backlight(uint32_t now) {
    int want = wanted_backlight(now);
    if (want != backlight && lcd_backlight(want)) {
        backlight = want;
        Serial.printf("[%lu ms] LCD: backlight %d%%\n", now, want);
    }
}

/* Brings the LCD up, waiting as long as the I/O expander takes to answer. */
void bring_up() {
    cfg = shared_config();
    last_touch = millis();
    backlight = wanted_backlight(last_touch);
    while (!lcd_board_begin()) {
    }
    set_layout(false); /* the LCD set-up leaves it in portrait */
    bring_ups++;
    read_battery();
    render();
    lcd_display_on();
    lcd_backlight(backlight);
    first_screen_ms = millis();
    Serial.printf("[%lu ms] LCD: first screen\n", millis());
}

void lcd_task(void *) {
    buf = (uint16_t *)malloc(LCD_H * STRIP * sizeof(uint16_t));
    if (!buf) {
        Serial.println("LCD: no memory for the strip buffer");
        vTaskDelete(nullptr);
    }
    bring_up();
    bool expander_lost = false;
    uint32_t last_draw = millis(), last_battery = last_draw, last_refresh = last_draw, last_log = 0;
    bool dirty = true;
    bool waking = false; /* the touch that woke the screen does nothing else */
    for (;;) {
        poll_touch();
        if (touch.pressed) {
            touch.pressed = false;
            cfg = shared_config();
            waking = backlight < cfg.lcd_bright;
            last_touch = millis();
            if (waking) {
                update_backlight(last_touch);
            } else {
                lcd_backlight(backlight); /* in case the expander lost it */
                if (screen == MAIN && in_graph(touch.x, touch.y) && active >= 0) {
                    dragging = true;
                    drag_to(touch.x);
                }
            }
            dirty = true;
        }
        if (touch.down) {
            last_touch = millis();
            if (dragging) {
                drag_to(touch.x);
                dirty = true;
            }
        }
        if (touch.released) {
            touch.released = false;
            if (dragging) {
                set_marker(drag_tenths);
                dragging = false;
                drag_tenths = -1;
            } else if (!waking) {
                cfg = shared_config();
                if (screen == MAIN) press_main(touch.x, touch.y);
                else if (screen == SETTINGS) press_settings(touch.x, touch.y);
                else press_advanced(touch.x, touch.y);
                cfg = shared_config(); /* a changed brightness applies at once */
            }
            waking = false;
            dirty = true;
        }
        uint32_t s = shared_frame(frame, seq);
        if (s != seq) {
            seq = s;
            if (seq) take_frame();
            dirty = true;
        }
        uint32_t now = millis();
        if (now - last_log >= 10000) {
            last_log = now;
            log_status();
        }
        if (now - last_battery >= BATTERY_READ_MS) {
            last_battery = now;
            read_battery();
        }
        if (EXPANDER_REFRESH && now - last_refresh >= 2000) {
            last_refresh = now;
            /* An expander that stops answering may have restarted (supply
             * dip) and reset the LCD with it: bring everything up again
             * once it answers. */
            if (!lcd_board_refresh(backlight < 0 ? 0 : backlight)) {
                if (!expander_lost) Serial.printf("[%lu ms] LCD: I/O expander lost\n", now);
                expander_lost = true;
                continue;
            }
            if (expander_lost) {
                expander_lost = false;
                bring_up();
                last_draw = millis();
                continue;
            }
        }
        /* landscape button panel: hide after PANEL_MS, slide in or out */
        bool sliding = false;
        if (landscape && screen == MAIN) {
            if (panel_open && (int32_t)(panel_until - now) <= 0) panel_open = false;
            int target = panel_open ? PANEL_W : 0;
            if (panel_px != target) {
                panel_px += constrain(target - panel_px, -24, 24);
                sliding = true;
            }
        }
        update_backlight(now);
        if (backlight == 0) { /* screen off: nothing to draw */
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        /* a low battery blinks: redraw every half second */
        if (battery >= 0 && battery < BATTERY_LOW_PERCENT && now / 500 != last_draw / 500) dirty = true;
        if (sliding || (dirty && now - last_draw >= 80) || now - last_draw >= 1000) {
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
