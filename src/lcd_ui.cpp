#if FPV_LCD
/* Touch LCD screens (240x320 portrait).
 *
 * Main: status line, spectrum (dBm axis, FPV band lines, markers), marker
 * table (level, channel, frequency) and buttons. Tapping or dragging on
 * the graph moves the selected marker (snapping to a shown FPV channel);
 * tapping a table row selects the marker.
 * Settings: span, gain, bands and the noise reference; the receiver and
 * advanced pages hold the rest.
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
               NOISE = rgb(120, 128, 138), ACCENT = rgb(0, 80, 200); /* "on" buttons: a clear, saturated blue (visible outdoors) */
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
bool flipped; /* landscape turned by 180 degrees */

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

enum Screen { MAIN, SETTINGS, ADVANCED, RECEIVER, WIFI } screen = MAIN;
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
    /* Saturation hint: the strongest point above the set dBFS (warn_dbfs). */
    warn = top / 2.0f - 84.3f > cfg.warn_dbfs;
}

/* ---- main screen geometry ----
 * Portrait: graph, marker table (4 rows) and a row of buttons below.
 * Landscape: graph almost the full width, the markers in one row of four
 * cells (level, whole MHz) below; the buttons are on a panel that slides in
 * from the right when the tab on the right edge (or, with no marker
 * selected, the graph) is tapped, and slides back after lcd_panel seconds
 * without a tap (0: never) or as soon as a button on it was pressed. */
const int PX = 32, PY = 18;
const int MAX_PW = 288;
int PW, PH; /* graph width and height */
int TY;     /* marker table (portrait: its header line) */
const int ROW = 20;          /* portrait table rows */
const int LROW = 18;         /* landscape marker row */
const int TAB_W = 16;        /* landscape: tab that opens the button panel */
const int PANEL_W = 72;      /* landscape: button panel */

bool panel_open;
int panel_px;                /* how far the panel has slid in, 0 .. PANEL_W */
uint32_t panel_until;
void open_panel() {
    panel_open = true;
    panel_until = millis() + (uint32_t)cfg.lcd_panel * 1000;
}
/* peak hold and average (toggles), peak hold reset, settings */
Rect BTN_HOLD, BTN_AVG, BTN_RESET, BTN_SETTINGS;

