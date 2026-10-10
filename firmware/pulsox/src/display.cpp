// TFT screens. See display.h for the API and the design this implements.
//
// Everything is drawn into a 128x128 RGB565 frame buffer in RAM and only the rectangles
// that changed are sent to the panel, so nothing flickers and a frame costs a few ms of
// SPI time. Text and icons are the 4-bit masks of display_assets.h (generated from the
// design file by tools/gen_display_assets.py), blended over the buffer.
//
// Layout numbers are the CSS of the mockups on a 128 px grid. Text baselines follow
// from the line boxes: Barlow Semi Condensed has ascent 1.0 em and descent 0.2 em, so
// a line box of height L with font size S puts the baseline at top + (L - 1.2 S) / 2 + S.
// Fractional positions are rounded to the pixel grid.
#include <Arduino.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"
#if !defined(DISPLAY_PPG_INPUT_HZ) && defined(SENSOR_FS_HZ)
#define DISPLAY_PPG_INPUT_HZ SENSOR_FS_HZ
#endif
#include <driver/gpio.h>
#include "display.h"
#include "display_assets.h"
#include "tft_st7735.h"

namespace {

constexpr int W = 128;
constexpr int H = 128;

// ---- Palette (RGB565 of the table in sheet D) -----------------------------------
constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}
constexpr uint16_t C_BG = 0x0000;
constexpr uint16_t C_PLOT_BG = rgb(0x08, 0x10, 0x18);
constexpr uint16_t C_LINE = rgb(0x10, 0x24, 0x29);  // dividers, plot border and grid
constexpr uint16_t C_TEXT = 0xFFFF;
constexpr uint16_t C_TEXT2 = rgb(0x8C, 0xA2, 0xB5);
constexpr uint16_t C_NORMAL = rgb(0x31, 0xE3, 0x8C);
constexpr uint16_t C_CAUTION = rgb(0xFF, 0xBA, 0x18);
constexpr uint16_t C_CRITICAL = rgb(0xFF, 0x41, 0x42);
constexpr uint16_t C_HEART = rgb(0xFF, 0x5D, 0x7B);
constexpr uint16_t C_WIFI = rgb(0x5A, 0xC7, 0xFF);
constexpr uint16_t C_BATTERY = rgb(0xC6, 0xD3, 0xDE);

// ---- Layout -------------------------------------------------------------------------
constexpr int BAR_LINE_Y = 15;       // bottom line of the status bar
constexpr int WIFI_X = 6, WIFI_Y = 2;
constexpr int BATT_X = 100, BATT_Y = 3;  // outline 22x10, right edge at x = 122
constexpr int BATT_TEXT_RIGHT = 97;      // 3 px gap before the battery
constexpr int BATT_TEXT_BASE = 11;
constexpr int BATT_LEVEL_X = BATT_X + 2, BATT_LEVEL_Y = BATT_Y + 2;
constexpr int BATT_LEVEL_W = 15, BATT_LEVEL_H = 6;

constexpr int SPO2_X = 6;            // left column of the values row: x 6-75
constexpr int SPO2_LABEL_BASE = 26;
constexpr int SPO2_VALUE_BASE = 62;  // the "%" shares this baseline
constexpr int BPM_CX = 101;          // right column, x 80-121
constexpr int BPM_VALUE_BASE = 53;
constexpr int BPM_LABEL_BASE = 65;
constexpr int HEART_X = BPM_CX - HEART_CX, HEART_Y = 25 - HEART_CY;  // heart center at (101, 25)

constexpr int PLOT_X = 4, PLOT_Y = 70, PLOT_W = 120, PLOT_H = 40;   // box with its 1 px border
constexpr int PLOT_COLS = PLOT_W - 2, PLOT_ROWS = PLOT_H - 2;       // 118 x 38 inside
constexpr float PLOT_BASE_Y = 33.0f, PLOT_PEAK_Y = 5.0f;  // trace baseline and peak, from the inner top

constexpr int BANNER_X = 4, BANNER_Y = 112, BANNER_W = 120, BANNER_H = 12;
constexpr int BANNER_TEXT_BASE = 122;
constexpr int BANNER_ICON_Y = 114;

constexpr int NF_ICON_X = 43, NF_ICON_Y = 25;  // "no finger" icon, 42x40 centered
constexpr int NF_ERROR_BASE = 92;
constexpr int NF_TEXT_BASE = 110;

// Letter-spacing of the mockups, in 1/16 px (em x font size).
constexpr int TRACK_LABEL = 9;    // "SpO2" label: .06em x 9 px
constexpr int TRACK_BANNER = 12;  // alert banner: .08em x 9 px
constexpr int TRACK_MEASURING = 14;  // "MIDIENDO": .1em x 9 px
constexpr int TRACK_BPM = 10;     // "BPM": .08em x 8 px
constexpr int TRACK_ERROR = 45;   // "ERROR": .14em x 20 px
constexpr int SUB_SHIFT = 1;      // the subscript "2" sits .22em x 6.5 px = 1.4 px lower

