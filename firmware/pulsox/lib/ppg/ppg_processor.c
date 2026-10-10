#include "ppg_processor.h"
#include <math.h>
#include <string.h>
#include "ppg_coefs.h"
#include "spo2_table.h"

// The coefficients in ppg_coefs.h are for one specific sample rate.
_Static_assert((int)SENSOR_FS_HZ == PPG_COEFS_FS_HZ,
               "SENSOR_FS_HZ changed: run tools/gen_ppg_coefs.py");
_Static_assert(PPG_BP_SECTIONS <= SOS_MAX_SECTIONS && PPG_LP_SECTIONS <= SOS_MAX_SECTIONS &&
                   PPG_VIEW_SECTIONS <= SOS_MAX_SECTIONS,
               "raise SOS_MAX_SECTIONS");

#define FINGER_DEBOUNCE_N ((int)(FINGER_DEBOUNCE_S * SENSOR_FS_HZ))
#define SETTLE_N ((int)(SETTLE_S * SENSOR_FS_HZ))
#define SATURATION_LEVEL (SATURATION_FRACTION * SENSOR_ADC_MAX)
#define REFRACTORY_N (BEAT_REFRACTORY_S * SENSOR_FS_HZ)
#define ENV_DECAY (1.0f - 1.0f / (BEAT_ENV_TAU_S * SENSOR_FS_HZ))  // per sample
#define LAST_BEAT_MAX_N ((uint32_t)(RR_MAX_S * SENSOR_FS_HZ))      // older than this: no pulse

// ---- Beat detection and heart rate -----------------------------------------------

static void beat_reset(PpgBeat *b) { memset(b, 0, sizeof(*b)); }

// Median of the accepted RR intervals (rr_n > 0).
static float rr_median(const PpgBeat *b) {
  float s[BPM_MEDIAN_N];
  int n = b->rr_n;
  for (int i = 0; i < n; i++) s[i] = b->rr[i];
  for (int i = 1; i < n; i++) {  // insertion sort
    float v = s[i];
    int j = i - 1;
    while (j >= 0 && s[j] > v) {
      s[j + 1] = s[j];
      j--;
    }
    s[j + 1] = v;
  }
  return (n & 1) ? s[n / 2] : 0.5f * (s[n / 2 - 1] + s[n / 2]);
}

static void rr_push(PpgBeat *b, float rr) {
  b->rr[b->rr_head] = rr;
  b->rr_head = (uint8_t)((b->rr_head + 1) % BPM_MEDIAN_N);
  if (b->rr_n < BPM_MEDIAN_N) b->rr_n++;
}

// A beat whose IR AC peak was at sample `peak_n`. Returns true if it counts as a beat.
static bool beat_register(PpgBeat *b, uint32_t peak_n) {
  if (b->have_last) {
    uint32_t d = peak_n - b->last_beat_n;
    if ((float)d < REFRACTORY_N) return false;  // the dicrotic wave or noise, not a new beat

    float rr = (float)d / SENSOR_FS_HZ;
    bool in_range = rr >= RR_MIN_S && rr <= RR_MAX_S;
    bool plausible = in_range;
    if (plausible && b->rr_n > 0) {
      float med = rr_median(b);
      plausible = fabsf(rr - med) <= RR_MAX_DEVIATION * med;
    }
    if (plausible) {
      rr_push(b, rr);
      b->rejects = 0;
    } else if (++b->rejects >= RR_MAX_REJECTS) {
      // The history no longer matches what is being measured (e.g. the pulse rate
      // really changed): start over instead of rejecting everything forever.
      b->rr_n = 0;
      b->rr_head = 0;
      b->rejects = 0;
      if (in_range) rr_push(b, rr);
    }
  }
  b->last_beat_n = peak_n;
  b->have_last = true;
  return true;
}

