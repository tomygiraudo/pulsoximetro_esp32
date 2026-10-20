// Compares the vertical scale of the pulse trace on the TFT, before and after src/display/trace_scale.c,
// on a capture CSV, on the PC.
//
// Every sample goes through the real pipeline (lib/ppg), the trace is built as display.cpp
// builds it (ir_view at 48 columns/s, a history of 118 columns) and every 40 ms, the frame rate
// of the plot, two scales are computed side by side:
//   antes  the old one: min and max of the whole window, widening at once, narrowing at 10 %
//   ahora  the new one: src/display/trace_scale.c
// For each, the metric of a frame is the height in pixels with which the last second of trace
// is drawn (clipped to the plot, as display.cpp does). A trace that fills the plot is ~28 px;
// below FLAT_PX the wave is, to the eye, a flat line.
//
// Frames are only counted once the finger has been on for 6 s (the median needs 5 blocks).
//
//   trace_host capture.csv [--spike S] [--press S PCT [LEN]] [--csv out.csv]
//
//   --spike S          at S seconds, ONE sample of the IR goes to full scale (a bus glitch, as the one at
//                      32.7 s of captures/pulgar_tomy_v3.csv)
//   --press S PCT [LEN] from S seconds, for LEN s (default 2), red and IR x (1 + PCT/100): the finger
//                      presses or slips
//   --csv out.csv      one row per frame: t, both scales and both heights, to plot them
//
// Build, from firmware/pulsox (any C99 compiler; `python -m ziglang cc` works too):
//   cc -O2 -std=c11 -Iinclude -Ilib/ppg -Isrc test/host/trace_host.c src/display/trace_scale.c \
//      lib/ppg/ppg_processor.c lib/ppg/sos_filter.c -lm -o trace_host
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ppg_processor.h"
#include "display/trace_scale.h"

#define LINE_MAX_LEN 512

// The numbers of src/display/display.cpp and src/display/display.h.
#define PLOT_COLS 118
#define PLOT_ROWS 38
#define PLOT_BASE_Y 33.0f
#define PLOT_PEAK_Y 5.0f
#define PX_PER_S 48.0f
#define MIN_SPAN 64.0f
#define FRAME_SAMPLES 4  // 40 ms at 100 samples/s (PLOT_FRAME_MS)

#define FLAT_PX 8.0f
#define WARMUP_COLS (6 * 48)

static int column(const char *header, const char *name) {
  char copy[LINE_MAX_LEN];
  strncpy(copy, header, sizeof(copy) - 1);
  copy[sizeof(copy) - 1] = '\0';
  int idx = 0;
  for (char *tok = strtok(copy, ",\r\n"); tok; tok = strtok(NULL, ",\r\n"), idx++) {
    if (strcmp(tok, name) == 0) return idx;
  }
  return -1;
}

static int field(const char *line, int idx, unsigned long *out) {
  const char *s = line;
  for (int i = 0; i < idx; i++) {
    s = strchr(s, ',');
    if (!s) return 0;
    s++;
  }
  char *end;
  *out = strtoul(s, &end, 10);
  return end != s;
}

static PpgProcessor ppg;

static float hist[PLOT_COLS];  // as in display.cpp
static int histHead, histCount;
static float ppgPhase, ppgSum;
static int ppgCount;

static TraceScale scale;                 // ahora
static float oldLo, oldHi;               // antes
static int oldValid;

static void reset_trace(void) {
  histHead = histCount = ppgCount = 0;
  ppgPhase = ppgSum = 0.0f;
  trace_scale_reset(&scale);
  oldValid = 0;
}

// displayPushPpg() + pushColumn() of display.cpp.
static void push_sample(float v) {
  ppgSum += v;
  ppgCount++;
  ppgPhase += PX_PER_S / SENSOR_FS_HZ;
  if (ppgPhase < 1.0f) return;
  ppgPhase -= 1.0f;
  hist[histHead] = ppgSum / ppgCount;
  histHead = (histHead + 1) % PLOT_COLS;
  if (histCount < PLOT_COLS) histCount++;
  trace_scale_push(&scale, hist[(histHead + PLOT_COLS - 1) % PLOT_COLS]);
  ppgSum = 0.0f;
  ppgCount = 0;
}

