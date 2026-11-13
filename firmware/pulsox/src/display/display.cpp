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
#include "display/display.h"
#include "display/display_assets.h"
#include "tft_st7735.h"
#include "display/trace_scale.h"

namespace {

constexpr int W = 128;
constexpr int H = 128;

// ---- Palette (RGB565 of the table in sheet D) -----------------------------------

/*******************************************************************************
 * @brief       Convierte un color de 8 bits por canal a RGB565.
 *
 * @details     Se queda con los 5 bits altos del rojo, 6 del verde y 5 del
 *              azul.
 *
 * @param       Entrada: r, g, b - componentes de 0 a 255.
 *              Salida:  retorno (uint16_t) - color RGB565.
 *
 * @peripherals Ninguno.
 *******************************************************************************/
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

constexpr int PROGRESS_X = 4, PROGRESS_Y = 124, PROGRESS_W = 120, PROGRESS_H = 2;  // under the banner
constexpr int TAG_X = 24, TAG_BASE = 11;     // "ENVIADO" / "NO ENVIADO" in the status bar of the result
constexpr int CONN_ICON_X = 40, CONN_ICON_Y = 28;  // big WiFi, 48x34
constexpr int TITLE_BASE = 92, SUBTITLE_BASE = 108;  // "Conectando" / "Colocá el dedo" and their subtitles

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
constexpr uint32_t CONN_STEP_MS = 350;  // "connecting": a new arc lights up this often
constexpr uint8_t CONN_ARC_DIM = 46;    // the arcs not lit yet: 18 % opacity

// ---- State --------------------------------------------------------------------------
enum class Scene : uint8_t { NONE, MEASURING, NO_FINGER, CONNECTING, WAIT_FINGER, RESULT };

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
TraceScale traceScale;         // vertical scale of the trace (trace_scale.h)
uint32_t plotMs = 0;

bool asleep = false;           // panel in sleep-in, backlight off (displaySleep)
bool blPending = false;        // the backlight waits for the first screen (displayBegin)

int progressPct = -1;          // measuring screen: 0-100 draws the bar under the banner, -1 hides it
bool resultSent = false;       // result screen: the measurement reached the cloud
int8_t connStep = 0;           // connecting screen: arcs lit, 0-3
uint32_t connMs = 0;

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

/*******************************************************************************
 * @brief       Marca un rectángulo como modificado, para reenviarlo al panel.
 *
 * @details     Lo recorta a la pantalla y lo ignora si ya está cubierto por
 *              otro; descarta los que él cubre y, si ya hay MAX_DIRTY (6), pasa
 *              a reenviar la pantalla completa.
 *
 * @param       Entrada: x, y, w, h - rectángulo, en píxeles.
 *              Salida:  dirty[], dirtyCount - lista de rectángulos pendientes.
 *
 * @peripherals Ninguno.
 *******************************************************************************/
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

/*******************************************************************************
 * @brief       Manda al panel los rectángulos modificados del frame buffer.
 *
 * @details     Por cada rectángulo abre una ventana de direcciones y escribe
 *              sus píxeles por SPI. Lo ensancha a una columna inicial par: el
 *              SPI lee de a 32 bits y el ESP32-C3 falla con un acceso
 *              desalineado.
 *
 * @param       Entrada: dirty[], dirtyCount, fb - rectángulos pendientes y
 *                       píxeles.
 *              Salida:  dirtyCount - vuelve a 0.
 *
 * @peripherals Internos: SPI. Externos: pantalla TFT.
 *******************************************************************************/
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

/*******************************************************************************
 * @brief       Mezcla dos colores RGB565 según un alfa.
 *
 * @details     Separa los tres canales con una máscara para mezclarlos con una
 *              sola multiplicación; el alfa va de 0 (fondo) a 255 (frente).
 *
 * @param       Entrada: bg, fg - color de fondo y de frente (RGB565).
 *              Entrada: a - alfa, de 0 a 255.
 *              Salida:  retorno (uint16_t) - color mezclado.
 *
 * @peripherals Ninguno.
 *******************************************************************************/
inline uint16_t blend(uint16_t bg, uint16_t fg, uint8_t a) {
  constexpr uint32_t MASK = 0x07E0F81F;  // spreads the 3 channels apart so one multiply mixes all
  uint32_t b = (bg | ((uint32_t)bg << 16)) & MASK;
  uint32_t f = (fg | ((uint32_t)fg << 16)) & MASK;
  uint32_t mixed = ((((f - b) * ((a + 4u) >> 3)) >> 5) + b) & MASK;  // alpha in 0-32
  return (uint16_t)(mixed | (mixed >> 16));
}

/*******************************************************************************
 * @brief       Pinta un píxel del frame buffer mezclándolo con lo que ya hay.
 *
 * @details     Fuera de la pantalla o con alfa 0 no hace nada; con alfa 255 lo
 *              reemplaza.
 *
 * @param       Entrada: x, y - posición del píxel.
 *              Entrada: color, a - color (RGB565) y alfa de 0 a 255.
 *              Salida:  fb - frame buffer: píxeles dibujados.
 *
 * @peripherals Ninguno.
 *******************************************************************************/
inline void blendPixel(int x, int y, uint16_t color, uint8_t a) {
  if ((unsigned)x >= (unsigned)W || (unsigned)y >= (unsigned)H || a == 0) return;
  uint16_t &p = fb[y * W + x];
  p = a == 255 ? color : blend(p, color, a);
}

/*******************************************************************************
 * @brief       Rellena un rectángulo del frame buffer con un color liso.
 *
 * @details     Lo recorta a la pantalla. No marca la zona como modificada: eso
 *              lo hace quien llama, con markDirty().
 *
 * @param       Entrada: x, y, w, h - rectángulo, en píxeles.
 *              Entrada: color - color de relleno (RGB565).
 *              Salida:  fb - frame buffer: píxeles dibujados.
 *
 * @peripherals Ninguno.
 *******************************************************************************/
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

/*******************************************************************************
 * @brief       Mezcla un color con alfa sobre un rectángulo del frame buffer.
 *
 * @details     Llama a blendPixel() por cada píxel del rectángulo.
 *
 * @param       Entrada: x, y, w, h - rectángulo, en píxeles.
 *              Entrada: color, a - color (RGB565) y alfa de 0 a 255.
 *              Salida:  fb - frame buffer: píxeles dibujados.
 *
 * @peripherals Ninguno.
 *******************************************************************************/
void blendRect(int x, int y, int w, int h, uint16_t color, uint8_t a) {
  for (int row = y; row < y + h; row++)
    for (int col = x; col < x + w; col++) blendPixel(col, row, color, a);
}

/*******************************************************************************
 * @brief       Calcula la cobertura de un píxel de una esquina redondeada.
 *
 * @details     Muestrea 4x4 puntos del píxel y cuenta cuántos caen dentro del
 *              círculo de radio r; sirve de alfa para suavizar el borde. (0, 0)
 *              es el píxel más externo.
 *
 * @param       Entrada: i, j - posición del píxel dentro de la esquina.
 *              Entrada: r - radio de la esquina.
 *              Salida:  retorno (uint8_t) - cobertura, de 0 a 255.
 *
 * @peripherals Ninguno.
 *******************************************************************************/
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

/*******************************************************************************
 * @brief       Dibuja un rectángulo relleno con esquinas redondeadas y
 *              suavizadas.
 *
 * @details     Rellena tres franjas rectas y mezcla cada píxel de las cuatro
 *              esquinas con su cobertura. Pensado para dibujarse sobre un fondo
 *              liso.
 *
 * @param       Entrada: x, y, w, h, r - rectángulo y radio de las esquinas.
 *              Entrada: color - color de relleno (RGB565).
 *              Salida:  fb - frame buffer: píxeles dibujados.
 *
 * @peripherals Ninguno.
 *******************************************************************************/
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

/*******************************************************************************
 * @brief       Dibuja una máscara alfa de 4 bits con un color.
 *
 * @details     La máscara guarda dos píxeles por byte, nibble alto primero;
 *              cada nibble se lleva a 0-255 (x17), se escala con la opacidad y
 *              se mezcla sobre el buffer.
 *
 * @param       Entrada: bitmap, w, h - máscara y su tamaño.
 *              Entrada: x, y - esquina superior izquierda.
 *              Entrada: color, opacity - color (RGB565) y opacidad global (255
 *                       = plena).
 *              Salida:  fb - frame buffer: píxeles dibujados.
 *
 * @peripherals Ninguno.
 *******************************************************************************/
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

/*******************************************************************************
 * @brief       Dibuja un icono (máscara con su desplazamiento) en una posición.
 *
 * @details     Llama a drawMask() con el desplazamiento xo/yo del icono; si el
 *              icono está vacío (w = 0) no hace nada.
 *
 * @param       Entrada: icon - icono (máscara, tamaño y desplazamiento).
 *              Entrada: x, y - posición de referencia.
 *              Entrada: color, opacity - color (RGB565) y opacidad global.
 *              Salida:  fb - frame buffer: píxeles dibujados.
 *
 * @peripherals Ninguno.
 *******************************************************************************/
void drawIcon(const DispIcon &icon, int x, int y, uint16_t color, uint8_t opacity = 255) {
  if (icon.w) drawMask(icon.bitmap, icon.w, icon.h, x + icon.xo, y + icon.yo, color, opacity);
}

// ---- Text ---------------------------------------------------------------------------

/*******************************************************************************
 * @brief       Devuelve el próximo código Unicode de un texto UTF-8.
 *
 * @details     Decodifica 1, 2 o 3 bytes y avanza el puntero; devuelve 0 al
 *              llegar al final. No contempla secuencias de 4 bytes.
 *
 * @param       Entrada: s - puntero al texto UTF-8.
 *              Salida:  s - avanza al carácter siguiente.
 *              Salida:  retorno (uint16_t) - código Unicode; 0 = fin del texto.
 *
 * @peripherals Ninguno.
 *******************************************************************************/
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

/*******************************************************************************
 * @brief       Busca un glifo por su código Unicode en una fuente.
 *
 * @details     Recorre la tabla de glifos de la fuente; devuelve nulo si no
 *              está.
 *
 * @param       Entrada: font - fuente.
 *              Entrada: code - código Unicode.
 *              Salida:  retorno (const DispGlyph *) - glifo, o NULL si la
 *                       fuente no lo tiene.
 *
 * @peripherals Ninguno.
 *******************************************************************************/
const DispGlyph *findGlyph(const DispFont &font, uint16_t code) {
  for (uint8_t i = 0; i < font.count; i++)
    if (font.glyphs[i].code == code) return &font.glyphs[i];
  return nullptr;
}

/*******************************************************************************
 * @brief       Dibuja un texto (o solo lo mide) y devuelve dónde queda el
 *              cursor.
 *
 * @details     Parte de x16 (1/16 px) sobre la línea base y suma el avance de
 *              cada glifo más el espaciado entre letras, como CSS. U+2082 (el 2
 *              subíndice de «SpO₂») se dibuja con la fuente `sub`, más abajo.
 *              Con draw = false solo mide.
 *
 * @param       Entrada: font, text - fuente y texto UTF-8.
 *              Entrada: x16, baseline - cursor inicial (1/16 px) y línea base.
 *              Entrada: color, opacity, track16 - color, opacidad y espaciado
 *                       (1/16 px).
 *              Entrada: sub, draw - fuente del subíndice (puede ser NULL);
 *                       false = solo medir.
 *              Salida:  retorno (int) - cursor final, en 1/16 px.
 *              Salida:  fb - texto dibujado (si draw).
 *
 * @peripherals Ninguno.
 *******************************************************************************/
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

/*******************************************************************************
 * @brief       Dibuja un texto en el frame buffer.
 *
 * @details     Llama a layoutText() con draw = true.
 *
 * @param       Entrada: font, text, x16, baseline - fuente, texto y posición (x
 *                       en 1/16 px).
 *              Entrada: color, track16, sub, opacity - color, espaciado,
 *                       subíndice y opacidad.
 *              Salida:  retorno (int) - cursor final, en 1/16 px.
 *              Salida:  fb - frame buffer: píxeles dibujados.
 *
 * @peripherals Ninguno.
 *******************************************************************************/
int drawText(const DispFont &font, int x16, int baseline, const char *text, uint16_t color, int track16 = 0,
             const DispFont *sub = nullptr, uint8_t opacity = 255) {
  return layoutText(font, x16, baseline, text, color, track16, sub, opacity, true);
}

/*******************************************************************************
 * @brief       Mide el ancho de un texto sin dibujarlo.
 *
 * @details     Llama a layoutText() con draw = false, partiendo de 0.
 *
 * @param       Entrada: font, text, track16, sub - fuente, texto, espaciado y
 *                       subíndice.
 *              Salida:  retorno (int) - ancho, en 1/16 px.
 *
 * @peripherals Ninguno.
 *******************************************************************************/
int textWidth(const DispFont &font, const char *text, int track16 = 0, const DispFont *sub = nullptr) {
  return layoutText(font, 0, 0, text, 0, track16, sub, 255, false);
}

/*******************************************************************************
 * @brief       Dibuja un texto centrado en una columna.
 *
 * @details     Resta la mitad de su ancho (con el espaciado final, como un flex
 *              item de CSS) al centro cx.
 *
 * @param       Entrada: font, text - fuente y texto.
 *              Entrada: cx, baseline - columna central (px) y línea base.
 *              Entrada: color, track16, sub - color, espaciado y subíndice.
 *              Salida:  fb - frame buffer: píxeles dibujados.
 *
 * @peripherals Ninguno.
 *******************************************************************************/
void drawTextCentered(const DispFont &font, int cx, int baseline, const char *text, uint16_t color,
                      int track16 = 0, const DispFont *sub = nullptr) {
  drawText(font, cx * 16 - textWidth(font, text, track16, sub) / 2, baseline, text, color, track16, sub);
}

// ---- Values ---------------------------------------------------------------------------

/*******************************************************************************
 * @brief       Devuelve el color del nivel de alerta actual.
 *
 * @details     Ámbar con CAUTION, rojo con CRITICAL y verde en cualquier otro
 *              caso (normal).
 *
 * @param       Entrada: alertLevel - nivel de alerta.
 *              Salida:  retorno (uint16_t) - color RGB565.
 *
 * @peripherals Ninguno.
 *******************************************************************************/
uint16_t levelColor() {
  switch (alertLevel) {
    case DisplayAlert::CAUTION: return C_CAUTION;
    case DisplayAlert::CRITICAL: return C_CRITICAL;
    default: return C_NORMAL;
  }
}

// ---- Screen parts ---------------------------------------------------------------------
// None of these touch the alert border (the outer 2 px), so the border can be left alone.

/*******************************************************************************
 * @brief       Dibuja la barra de estado: WiFi, batería y, en el resultado,
 *              ENVIADO / NO ENVIADO.
 *
 * @details     Limpia la barra y traza su línea inferior (sin pisar el borde de
 *              alerta); dibuja el icono WiFi (azul con conexión, gris sin
 *              ella), la batería con su nivel y su porcentaje. En RESULT agrega
 *              la etiqueta y omite el porcentaje si no entran.
 *
 * @param       Entrada: scene, alertLevel, wifiOn, battPct, resultSent - qué
 *                       mostrar.
 *              Salida:  fb - frame buffer: píxeles dibujados.
 *
 * @peripherals Internos: Ninguno.
 *              Externos: pantalla TFT (se escribe luego en flushDirty()).
 *******************************************************************************/
void drawStatusBar() {
  const bool borderOn = (scene == Scene::MEASURING || scene == Scene::RESULT) && alertLevel != DisplayAlert::NORMAL;
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
  const int battLeft16 = BATT_TEXT_RIGHT * 16 - textWidth(FONT_S9, text);

  // The result screen says in the bar whether the measurement reached the cloud. If the tag and the
  // battery percentage do not both fit, the percentage goes: the battery icon still shows the level.
  bool showBattText = true;
  if (scene == Scene::RESULT) {
    const char *tag = resultSent ? "ENVIADO" : "NO ENVIADO";
    const int tagEnd16 = TAG_X * 16 + textWidth(FONT_S9, tag);
    drawText(FONT_S9, TAG_X * 16, TAG_BASE, tag, resultSent ? C_NORMAL : C_CAUTION);
    showBattText = tagEnd16 + 3 * 16 <= battLeft16;
  }
  if (showBattText) drawText(FONT_S9, battLeft16, BATT_TEXT_BASE, text, C_BATTERY);
  markDirty(0, 0, W, BAR_LINE_Y + 1);
}

/*******************************************************************************
 * @brief       Dibuja el valor de SpO2 (o "--") con su etiqueta.
 *
 * @details     Etiqueta «SpO₂», valor grande en el color del nivel (gris si no
 *              hay valor) y el signo «%».
 *
 * @param       Entrada: spo2Value, alertLevel - valor y nivel de alerta.
 *              Salida:  fb - frame buffer: píxeles dibujados.
 *
 * @peripherals Internos: Ninguno.
 *              Externos: pantalla TFT (se escribe luego en flushDirty()).
 *******************************************************************************/
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

/*******************************************************************************
 * @brief       Dibuja el valor de BPM (o "--") con su etiqueta.
 *
 * @details     Valor centrado en la columna derecha, blanco (gris si no hay
 *              valor), y la etiqueta «BPM».
 *
 * @param       Entrada: bpmValue - frecuencia cardíaca; <= 0 = sin valor.
 *              Salida:  fb - frame buffer: píxeles dibujados.
 *
 * @peripherals Internos: Ninguno.
 *              Externos: pantalla TFT (se escribe luego en flushDirty()).
 *******************************************************************************/
void drawBpm() {
  fillRect(80, 34, 42, 34, C_BG);
  char text[8] = "--";
  if (bpmValue > 0) snprintf(text, sizeof(text), "%d", bpmValue);
  drawTextCentered(FONT_B22, BPM_CX, BPM_VALUE_BASE, text, bpmValue > 0 ? C_TEXT : C_TEXT2);
  drawTextCentered(FONT_S8, BPM_CX, BPM_LABEL_BASE, "BPM", C_TEXT2, TRACK_BPM);
  markDirty(80, 34, 42, 34);
}

/*******************************************************************************
 * @brief       Dibuja el corazón con una escala y una opacidad dadas.
 *
 * @details     Borra la zona y dibuja el icono precalculado de esa escala con
 *              opacidad opacityQ x 17. Recuerda lo dibujado para no repetirlo.
 *
 * @param       Entrada: scaleIdx - índice de escala (ICON_HEART[]).
 *              Entrada: opacityQ - opacidad, de 0 a 15.
 *              Salida:  heartScaleDrawn, heartOpacityDrawn - lo que quedó en
 *                       pantalla.
 *              Salida:  fb - frame buffer: píxeles dibujados.
 *
 * @peripherals Internos: Ninguno.
 *              Externos: pantalla TFT (se escribe luego en flushDirty()).
 *******************************************************************************/
void drawHeart(int scaleIdx, int opacityQ) {
  fillRect(HEART_X, HEART_Y, HEART_ZONE_W, HEART_ZONE_H, C_BG);
  drawIcon(ICON_HEART[scaleIdx], HEART_X, HEART_Y, C_HEART, (uint8_t)(opacityQ * 17));
  markDirty(HEART_X, HEART_Y, HEART_ZONE_W, HEART_ZONE_H);
  heartScaleDrawn = (int8_t)scaleIdx;
  heartOpacityDrawn = (int8_t)opacityQ;
}

/*******************************************************************************
 * @brief       Dibuja el banner inferior: MIDIENDO / COMPLETADO o la alerta de
 *              SpO2.
 *
 * @details     Con nivel normal, un punto verde y «MIDIENDO» (o «COMPLETADO» en
 *              el resultado). Con precaución o crítico, una barra del color del
 *              nivel con su icono y «SpO₂ BAJA» o «¡SpO₂ CRÍTICA!».
 *
 * @param       Entrada: scene, alertLevel - pantalla y nivel de alerta.
 *              Salida:  fb - frame buffer: píxeles dibujados.
 *
 * @peripherals Internos: Ninguno.
 *              Externos: pantalla TFT (se escribe luego en flushDirty()).
 *******************************************************************************/
void drawBanner() {
  fillRect(BANNER_X, BANNER_Y, BANNER_W, BANNER_H, C_BG);
  const int cx = BANNER_X + BANNER_W / 2;
  if (alertLevel == DisplayAlert::NORMAL) {
    const char *text = scene == Scene::RESULT ? "COMPLETADO" : "MIDIENDO";
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

/*******************************************************************************
 * @brief       Dibuja la barra de progreso de la medición, bajo el banner.
 *
 * @details     Fondo, riel y relleno proporcional a progressPct, en el color
 *              del nivel de alerta; con progressPct < 0 queda oculta.
 *
 * @param       Entrada: progressPct - avance, de 0 a 100; -1 = oculta.
 *              Salida:  fb - frame buffer: píxeles dibujados.
 *
 * @peripherals Internos: Ninguno.
 *              Externos: pantalla TFT (se escribe luego en flushDirty()).
 *******************************************************************************/
void drawProgress() {
  fillRect(PROGRESS_X, PROGRESS_Y, PROGRESS_W, PROGRESS_H, C_BG);
  if (progressPct >= 0) {
    fillRect(PROGRESS_X, PROGRESS_Y, PROGRESS_W, PROGRESS_H, C_LINE);
    int w = PROGRESS_W * progressPct / 100;
    if (progressPct > 0 && w < 1) w = 1;
    fillRect(PROGRESS_X, PROGRESS_Y, w, PROGRESS_H, levelColor());
  }
  markDirty(PROGRESS_X, PROGRESS_Y, PROGRESS_W, PROGRESS_H);
}

/*******************************************************************************
 * @brief       Dibuja el borde de alerta de 2 px alrededor de la pantalla.
 *
 * @details     Lo borra y mezcla el color del nivel con el alfa recibido (0 lo
 *              deja borrado). Repone la línea inferior de la barra de estado,
 *              lo único no negro que hay debajo.
 *
 * @param       Entrada: alpha - opacidad del borde, de 0 (borrado) a 255
 *                       (pleno).
 *              Salida:  fb - frame buffer: píxeles dibujados.
 *
 * @peripherals Internos: Ninguno.
 *              Externos: pantalla TFT (se escribe luego en flushDirty()).
 *******************************************************************************/
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

/*******************************************************************************
 * @brief       Calcula la raíz cuadrada entera de un número no negativo.
 *
 * @details     Algoritmo bit a bit, solo con enteros: el ESP32-C3 no tiene FPU.
 *
 * @param       Entrada: v - número, v >= 0.
 *              Salida:  retorno (int) - parte entera de la raíz cuadrada.
 *
 * @peripherals Ninguno.
 *******************************************************************************/
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

/*******************************************************************************
 * @brief       Agrega un segmento suavizado de 2 px de ancho al buffer de
 *              cobertura de la traza.
 *
 * @details     Une los centros de las columnas i e i+1 con una banda de 1 px de
 *              semiancho (distancia a la recta, rampa de 0,5 px), cortada 1 px
 *              más allá de los extremos, lo que también cubre las uniones.
 *              Aritmética entera: sin FPU.
 *
 * @param       Entrada: i - columna de inicio del segmento.
 *              Entrada: y0, y1 - alturas de los extremos, en 1/16 px desde el
 *                       borde superior.
 *              Salida:  cov[] - cobertura de cada píxel (0-16); conserva el
 *                       máximo.
 *
 * @peripherals Ninguno.
 *******************************************************************************/
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

/*******************************************************************************
 * @brief       Dibuja el cuadro de la traza del pulso y su curva suavizada.
 *
 * @details     Caja con borde, fondo y grilla. Escala el historial con
 *              trace_scale (un pico se recorta contra el borde y no aplana la
 *              onda), traza los segmentos en cov[] y los mezcla con el color
 *              del nivel. Antes de llenar el historial, crece desde la derecha.
 *
 * @param       Entrada: hist[], histHead, histCount - historial de columnas de
 *                       la traza.
 *              Entrada: traceScale, alertLevel - escala vertical y nivel de
 *                       alerta.
 *              Salida:  cov[], plotDirty - cobertura de la traza; plotDirty =
 *                       false.
 *              Salida:  fb - frame buffer: píxeles dibujados.
 *
 * @peripherals Internos: Ninguno.
 *              Externos: pantalla TFT (se escribe luego en flushDirty()).
 *******************************************************************************/
void drawPlot() {
  // Box: 1 px border, background and grid (a line every 19 px down and 20 px across,
  // starting at the top-left of the inside).
  fillRoundRect(PLOT_X, PLOT_Y, PLOT_W, PLOT_H, 2, C_LINE);
  fillRoundRect(PLOT_X + 1, PLOT_Y + 1, PLOT_COLS, PLOT_ROWS, 1, C_PLOT_BG);
  for (int gy = 0; gy < PLOT_ROWS; gy += 19) fillRect(PLOT_X + 1, PLOT_Y + 1 + gy, PLOT_COLS, 1, C_LINE);
  for (int gx = 0; gx < PLOT_COLS; gx += 20) fillRect(PLOT_X + 1 + gx, PLOT_Y + 1, 1, PLOT_ROWS, C_LINE);

  // Trace: scale the history to the band between the baseline and the peak. The scale is the
  // robust one of trace_scale.c, so a spike or a finger movement does not flatten the wave:
  // the spike is drawn clipped against the edge of the plot. Until the history fills up, the
  // trace grows from the right edge.
  const int first = PLOT_COLS - histCount;  // first column with data
  int32_t yq[PLOT_COLS];
  if (histCount > 0) {
    float lo = hist[(histHead + first) % PLOT_COLS], hi = lo;
    for (int i = first + 1; i < PLOT_COLS; i++) {
      const float v = hist[(histHead + i) % PLOT_COLS];
      if (v < lo) lo = v;
      if (v > hi) hi = v;
    }
    float scaleLo, scaleHi;
    trace_scale_update(&traceScale, lo, hi, &scaleLo, &scaleHi);
    float span = scaleHi - scaleLo;
    float base = scaleLo;
    if (span < DISPLAY_PPG_MIN_SPAN) {
      base -= (DISPLAY_PPG_MIN_SPAN - span) * 0.5f;
      span = DISPLAY_PPG_MIN_SPAN;
    }
    const float k = (PLOT_BASE_Y - PLOT_PEAK_Y) / span * 16.0f;
    // Centers of the first and last row, in 1/16 px: where a clipped value sits.
    constexpr float CLIP_TOP = 8.0f, CLIP_BOTTOM = PLOT_ROWS * 16.0f - 8.0f;
    for (int i = first; i < PLOT_COLS; i++) {
      float v = hist[(histHead + i) % PLOT_COLS];
      float y = PLOT_BASE_Y * 16.0f - (v - base) * k;
      if (y < CLIP_TOP) y = CLIP_TOP;
      if (y > CLIP_BOTTOM) y = CLIP_BOTTOM;
      yq[i] = (int32_t)(y + 0.5f);
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

/*******************************************************************************
 * @brief       Agrega una columna al historial de la traza.
 *
 * @details     Escribe v en el anillo hist[], avanza histHead e histCount,
 *              alimenta la escala (trace_scale_push) y marca plotDirty para
 *              redibujar.
 *
 * @param       Entrada: v - valor de la columna (IR filtrado, promediado).
 *              Salida:  hist[], histHead, histCount, plotDirty - historial
 *                       actualizado.
 *
 * @peripherals Ninguno.
 *******************************************************************************/
void pushColumn(float v) {
  hist[histHead] = v;
  histHead = (histHead + 1) % PLOT_COLS;
  if (histCount < PLOT_COLS) histCount++;
  trace_scale_push(&traceScale, v);
  plotDirty = true;
}

/*******************************************************************************
 * @brief       Vacía la traza para empezar una nueva.
 *
 * @details     Pone a cero el historial, la escala y el acumulador de muestras
 *              por columna.
 *
 * @param       Salida:  histCount, histHead, traceScale - historial y escala
 *                       vacíos.
 *              Salida:  ppgPhase, ppgSum, ppgCount - acumulador en cero.
 *
 * @peripherals Ninguno.
 *******************************************************************************/
void resetPlot() {
  histCount = 0;
  histHead = 0;
  trace_scale_reset(&traceScale);
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

/*******************************************************************************
 * @brief       Calcula la escala y la opacidad del corazón en un punto del
 *              latido.
 *
 * @details     Interpola, con salida suave (ease-out), entre los keyframes
 *              `beat` de BEAT[].
 *
 * @param       Entrada: phase - posición en el latido, de 0 a 1.
 *              Salida:  scale, opacity - escala y opacidad del corazón.
 *
 * @peripherals Ninguno.
 *******************************************************************************/
void beatAt(float phase, float &scale, float &opacity) {
  int i = 0;
  while (i < 4 && phase > BEAT[i + 1].t) i++;
  float u = (phase - BEAT[i].t) / (BEAT[i + 1].t - BEAT[i].t);
  u = u * (2.0f - u);  // ease-out
  scale = BEAT[i].scale + (BEAT[i + 1].scale - BEAT[i].scale) * u;
  opacity = BEAT[i].opacity + (BEAT[i + 1].opacity - BEAT[i].opacity) * u;
}

/*******************************************************************************
 * @brief       Anima el corazón y lo redibuja si cambió.
 *
 * @details     Avanza la fase según el BPM (tras una pausa limita el salto a
 *              200 ms). En RESULT queda quieto y brillante; sin BPM, quieto y
 *              tenue. Redibuja solo si cambió la escala o la opacidad
 *              cuantizadas.
 *
 * @param       Entrada: now - millis() actual.
 *              Entrada: force - true ignora el ritmo de cuadros
 *                       (HEART_FRAME_MS).
 *              Entrada: scene, bpmValue - pantalla y frecuencia cardíaca.
 *              Salida:  heartPhase, heartMs - fase del latido y último cuadro.
 *              Salida:  fb - frame buffer: píxeles dibujados.
 *
 * @peripherals Internos: Ninguno.
 *              Externos: pantalla TFT (se escribe luego en flushDirty()).
 *******************************************************************************/
void updateHeart(uint32_t now, bool force) {
  if (!force && now - heartMs < HEART_FRAME_MS) return;
  uint32_t dt = force ? 0 : now - heartMs;
  heartMs = now;
  float scale, opacity;
  if (scene == Scene::RESULT) {
    scale = 1.0f;  // the result is final: a still, bright heart
    opacity = 1.0f;
  } else if (bpmValue > 0) {
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

/*******************************************************************************
 * @brief       Dibuja completa la pantalla de medición (también la de
 *              resultado).
 *
 * @details     Limpia y dibuja barra de estado, SpO2, BPM, corazón, traza y
 *              banner (y la barra de progreso al medir). Reinicia el parpadeo
 *              del borde y marca toda la pantalla como modificada.
 *
 * @param       Entrada: scene, alertLevel - pantalla y nivel de alerta.
 *              Salida:  alarmOn, alarmT0 - parpadeo del borde reiniciado.
 *              Salida:  fb - frame buffer: píxeles dibujados.
 *
 * @peripherals Internos: Ninguno.
 *              Externos: pantalla TFT (se escribe luego en flushDirty()).
 *******************************************************************************/
void renderMeasuring() {
  fillRect(0, 0, W, H, C_BG);
  drawStatusBar();
  drawSpo2();
  drawBpm();
  updateHeart(millis(), true);
  drawPlot();
  drawBanner();
  if (scene == Scene::MEASURING) drawProgress();
  alarmOn = -1;
  alarmT0 = millis();
  if (alertLevel == DisplayAlert::CAUTION) drawBorder(255);
  markDirty(0, 0, W, H);
}

/*******************************************************************************
 * @brief       Dibuja el icono del sensor con su arco; con la insignia de error
 *              si se pide.
 *
 * @details     Borra la zona y dibuja el arco (con su opacidad) y el cuerpo del
 *              sensor; con badge agrega la insignia roja con la «X».
 *
 * @param       Entrada: arcOpacity - opacidad del arco, de 0 a 255.
 *              Entrada: badge - true agrega la insignia de error.
 *              Salida:  fb - frame buffer: píxeles dibujados.
 *
 * @peripherals Internos: Ninguno.
 *              Externos: pantalla TFT (se escribe luego en flushDirty()).
 *******************************************************************************/
void drawNoFingerIcon(uint8_t arcOpacity, bool badge) {
  fillRect(NF_ICON_X, NF_ICON_Y, 42, 40, C_BG);
  drawIcon(ICON_NOFINGER_ARC, NF_ICON_X, NF_ICON_Y, C_TEXT2, arcOpacity);
  drawIcon(ICON_NOFINGER_BODY, NF_ICON_X, NF_ICON_Y, C_TEXT2);
  if (badge) {
    drawIcon(ICON_NOFINGER_BADGE, NF_ICON_X, NF_ICON_Y, C_CRITICAL);
    drawIcon(ICON_NOFINGER_X, NF_ICON_X, NF_ICON_Y, C_BG);
  }
  markDirty(NF_ICON_X, NF_ICON_Y, 42, 40);
}

/*******************************************************************************
 * @brief       Calcula la opacidad del arco animado de «buscando dedo».
 *
 * @details     Keyframes `seek`: el arco baja de 1 a 0,25 y vuelve a 1 cada
 *              SEEK_PERIOD_MS, con ease-in-out en cada tramo.
 *
 * @param       Entrada: now - millis() actual.
 *              Salida:  retorno (int) - opacidad, de 0 a 15.
 *
 * @peripherals Ninguno.
 *******************************************************************************/
int seekOpacityAt(uint32_t now) {
  float p = (now % SEEK_PERIOD_MS) / (float)SEEK_PERIOD_MS;
  float u = p < 0.5f ? p * 2.0f : (1.0f - p) * 2.0f;  // 0 -> 1 -> 0
  u = u * u * (3.0f - 2.0f * u);
  return (int)((1.0f - 0.75f * u) * 15.0f + 0.5f);
}

/*******************************************************************************
 * @brief       Dibuja la pantalla de error «Dedo no encontrado».
 *
 * @details     Barra de estado, icono del sensor con insignia de error y arco
 *              animado, y los textos «ERROR» y «Dedo no encontrado».
 *
 * @param       Salida:  seekOpacityDrawn - opacidad del arco dibujada.
 *              Salida:  fb - frame buffer: píxeles dibujados.
 *
 * @peripherals Internos: Ninguno.
 *              Externos: pantalla TFT (se escribe luego en flushDirty()).
 *******************************************************************************/
void renderNoFinger() {
  fillRect(0, 0, W, H, C_BG);
  drawStatusBar();
  seekOpacityDrawn = (int8_t)seekOpacityAt(millis());
  drawNoFingerIcon((uint8_t)(seekOpacityDrawn * 17), true);
  drawTextCentered(FONT_B20, W / 2, NF_ERROR_BASE, "ERROR", C_CRITICAL, TRACK_ERROR);
  drawTextCentered(FONT_S13, W / 2, NF_TEXT_BASE, "Dedo no encontrado", C_TEXT);
  markDirty(0, 0, W, H);
}

/*******************************************************************************
 * @brief       Dibuja la pantalla «Colocá el dedo»: sensor con arco animado,
 *              sin insignia de error.
 *
 * @details     Barra de estado, icono del sensor sin la insignia y los textos
 *              «Colocá el dedo» y «en el sensor». Se usa con la conexión ya
 *              resuelta, a la espera del dedo.
 *
 * @param       Salida:  seekOpacityDrawn - opacidad del arco dibujada.
 *              Salida:  fb - frame buffer: píxeles dibujados.
 *
 * @peripherals Internos: Ninguno.
 *              Externos: pantalla TFT (se escribe luego en flushDirty()).
 *******************************************************************************/
void renderWaitFinger() {
  fillRect(0, 0, W, H, C_BG);
  drawStatusBar();
  seekOpacityDrawn = (int8_t)seekOpacityAt(millis());
  drawNoFingerIcon((uint8_t)(seekOpacityDrawn * 17), false);
  drawTextCentered(FONT_S13, W / 2, TITLE_BASE, "Colocá el dedo", C_TEXT);
  drawTextCentered(FONT_S9, W / 2, SUBTITLE_BASE, "en el sensor", C_TEXT2);
  markDirty(0, 0, W, H);
}

/*******************************************************************************
 * @brief       Dibuja el icono grande de WiFi con `step` arcos encendidos.
 *
 * @details     El punto siempre en pleno; los arcos hasta `step` en pleno y el
 *              resto tenues (CONN_ARC_DIM).
 *
 * @param       Entrada: step - arcos encendidos, de 0 (solo el punto) a 3.
 *              Salida:  fb - frame buffer: píxeles dibujados.
 *
 * @peripherals Internos: Ninguno.
 *              Externos: pantalla TFT (se escribe luego en flushDirty()).
 *******************************************************************************/
void drawConnIcon(int step) {
  fillRect(CONN_ICON_X, CONN_ICON_Y, 48, 34, C_BG);
  drawIcon(ICON_WIFIBIG_DOT, CONN_ICON_X, CONN_ICON_Y, C_WIFI);
  drawIcon(ICON_WIFIBIG_ARC1, CONN_ICON_X, CONN_ICON_Y, C_WIFI, step >= 1 ? 255 : CONN_ARC_DIM);
  drawIcon(ICON_WIFIBIG_ARC2, CONN_ICON_X, CONN_ICON_Y, C_WIFI, step >= 2 ? 255 : CONN_ARC_DIM);
  drawIcon(ICON_WIFIBIG_ARC3, CONN_ICON_X, CONN_ICON_Y, C_WIFI, step >= 3 ? 255 : CONN_ARC_DIM);
  markDirty(CONN_ICON_X, CONN_ICON_Y, 48, 34);
}

/*******************************************************************************
 * @brief       Dibuja la pantalla «Conectando»: icono de WiFi animado y
 *              subtítulo.
 *
 * @details     Barra de estado, icono con 0 arcos encendidos y los textos
 *              «Conectando» y «WiFi y nube». Reinicia la animación.
 *
 * @param       Salida:  connStep, connMs - animación reiniciada.
 *              Salida:  fb - frame buffer: píxeles dibujados.
 *
 * @peripherals Internos: Ninguno.
 *              Externos: pantalla TFT (se escribe luego en flushDirty()).
 *******************************************************************************/
void renderConnecting() {
  fillRect(0, 0, W, H, C_BG);
  drawStatusBar();
  connStep = 0;
  connMs = millis();
  drawConnIcon(connStep);
  drawTextCentered(FONT_S13, W / 2, TITLE_BASE, "Conectando", C_TEXT);
  drawTextCentered(FONT_S9, W / 2, SUBTITLE_BASE, "WiFi y nube", C_TEXT2);
  markDirty(0, 0, W, H);
}

}  // namespace

// ---- Public API ---------------------------------------------------------------------------

/*******************************************************************************
 * @brief       Calcula el nivel de alerta de una lectura de SpO2.
 *
 * @details     NORMAL con SpO2 >= DISPLAY_SPO2_NORMAL_MIN (96) o sin valor (<=
 *              0); CAUTION desde DISPLAY_SPO2_CAUTION_MIN (93); CRITICAL por
 *              debajo.
 *
 * @param       Entrada: spo2 - SpO2, en %.
 *              Salida:  retorno (DisplayAlert) - NORMAL, CAUTION o CRITICAL.
 *
 * @peripherals Ninguno.
 *******************************************************************************/
DisplayAlert displayAlertFor(int spo2) {
  if (spo2 <= 0 || spo2 >= DISPLAY_SPO2_NORMAL_MIN) return DisplayAlert::NORMAL;
  return spo2 >= DISPLAY_SPO2_CAUTION_MIN ? DisplayAlert::CAUTION : DisplayAlert::CRITICAL;
}

/*******************************************************************************
 * @brief       Inicializa la pantalla y reserva sus buffers.
 *
 * @details     Reserva el frame buffer (128x128 RGB565, 32 KB) y el buffer de
 *              cobertura de la traza; si falta RAM los libera y devuelve false
 *              (el resto de las llamadas quedan sin efecto). Llama a tftBegin()
 *              y deja el backlight apagado hasta la primera pantalla.
 *
 * @param       Salida:  retorno (bool) - false si no hubo RAM.
 *              Salida:  fb, cov, blPending - buffers y backlight pendiente.
 *
 * @peripherals Internos: SPI, GPIO (vía tftBegin()). Externos: pantalla TFT.
 *******************************************************************************/
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
  tftBegin();  // black screen, backlight off (TFT_BL_ON_AT_BEGIN 0)
  blPending = true;  // displayUpdate() lights it with the first screen that gets drawn
  return true;
}

/*******************************************************************************
 * @brief       Fija el estado de la barra superior: WiFi y batería.
 *
 * @details     Limita la batería a 100. En la pantalla de resultado el icono
 *              WiFi lo manda el resultado (enviado o no) y no se cambia.
 *              Redibuja la barra solo si algo cambió.
 *
 * @param       Entrada: wifiConnected - true si hay conexión.
 *              Entrada: batteryPct - batería, de 0 a 100 %.
 *              Salida:  wifiOn, battPct - estado mostrado.
 *
 * @peripherals Internos: Ninguno.
 *              Externos: pantalla TFT (se escribe luego en displayUpdate()).
 *******************************************************************************/
void displaySetStatus(bool wifiConnected, uint8_t batteryPct) {
  if (!fb) return;
  if (batteryPct > 100) batteryPct = 100;
  if (scene == Scene::RESULT) wifiConnected = wifiOn;  // the result screen owns the icon: sent or not
  if (wifiConnected == wifiOn && batteryPct == battPct) return;
  wifiOn = wifiConnected;
  battPct = batteryPct;
  if (scene != Scene::NONE) drawStatusBar();
}

/*******************************************************************************
 * @brief       Muestra los valores de SpO2 y BPM en la pantalla de medición.
 *
 * @details     Un valor <= 0 se ve como "--". Si la pantalla es nueva o cambia
 *              el nivel de alerta redibuja todo; si no, solo el valor que
 *              cambió.
 *
 * @param       Entrada: spo2, bpm - SpO2 (%) y frecuencia cardíaca; <= 0 = sin
 *                       valor.
 *              Salida:  scene, alertLevel, spo2Value, bpmValue - estado
 *                       mostrado.
 *              Salida:  progressPct, heartPhase - reiniciados si la pantalla es
 *                       nueva.
 *
 * @peripherals Internos: Ninguno.
 *              Externos: pantalla TFT (se escribe luego en displayUpdate()).
 *******************************************************************************/
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
      progressPct = -1;
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

/*******************************************************************************
 * @brief       Pasa a la pantalla de error «Dedo no encontrado».
 *
 * @details     Pone la escena NO_FINGER, borra los valores y el nivel de alerta
 *              y la dibuja. No hace nada si ya está en esa pantalla.
 *
 * @param       Salida:  scene, alertLevel, spo2Value, bpmValue - estado
 *                       mostrado.
 *
 * @peripherals Internos: Ninguno.
 *              Externos: pantalla TFT (se escribe luego en displayUpdate()).
 *******************************************************************************/
void displaySetNoFinger() {
  if (!fb || scene == Scene::NO_FINGER) return;
  scene = Scene::NO_FINGER;
  alertLevel = DisplayAlert::NORMAL;
  spo2Value = 0;
  bpmValue = 0;
  renderNoFinger();
}

/*******************************************************************************
 * @brief       Pasa a la pantalla «Conectando».
 *
 * @details     Pone la escena CONNECTING con alerta NORMAL y la dibuja. No hace
 *              nada si ya está en esa pantalla.
 *
 * @param       Salida:  scene, alertLevel - estado mostrado.
 *
 * @peripherals Internos: Ninguno.
 *              Externos: pantalla TFT (se escribe luego en displayUpdate()).
 *******************************************************************************/
void displaySetConnecting() {
  if (!fb || scene == Scene::CONNECTING) return;
  scene = Scene::CONNECTING;
  alertLevel = DisplayAlert::NORMAL;
  renderConnecting();
}

/*******************************************************************************
 * @brief       Pasa a la pantalla «Colocá el dedo», con la conexión ya
 *              resuelta.
 *
 * @details     Pone la escena WAIT_FINGER con alerta NORMAL y la dibuja. No
 *              hace nada si ya está en esa pantalla.
 *
 * @param       Salida:  scene, alertLevel - estado mostrado.
 *
 * @peripherals Internos: Ninguno.
 *              Externos: pantalla TFT (se escribe luego en displayUpdate()).
 *******************************************************************************/
void displaySetWaitFinger() {
  if (!fb || scene == Scene::WAIT_FINGER) return;
  scene = Scene::WAIT_FINGER;
  alertLevel = DisplayAlert::NORMAL;
  renderWaitFinger();
}

/*******************************************************************************
 * @brief       Fija el avance de la barra de progreso de la medición.
 *
 * @details     De 0 a 100; un valor negativo la oculta. Solo se redibuja en la
 *              pantalla de medición y si el valor cambió.
 *
 * @param       Entrada: pct - avance en %, o negativo para ocultar.
 *              Salida:  progressPct - avance mostrado.
 *
 * @peripherals Internos: Ninguno.
 *              Externos: pantalla TFT (se escribe luego en displayUpdate()).
 *******************************************************************************/
void displaySetProgress(int pct) {
  if (!fb) return;
  if (pct > 100) pct = 100;
  if (pct < 0) pct = -1;
  if (pct == progressPct) return;
  progressPct = pct;
  if (scene == Scene::MEASURING) drawProgress();
}

/*******************************************************************************
 * @brief       Muestra la pantalla de resultado final de la medición.
 *
 * @details     Es la pantalla de medición congelada: valores, última traza
 *              (vacía si no se venía midiendo), corazón quieto, «COMPLETADO» o
 *              el banner del nivel, y en la barra ENVIADO / NO ENVIADO (WiFi
 *              azul si llegó a la nube).
 *
 * @param       Entrada: spo2, bpm - resultado; <= 0 se ve como "--".
 *              Entrada: sent - true si la medición llegó a la nube.
 *              Salida:  scene, resultSent, wifiOn, spo2Value, bpmValue,
 *                       alertLevel - estado mostrado.
 *
 * @peripherals Internos: Ninguno.
 *              Externos: pantalla TFT (se escribe luego en displayUpdate()).
 *******************************************************************************/
void displaySetResult(int spo2, int bpm, bool sent) {
  if (!fb) return;
  if (spo2 > 100) spo2 = 100;
  if (bpm > 999) bpm = 999;
  if (scene != Scene::MEASURING) resetPlot();  // no live trace to freeze: an empty plot, not an old one
  scene = Scene::RESULT;
  resultSent = sent;
  wifiOn = sent;  // blue when it reached the cloud, grey when not
  spo2Value = spo2;
  bpmValue = bpm;
  alertLevel = displayAlertFor(spo2);
  renderMeasuring();
}

/*******************************************************************************
 * @brief       Agrega una muestra de la señal PPG a la traza de la pantalla.
 *
 * @details     Promedia las muestras que caen en cada columna
 *              (DISPLAY_PPG_PX_PER_S columnas por segundo sobre
 *              DISPLAY_PPG_INPUT_HZ muestras) y, al completar una, la empuja al
 *              historial. Es barata: no dibuja.
 *
 * @param       Entrada: sample - IR filtrado, con el latido hacia arriba.
 *              Salida:  ppgSum, ppgCount, ppgPhase - acumulador de la columna
 *                       en curso.
 *              Salida:  hist[], plotDirty - historial, cuando se completa una
 *                       columna.
 *
 * @peripherals Ninguno.
 *******************************************************************************/
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

/*******************************************************************************
 * @brief       Apaga la pantalla para dormir.
 *
 * @details     Apaga el backlight y pone el controlador en DISPOFF + SLPIN; el
 *              frame buffer se conserva. No hace nada si no hay pantalla o ya
 *              duerme.
 *
 * @param       Salida:  asleep - true.
 *
 * @peripherals Internos: GPIO (backlight), SPI. Externos: pantalla TFT.
 *******************************************************************************/
void displaySleep() {
  if (!fb || asleep) return;
  asleep = true;
  tftBacklight(false);
  TftPanel &tft = tftScreen();
  tft.enableDisplay(false);  // DISPOFF
  tft.enableSleep(true);     // SLPIN
}

/*******************************************************************************
 * @brief       Despierta la pantalla: la redibuja entera y recién entonces
 *              prende el backlight.
 *
 * @details     SLPOUT (el controlador necesita 120 ms), DISPON, reenvía todo el
 *              frame buffer y después enciende el backlight, para no mostrar
 *              basura.
 *
 * @param       Salida:  asleep, blPending - false.
 *
 * @peripherals Internos: SPI, GPIO (backlight). Externos: pantalla TFT.
 *******************************************************************************/
void displayWake() {
  if (!fb || !asleep) return;
  TftPanel &tft = tftScreen();
  tft.enableSleep(false);  // SLPOUT: the controller needs 120 ms before the next command
  delay(120);
  tft.enableDisplay(true);
  asleep = false;
  markDirty(0, 0, W, H);
  flushDirty();
  blPending = false;
  tftBacklight(true);
}

/*******************************************************************************
 * @brief       Retiene o libera los pines de la pantalla durante el sueño.
 *
 * @details     Con hold = true deja el backlight apagado y CS y RST en alto, y
 *              los congela con gpio_hold_en: la pantalla sigue dormida y
 *              oscura. Con false los libera; hacerlo apenas se despierta, antes
 *              de displayBegin().
 *
 * @param       Entrada: hold - true = retener, false = liberar.
 *
 * @peripherals Internos: GPIO (modo y retención de backlight, CS y RST),
 *              light/deep sleep.
 *              Externos: pantalla TFT (backlight, CS y RST).
 *******************************************************************************/
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

/*******************************************************************************
 * @brief       Corre las animaciones y manda al panel lo que cambió.
 *
 * @details     Llamar en cada vuelta del loop(). Anima el corazón y la traza al
 *              medir, el borde parpadeante de CRITICAL, el arco de «buscando
 *              dedo» y los arcos de «Conectando»; luego flushDirty() y, tras la
 *              primera pantalla, enciende el backlight.
 *
 * @param       Entrada: scene, alertLevel, asleep, blPending - qué animar y si
 *                       hay que prender la luz.
 *              Salida:  estado de las animaciones - heartMs, plotMs, seekMs,
 *                       connMs, alarmOn...
 *
 * @peripherals Internos: SPI, GPIO (backlight). Externos: pantalla TFT.
 *******************************************************************************/
void displayUpdate() {
  if (!fb || asleep) return;
  const uint32_t now = millis();
  if (scene == Scene::MEASURING || scene == Scene::RESULT) {
    if (scene == Scene::MEASURING) {  // the result screen is a still picture: heart and trace stay as drawn
      updateHeart(now, false);
      if (plotDirty && now - plotMs >= PLOT_FRAME_MS) {
        plotMs = now;
        drawPlot();
      }
    }
    if (alertLevel == DisplayAlert::CRITICAL) {
      const int8_t on = ((now - alarmT0) % ALARM_PERIOD_MS) < ALARM_PERIOD_MS / 2;
      if (on != alarmOn) {
        alarmOn = on;
        drawBorder(on ? 255 : ALARM_DIM);
      }
    }
  } else if ((scene == Scene::NO_FINGER || scene == Scene::WAIT_FINGER) && now - seekMs >= SEEK_FRAME_MS) {
    seekMs = now;
    const int op = seekOpacityAt(now);
    if (op != seekOpacityDrawn) {
      seekOpacityDrawn = (int8_t)op;
      drawNoFingerIcon((uint8_t)(op * 17), scene == Scene::NO_FINGER);
    }
  } else if (scene == Scene::CONNECTING && now - connMs >= CONN_STEP_MS) {
    connMs = now;
    connStep = (int8_t)((connStep + 1) % 4);
    drawConnIcon(connStep);
  }
  flushDirty();
  if (blPending && scene != Scene::NONE) {  // the first screen is on the panel: now the light, no flash of garbage
    blPending = false;
    tftBacklight(true);
  }
}
