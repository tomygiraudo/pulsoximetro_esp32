#include "ppg_session.h"
#include <string.h>

#define W MEAS_RESULT_WINDOW_S

/*******************************************************************************
 * @brief       Calcula la mediana de v[0..n), ordenando v en el lugar.
 *
 * @details     Ordena por inserción y toma el elemento del medio (o el promedio
 *              de los dos centrales). Requiere n > 0.
 *
 * @param       Entrada: n - cantidad de elementos.
 *              Salida:  v - queda ordenado de menor a mayor.
 *              Salida:  retorno (float) - la mediana.
 *
 * @peripherals Ninguno.
 *******************************************************************************/
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

/*******************************************************************************
 * @brief       Deja una sesión de medición lista para empezar.
 *
 * @details     Pone todo en cero con memset: estado RUNNING, contadores y
 *              ventana vacíos.
 *
 * @param       Salida:  s - sesión reiniciada.
 *
 * @peripherals Ninguno.
 *******************************************************************************/
void ppg_session_init(PpgSession *s) { memset(s, 0, sizeof(*s)); }

/*******************************************************************************
 * @brief       Marca la sesión como descartada y guarda el motivo.
 *
 * @details     Pasa el estado a PPG_SESSION_ABORTED y anota la razón.
 *
 * @param       Entrada: r - motivo del descarte.
 *              Salida:  s->state, s->reason - ABORTED y el motivo.
 *
 * @peripherals Ninguno.
 *******************************************************************************/
static void abort_with(PpgSession *s, PpgAbortReason r) {
  s->state = PPG_SESSION_ABORTED;
  s->reason = r;
}

/*******************************************************************************
 * @brief       Cuenta los segundos buenos de la ventana y, si se pide, calcula
 *              sus medianas.
 *
 * @details     Recorre el anillo de los últimos MEAS_RESULT_WINDOW_S segundos,
 *              junta SpO2, BPM y calidad de los marcados como buenos y saca la
 *              mediana de cada uno. Con spo2 nulo solo cuenta.
 *
 * @param       Entrada: s - sesión (ventana w_* y w_count).
 *              Salida:  spo2, bpm, quality - medianas de los segundos buenos
 *                       (NULL: solo cuenta).
 *              Salida:  retorno (int) - cantidad de segundos buenos en la
 *                       ventana.
 *
 * @peripherals Ninguno.
 *******************************************************************************/
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

/*******************************************************************************
 * @brief       Juzga la medición con la salida de un segundo del procesador
 *              PPG.
 *
 * @details     Aborta si falta el dedo, hay saturación o la señal es errática.
 *              Pasado MEAS_WARMUP_S puntúa cada segundo (bueno o malo). Termina
 *              con la mediana de los buenos al cumplir MEAS_MIN_S y los mínimos
 *              de validez; descarta a MEAS_MAX_S.
 *
 * @param       Entrada: o - salida del procesador PPG de este segundo.
 *              Entrada: s - sesión en curso.
 *              Salida:  retorno (PpgSessionState) - RUNNING, DONE o ABORTED.
 *              Salida:  s - contadores, ventana y, al terminar,
 *                       spo2/bpm/quality o reason.
 *
 * @peripherals Ninguno.
 *******************************************************************************/
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

/*******************************************************************************
 * @brief       Calcula el avance de la medición, de 0 a 100 %.
 *
 * @details     seconds * 100 / MEAS_MIN_S con tope 100: llega al 100 % cuando
 *              la medición puede terminar por primera vez.
 *
 * @param       Entrada: s->seconds - segundos procesados.
 *              Salida:  retorno (int) - porcentaje de avance.
 *
 * @peripherals Ninguno.
 *******************************************************************************/
int ppg_session_progress(const PpgSession *s) {
  int pct = s->seconds * 100 / MEAS_MIN_S;
  return pct > 100 ? 100 : pct;
}

/*******************************************************************************
 * @brief       Devuelve el motivo de descarte como texto corto en español.
 *
 * @details     Para los logs: sin tildes. Un valor desconocido da "ninguno".
 *
 * @param       Entrada: r - motivo del descarte.
 *              Salida:  retorno (const char *) - texto del motivo.
 *
 * @peripherals Ninguno.
 *******************************************************************************/
const char *ppg_abort_text(PpgAbortReason r) {
  switch (r) {
    case PPG_ABORT_FINGER_LOST: return "dedo retirado";
    case PPG_ABORT_SATURATED: return "sensor saturado";
    case PPG_ABORT_ERRATIC: return "senal erratica";
    case PPG_ABORT_INSUFFICIENT: return "lecturas insuficientes";
    default: return "ninguno";
  }
}