// Height in px of the last second of the trace drawn with the scale lo..hi; *clipped gets how
// many of its columns hit the edge of the plot.
static float last_second_height(float lo, float hi, int *clipped) {
  float span = hi - lo, base = lo;
  if (span < MIN_SPAN) {
    base -= (MIN_SPAN - span) * 0.5f;
    span = MIN_SPAN;
  }
  const float k = (PLOT_BASE_Y - PLOT_PEAK_Y) / span * 16.0f;
  float ymin = 1e30f, ymax = -1e30f;
  *clipped = 0;
  for (int i = PLOT_COLS - (int)PX_PER_S; i < PLOT_COLS; i++) {
    float y = PLOT_BASE_Y * 16.0f - (hist[(histHead + i) % PLOT_COLS] - base) * k;
    if (y < 8.0f || y > PLOT_ROWS * 16.0f - 8.0f) (*clipped)++;
    if (y < 8.0f) y = 8.0f;
    if (y > PLOT_ROWS * 16.0f - 8.0f) y = PLOT_ROWS * 16.0f - 8.0f;
    if (y < ymin) ymin = y;
    if (y > ymax) ymax = y;
  }
  return (ymax - ymin) / 16.0f;
}

typedef struct {
  float *h;
  int n, flat, run, longest_run;
  long clipped_cols, clipped_frames;
} Stats;

static void stats_add(Stats *s, float h, int clipped) {
  s->h[s->n++] = h;
  s->clipped_cols += clipped;
  if (clipped) s->clipped_frames++;
  if (h < FLAT_PX) {
    s->flat++;
    if (++s->run > s->longest_run) s->longest_run = s->run;
  } else {
    s->run = 0;
  }
}

static int cmp_float(const void *a, const void *b) {
  float x = *(const float *)a, y = *(const float *)b;
  return (x > y) - (x < y);
}