// ---- Animation ----------------------------------------------------------------------
constexpr uint32_t HEART_FRAME_MS = 33;
constexpr uint32_t PLOT_FRAME_MS = 40;
constexpr uint32_t SEEK_FRAME_MS = 50;
constexpr uint32_t SEEK_PERIOD_MS = 1200;
constexpr uint32_t ALARM_PERIOD_MS = 1000;
constexpr uint8_t ALARM_DIM = 31;  // the border never goes fully off: 12 % opacity

// ---- State --------------------------------------------------------------------------
enum class Scene : uint8_t { NONE, MEASURING, NO_FINGER };

uint16_t *fb = nullptr;   // frame buffer, native byte order (the SPI driver swaps)
uint8_t *cov = nullptr;   // trace coverage, PLOT_ROWS x PLOT_COLS, 0-16

Scene scene = Scene::NONE;
DisplayAlert alertLevel = DisplayAlert::NORMAL;
int spo2Value = 0;
int bpmValue = 0;
bool wifiOn = false;
uint8_t battPct = 0;

float heartPhase = 0.0f;       // 0-1 within the beat
int8_t heartScaleDrawn = -1;   // what is on screen, to skip redundant redraws
int8_t heartOpacityDrawn = -1;
uint32_t heartMs = 0;

float hist[PLOT_COLS];         // trace history, one value per pixel column
int histHead = 0;              // index of the oldest value (the next one to be overwritten)
int histCount = 0;             // columns that hold data; the newest sit at the right edge
bool plotDirty = false;
float ppgPhase = 0.0f;         // columns owed to the trace, accumulates DISPLAY_PPG_PX_PER_S / INPUT_HZ
float ppgSum = 0.0f;
int ppgCount = 0;
float scaleLo = 0.0f, scaleHi = 0.0f;
bool scaleValid = false;
uint32_t plotMs = 0;

bool asleep = false;           // panel in sleep-in, backlight off (displaySleep)

uint32_t alarmT0 = 0;
int8_t alarmOn = -1;           // blink state on screen, -1 = unknown

int8_t seekOpacityDrawn = -1;
uint32_t seekMs = 0;

// ---- Dirty rectangles ---------------------------------------------------------------
struct Rect {
  int16_t x, y, w, h;
};
constexpr int MAX_DIRTY = 6;
Rect dirty[MAX_DIRTY];
int dirtyCount = 0;

void markDirty(int x, int y, int w, int h) {
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > W) w = W - x;
  if (y + h > H) h = H - y;
  if (w <= 0 || h <= 0) return;
  for (int i = 0; i < dirtyCount; i++) {
    const Rect &d = dirty[i];
    if (x >= d.x && y >= d.y && x + w <= d.x + d.w && y + h <= d.y + d.h) return;  // already covered
  }
  for (int i = dirtyCount - 1; i >= 0; i--) {  // drop what the new one covers
    const Rect &d = dirty[i];
    if (d.x >= x && d.y >= y && d.x + d.w <= x + w && d.y + d.h <= y + h) dirty[i] = dirty[--dirtyCount];
  }
  if (dirtyCount == MAX_DIRTY) {  // too fragmented: send everything
    dirty[0] = {0, 0, W, H};
    dirtyCount = 1;
    return;
  }
  dirty[dirtyCount++] = {(int16_t)x, (int16_t)y, (int16_t)w, (int16_t)h};
}

void flushDirty() {
  if (dirtyCount == 0) return;
  TftPanel &tft = tftScreen();
  tft.startWrite();
  for (int i = 0; i < dirtyCount; i++) {
    const Rect &d = dirty[i];
    // The SPI driver reads the pixels as 32-bit words and the ESP32-C3 faults on a
    // misaligned one, so every row must start on an even pixel: widen the rectangle.
    const int x = d.x & ~1;
    const int w = ((d.x + d.w + 1) & ~1) - x;
    tft.setAddrWindow(x, d.y, w, d.h);
    if (w == W) {  // full rows are contiguous in the buffer: one transfer
      tft.writePixels(fb + d.y * W, (uint32_t)w * d.h);
    } else {
      for (int row = 0; row < d.h; row++) tft.writePixels(fb + (d.y + row) * W + x, w);
    }
  }
  tft.endWrite();
  dirtyCount = 0;
}

// ---- Drawing primitives -------------------------------------------------------------