void set_settings_layout(bool land);
void set_layout(bool land) {
    landscape = land;
    set_settings_layout(land);
    if (!land) {
        scr_w = LCD_W, scr_h = LCD_H;
        PW = LCD_W - PX, PH = 150;
        TY = PY + PH + 20;
        BTN_HOLD = {0, 288, 57, 32}, BTN_AVG = {61, 288, 57, 32}, BTN_RESET = {122, 288, 57, 32},
        BTN_SETTINGS = {183, 288, 57, 32};
    } else {
        scr_w = LCD_H, scr_h = LCD_W;
        /* without touch there is no button panel and no tab for it */
        PW = scr_w - (LCD_HAS_TOUCH ? TAB_W : 0) - PX;
        PH = scr_h - PY - 38; /* 184 on 320x240, 116 on 320x172 */
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
        if (!LCD_HAS_TOUCH) return; /* no buttons on screen */
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

/* ---- settings screens (settings, receiver, advanced) ----
 * They follow the main screen's orientation. Rows are SROW apart from SY.
 * The label is on the left with the row's value (portrait: below the label,
 * landscape: beside it); the buttons are on a grid of four columns at the
 * right edge (pairs take two columns each, the band row five equal cells),
 * so the buttons of all rows line up. */
int SY = 40, SROW = 44, BH = 36, AREA = 158; /* portrait until set_settings_layout() */
void set_settings_layout(bool land) {
    if (land) SY = 36, SROW = 34, BH = 30, AREA = 162; /* 320x240 */
    else SY = 40, SROW = 44, BH = 36, AREA = 158;      /* 240x320 */
}
int row_y(int r) { return SY + r * SROW; }
int row_text_y(int r) { return row_y(r) + (BH - 16) / 2; }
/* title row: [Wi-Fi] [受信] [詳細] [戻る] from the right, room for four */
int title_w() { return min(72, (scr_w - 3 * 4) / 4); }
Rect title_button(int i) { return {scr_w - title_w() - i * (title_w() + 4), 0, title_w(), 34}; }
#define BTN_BACK title_button(0)
#define BTN_ADVANCED title_button(1)
#define BTN_RECEIVER title_button(2)
#define BTN_WIFI title_button(3)
/* columns i .. j - 1 of n equal columns (2 px gaps) in row r */
Rect cell(int r, int i, int j, int n) {
    int x0 = scr_w - AREA, xa = x0 + i * (AREA + 2) / n, xb = x0 + j * (AREA + 2) / n - 2;
    return {xa, row_y(r), xb - xa, BH};
}
Rect col(int r, int i) { return cell(r, i, i + 1, 4); }       /* one of four columns */
Rect half(int r, int i) { return cell(r, 2 * i, 2 * i + 2, 4); } /* left or right pair */
Rect band_button(int b) { return cell(4, b, b + 1, 5); }

/* row label and its value (none: nullptr) */
void label(int r, const char *name, const char *value = nullptr, uint16_t vc = BLUE) {
    int vx = scr_w - AREA - 6; /* values right-aligned before the buttons */
    if (!value) {
        text(0, row_text_y(r), name, WHITE);
    } else if (landscape) {
        text(0, row_text_y(r), name, WHITE);
        text_right(vx, row_text_y(r), value, vc);
    } else {
        text(0, row_y(r) + 1, name, WHITE);
        text_right(vx, row_y(r) + BH - 17, value, vc);
    }
}
/* four buttons in a row: [l0][l1][l2][l3] */
void four(int r, const char *const l[4]) {
    for (int i = 0; i < 4; i++) button(col(r, i), l[i]);
}

void draw_title(const char *title, int buttons) {
    /* the title only where the buttons leave room (not on 240 px with three) */
    if (scr_w - buttons * (title_w() + 4) >= 72) text(4, 9, title, WHITE);
    button(BTN_BACK, "戻る");
    if (buttons > 1) button(BTN_ADVANCED, "詳細");
    if (buttons > 2) button(BTN_RECEIVER, "受信");
    if (buttons > 3) button(BTN_WIFI, "Wi-Fi");
}

/* ---- settings (first page): span, gain, bands, noise reference ----
 * Each change goes through the same "cfg" lines as the page's apply buttons:
 * it is saved and restarts the sweep where needed. */
/* span presets: three equal buttons (the range itself shows below スパン) */
const int PRESETS = 3;
const unsigned PRESET[PRESETS][2] = {{5650, 5850}, {5640, 5950}, {2400, 2500}};
const char *const PRESET_NAME[PRESETS] = {"5.8G", "5.8G広", "2.4G"};
Rect preset_button(int i) { return cell(0, i, i + 1, PRESETS); }
const char *const MHZ_STEP[4] = {"-10", "-1", "+1", "+10"};

void draw_settings() {
    char s[32];
    draw_title("設定", 4);
    snprintf(s, sizeof(s), "%u-%u", cfg.start_mhz, cfg.stop_mhz);
    label(0, "スパン", s);
    for (int i = 0; i < PRESETS; i++)
        button(preset_button(i), PRESET_NAME[i], cfg.start_mhz == PRESET[i][0] && cfg.stop_mhz == PRESET[i][1]);
    snprintf(s, sizeof(s), "%u", cfg.start_mhz);
    label(1, "開始", s);
    four(1, MHZ_STEP);
    snprintf(s, sizeof(s), "%u", cfg.stop_mhz);
    label(2, "終了", s);
    four(2, MHZ_STEP);
    if (cfg.gain < 0) strcpy(s, "自動");
    else snprintf(s, sizeof(s), "%d", cfg.gain);
    label(3, "ゲイン", s);
    static const char *const GAIN_STEP[4] = {"-10", "-1", "+1", "+10"};
    four(3, GAIN_STEP);
    label(4, "バンド");
    for (int b = 0; b < 5; b++) {
        char name[2] = {BAND_NAME[b], 0};
        button(band_button(b), name, cfg.bands >> b & 1);
    }
    label(5, "ノイズ基準", has_noise ? "保存済み" : "未保存", has_noise ? GREEN : GRAY);
    button(half(5, 0), "保存");
    button(half(5, 1), "消去");
}

/* ---- receiver settings: step, dwell, detector, calibration, graph scale ---- */
const char *const STEP_NAME[] = {"40", "30", "15", "7.5"};

void draw_receiver() {
    char s[32];
    draw_title("受信設定", 1);
    label(0, "ステップ");
    for (int i = 0; i < 4; i++) button(col(0, i), STEP_NAME[i], (int)cfg.rate_code == i);
    snprintf(s, sizeof(s), "%u", cfg.dwell_ms);
    label(1, "滞在ms", s);
    static const char *const DWELL_STEP[4] = {"-50", "-5", "+5", "+50"};
    four(1, DWELL_STEP);
    label(2, "検波");
    button(half(2, 0), "最大", cfg.max_hold);
    button(half(2, 1), "平均", !cfg.max_hold);
    snprintf(s, sizeof(s), "%+.1f", cfg.cal_tenths / 10.0);
    label(3, "補正dB", s);
    static const char *const CAL_STEP[4] = {"-5", "-.5", "+.5", "+5"};
    four(3, CAL_STEP);
    snprintf(s, sizeof(s), "%ddBm", cfg.ytop);
    label(4, "上限", s);
    button(half(4, 0), "-5");
    button(half(4, 1), "+5");
    snprintf(s, sizeof(s), "%ddB", cfg.yrange);
    label(5, "範囲", s);
    button(half(5, 0), "-5");
    button(half(5, 1), "+5");
}

/* ---- advanced settings: orientation, warning, brightness, power saving,
 * landscape button panel ---- */
void sleep_text(int seconds, char *out, size_t size) {
    if (seconds <= 0) snprintf(out, size, "しない");
    else if (seconds < 60) snprintf(out, size, "%d秒", seconds);
    else if (seconds % 60 == 0) snprintf(out, size, "%d分", seconds / 60);
    else snprintf(out, size, "%d分%d秒", seconds / 60, seconds % 60);
}

void draw_advanced() {
    char s[32];
    draw_title("詳細設定", 1);
    /* orientation, and turned by 180 degrees (either orientation) */
    label(0, "画面の向き");
    button(cell(0, 0, 1, 3), "縦", !cfg.lcd_land); /* three equal buttons, same edges as the rows */
    button(cell(0, 1, 2, 3), "横", cfg.lcd_land);
    button(cell(0, 2, 3, 3), "反転", cfg.lcd_flip);
    /* "ゲイン下げて" shows when the strongest point exceeds this (dBFS) */
    snprintf(s, sizeof(s), "%ddBFS", cfg.warn_dbfs);
    label(1, "警告", s);
    static const char *const WARN_STEP[4] = {"-5", "-1", "+1", "+5"};
    four(1, WARN_STEP);
    snprintf(s, sizeof(s), "%d%%", cfg.lcd_bright);
    label(2, "明るさ", s);
    button(half(2, 0), "-10");
    button(half(2, 1), "+10");
    sleep_text(cfg.lcd_sleep, s, sizeof(s));
    label(3, "省電力", s);
    static const char *const SLEEP_STEP[4] = {"-60", "-10", "+10", "+60"};
    four(3, SLEEP_STEP);
    /* landscape button panel: seconds until it slides back by itself */
    if (cfg.lcd_panel > 0) snprintf(s, sizeof(s), "%d秒", cfg.lcd_panel);
    else strcpy(s, "押すまで");
    label(4, "ボタン", s);
    button(half(4, 0), "-1秒");
    button(half(4, 1), "+1秒");
    /* Wi-Fi state (set on the web page): where to find the page */
    /* (yellow: host because the client connection failed) */
    const char *mode = st.wifi_state == 3   ? "OFF"
                       : st.wifi_state == 1 ? "接続中…"
                       : st.wifi_state == 2 ? "クライアント"
                                            : "ホスト";
    uint16_t c = st.wifi_fallback ? YELLOW : GRAY;
    snprintf(s, sizeof(s), "Wi-Fi: %s", mode);
    if (landscape) { /* one line */
        int x = text(0, row_text_y(5), s, c);
        text(x + 8, row_text_y(5), st.wifi_ip, c);
    } else {
        text(0, row_y(5) + 1, s, c);
        text(56, row_y(5) + BH - 17, st.wifi_ip, c);
    }
}

/* ---- Wi-Fi screen: on / off, and the settings made on the web page
 * (mode, the host's and the joined network's names and passwords) ---- */
WifiInfo wi;

/* Text wrapped at max_w pixels; returns the y below it. */
int text_wrap(int x, int y, int max_w, const char *s, uint16_t c, int line_h) {
    char line[80];
    while (*s) {
        size_t n = 0;
        int w = 0;
        const char *p = s;
        while (*p) {
            const char *q = p;
            int gw = glyph(next_code(q))->width;
            if (w + gw > max_w && n) break;
            if (n + (q - p) >= sizeof(line)) break;
            memcpy(line + n, p, q - p);
            n += q - p;
            w += gw;
            p = q;
        }
        line[n] = '\0';
        text(x, y, line, c);
        y += line_h;
        s = p;
    }
    return y;
}

void draw_wifi() {
    char s[48];
    draw_title("Wi-Fi設定", 1);
    label(0, "Wi-Fi");
    button(half(0, 0), "ON", wi.on);
    button(half(0, 1), "OFF", !wi.on);
    const int LH = landscape ? 18 : 20, VX = landscape ? 96 : 92, VW = scr_w - VX;
    int y = row_y(1);
    /* state and the page's address */
    if (!wi.on) strcpy(s, "OFF");
    else if (wi.state == 1) strcpy(s, "接続中…");
    else if (wi.state == 2) strcpy(s, "クライアント");
    else strcpy(s, wi.fallback ? "ホスト（接続失敗）" : "ホスト");
    text(0, y, "状態", GRAY);
    y = text_wrap(VX, y, VW, s, !wi.on ? RED : wi.fallback ? YELLOW : GREEN, LH);
    if (wi.on && wi.ip[0]) {
        text(0, y, "アドレス", GRAY);
        y = text_wrap(VX, y, VW, wi.ip, WHITE, LH);
    }
    if (wi.mode == 1) snprintf(s, sizeof(s), "クライアント %d秒", wi.wait);
    else strcpy(s, "ホスト");
    text(0, y, "モード", GRAY);
    y = text_wrap(VX, y, VW, s, WHITE, LH);
    /* the host (own access point) and the network joined in client mode */
    text(0, y, "ホスト", BLUE);
    y += LH;
    text(4, y, "SSID", GRAY);
    y = text_wrap(VX, y, VW, wi.ap_ssid, WHITE, LH);
    text(4, y, "パスワード", GRAY);
    y = text_wrap(VX, y, VW, wi.ap_pass, WHITE, LH);
    text(0, y, "接続先", BLUE);
    y += LH;
    text(4, y, "SSID", GRAY);
    y = text_wrap(VX, y, VW, wi.ssid[0] ? wi.ssid : "未設定", WHITE, LH);
    text(4, y, "パスワード", GRAY);
    text_wrap(VX, y, VW, wi.pass[0] ? wi.pass : "なし", WHITE, LH);
}

void press_wifi(int x, int y) {
    if (BTN_BACK.in(x, y)) screen = SETTINGS;
    else if (half(0, 0).in(x, y)) shared_set_wifi_on(true);
    else if (half(0, 1).in(x, y)) shared_set_wifi_on(false);
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
            /* panel buttons: four rows, gaps included; the panel slides
             * back once a button was pressed */
            panel_open = false;
            switch (constrain((y - PY) * 4 / PH, 0, 3)) {
            case 0: apply("cfg hold=%d", !cfg.hold); break;
            case 1: apply("cfg avg=%d", !cfg.avg); break;
            case 2: memcpy(peak, data, N); break;
            default:
                panel_open = false, panel_px = 0;
                screen = SETTINGS;
            }
        } else if (x >= scr_w - TAB_W - 4 && y < TY - 2) {
            if (panel_open) panel_open = false; /* the tab closes it again */
            else open_panel();
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
    static const int WARN[] = {-5, -1, 1, 5}, SLEEP[] = {-60, -10, 10, 60};
    if (BTN_BACK.in(x, y)) screen = SETTINGS;
    else if (cell(0, 0, 1, 3).in(x, y)) apply("cfg lland=%d", 0);
    else if (cell(0, 1, 2, 3).in(x, y)) apply("cfg lland=%d", 1);
    else if (cell(0, 2, 3, 3).in(x, y)) apply("cfg lflip=%d", !cfg.lcd_flip);
    else if (half(2, 0).in(x, y)) apply("cfg lbright=%d", max(cfg.lcd_bright - 10, 10));
    else if (half(2, 1).in(x, y)) apply("cfg lbright=%d", min(cfg.lcd_bright + 10, 100));
    else if (half(4, 0).in(x, y)) apply("cfg lpanel=%d", max(cfg.lcd_panel - 1, 0));
    else if (half(4, 1).in(x, y)) apply("cfg lpanel=%d", min(cfg.lcd_panel + 1, 30));
    else
        for (int i = 0; i < 4; i++) {
            if (col(1, i).in(x, y)) apply("cfg warn=%d", constrain(cfg.warn_dbfs + WARN[i], -60, 0));
            else if (col(3, i).in(x, y)) apply("cfg lsleep=%d", constrain(cfg.lcd_sleep + SLEEP[i], 0, 600));
        }
}

/* Gain steps: -1 is the hardware AGC, manual gains are 2 .. gain_max, so
 * stepping down from 2 gives the AGC and up from it gives 2. */
int step_gain(int gain, int delta) {
    int top = st.gain_max ? (int)st.gain_max : 90;
    if (gain < 0) return delta > 0 ? 2 : -1;
    int g = gain + delta;
    if (g < 2) return -1;
    return min(g, top);
}

void press_receiver(int x, int y) {
    static const int DWELL[] = {-50, -5, 5, 50}, CAL[] = {-50, -5, 5, 50};
    if (BTN_BACK.in(x, y)) {
        screen = SETTINGS;
        return;
    }
    for (int i = 0; i < 4; i++) {
        if (col(0, i).in(x, y)) apply("cfg rate=%d", i);
        else if (col(1, i).in(x, y)) apply("cfg dwell=%d", constrain((int)cfg.dwell_ms + DWELL[i], 5, 500));
        else if (col(3, i).in(x, y)) apply("cfg cal=%d", constrain(cfg.cal_tenths + CAL[i], -1000, 1000));
    }
    if (half(2, 0).in(x, y)) apply("cfg det=%d", 1);
    else if (half(2, 1).in(x, y)) apply("cfg det=%d", 0);
    else if (half(4, 0).in(x, y)) apply("cfg ytop=%d", cfg.ytop - 5);
    else if (half(4, 1).in(x, y)) apply("cfg ytop=%d", cfg.ytop + 5);
    else if (half(5, 0).in(x, y)) apply("cfg yrange=%d", max(cfg.yrange - 5, 10));
    else if (half(5, 1).in(x, y)) apply("cfg yrange=%d", cfg.yrange + 5);
}

void set_span(int start, int stop) {
    char line[40];
    snprintf(line, sizeof(line), "cfg start=%d stop=%d", start, stop);
    shared_apply(line);
}

void press_settings(int x, int y) {
    static const int MHZ[] = {-10, -1, 1, 10}, GAIN[] = {-10, -1, 1, 10};
    char line[32];
    int a = cfg.start_mhz, b = cfg.stop_mhz;
    if (BTN_BACK.in(x, y)) screen = MAIN;
    else if (BTN_ADVANCED.in(x, y)) screen = ADVANCED;
    else if (BTN_RECEIVER.in(x, y)) screen = RECEIVER;
    else if (BTN_WIFI.in(x, y)) screen = WIFI;
    else if (preset_button(0).in(x, y)) set_span(PRESET[0][0], PRESET[0][1]);
    else if (preset_button(1).in(x, y)) set_span(PRESET[1][0], PRESET[1][1]);
    else if (preset_button(2).in(x, y)) set_span(PRESET[2][0], PRESET[2][1]);
    else if (half(5, 1).in(x, y)) shared_clear_noise();
    else if (half(5, 0).in(x, y) && have) {
        /* the running average of the current span, as the page saves it */
        static uint8_t out[FRAME_SIZE];
        memset(out, 0, FRAME_HEADER);
        memcpy(out, "NSE2", 4);
        uint32_t fa = (uint32_t)lround(f0 * 1000), fb = (uint32_t)lround(f1 * 1000);
        uint16_t n = N;
        memcpy(out + 4, &fa, 4);
        memcpy(out + 8, &fb, 4);
        memcpy(out + 12, &n, 2);
        memcpy(out + FRAME_HEADER, avg8, N);
        shared_store_noise(out);
    } else {
        /* the span keeps at least 5 MHz (as the firmware enforces) */
        for (int i = 0; i < 4; i++) {
            if (col(1, i).in(x, y)) set_span(constrain(a + MHZ[i], 2300, b - 5), b);
            else if (col(2, i).in(x, y)) set_span(a, constrain(b + MHZ[i], a + 5, 6000));
            else if (col(3, i).in(x, y)) apply("cfg gain=%d", step_gain(cfg.gain, GAIN[i]));
        }
        for (int k = 0; k < 5; k++)
            if (band_button(k).in(x, y)) {
                snprintf(line, sizeof(line), "cfg bands=%u", cfg.bands ^ (1u << k));
                shared_apply(line);
            }
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
            x = flipped ? LCD_H - 1 - y : y;
            y = flipped ? px : LCD_W - 1 - px;
        } else if (flipped) { /* portrait turned by 180 degrees */
            x = LCD_W - 1 - x;
            y = LCD_H - 1 - y;
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
    /* all screens follow the chosen orientation; a board without touch has
     * nothing but the main screen, always landscape */
    bool land = cfg.lcd_land || !LCD_HAS_TOUCH;
    /* "上下反転" turns either orientation by 180 degrees */
    bool flip = land ? LCD_LANDSCAPE_FLIP != cfg.lcd_flip : cfg.lcd_flip;
    if (land != landscape || flip != flipped) {
        lcd_set_landscape(land, flip);
        flipped = flip;
        set_layout(land);
    }
    st = shared_status();
    if (screen == WIFI) wi = shared_wifi();
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
        else if (screen == RECEIVER) draw_receiver();
        else if (screen == WIFI) draw_wifi();
        else draw_advanced();
        lcd_push_rows(strip_y, min(STRIP, scr_h - strip_y), scr_w, buf);
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

int backlight = -1; /* percent currently set, -1 = not yet */

/* Every BATTERY_READ_MS. Each read is a transfer the expander may garble
 * (seen: about 1 in 10 at a 2 s interval), so it reads seldom. */
void read_battery() {
    if (!BATTERY_GAUGE) return;
    static float mv_avg;
    static unsigned misses;
    int raw = lcd_battery_raw();
    float mv = raw * BATTERY_MV_PER_COUNT;
    /* A reading far from the average is a garbled transfer (seen: 67
     * instead of about 435), not the battery: it is dropped. */
    bool garbled = raw >= 0 && mv_avg && fabsf(mv - mv_avg) > 300;
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
    shared_set_battery(battery);
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
    if (want != backlight && lcd_backlight(want)) backlight = want;
}

#ifndef LCD_BUTTON_LONG_MS
#define LCD_BUTTON_LONG_MS 1000
#endif
/* The board's user button (1.47: BOOT): a short press resets the peak hold,
 * a long one (LCD_BUTTON_LONG_MS) switches peak hold on or off. Like a tap,
 * a press on a dimmed or dark screen only brings the brightness back.
 * Returns true when the screen should be redrawn. */
bool poll_button() {
    static bool down, held, woke;
    static uint32_t since;
    uint32_t now = millis();
    bool pressed = lcd_user_button();
    if (pressed && !down) {
        down = true, held = false, since = now;
        cfg = shared_config();
        woke = backlight < cfg.lcd_bright;
        last_touch = now;
        if (woke) update_backlight(now);
        return woke;
    }
    if (pressed) {
        last_touch = now;
        if (!held && !woke && now - since >= LCD_BUTTON_LONG_MS) {
            held = true;
            apply("cfg hold=%d", !cfg.hold);
            return true;
        }
        return false;
    }
    if (down) {
        down = false;
        if (!held && !woke && now - since >= 30) { /* 30 ms: no bounce */
            memcpy(peak, data, N);
            return true;
        }
    }
    return false;
}

/* Brings the LCD up, waiting as long as the I/O expander takes to answer. */
void bring_up() {
    cfg = shared_config();
    last_touch = millis();
    backlight = wanted_backlight(last_touch);
    while (!lcd_board_begin()) {
    }
    set_layout(false); /* the LCD set-up leaves it in portrait */
    flipped = false;
    read_battery();
    render();
    lcd_display_on();
    lcd_backlight(backlight);
}

void lcd_task(void *) {
    buf = (uint16_t *)malloc(LCD_H * STRIP * sizeof(uint16_t));
    if (!buf) vTaskDelete(nullptr); /* no memory for the strip buffer */
    bring_up();
    bool expander_lost = false;
    uint32_t last_draw = millis(), last_battery = last_draw, last_refresh = last_draw;
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
                else if (screen == RECEIVER) press_receiver(touch.x, touch.y);
                else if (screen == WIFI) press_wifi(touch.x, touch.y);
                else press_advanced(touch.x, touch.y);
                cfg = shared_config(); /* a changed brightness applies at once */
            }
            waking = false;
            dirty = true;
        }
        if (poll_button()) dirty = true;
        uint32_t s = shared_frame(frame, seq);
        if (s != seq) {
            seq = s;
            if (seq) take_frame();
            dirty = true;
        }
        uint32_t now = millis();
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
        /* landscape button panel: hide after lcd_panel seconds, slide in or out */
        bool sliding = false;
        if (landscape && screen == MAIN) {
            if (panel_open && cfg.lcd_panel > 0 && (int32_t)(panel_until - now) <= 0) panel_open = false;
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