// One sample of the IR AC. `ir_dc` sets the floor of the threshold.
static void beat_update(PpgProcessor *p, float x, float ir_dc) {
  PpgBeat *b = &p->beat;
  b->env = (x > b->env) ? x : b->env * ENV_DECAY;

  float thr = BEAT_THRESHOLD_K * b->env;
  float floor_amp = BEAT_MIN_AMP_PCT * 0.01f * ir_dc;
  if (thr < floor_amp) thr = floor_amp;
  p->dbg.beat_thr = thr;

  if (!b->above) {
    if (x > thr) {
      b->above = true;
      b->peak_val = x;
      b->peak_n = p->n;
    }
  } else if (x > b->peak_val) {
    b->peak_val = x;
    b->peak_n = p->n;
  } else if (x < thr) {  // the excursion is over: its maximum is the beat
    b->above = false;
    p->dbg.beat = beat_register(b, b->peak_n);
  }
}

// ---- Output ----------------------------------------------------------------------

static void evaluate_heart_rate(PpgProcessor *p) {
  const PpgBeat *b = &p->beat;
  PpgOutput *o = &p->out;
  o->bpm = 0.0f;
  o->bpm_valid = false;
  // No beat for a while: whatever the history says is stale, report nothing.
  if (b->rr_n == 0 || (p->n - b->last_beat_n) > LAST_BEAT_MAX_N) return;

  float med = rr_median(b);
  o->bpm = 60.0f / med;
  if (b->rr_n < BPM_VALID_MIN_RR) return;

  float mean = 0.0f, var = 0.0f;
  for (int i = 0; i < b->rr_n; i++) mean += b->rr[i];
  mean /= b->rr_n;
  for (int i = 0; i < b->rr_n; i++) var += (b->rr[i] - mean) * (b->rr[i] - mean);
  float cv = sqrtf(var / b->rr_n) / mean;
  o->bpm_valid = cv <= BPM_VALID_MAX_CV;
}

static void evaluate_spo2(PpgProcessor *p) {
  PpgOutput *o = &p->out;
  o->spo2 = 0.0f;
  o->spo2_valid = false;
  o->r_ratio = 0.0f;
  o->perfusion_index = 0.0f;
  if (!p->finger || p->settle_left > 0 || p->w_count < PPG_WINDOW_N) return;

  double ac2_red = 0, ac2_ir = 0, dc_red = 0, dc_ir = 0;
  for (int i = 0; i < PPG_WINDOW_N; i++) {
    ac2_red += p->w_ac2_red[i];
    ac2_ir += p->w_ac2_ir[i];
    dc_red += p->w_dc_red[i];
    dc_ir += p->w_dc_ir[i];
  }
  double rms_red = sqrt(ac2_red / PPG_WINDOW_N), rms_ir = sqrt(ac2_ir / PPG_WINDOW_N);
  dc_red /= PPG_WINDOW_N;
  dc_ir /= PPG_WINDOW_N;
  if (dc_red <= 0.0 || dc_ir <= 0.0 || rms_ir <= 0.0) return;

  double r = (rms_red / dc_red) / (rms_ir / dc_ir);
  o->r_ratio = (float)r;
  o->perfusion_index = (float)(100.0 * rms_ir / dc_ir);

  int idx = (int)(r * 100.0 + 0.5);
  bool in_table = idx >= SPO2_TABLE_IDX_MIN && idx < SPO2_TABLE_SIZE;
  if (in_table && !o->saturated && o->perfusion_index >= SPO2_MIN_PI_PCT &&
      SPO2_TABLE[idx] >= SPO2_MIN_VALID) {
    o->spo2 = (float)SPO2_TABLE[idx];
    o->spo2_valid = true;
  }
}

static void evaluate(PpgProcessor *p) {
  p->out.finger = p->finger;
  p->out.saturated = p->sat_hold > 0;
  evaluate_spo2(p);
  evaluate_heart_rate(p);
}

// ---- Finger, reset, sample entry point ----------------------------------------------