// Mix of fg over bg, a = 0 (bg) to 255 (fg). Both RGB565.
inline uint16_t blend(uint16_t bg, uint16_t fg, uint8_t a) {
  constexpr uint32_t MASK = 0x07E0F81F;  // spreads the 3 channels apart so one multiply mixes all
  uint32_t b = (bg | ((uint32_t)bg << 16)) & MASK;
  uint32_t f = (fg | ((uint32_t)fg << 16)) & MASK;
  uint32_t mixed = ((((f - b) * ((a + 4u) >> 3)) >> 5) + b) & MASK;  // alpha in 0-32
  return (uint16_t)(mixed | (mixed >> 16));
}

inline void blendPixel(int x, int y, uint16_t color, uint8_t a) {
  if ((unsigned)x >= (unsigned)W || (unsigned)y >= (unsigned)H || a == 0) return;
  uint16_t &p = fb[y * W + x];
  p = a == 255 ? color : blend(p, color, a);
}

void fillRect(int x, int y, int w, int h, uint16_t color) {
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > W) w = W - x;
  if (y + h > H) h = H - y;
  for (int row = y; row < y + h; row++) {
    uint16_t *p = fb + row * W + x;
    for (int i = 0; i < w; i++) p[i] = color;
  }
}

void blendRect(int x, int y, int w, int h, uint16_t color, uint8_t a) {
  for (int row = y; row < y + h; row++)
    for (int col = x; col < x + w; col++) blendPixel(col, row, color, a);
}

// Coverage (0-255) of the pixel (i, j) of a rounded corner of radius r, with (0, 0) the
// outermost pixel. 4x4 samples.
uint8_t cornerCoverage(int i, int j, int r) {
  int inside = 0;
  for (int sy = 0; sy < 4; sy++) {
    for (int sx = 0; sx < 4; sx++) {
      int dx = r * 8 - (i * 8 + sx * 2 + 1);  // distance to the circle center, in 1/8 px
      int dy = r * 8 - (j * 8 + sy * 2 + 1);
      if (dx * dx + dy * dy <= r * r * 64) inside++;
    }
  }
  return (uint8_t)(inside * 255 / 16);
}

// Filled rectangle with anti-aliased rounded corners. Meant to be drawn over a flat background.
void fillRoundRect(int x, int y, int w, int h, int r, uint16_t color) {
  fillRect(x, y + r, w, h - 2 * r, color);
  fillRect(x + r, y, w - 2 * r, r, color);
  fillRect(x + r, y + h - r, w - 2 * r, r, color);
  for (int j = 0; j < r; j++) {
    for (int i = 0; i < r; i++) {
      uint8_t a = cornerCoverage(i, j, r);
      blendPixel(x + i, y + j, color, a);
      blendPixel(x + w - 1 - i, y + j, color, a);
      blendPixel(x + i, y + h - 1 - j, color, a);
      blendPixel(x + w - 1 - i, y + h - 1 - j, color, a);
    }
  }
}

// 4-bit alpha mask (two pixels per byte, high nibble first) in a color; opacity scales it.
void drawMask(const uint8_t *bitmap, int w, int h, int x, int y, uint16_t color, uint8_t opacity = 255) {
  for (int row = 0; row < h; row++) {
    int py = y + row;
    if ((unsigned)py >= (unsigned)H) continue;
    for (int col = 0; col < w; col++) {
      int idx = row * w + col;
      uint8_t byte = bitmap[idx >> 1];
      uint8_t a4 = (idx & 1) ? (byte & 0x0F) : (byte >> 4);
      if (a4 == 0) continue;
      uint8_t a = a4 * 17;
      if (opacity != 255) a = (uint8_t)((a * (opacity + 1)) >> 8);
      blendPixel(x + col, py, color, a);
    }
  }
}

void drawIcon(const DispIcon &icon, int x, int y, uint16_t color, uint8_t opacity = 255) {
  if (icon.w) drawMask(icon.bitmap, icon.w, icon.h, x + icon.xo, y + icon.yo, color, opacity);
}

// ---- Text ---------------------------------------------------------------------------

uint16_t nextCode(const char *&s) {  // next UTF-8 code point, 0 at the end
  uint8_t c = (uint8_t)*s;
  if (c == 0) return 0;
  s++;
  if (c < 0x80) return c;
  if ((c & 0xE0) == 0xC0) {
    uint16_t code = c & 0x1F;
    if (*s) code = (code << 6) | (*s++ & 0x3F);
    return code;
  }
  uint16_t code = c & 0x0F;  // three bytes
  for (int i = 0; i < 2 && *s; i++) code = (code << 6) | (*s++ & 0x3F);
  return code;
}

const DispGlyph *findGlyph(const DispFont &font, uint16_t code) {
  for (uint8_t i = 0; i < font.count; i++)
    if (font.glyphs[i].code == code) return &font.glyphs[i];
  return nullptr;
}

