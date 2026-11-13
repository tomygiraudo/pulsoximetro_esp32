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

/*******************************************************************************
 * @brief       Borra el estado del detector de latidos.
 *
 * @details     Pone en cero toda la estructura PpgBeat con memset: envolvente,
 *              excursión actual e historial de intervalos RR.
 *
 * @param       Salida:  b - estado del detector, puesto en cero.
 *
 * @peripherals Ninguno.
 *******************************************************************************/
static void beat_reset(PpgBeat *b) { memset(b, 0, sizeof(*b)); }

/*******************************************************************************
 * @brief       Calcula la mediana de los intervalos RR aceptados.
 *
 * @details     Copia los rr_n valores del anillo rr[], los ordena por inserción
 *              y toma el del medio (o el promedio de los dos centrales).
 *              Requiere rr_n > 0.
 *
 * @param       Entrada: b - estado de latidos (rr[] y rr_n).
 *              Salida:  retorno (float) - mediana de los RR, en segundos.
 *
 * @peripherals Ninguno.
 *******************************************************************************/
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

/*******************************************************************************
 * @brief       Guarda un intervalo RR aceptado en los dos anillos de historial.
 *
 * @details     Lo escribe en rr[] (BPM_MEDIAN_N, para la mediana y la validez)
 *              y en rr_avg[] (BPM_AVG_N, para el promedio del BPM), avanzando
 *              cada cabeza hasta llenar el anillo.
 *
 * @param       Entrada: rr - intervalo entre dos latidos, en segundos.
 *              Salida:  b - actualiza rr[], rr_head, rr_n, rr_avg[], avg_head y
 *                       avg_n.
 *
 * @peripherals Ninguno.
 *******************************************************************************/
static void rr_push(PpgBeat *b, float rr) {
  b->rr[b->rr_head] = rr;
  b->rr_head = (uint8_t)((b->rr_head + 1) % BPM_MEDIAN_N);
  if (b->rr_n < BPM_MEDIAN_N) b->rr_n++;
  b->rr_avg[b->avg_head] = rr;
  b->avg_head = (uint8_t)((b->avg_head + 1) % BPM_AVG_N);
  if (b->avg_n < BPM_AVG_N) b->avg_n++;
}

/*******************************************************************************
 * @brief       Calcula el promedio de los RR aceptados del anillo largo.
 *
 * @details     Suma los avg_n valores de rr_avg[] y divide por avg_n: el BPM
 *              sale de este promedio. Requiere avg_n > 0.
 *
 * @param       Entrada: b - estado de latidos (rr_avg[] y avg_n).
 *              Salida:  retorno (float) - RR medio, en segundos.
 *
 * @peripherals Ninguno.
 *******************************************************************************/
static float rr_avg_mean(const PpgBeat *b) {
  float sum = 0.0f;
  for (int i = 0; i < b->avg_n; i++) sum += b->rr_avg[i];
  return sum / b->avg_n;
}

/*******************************************************************************
 * @brief       Registra un latido candidato y dice si cuenta como latido.
 *
 * @details     Lo descarta si llega antes de BEAT_REFRACTORY_S del anterior
 *              (onda dicrótica o ruido). Si el RR está en rango y a menos de
 *              RR_MAX_DEVIATION de la mediana lo guarda con rr_push(); tras
 *              RR_MAX_REJECTS rechazos seguidos olvida el historial.
 *
 * @param       Entrada: peak_n - índice de muestra del pico del IR AC.
 *              Entrada: b - estado de latidos (último latido, historial de RR).
 *              Salida:  retorno (bool) - false si cae en el período
 *                       refractario; true si cuenta.
 *              Salida:  b - actualiza last_beat_n, have_last, rejects y los
 *                       anillos de RR.
 *
 * @peripherals Ninguno.
 *******************************************************************************/
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
      b->avg_n = 0;
      b->avg_head = 0;
      b->rejects = 0;
      if (in_range) rr_push(b, rr);
    }
  }
  b->last_beat_n = peak_n;
  b->have_last = true;
  return true;
}

/*******************************************************************************
 * @brief       Procesa una muestra del IR AC con el detector de latidos.
 *
 * @details     Actualiza la envolvente de picos (decae con BEAT_ENV_TAU_S) y el
 *              umbral BEAT_THRESHOLD_K * envolvente, con piso en
 *              BEAT_MIN_AMP_PCT % del DC. Sigue el máximo de cada excursión
 *              sobre el umbral y, al bajar de él, lo declara latido.
 *
 * @param       Entrada: x - muestra del IR AC (filtrada e invertida).
 *              Entrada: ir_dc - DC del IR: fija el piso del umbral.
 *              Entrada: p->n - índice de la muestra actual.
 *              Salida:  p->beat - envolvente, excursión actual e historial de
 *                       RR.
 *              Salida:  p->dbg - beat_thr (umbral) y beat (hubo latido en esta
 *                       muestra).
 *
 * @peripherals Ninguno.
 *******************************************************************************/
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

