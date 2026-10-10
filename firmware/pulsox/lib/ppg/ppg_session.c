#include "ppg_session.h"
#include <string.h>

#define W MEAS_RESULT_WINDOW_S

// Median of v[0..n) (n > 0); sorts v in place.
static float median(float *v, int n) {
  for (int i = 1; i < n; i++) {  // insertion sort
    float x = v[i];
    int j = i - 1;
    while (j >= 0 && v[j] > x) {
      v[j + 1] = v[j];
      j--;
    }
    v[j + 1] = x;
  }
  return (n & 1) ? v[n / 2] : 0.5f * (v[n / 2 - 1] + v[n / 2]);
}

void ppg_session_init(PpgSession *s) { memset(s, 0, sizeof(*s)); }

static void abort_with(PpgSession *s, PpgAbortReason r) {
  s->state = PPG_SESSION_ABORTED;
  s->reason = r;
}

// The good seconds of the window: how many, and their medians when asked for.
static int window_good(const PpgSession *s, float *spo2, float *bpm, float *quality) {
  float a[W], b[W], c[W];
  int n = 0;
  for (int i = 0; i < s->w_count; i++) {
    if (!s->w_good[i]) continue;
    a[n] = s->w_spo2[i];
    b[n] = s->w_bpm[i];
    c[n] = s->w_quality[i];
    n++;
  }
  if (n > 0 && spo2) {
    *spo2 = median(a, n);
    *bpm = median(b, n);
    *quality = median(c, n);
  }
  return n;
}

PpgSessionState ppg_session_push(PpgSession *s, const PpgOutput *o) {
  if (s->state != PPG_SESSION_RUNNING) return s->state;
  s->seconds++;

  s->finger_lost_run = o->finger ? 0 : (uint16_t)(s->finger_lost_run + 1);
  if (s->finger_lost_run >= MEAS_FINGER_LOST_S) {
    abort_with(s, PPG_ABORT_FINGER_LOST);
    return s->state;
  }
  s->saturated_run = o->saturated ? (uint16_t)(s->saturated_run + 1) : 0;
  if (s->saturated_run >= MEAS_SATURATED_MAX_S) {
    abort_with(s, PPG_ABORT_SATURATED);
    return s->state;
  }

  bool good = false;
  if (s->seconds > MEAS_WARMUP_S) {
    good = o->spo2_valid && o->bpm_valid && o->quality >= MEAS_QUALITY_MIN;
    s->bad_run = good ? 0 : (uint16_t)(s->bad_run + 1);
    if (good) s->valid_seconds++;
    if (s->bad_run >= MEAS_INVALID_MAX_S) {
      abort_with(s, PPG_ABORT_ERRATIC);
      return s->state;
    }
  }

  s->w_spo2[s->w_head] = o->spo2;
  s->w_bpm[s->w_head] = o->bpm;
  s->w_quality[s->w_head] = o->quality;
  s->w_good[s->w_head] = good;
  s->w_head = (uint8_t)((s->w_head + 1) % W);
  if (s->w_count < W) s->w_count++;

  if (s->seconds >= MEAS_MIN_S && s->valid_seconds >= MEAS_MIN_VALID_S &&
      window_good(s, 0, 0, 0) >= MEAS_RESULT_MIN_N) {
    window_good(s, &s->spo2, &s->bpm, &s->quality);
    s->state = PPG_SESSION_DONE;
  } else if (s->seconds >= MEAS_MAX_S) {
    abort_with(s, PPG_ABORT_INSUFFICIENT);
  }
  return s->state;
}

int ppg_session_progress(const PpgSession *s) {
  int pct = s->seconds * 100 / MEAS_MIN_S;
  return pct > 100 ? 100 : pct;
}

const char *ppg_abort_text(PpgAbortReason r) {
  switch (r) {
    case PPG_ABORT_FINGER_LOST: return "dedo retirado";
    case PPG_ABORT_SATURATED: return "sensor saturado";
    case PPG_ABORT_ERRATIC: return "senal erratica";
    case PPG_ABORT_INSUFFICIENT: return "lecturas insuficientes";
    default: return "ninguno";
  }
}