// Draws `text` with the pen starting at x16 (1/16 px) on the given baseline and returns
// the pen position after the last glyph (letter-spacing included, as CSS does). U+2082 (the
// subscript 2 of "SpO₂") is drawn from `sub`, lowered. With draw = false it only measures.
int layoutText(const DispFont &font, int x16, int baseline, const char *text, uint16_t color, int track16,
               const DispFont *sub, uint8_t opacity, bool draw) {
  while (uint16_t code = nextCode(text)) {
    const DispFont *f = &font;
    int lower = 0;
    if (code == 0x2082) {
      f = sub ? sub : &font;
      code = '2';
      lower = SUB_SHIFT;
    }
    const DispGlyph *g = findGlyph(*f, code);
    if (!g) continue;
    if (draw && g->w)
      drawMask(f->bitmap + g->off, g->w, g->h, ((x16 + 8) >> 4) + g->xo, baseline + lower + g->yo, color, opacity);
    x16 += g->adv + track16;
  }
  return x16;
}

int drawText(const DispFont &font, int x16, int baseline, const char *text, uint16_t color, int track16 = 0,
             const DispFont *sub = nullptr, uint8_t opacity = 255) {
  return layoutText(font, x16, baseline, text, color, track16, sub, opacity, true);
}

int textWidth(const DispFont &font, const char *text, int track16 = 0, const DispFont *sub = nullptr) {
  return layoutText(font, 0, 0, text, 0, track16, sub, 255, false);
}

// Text centered on cx, as a flex item would be: the trailing letter-spacing counts.
void drawTextCentered(const DispFont &font, int cx, int baseline, const char *text, uint16_t color,
                      int track16 = 0, const DispFont *sub = nullptr) {
  drawText(font, cx * 16 - textWidth(font, text, track16, sub) / 2, baseline, text, color, track16, sub);
}

// ---- Values ---------------------------------------------------------------------------

uint16_t levelColor() {
  switch (alertLevel) {
    case DisplayAlert::CAUTION: return C_CAUTION;
    case DisplayAlert::CRITICAL: return C_CRITICAL;
    default: return C_NORMAL;
  }
}

// ---- Screen parts ---------------------------------------------------------------------
// None of these touch the alert border (the outer 2 px), so the border can be left alone.

void drawStatusBar() {
  const bool borderOn = scene == Scene::MEASURING && alertLevel != DisplayAlert::NORMAL;
  fillRect(2, 2, W - 4, BAR_LINE_Y - 2, C_BG);
  if (borderOn) {
    fillRect(2, BAR_LINE_Y, W - 4, 1, C_LINE);  // the ends belong to drawBorder()
  } else {
    fillRect(0, BAR_LINE_Y, W, 1, C_LINE);
  }
  drawIcon(ICON_WIFI, WIFI_X, WIFI_Y, wifiOn ? C_WIFI : C_TEXT2);

  uint16_t levelCol = battPct > 20 ? C_NORMAL : battPct > 10 ? C_CAUTION : C_CRITICAL;
  drawIcon(ICON_BATTERY, BATT_X, BATT_Y, C_BATTERY);
  int levelW = BATT_LEVEL_W * battPct / 100;
  if (battPct > 0 && levelW < 1) levelW = 1;
  fillRect(BATT_LEVEL_X, BATT_LEVEL_Y, levelW, BATT_LEVEL_H, levelCol);

  char text[8];
  snprintf(text, sizeof(text), "%u%%", (unsigned)battPct);
  drawText(FONT_S9, BATT_TEXT_RIGHT * 16 - textWidth(FONT_S9, text), BATT_TEXT_BASE, text, C_BATTERY);
  markDirty(0, 0, W, BAR_LINE_Y + 1);
}

void drawSpo2() {
  fillRect(SPO2_X, 16, 74, 52, C_BG);
  drawText(FONT_S9, SPO2_X * 16, SPO2_LABEL_BASE, "SpO₂", C_TEXT2, TRACK_LABEL, &FONT_S6);
  const bool have = spo2Value > 0;
  const uint16_t color = have ? levelColor() : C_TEXT2;
  char text[8] = "--";
  if (have) snprintf(text, sizeof(text), "%d", spo2Value);
  int end = drawText(FONT_B38, SPO2_X * 16, SPO2_VALUE_BASE, text, color);
  drawText(FONT_S15, end + 16, SPO2_VALUE_BASE, "%", color);  // 1 px gap
  markDirty(SPO2_X, 16, 74, 52);
}

void drawBpm() {
  fillRect(80, 34, 42, 34, C_BG);
  char text[8] = "--";
  if (bpmValue > 0) snprintf(text, sizeof(text), "%d", bpmValue);
  drawTextCentered(FONT_B22, BPM_CX, BPM_VALUE_BASE, text, bpmValue > 0 ? C_TEXT : C_TEXT2);
  drawTextCentered(FONT_S8, BPM_CX, BPM_LABEL_BASE, "BPM", C_TEXT2, TRACK_BPM);
  markDirty(80, 34, 42, 34);
}