/*******************************************************************************
 * @brief       Calcula el coeficiente de variación de los RR aceptados.
 *
 * @details     Desvío estándar de rr[] dividido por su media: mide cuán
 *              irregulares son los latidos. Requiere rr_n > 0.
 *
 * @param       Entrada: b - estado de latidos (rr[] y rr_n).
 *              Salida:  retorno (float) - coeficiente de variación (sin
 *                       unidad).
 *
 * @peripherals Ninguno.
 *******************************************************************************/
static float rr_cv(const PpgBeat *b) {
  float mean = 0.0f, var = 0.0f;
  for (int i = 0; i < b->rr_n; i++) mean += b->rr[i];
  mean /= b->rr_n;
  for (int i = 0; i < b->rr_n; i++) var += (b->rr[i] - mean) * (b->rr[i] - mean);
  return sqrtf(var / b->rr_n) / mean;
}

/*******************************************************************************
 * @brief       Calcula el BPM y si es confiable, a partir de los RR aceptados.
 *
 * @details     BPM = 60 / promedio de rr_avg[]. Sin RR, o sin latido hace más
 *              de RR_MAX_S, no informa nada. Es válido con al menos
 *              BPM_VALID_MIN_RR intervalos y un coeficiente de variación <=
 *              BPM_VALID_MAX_CV.
 *
 * @param       Entrada: p->beat, p->n - historial de latidos y muestra actual.
 *              Salida:  p->out - bpm y bpm_valid.
 *
 * @peripherals Ninguno.
 *******************************************************************************/
static void evaluate_heart_rate(PpgProcessor *p) {
  const PpgBeat *b = &p->beat;
  PpgOutput *o = &p->out;
  o->bpm = 0.0f;
  o->bpm_valid = false;
  // No beat for a while: whatever the history says is stale, report nothing.
  if (b->rr_n == 0 || (p->n - b->last_beat_n) > LAST_BEAT_MAX_N) return;

  o->bpm = 60.0f / rr_avg_mean(b);
  if (b->rr_n < BPM_VALID_MIN_RR) return;

  o->bpm_valid = rr_cv(b) <= BPM_VALID_MAX_CV;
}

/*******************************************************************************
 * @brief       Calcula un índice de calidad de 0 a 1: pulso fuerte que late con
 *              regularidad.
 *
 * @details     Es el producto de q_pi (índice de perfusión sobre
 *              QUALITY_PI_GOOD_PCT, tope 1; 0 si pasa de QUALITY_PI_MAX_PCT) y
 *              q_rr (1 - CV / QUALITY_CV_BAD; 0,5 con menos de 3 RR). Es 0 sin
 *              dedo o saturado. Usa el resultado de evaluate_spo2().
 *
 * @param       Entrada: p->out - finger, saturated y perfusion_index.
 *              Entrada: p->beat, p->n - historial de latidos y muestra actual.
 *              Salida:  p->out.quality - calidad, de 0 a 1.
 *
 * @peripherals Ninguno.
 *******************************************************************************/
static void evaluate_quality(PpgProcessor *p) {
  const PpgBeat *b = &p->beat;
  PpgOutput *o = &p->out;
  o->quality = 0.0f;
  if (!o->finger || o->saturated) return;

  float q_pi = o->perfusion_index / QUALITY_PI_GOOD_PCT;
  if (q_pi > 1.0f) q_pi = 1.0f;
  if (o->perfusion_index > QUALITY_PI_MAX_PCT) q_pi = 0.0f;  // far more AC than a pulse: motion or noise

  float q_rr = 0.5f;  // too few beats to judge their regularity yet: neutral
  if (b->rr_n >= 3) {
    if ((p->n - b->last_beat_n) > LAST_BEAT_MAX_N) {
      q_rr = 0.0f;  // the pulse stopped
    } else {
      float k = rr_cv(b) / QUALITY_CV_BAD;
      q_rr = 1.0f - (k > 1.0f ? 1.0f : k);
    }
  }
  o->quality = q_pi * q_rr;
}