static void stats_print(const char *name, Stats *s) {
  if (s->n == 0) {
    printf("  %-6s sin cuadros\n", name);
    return;
  }
  qsort(s->h, s->n, sizeof(float), cmp_float);
  printf("  %-6s altura mediana %5.1f px | p10 %5.1f px | aplanados %5.1f %% | racha aplanada max %.2f s | "
         "recortado: %.1f %% de las columnas, en %.1f %% de los cuadros\n",
         name, s->h[s->n / 2], s->h[s->n / 10], 100.0f * s->flat / s->n,
         s->longest_run * FRAME_SAMPLES / SENSOR_FS_HZ, 100.0 * s->clipped_cols / ((double)s->n * (int)PX_PER_S),
         100.0 * s->clipped_frames / s->n);
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "uso: %s captura.csv [--spike S] [--press S PCT [LEN]] [--csv out.csv]\n", argv[0]);
    return 2;
  }
  int spike_s = -1, press_s = -1, press_pct = 0, press_len = 2;
  const char *csv_path = NULL;
  for (int i = 2; i < argc; i++) {
    if (!strcmp(argv[i], "--spike") && i + 1 < argc) {
      spike_s = atoi(argv[++i]);
    } else if (!strcmp(argv[i], "--press") && i + 2 < argc) {
      press_s = atoi(argv[++i]);
      press_pct = atoi(argv[++i]);
      if (i + 1 < argc && argv[i + 1][0] != '-') press_len = atoi(argv[++i]);
    } else if (!strcmp(argv[i], "--csv") && i + 1 < argc) {
      csv_path = argv[++i];
    } else {
      fprintf(stderr, "argumento desconocido: %s\n", argv[i]);
      return 2;
    }
  }

  FILE *in = fopen(argv[1], "r");
  if (!in) {
    perror(argv[1]);
    return 1;
  }
  FILE *fc = NULL;
  if (csv_path) {
    fc = fopen(csv_path, "w");
    if (!fc) {
      perror(csv_path);
      return 1;
    }
    fprintf(fc, "t,lo_antes,hi_antes,lo_ahora,hi_ahora,h_antes,h_ahora\n");
  }

  char line[LINE_MAX_LEN];
  if (!fgets(line, sizeof(line), in)) return 1;
  int cn = column(line, "n"), cred = column(line, "red"), cir = column(line, "ir");
  if (cn < 0 || cred < 0 || cir < 0) {
    fprintf(stderr, "%s: faltan columnas n, red, ir\n", argv[1]);
    return 1;
  }

  // One frame every FRAME_SAMPLES samples: room for every sample of a 10 minute capture.
  const int max_frames = 600 * (int)SENSOR_FS_HZ / FRAME_SAMPLES;
  Stats before = {malloc(max_frames * sizeof(float)), 0, 0, 0, 0, 0, 0};
  Stats after = {malloc(max_frames * sizeof(float)), 0, 0, 0, 0, 0, 0};

  ppg_init(&ppg);
  reset_trace();
  unsigned long n, red, ir;
  long rows = 0;
  int finger = 0;
  long cols_since_finger = 0;
  while (fgets(line, sizeof(line), in)) {
    if (!field(line, cn, &n) || !field(line, cred, &red) || !field(line, cir, &ir)) continue;
    const double t = rows / SENSOR_FS_HZ;
    if (spike_s >= 0 && rows == (long)(spike_s * SENSOR_FS_HZ)) ir = (unsigned long)SENSOR_ADC_MAX;
    if (press_s >= 0 && t >= press_s && t < press_s + press_len) {
      red = (unsigned long)(red * (1.0 + press_pct / 100.0));
      ir = (unsigned long)(ir * (1.0 + press_pct / 100.0));
    }

    ppg_push(&ppg, (uint32_t)red, (uint32_t)ir);
    const PpgDebug *d = ppg_debug(&ppg);
    if (d->finger && !finger) {  // the screen starts a new trace when the finger arrives
      reset_trace();
      cols_since_finger = 0;
    }
    finger = d->finger;
    rows++;
    if (!finger) continue;

    push_sample(d->ir_view);
    if (ppgCount == 0) cols_since_finger++;  // push_sample() just closed a column

    if (rows % FRAME_SAMPLES != 0 || histCount == 0) continue;

    // The old scale, as display.cpp had it.
    float lo = hist[(histHead + PLOT_COLS - histCount) % PLOT_COLS], hi = lo;
    for (int i = PLOT_COLS - histCount + 1; i < PLOT_COLS; i++) {
      const float v = hist[(histHead + i) % PLOT_COLS];
      if (v < lo) lo = v;
      if (v > hi) hi = v;
    }
    if (!oldValid) {
      oldLo = lo;
      oldHi = hi;
      oldValid = 1;
    }
    oldLo = lo < oldLo ? lo : oldLo + (lo - oldLo) * 0.1f;
    oldHi = hi > oldHi ? hi : oldHi + (hi - oldHi) * 0.1f;
    // The new one.
    float newLo, newHi;
    trace_scale_update(&scale, lo, hi, &newLo, &newHi);

    if (histCount < PLOT_COLS || cols_since_finger < WARMUP_COLS) continue;
    int cb, ca;
    const float hb = last_second_height(oldLo, oldHi, &cb), ha = last_second_height(newLo, newHi, &ca);
    if (before.n < max_frames) stats_add(&before, hb, cb);
    if (after.n < max_frames) stats_add(&after, ha, ca);
    if (fc) fprintf(fc, "%.2f,%.1f,%.1f,%.1f,%.1f,%.2f,%.2f\n", t, oldLo, oldHi, newLo, newHi, hb, ha);
  }
  fclose(in);
  if (fc) fclose(fc);

  printf("%s: %ld muestras (%.0f s)", argv[1], rows, rows / SENSOR_FS_HZ);
  if (spike_s >= 0) printf(", --spike %d", spike_s);
  if (press_s >= 0) printf(", --press %d %d %d", press_s, press_pct, press_len);
  printf(", %d cuadros evaluados\n", before.n);
  stats_print("antes", &before);
  stats_print("ahora", &after);
  return 0;
}