void drawHeart(int scaleIdx, int opacityQ) {
  fillRect(HEART_X, HEART_Y, HEART_ZONE_W, HEART_ZONE_H, C_BG);
  drawIcon(ICON_HEART[scaleIdx], HEART_X, HEART_Y, C_HEART, (uint8_t)(opacityQ * 17));
  markDirty(HEART_X, HEART_Y, HEART_ZONE_W, HEART_ZONE_H);
  heartScaleDrawn = (int8_t)scaleIdx;
  heartOpacityDrawn = (int8_t)opacityQ;
}

void drawBanner() {
  fillRect(BANNER_X, BANNER_Y, BANNER_W, BANNER_H, C_BG);
  const int cx = BANNER_X + BANNER_W / 2;
  if (alertLevel == DisplayAlert::NORMAL) {
    const char *text = "MIDIENDO";
    int total = 4 * 16 + 4 * 16 + textWidth(FONT_S9, text, TRACK_MEASURING);  // dot, gap, text
    int left = cx * 16 - total / 2;
    fillRoundRect((left + 8) >> 4, BANNER_Y + 4, 4, 4, 2, C_NORMAL);
    drawText(FONT_S9, left + 8 * 16, BANNER_TEXT_BASE, text, C_TEXT2, TRACK_MEASURING);
  } else {
    const bool critical = alertLevel == DisplayAlert::CRITICAL;
    const uint16_t color = levelColor();
    const char *text = critical ? "¡SpO₂ CRÍTICA!" : "SpO₂ BAJA";
    int total = 9 * 16 + 3 * 16 + textWidth(FONT_B9, text, TRACK_BANNER, &FONT_B6);  // icon, gap, text
    int left = cx * 16 - total / 2;
    fillRoundRect(BANNER_X, BANNER_Y, BANNER_W, BANNER_H, 2, color);
    drawIcon(critical ? ICON_ALARM : ICON_WARN, (left + 8) >> 4, BANNER_ICON_Y, C_BG);
    drawText(FONT_B9, left + 12 * 16, BANNER_TEXT_BASE, text, C_BG, TRACK_BANNER, &FONT_B6);
  }
  markDirty(BANNER_X, BANNER_Y, BANNER_W, BANNER_H);
}

// Alert border, 2 px around the whole screen. alpha 0 clears it. The only thing under it
// that is not black is the status bar's bottom line, which is put back.
void drawBorder(uint8_t alpha) {
  const int strips[4][4] = {{0, 0, W, 2}, {0, H - 2, W, 2}, {0, 2, 2, H - 4}, {W - 2, 2, 2, H - 4}};
  for (const auto &s : strips) fillRect(s[0], s[1], s[2], s[3], C_BG);
  fillRect(0, BAR_LINE_Y, 2, 1, C_LINE);
  fillRect(W - 2, BAR_LINE_Y, 2, 1, C_LINE);
  if (alpha) {
    const uint16_t color = levelColor();
    for (const auto &s : strips) blendRect(s[0], s[1], s[2], s[3], color, alpha);
  }
  markDirty(0, 0, W, 2);
  markDirty(0, H - 2, W, 2);
  markDirty(0, 2, 2, H - 4);
  markDirty(W - 2, 2, 2, H - 4);
}

// ---- PPG trace ------------------------------------------------------------------------

int isqrt(int32_t v) {  // integer square root, v >= 0
  int32_t r = 0, bit = 1 << 28;
  while (bit > v) bit >>= 2;
  while (bit) {
    if (v >= r + bit) {
      v -= r + bit;
      r = (r >> 1) + bit;
    } else {
      r >>= 1;
    }
    bit >>= 2;
  }
  return (int)r;
}

// Adds a 2 px wide anti-aliased segment, between the centers of columns i and i+1, to the
// coverage buffer. y in 1/16 px from the top of the plot. Integer math: the ESP32-C3 has
// no FPU. The segment is a band of half-width 1 px around the line (distance to the
// infinite line, 0.5 px ramp) cut 1 px beyond the ends, which also covers the joints.
void strokeSegment(int i, int32_t y0, int32_t y1) {
  const int32_t dy = y1 - y0;
  const int32_t len = isqrt(256 + dy * dy);
  const int32_t inv = 65536 / len;  // 1 / len, 16.16
  const int32_t x0 = i * 16 + 8;
  int rowMin = (((dy < 0 ? y1 : y0)) - 24) >> 4;
  int rowMax = (((dy < 0 ? y0 : y1)) + 24) >> 4;
  if (rowMin < 0) rowMin = 0;
  if (rowMax > PLOT_ROWS - 1) rowMax = PLOT_ROWS - 1;
  const int colMin = i > 0 ? i - 1 : 0;
  const int colMax = i + 2 < PLOT_COLS ? i + 2 : PLOT_COLS - 1;
  for (int row = rowMin; row <= rowMax; row++) {
    const int32_t ay = row * 16 + 8 - y0;
    for (int col = colMin; col <= colMax; col++) {
      const int32_t ax = col * 16 + 8 - x0;
      const int32_t cross = ax * dy - ay * 16;
      const int32_t dist = (((cross < 0) ? -cross : cross) * inv) >> 16;
      if (dist >= 24) continue;
      const int32_t along = ((ax * 16 + ay * dy) * inv) >> 16;
      if (along < -16 || along > len + 16) continue;
      uint8_t a = (uint8_t)(24 - dist);
      if (a > 16) a = 16;
      uint8_t &c = cov[row * PLOT_COLS + col];
      if (a > c) c = a;
    }
  }
}