/*******************************************************************************
 * @brief       Calcula SpO2, la relación R y los índices de perfusión de la
 *              ventana.
 *
 * @details     Con la ventana de SPO2_WINDOW_S s llena, saca el RMS del AC y la
 *              media del DC de cada canal, R = (RMS_red / DC_red) / (RMS_ir /
 *              DC_ir) y SpO2 = SPO2_TABLE[round(R * 100)]. Válido sin
 *              saturación, con PI >= SPO2_MIN_PI_PCT y SpO2 >= SPO2_MIN_VALID.
 *
 * @param       Entrada: p->w_ac2_*, p->w_dc_*, p->w_count - ventana deslizante
 *                       de AC^2 y DC.
 *              Entrada: p->finger, p->settle_left - sin dedo o asentando no se
 *                       calcula.
 *              Salida:  p->out - spo2, spo2_valid, r_ratio, índices de
 *                       perfusión y RMS de cada canal.
 *
 * @peripherals Ninguno.
 *******************************************************************************/
static void evaluate_spo2(PpgProcessor *p) {
  PpgOutput *o = &p->out;
  o->spo2 = 0.0f;
  o->spo2_valid = false;
  o->r_ratio = 0.0f;
  o->perfusion_index = 0.0f;
  o->perfusion_index_red = 0.0f;
  o->ac_rms_ir = 0.0f;
  o->ac_rms_red = 0.0f;
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
  o->perfusion_index_red = (float)(100.0 * rms_red / dc_red);
  o->ac_rms_ir = (float)rms_ir;
  o->ac_rms_red = (float)rms_red;

  int idx = (int)(r * 100.0 + 0.5);
  bool in_table = idx >= SPO2_TABLE_IDX_MIN && idx < SPO2_TABLE_SIZE;
  if (in_table && !o->saturated && o->perfusion_index >= SPO2_MIN_PI_PCT &&
      SPO2_TABLE[idx] >= SPO2_MIN_VALID) {
    o->spo2 = (float)SPO2_TABLE[idx];
    o->spo2_valid = true;
  }
}

/*******************************************************************************
 * @brief       Refresca la salida del procesador (una vez por segundo).
 *
 * @details     Copia dedo y saturación (sat_hold > 0) a la salida y llama, en
 *              este orden, a evaluate_spo2(), evaluate_heart_rate() y
 *              evaluate_quality(): la calidad usa el índice de perfusión que
 *              calcula la primera.
 *
 * @param       Entrada: p->finger, p->sat_hold - dedo presente y saturación
 *                       vigente.
 *              Salida:  p->out - resultado completo del segundo.
 *
 * @peripherals Ninguno.
 *******************************************************************************/
static void evaluate(PpgProcessor *p) {
  p->out.finger = p->finger;
  p->out.saturated = p->sat_hold > 0;
  evaluate_spo2(p);
  evaluate_heart_rate(p);
  evaluate_quality(p);
}

// ---- Finger, reset, sample entry point ----------------------------------------------

/*******************************************************************************
 * @brief       Prepara el procesamiento en el instante en que aparece el dedo.
 *
 * @details     Reinicia los cinco filtros y los precarga con la muestra actual
 *              (sos_prime) para que la primera no parezca un escalón desde
 *              cero. Vacía la ventana, carga SETTLE_N muestras de asentamiento
 *              y borra el detector de latidos.
 *
 * @param       Entrada: red, ir - primera muestra con dedo (cuentas del ADC).
 *              Salida:  p - filtros, ventana (w_head, w_count), settle_left y
 *                       beat reiniciados.
 *
 * @peripherals Ninguno.
 *******************************************************************************/
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

/*******************************************************************************
 * @brief       Inicializa el procesador PPG desde cero.
 *
 * @details     Pone todo en cero con memset y deja to_eval = PPG_FS, así la
 *              primera salida llega al cabo de un segundo. También se usa al
 *              reiniciar el sensor.
 *
 * @param       Salida:  p - procesador reiniciado.
 *
 * @peripherals Ninguno.
 *******************************************************************************/
void ppg_init(PpgProcessor *p) {
  memset(p, 0, sizeof(*p));
  p->to_eval = PPG_FS;
}

/*******************************************************************************
 * @brief       Procesa una muestra cruda RED/IR y, una vez por segundo,
 *              refresca la salida.
 *
 * @details     Detecta saturación y dedo (histéresis y antirrebote sobre el IR
 *              crudo), filtra AC y DC de cada canal, acumula AC^2 y DC en la
 *              ventana y alimenta el detector de latidos. Cada PPG_FS muestras
 *              llama a evaluate() y devuelve true.
 *
 * @param       Entrada: red, ir - muestra cruda de 18 bits (cuentas del ADC).
 *              Entrada: p - estado del procesador.
 *              Salida:  retorno (bool) - true cuando se refrescó la salida (una
 *                       vez por segundo).
 *              Salida:  p->out, p->dbg - resultado y valores intermedios de
 *                       esta muestra.
 *
 * @peripherals Ninguno.
 *******************************************************************************/
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
