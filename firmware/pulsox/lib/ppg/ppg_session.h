// Judges one measurement from the once-per-second output of the pipeline (ppg_processor.h):
// when it is finished, with which result, or why it has to be discarded. Plain C, no Arduino
// dependency, so tools/session_host.c can run it on the PC against recorded captures.
//
// Per second (ppg_session_push):
//   * finger gone for MEAS_FINGER_LOST_S seconds in a row     -> ABORTED, FINGER_LOST
//   * saturated for MEAS_SATURATED_MAX_S seconds in a row      -> ABORTED, SATURATED
//   * after the MEAS_WARMUP_S warm-up, a second is GOOD when SpO2 and BPM are valid and the
//     quality is at least MEAS_QUALITY_MIN; MEAS_INVALID_MAX_S bad ones in a row
//                                                              -> ABORTED, ERRATIC
//   * from MEAS_MIN_S on, with MEAS_MIN_VALID_S good seconds in all and MEAS_RESULT_MIN_N of
//     them in the last MEAS_RESULT_WINDOW_S -> DONE, the result is the median of those
//   * MEAS_MAX_S reached without that                          -> ABORTED, INSUFFICIENT
// Once DONE or ABORTED it stays that way: the next measurement starts with ppg_session_init().
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "ppg_processor.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  PPG_SESSION_RUNNING = 0,
  PPG_SESSION_DONE,
  PPG_SESSION_ABORTED,
} PpgSessionState;

typedef enum {
  PPG_ABORT_NONE = 0,
  PPG_ABORT_FINGER_LOST,
  PPG_ABORT_SATURATED,
  PPG_ABORT_ERRATIC,
  PPG_ABORT_INSUFFICIENT,
} PpgAbortReason;

typedef struct {
  PpgSessionState state;
  PpgAbortReason reason;      // why, when ABORTED
  uint16_t seconds;           // outputs fed so far
  uint16_t valid_seconds;     // good seconds since the warm-up
  uint16_t finger_lost_run, saturated_run, bad_run;

  // The last MEAS_RESULT_WINDOW_S seconds, as a ring.
  float w_spo2[MEAS_RESULT_WINDOW_S], w_bpm[MEAS_RESULT_WINDOW_S], w_quality[MEAS_RESULT_WINDOW_S];
  bool w_good[MEAS_RESULT_WINDOW_S];
  uint8_t w_head, w_count;

  // The result, when DONE.
  float spo2, bpm, quality;
} PpgSession;

void ppg_session_init(PpgSession *s);

// Feeds the output of one second. Returns the state after it.
PpgSessionState ppg_session_push(PpgSession *s, const PpgOutput *o);

// How far along the measurement is, 0-100: reaches 100 at MEAS_MIN_S, when it can finish first.
int ppg_session_progress(const PpgSession *s);

// Short Spanish text of the reason, for logs.
const char *ppg_abort_text(PpgAbortReason r);

#ifdef __cplusplus
}
#endif