void drawPlot() {
  // Box: 1 px border, background and grid (a line every 19 px down and 20 px across,
  // starting at the top-left of the inside).
  fillRoundRect(PLOT_X, PLOT_Y, PLOT_W, PLOT_H, 2, C_LINE);
  fillRoundRect(PLOT_X + 1, PLOT_Y + 1, PLOT_COLS, PLOT_ROWS, 1, C_PLOT_BG);
  for (int gy = 0; gy < PLOT_ROWS; gy += 19) fillRect(PLOT_X + 1, PLOT_Y + 1 + gy, PLOT_COLS, 1, C_LINE);
  for (int gx = 0; gx < PLOT_COLS; gx += 20) fillRect(PLOT_X + 1 + gx, PLOT_Y + 1, 1, PLOT_ROWS, C_LINE);

  // Trace: autoscale the history to the band between the baseline and the peak. Until the
  // history fills up, the trace grows from the right edge.
  const int first = PLOT_COLS - histCount;  // first column with data
  int32_t yq[PLOT_COLS];
  if (histCount > 0) {
    float lo = hist[(histHead + first) % PLOT_COLS], hi = lo;
    for (int i = first + 1; i < PLOT_COLS; i++) {
      const float v = hist[(histHead + i) % PLOT_COLS];
      if (v < lo) lo = v;
      if (v > hi) hi = v;
    }
    if (!scaleValid) {
      scaleLo = lo;
      scaleHi = hi;
      scaleValid = true;
    }
    scaleLo = lo < scaleLo ? lo : scaleLo + (lo - scaleLo) * 0.1f;  // widen at once, narrow slowly
    scaleHi = hi > scaleHi ? hi : scaleHi + (hi - scaleHi) * 0.1f;
    float span = scaleHi - scaleLo;
    float base = scaleLo;
    if (span < DISPLAY_PPG_MIN_SPAN) {
      base -= (DISPLAY_PPG_MIN_SPAN - span) * 0.5f;
      span = DISPLAY_PPG_MIN_SPAN;
    }
    const float k = (PLOT_BASE_Y - PLOT_PEAK_Y) / span * 16.0f;
    for (int i = first; i < PLOT_COLS; i++) {
      float v = hist[(histHead + i) % PLOT_COLS];
      yq[i] = (int32_t)(PLOT_BASE_Y * 16.0f - (v - base) * k + 0.5f);
    }
  }
  memset(cov, 0, PLOT_ROWS * PLOT_COLS);
  for (int i = first; i + 1 < PLOT_COLS; i++) strokeSegment(i, yq[i], yq[i + 1]);

  const uint16_t color = levelColor();
  for (int row = 0; row < PLOT_ROWS; row++) {
    for (int col = 0; col < PLOT_COLS; col++) {
      uint8_t c = cov[row * PLOT_COLS + col];
      if (c) blendPixel(PLOT_X + 1 + col, PLOT_Y + 1 + row, color, c >= 16 ? 255 : (uint8_t)(c * 16));
    }
  }
  markDirty(PLOT_X, PLOT_Y, PLOT_W, PLOT_H);
  plotDirty = false;
}

void pushColumn(float v) {
  hist[histHead] = v;
  histHead = (histHead + 1) % PLOT_COLS;
  if (histCount < PLOT_COLS) histCount++;
  plotDirty = true;
}

void resetPlot() {
  histCount = 0;
  histHead = 0;
  scaleValid = false;
  ppgPhase = 0.0f;
  ppgSum = 0.0f;
  ppgCount = 0;
}

// ---- Heart animation --------------------------------------------------------------------
// The `beat` keyframes of the mockups: scale and opacity at a point of the cycle, eased
// out between keyframes.
struct BeatKey {
  float t, scale, opacity;
};
constexpr BeatKey BEAT[] = {{0.00f, 0.90f, 0.40f}, {0.10f, 1.28f, 1.00f}, {0.28f, 1.00f, 1.00f},
                            {0.40f, 1.12f, 1.00f}, {0.55f, 0.95f, 0.55f}, {1.00f, 0.90f, 0.40f}};

