// Real-time PPG pipeline: raw RED/IR samples in, SpO2 and heart rate out.
// Plain C, no Arduino dependency; tunables come from include/config.h.
//
// Per sample (one call to ppg_push):
//   1. Finger detection on the raw IR (thresholds with hysteresis and debounce).
//      When the finger appears everything is reset and the filters are primed with
//      that sample, then SETTLE_S seconds are skipped while they settle.
//   2. Band-pass (AC) and low-pass (DC) on RED and IR, the filters of
//      tools/procesamiento.py run in one pass. The AC is inverted so a pulse is a
//      positive peak.
//   3. AC^2 and DC go into a sliding window of SPO2_WINDOW_S seconds.
//   4. Beat detector on the IR AC: adaptive threshold, refractory period, RR
//      intervals.
//   5. A separate, wider band-pass on the IR (ir_view in PpgDebug), only to draw the
//      pulse: the measuring filter is too narrow to show the dicrotic notch.
// Once per second (when ppg_push returns true) the output is refreshed:
//   R     = (RMS(AC_red) / mean(DC_red)) / (RMS(AC_ir) / mean(DC_ir))
//   SpO2  = SPO2_TABLE[round(R * 100)]
//   BPM   = 60 / median of the last BPM_MEDIAN_N RR intervals
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "config.h"
#include "sos_filter.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PPG_FS ((int)SENSOR_FS_HZ)  // samples per second
#define PPG_WINDOW_N (SPO2_WINDOW_S * PPG_FS)

// Result, refreshed once per second.
typedef struct {
  bool finger;            // a finger is on the sensor
  bool saturated;         // a raw sample hit the ADC limit within the window
  bool spo2_valid;
  bool bpm_valid;
  float spo2;             // %, 0 when not valid
  float r_ratio;          // R, filled whenever it could be computed (even if SpO2 is not valid)
  float perfusion_index;  // % : RMS of the IR AC over the mean IR DC
  float bpm;              // provisional (valid = false) until enough beats were seen
} PpgOutput;

// Intermediate values of the last sample, for the CSV stream and for validation.
typedef struct {
  float red_ac, ir_ac;  // band-passed and inverted
  float red_dc, ir_dc;  // low-passed
  float ir_view;        // IR through the wide display band-pass, inverted: for drawing, not measuring
  float beat_thr;       // beat detection threshold
  bool beat;            // a beat was declared on this sample (its peak was a few samples earlier)
  bool finger;          // finger state on this sample
} PpgDebug;

typedef struct {
  float env;        // peak envelope of the IR AC
  bool above;       // the IR AC is above the threshold
  float peak_val;   // highest value of the current excursion
  uint32_t peak_n;  // ... and where it happened
  bool have_last;
  uint32_t last_beat_n;
  float rr[BPM_MEDIAN_N];  // accepted RR intervals, in seconds, as a ring
  uint8_t rr_n, rr_head;
  uint8_t rejects;  // rejected RRs in a row
} PpgBeat;

typedef struct {
  uint32_t n;  // samples since ppg_init

  bool finger;
  uint8_t debounce;
  uint16_t settle_left;
  uint16_t sat_hold;  // samples left until the last saturated one leaves the window
  uint16_t to_eval;   // samples until the next refresh of the output

  SosFilter bp_red, bp_ir, lp_red, lp_ir, view_ir;

  // Sliding window, as rings: AC^2 and DC of each channel.
  float w_ac2_red[PPG_WINDOW_N], w_ac2_ir[PPG_WINDOW_N];
  float w_dc_red[PPG_WINDOW_N], w_dc_ir[PPG_WINDOW_N];
  uint16_t w_head, w_count;

  PpgBeat beat;
  PpgOutput out;
  PpgDebug dbg;
} PpgProcessor;

// Starts from scratch (also what to call when the sensor is restarted).
void ppg_init(PpgProcessor *p);

// Feeds one raw sample (18-bit ADC counts). Returns true when the output was
// refreshed, which happens once per second.
bool ppg_push(PpgProcessor *p, uint32_t red, uint32_t ir);

static inline const PpgOutput *ppg_output(const PpgProcessor *p) { return &p->out; }
static inline const PpgDebug *ppg_debug(const PpgProcessor *p) { return &p->dbg; }

#ifdef __cplusplus
}
#endif