// The finger just appeared on this sample: forget everything and prime the filters,
// so the first sample does not look like a step from zero.
static void start_measurement(PpgProcessor *p, uint32_t red, uint32_t ir) {
  sos_init(&p->bp_red, PPG_BP_SOS, PPG_BP_SECTIONS);
  sos_init(&p->bp_ir, PPG_BP_SOS, PPG_BP_SECTIONS);
  sos_init(&p->lp_red, PPG_LP_SOS, PPG_LP_SECTIONS);
  sos_init(&p->lp_ir, PPG_LP_SOS, PPG_LP_SECTIONS);
  sos_init(&p->view_ir, PPG_VIEW_SOS, PPG_VIEW_SECTIONS);
  sos_prime(&p->view_ir, (double)ir);
  sos_prime(&p->bp_red, (double)red);
  sos_prime(&p->bp_ir, (double)ir);
  sos_prime(&p->lp_red, (double)red);
  sos_prime(&p->lp_ir, (double)ir);
  p->w_head = 0;
  p->w_count = 0;
  p->settle_left = SETTLE_N;
  beat_reset(&p->beat);
}

void ppg_init(PpgProcessor *p) {
  memset(p, 0, sizeof(*p));
  p->to_eval = PPG_FS;
}

bool ppg_push(PpgProcessor *p, uint32_t red, uint32_t ir) {
  p->n++;
  p->dbg.beat = false;

  // Saturation: the clipped samples distort the filters for as long as they are in the window.
  if ((float)red >= SATURATION_LEVEL || (float)ir >= SATURATION_LEVEL) {
    p->sat_hold = PPG_WINDOW_N;
  } else if (p->sat_hold > 0) {
    p->sat_hold--;
  }

  // Finger, from the raw IR: on above FINGER_ON_IR_DC, off below FINGER_OFF_IR_DC, each
  // only after FINGER_DEBOUNCE_S seconds.
  bool wants_change = p->finger ? ((float)ir < FINGER_OFF_IR_DC) : ((float)ir > FINGER_ON_IR_DC);
  if (!wants_change) {
    p->debounce = 0;
  } else if (++p->debounce >= FINGER_DEBOUNCE_N) {
    p->debounce = 0;
    p->finger = !p->finger;
    if (p->finger) {
      start_measurement(p, red, ir);
    } else {
      beat_reset(&p->beat);
      p->w_count = 0;
    }
  }

  if (p->finger) {
    double bp_red = sos_step(&p->bp_red, (double)red);
    double bp_ir = sos_step(&p->bp_ir, (double)ir);
    double dc_red = sos_step(&p->lp_red, (double)red);
    double dc_ir = sos_step(&p->lp_ir, (double)ir);
    float ac_red = (float)-bp_red, ac_ir = (float)-bp_ir;  // inverted: a pulse is a positive peak
    p->dbg.red_ac = ac_red;
    p->dbg.ir_ac = ac_ir;
    p->dbg.red_dc = (float)dc_red;
    p->dbg.ir_dc = (float)dc_ir;
    p->dbg.ir_view = (float)-sos_step(&p->view_ir, (double)ir);

    if (p->settle_left > 0) {
      p->settle_left--;
      p->dbg.beat_thr = 0.0f;
    } else {
      p->w_ac2_red[p->w_head] = ac_red * ac_red;
      p->w_ac2_ir[p->w_head] = ac_ir * ac_ir;
      p->w_dc_red[p->w_head] = (float)dc_red;
      p->w_dc_ir[p->w_head] = (float)dc_ir;
      p->w_head = (uint16_t)((p->w_head + 1) % PPG_WINDOW_N);
      if (p->w_count < PPG_WINDOW_N) p->w_count++;
      beat_update(p, ac_ir, (float)dc_ir);
    }
  } else {
    memset(&p->dbg, 0, sizeof(p->dbg));
  }
  p->dbg.finger = p->finger;

  if (--p->to_eval > 0) return false;
  p->to_eval = PPG_FS;
  evaluate(p);
  return true;
}