void beatAt(float phase, float &scale, float &opacity) {
  int i = 0;
  while (i < 4 && phase > BEAT[i + 1].t) i++;
  float u = (phase - BEAT[i].t) / (BEAT[i + 1].t - BEAT[i].t);
  u = u * (2.0f - u);  // ease-out
  scale = BEAT[i].scale + (BEAT[i + 1].scale - BEAT[i].scale) * u;
  opacity = BEAT[i].opacity + (BEAT[i + 1].opacity - BEAT[i].opacity) * u;
}

void updateHeart(uint32_t now, bool force) {
  if (!force && now - heartMs < HEART_FRAME_MS) return;
  uint32_t dt = force ? 0 : now - heartMs;
  heartMs = now;
  float scale, opacity;
  if (bpmValue > 0) {
    if (dt > 200) dt = 200;  // after a stall, do not jump through several beats
    heartPhase += dt * bpmValue / 60000.0f;
    heartPhase -= floorf(heartPhase);
    beatAt(heartPhase, scale, opacity);
  } else {
    scale = 1.0f;  // no rate yet: a steady, dim heart
    opacity = BEAT[0].opacity;
  }
  int idx = (int)((scale - HEART_SCALE_MIN) / (HEART_SCALE_MAX - HEART_SCALE_MIN) * (HEART_SCALES - 1) + 0.5f);
  if (idx < 0) idx = 0;
  if (idx > HEART_SCALES - 1) idx = HEART_SCALES - 1;
  int op = (int)(opacity * 15.0f + 0.5f);
  if (idx != heartScaleDrawn || op != heartOpacityDrawn) drawHeart(idx, op);
}

// ---- Screens ----------------------------------------------------------------------------

void renderMeasuring() {
  fillRect(0, 0, W, H, C_BG);
  drawStatusBar();
  drawSpo2();
  drawBpm();
  updateHeart(millis(), true);
  drawPlot();
  drawBanner();
  alarmOn = -1;
  alarmT0 = millis();
  if (alertLevel == DisplayAlert::CAUTION) drawBorder(255);
  markDirty(0, 0, W, H);
}

void drawNoFingerIcon(uint8_t arcOpacity) {
  fillRect(NF_ICON_X, NF_ICON_Y, 42, 40, C_BG);
  drawIcon(ICON_NOFINGER_ARC, NF_ICON_X, NF_ICON_Y, C_TEXT2, arcOpacity);
  drawIcon(ICON_NOFINGER_BODY, NF_ICON_X, NF_ICON_Y, C_TEXT2);
  drawIcon(ICON_NOFINGER_BADGE, NF_ICON_X, NF_ICON_Y, C_CRITICAL);
  drawIcon(ICON_NOFINGER_X, NF_ICON_X, NF_ICON_Y, C_BG);
  markDirty(NF_ICON_X, NF_ICON_Y, 42, 40);
}

// The `seek` keyframes: the arc fades 1 -> 0.25 -> 1, ease-in-out each way.
int seekOpacityAt(uint32_t now) {
  float p = (now % SEEK_PERIOD_MS) / (float)SEEK_PERIOD_MS;
  float u = p < 0.5f ? p * 2.0f : (1.0f - p) * 2.0f;  // 0 -> 1 -> 0
  u = u * u * (3.0f - 2.0f * u);
  return (int)((1.0f - 0.75f * u) * 15.0f + 0.5f);
}

void renderNoFinger() {
  fillRect(0, 0, W, H, C_BG);
  drawStatusBar();
  seekOpacityDrawn = (int8_t)seekOpacityAt(millis());
  drawNoFingerIcon((uint8_t)(seekOpacityDrawn * 17));
  drawTextCentered(FONT_B20, W / 2, NF_ERROR_BASE, "ERROR", C_CRITICAL, TRACK_ERROR);
  drawTextCentered(FONT_S13, W / 2, NF_TEXT_BASE, "Dedo no encontrado", C_TEXT);
  markDirty(0, 0, W, H);
}

}  // namespace

// ---- Public API ---------------------------------------------------------------------------

DisplayAlert displayAlertFor(int spo2) {
  if (spo2 <= 0 || spo2 >= DISPLAY_SPO2_NORMAL_MIN) return DisplayAlert::NORMAL;
  return spo2 >= DISPLAY_SPO2_CAUTION_MIN ? DisplayAlert::CAUTION : DisplayAlert::CRITICAL;
}

bool displayBegin() {
  if (fb) return true;
  fb = (uint16_t *)malloc(W * H * sizeof(uint16_t));
  cov = (uint8_t *)malloc(PLOT_ROWS * PLOT_COLS);
  if (!fb || !cov) {
    free(fb);
    free(cov);
    fb = nullptr;
    cov = nullptr;
    return false;
  }
  tftBegin();
  displaySetNoFinger();
  flushDirty();
  tftBacklight(true);  // tftBegin() leaves it off (TFT_BL_ON_AT_BEGIN 0): light up with something drawn
  return true;
}

void displaySetStatus(bool wifiConnected, uint8_t batteryPct) {
  if (!fb) return;
  if (batteryPct > 100) batteryPct = 100;
  if (wifiConnected == wifiOn && batteryPct == battPct) return;
  wifiOn = wifiConnected;
  battPct = batteryPct;
  if (scene != Scene::NONE) drawStatusBar();
}

void displaySetMeasuring(int spo2, int bpm) {
  if (!fb) return;
  if (spo2 > 100) spo2 = 100;
  if (bpm > 999) bpm = 999;
  const DisplayAlert level = displayAlertFor(spo2);
  const bool fresh = scene != Scene::MEASURING;
  const bool spo2Changed = spo2 != spo2Value;
  const bool bpmChanged = bpm != bpmValue;
  if (!fresh && !spo2Changed && !bpmChanged) return;

  spo2Value = spo2;
  bpmValue = bpm;
  if (fresh || level != alertLevel) {  // new screen or new color scheme: redraw it all
    if (fresh) {
      scene = Scene::MEASURING;
      resetPlot();
      heartPhase = 0.0f;
      heartMs = millis();
    }
    alertLevel = level;
    renderMeasuring();
    return;
  }
  if (spo2Changed) drawSpo2();
  if (bpmChanged) drawBpm();
}

void displaySetNoFinger() {
  if (!fb || scene == Scene::NO_FINGER) return;
  scene = Scene::NO_FINGER;
  alertLevel = DisplayAlert::NORMAL;
  spo2Value = 0;
  bpmValue = 0;
  renderNoFinger();
}

void displayPushPpg(float sample) {
  if (!fb) return;
  ppgSum += sample;
  ppgCount++;
  ppgPhase += DISPLAY_PPG_PX_PER_S / DISPLAY_PPG_INPUT_HZ;
  if (ppgPhase >= 1.0f) {  // one column owed: the mean of the samples that fell into it
    ppgPhase -= 1.0f;
    pushColumn(ppgSum / ppgCount);
    ppgSum = 0.0f;
    ppgCount = 0;
  }
}

void displaySleep() {
  if (!fb || asleep) return;
  asleep = true;
  tftBacklight(false);
  TftPanel &tft = tftScreen();
  tft.enableDisplay(false);  // DISPOFF
  tft.enableSleep(true);     // SLPIN
}

void displayWake() {
  if (!fb || !asleep) return;
  TftPanel &tft = tftScreen();
  tft.enableSleep(false);  // SLPOUT: the controller needs 120 ms before the next command
  delay(120);
  tft.enableDisplay(true);
  asleep = false;
  markDirty(0, 0, W, H);
  flushDirty();
  tftBacklight(true);
}

void displayHoldPins(bool hold) {
  const gpio_num_t pins[] = {(gpio_num_t)PIN_BL_TFT, (gpio_num_t)PIN_CS_TFT, (gpio_num_t)PIN_RST_TFT};
  if (hold) {
    pinMode(PIN_BL_TFT, OUTPUT);
    pinMode(PIN_CS_TFT, OUTPUT);
    pinMode(PIN_RST_TFT, OUTPUT);
    digitalWrite(PIN_BL_TFT, !TFT_BL_ON);
    digitalWrite(PIN_CS_TFT, HIGH);
    digitalWrite(PIN_RST_TFT, HIGH);
    for (gpio_num_t p : pins) gpio_hold_en(p);
  } else {
    for (gpio_num_t p : pins) gpio_hold_dis(p);
  }
}

void displayUpdate() {
  if (!fb || asleep) return;
  const uint32_t now = millis();
  if (scene == Scene::MEASURING) {
    updateHeart(now, false);
    if (plotDirty && now - plotMs >= PLOT_FRAME_MS) {
      plotMs = now;
      drawPlot();
    }
    if (alertLevel == DisplayAlert::CRITICAL) {
      const int8_t on = ((now - alarmT0) % ALARM_PERIOD_MS) < ALARM_PERIOD_MS / 2;
      if (on != alarmOn) {
        alarmOn = on;
        drawBorder(on ? 255 : ALARM_DIM);
      }
    }
  } else if (scene == Scene::NO_FINGER && now - seekMs >= SEEK_FRAME_MS) {
    seekMs = now;
    const int op = seekOpacityAt(now);
    if (op != seekOpacityDrawn) {
      seekOpacityDrawn = (int8_t)op;
      drawNoFingerIcon((uint8_t)(op * 17));
    }
  }
  flushDirty();
}
