// The PulsOx application. See app.h for the states. Orchestration only: the sensor driver
// lives in lib/max30102, the signal processing in lib/ppg (the pipeline and the judge of the
// measurement), the screens in src/display/display.cpp, the network in src/cloud/cloud_link.cpp and the
// sleep in src/board/power.cpp.
//
// While it measures it starts the MAX30102 at SENSOR_FS_HZ samples/s, feeds every sample to the
// pipeline (finger, filters, SpO2, heart rate), shows the result and the pulse trace on the
// TFT, sends one `live` tick per second to Firebase, and prints one line per second with the
// result and the sensor health (samples/s, last RED/IR, FIFO overflows, slowest screen
// update). In CSV mode (send 'c') it also streams every sample with its filtered values, and
// the result, for tools/capture.py and test/host/validate_ppg.py.
#include "app/app.h"
#include <Arduino.h>
#include <Wire.h>
#include "board/button.h"
#include "cloud/cloud_link.h"
#include "config.h"
#include "debug/debug_log.h"
#include "display/display.h"
#include "debug/i2c_diag.h"
#include "max30102.h"
#include "board/power.h"
#include "ppg_processor.h"
#include "ppg_session.h"
#include "debug/serial_stream.h"
#include "board/status_led.h"

enum class State : uint8_t { CONNECTING, WAIT_FINGER, MEASURING, CLOSING, RESULT, ERROR_SCREEN };

/*******************************************************************************
 * @brief       Devuelve el nombre en texto de un estado de la aplicación.
 *
 * @details     Para los logs: CONECTANDO, ESPERA_DEDO, MIDIENDO, CERRANDO,
 *              RESULTADO; cualquier otro da ERROR.
 *
 * @param       Entrada: s - estado.
 *              Salida:  retorno (const char *) - nombre del estado.
 *
 * @peripherals Ninguno.
 *******************************************************************************/
static const char *stateName(State s) {
  switch (s) {
    case State::CONNECTING: return "CONECTANDO";
    case State::WAIT_FINGER: return "ESPERA_DEDO";
    case State::MEASURING: return "MIDIENDO";
    case State::CLOSING: return "CERRANDO";
    case State::RESULT: return "RESULTADO";
    default: return "ERROR";
  }
}

static Max30102 sensor;
static PpgProcessor ppg;
static PpgSession session;
static const Max30102Config SENSOR_CFG = {SENSOR_FIFO_CONFIG, SENSOR_SPO2_CONFIG,
                                          SENSOR_LED_RED_PA, SENSOR_LED_IR_PA};

static State state = State::CONNECTING;
static uint32_t stateSince = 0;
static uint32_t awakeSince = 0;      // millis() of the button press that started the flow
static uint32_t buttonPresses = 0;

static bool wireUp = false;
static bool streaming = false;       // the sensor is sampling
static uint8_t i2cFails = 0;
static uint32_t sampleIndex = 0;     // samples since the sensor was started
static uint32_t lastOverflow = 0;
static uint32_t lastCorrupt = 0;
static PpgSample lastSample = {0, 0};
static uint16_t samplesInWindow = 0;
static uint32_t statsAt = 0;
static uint32_t displayWorstUs = 0;  // slowest displayUpdate() since the last LIVE line
static float livePiIr = 0.0f;        // PI and AC (counts) of each channel in the latest pipeline result, for the LIVE line
static float livePiRed = 0.0f;
static float liveAcIr = 0.0f;
static float liveAcRed = 0.0f;

static bool fingerPresent = false;   // the pipeline's verdict on the latest sample
static bool fingerShown = false;     // what the measuring screen says: measuring (true) or "no finger"

static bool closingToResult = false; // CLOSING: it ends in RESULT (true) or ERROR_SCREEN (false)

// One second of the pulse trace for the cloud: the view trace, decimated to CLOUD_PPG_FS.
static_assert(PPG_FS % CLOUD_PPG_FS == 0, "CLOUD_PPG_FS has to divide the sensor rate");
static int16_t tickPpg[CLOUD_PPG_FS];
static uint8_t tickPpgN = 0;
static float decimSum = 0.0f;
static uint8_t decimN = 0;
static uint32_t measStartMs = 0;

/*******************************************************************************
 * @brief       Cambia de estado y anota cuándo ocurrió.
 *
 * @details     Loguea la transición si el estado es otro y guarda millis() en
 *              stateSince, la base de los timeouts de cada estado.
 *
 * @param       Entrada: s - nuevo estado.
 *              Salida:  state, stateSince - estado vigente y su instante de
 *                       entrada.
 *
 * @peripherals Internos: Serial/USB-CDC (log). Externos: Ninguno.
 *******************************************************************************/
static void setState(State s) {
  if (s != state) DBG("APP", "%s -> %s", stateName(state), stateName(s));
  state = s;
  stateSince = millis();
}

/*******************************************************************************
 * @brief       Redondea un float al entero más cercano (valores no negativos).
 *
 * @details     Calcula (int)(v + 0.5f).
 *
 * @param       Entrada: v - valor a redondear.
 *              Salida:  retorno (int) - valor redondeado.
 *
 * @peripherals Ninguno.
 *******************************************************************************/
static int roundi(float v) { return (int)(v + 0.5f); }

// ---- I2C and the sensor --------------------------------------------------------------------------

/*******************************************************************************
 * @brief       Inicia el bus I2C del sensor si todavía no está iniciado.
 *
 * @details     Wire.begin con PIN_SDA_OX y PIN_SCL_OX y reloj I2C_CLOCK_HZ;
 *              wireUp evita repetirlo.
 *
 * @param       Entrada: wireUp - si el bus ya está iniciado.
 *              Salida:  wireUp - pasa a true.
 *
 * @peripherals Internos: I2C (Wire). Externos: MAX30102 (su bus).
 *******************************************************************************/
static void ensureWire() {
  if (wireUp) return;
  Wire.begin(PIN_SDA_OX, PIN_SCL_OX);
  Wire.setClock(I2C_CLOCK_HZ);
  wireUp = true;
}

/*******************************************************************************
 * @brief       Libera el bus I2C (Wire.end), por ejemplo antes de dormir.
 *
 * @details     Si el bus estaba iniciado lo apaga y limpia wireUp.
 *
 * @param       Entrada: wireUp - si el bus está iniciado.
 *              Salida:  wireUp - pasa a false.
 *
 * @peripherals Internos: I2C (Wire). Externos: Ninguno.
 *******************************************************************************/
static void releaseWire() {
  if (!wireUp) return;
  Wire.end();
  wireUp = false;
}

/*******************************************************************************
 * @brief       Arranca el MAX30102 y deja listo el procesamiento para medir.
 *
 * @details     Inicia el bus y hace begin() y configure() del sensor. Si falla
 *              lo loguea y corre i2cDiagnose() una sola vez por racha de
 *              fallos. Si sale bien pone a cero los contadores y reinicia el
 *              procesador PPG.
 *
 * @param       Entrada: SENSOR_CFG - configuración del sensor (FIFO, SPO2,
 *                       corriente de LED).
 *              Salida:  retorno (bool) - true si el sensor quedó muestreando.
 *              Salida:  i2cFails, sampleIndex, samplesInWindow, statsAt -
 *                       contadores reiniciados.
 *              Salida:  lastOverflow, lastCorrupt, fingerPresent - puestos a
 *                       cero / false.
 *              Salida:  ppg - procesador PPG reiniciado.
 *
 * @peripherals Internos: I2C (Wire), Serial/USB-CDC (log). Externos: MAX30102.
 *******************************************************************************/
static bool startSensor() {
  static bool diagnosed = false;  // run the I2C diagnosis once per failure streak, not on every retry
  ensureWire();
  if (!sensor.begin(Wire)) {
    DBG("MAX", "no se pudo iniciar el MAX30102: err=%u (%s)", sensor.lastError(),
        Max30102::errorText(sensor.lastError()));
    if (!diagnosed) {
      diagnosed = true;
      i2cDiagnose();
    }
    return false;
  }
  diagnosed = false;
  if (!sensor.configure(SENSOR_CFG)) {
    DBG("MAX", "no se pudo configurar el MAX30102: err=%u (%s)", sensor.lastError(),
        Max30102::errorText(sensor.lastError()));
    return false;
  }
  i2cFails = 0;
  sampleIndex = 0;
  lastOverflow = 0;
  lastCorrupt = 0;
  samplesInWindow = 0;
  fingerPresent = false;
  ppg_init(&ppg);
  statsAt = millis();
  DBG("MAX", "sensor configurado: %.0f muestras/s, LEDs RED=0x%02X IR=0x%02X", SENSOR_FS_HZ,
      SENSOR_LED_RED_PA, SENSOR_LED_IR_PA);
  return true;
}

/*******************************************************************************
 * @brief       Manda el MAX30102 a shutdown y verifica que quedó apagado.
 *
 * @details     El sensor se alimenta del 3V3 de la placa, no del ESP32:
 *              conserva su estado tras un reset, por eso siempre se lo apaga
 *              explícitamente. begin() lo reinicia antes (le da el bus al
 *              driver) y shutdown() se reintenta hasta 3 veces.
 *
 * @param       Salida:  retorno (bool) - true si quedó en shutdown.
 *              Salida:  streaming - pasa a false.
 *
 * @peripherals Internos: I2C (Wire). Externos: MAX30102.
 *******************************************************************************/
static bool shutdownSensor() {
  ensureWire();
  streaming = false;
  sensor.begin(Wire);  // its result does not matter: shutdown() is what has to succeed
  for (uint8_t i = 0; i < 3; i++) {
    if (sensor.shutdown()) return true;
    delay(100);
  }
  return false;
}

// ---- What the screen shows while measuring -----------------------------------------------------------

/*******************************************************************************
 * @brief       Alimenta la pantalla con cada muestra mientras se mide.
 *
 * @details     La pantalla sigue al dedo muestra a muestra (el pipeline
 *              refresca su resultado una vez por segundo): alterna entre
 *              «midiendo» y «sin dedo» y dibuja la traza del IR filtrado para
 *              pantalla (ir_view), con el latido hacia arriba.
 *
 * @param       Entrada: d - valores intermedios de la última muestra (finger,
 *                       ir_view).
 *              Salida:  fingerShown - lo que dice la pantalla: midiendo o sin
 *                       dedo.
 *
 * @peripherals Internos: Ninguno.
 *              Externos: pantalla TFT (se escribe luego en displayUpdate()).
 *******************************************************************************/
static void feedDisplay(const PpgDebug &d) {
  if (d.finger != fingerShown) {
    fingerShown = d.finger;
    if (fingerShown) {
      displaySetMeasuring(0, 0);  // "--" until the first valid values
    } else {
      displaySetNoFinger();
    }
  }
  if (fingerShown) displayPushPpg(d.ir_view);
}

/*******************************************************************************
 * @brief       Muestra en pantalla el resultado de cada segundo del pipeline.
 *
 * @details     Lo ignora si la pantalla ya dice «sin dedo» (podría ser del dedo
 *              recién retirado). Un valor no válido se pasa como 0 y la
 *              pantalla lo dibuja como "--".
 *
 * @param       Entrada: o - salida del procesador PPG.
 *              Entrada: fingerShown - si la pantalla está en «midiendo».
 *              Salida:  pantalla - SpO2 y BPM redondeados (0 = no válido).
 *
 * @peripherals Internos: Ninguno.
 *              Externos: pantalla TFT (se escribe luego en displayUpdate()).
 *******************************************************************************/
static void showResult(const PpgOutput &o) {
  if (!fingerShown) return;  // the result may still describe the finger that was just removed
  displaySetMeasuring(o.spo2_valid ? roundi(o.spo2) : 0, o.bpm_valid ? roundi(o.bpm) : 0);
}

/*******************************************************************************
 * @brief       Actualiza la pantalla y mide cuánto tardó.
 *
 * @details     Llama a displayUpdate() y guarda en displayWorstUs la mayor
 *              duración desde la última línea LIVE.
 *
 * @param       Salida:  displayWorstUs - peor tiempo de displayUpdate(), en
 *                       microsegundos.
 *
 * @peripherals Internos: SPI (vía displayUpdate()). Externos: pantalla TFT.
 *******************************************************************************/
static void updateDisplay() {
  uint32_t t0 = micros();
  displayUpdate();
  uint32_t dt = micros() - t0;
  if (dt > displayWorstUs) displayWorstUs = dt;
}

/*******************************************************************************
 * @brief       Junta la traza del pulso para la nube, decimada a CLOUD_PPG_FS
 *              muestras/s.
 *
 * @details     Cada muestra con dedo suma ir_view (la misma traza que dibuja la
 *              pantalla); cada PPG_FS / CLOUD_PPG_FS muestras guarda el
 *              promedio, saturado a int16, en tickPpg[].
 *
 * @param       Entrada: d - finger e ir_view de la muestra.
 *              Salida:  tickPpg[], tickPpgN - un segundo de traza decimada.
 *              Salida:  decimSum, decimN - acumulador de la decimación.
 *
 * @peripherals Ninguno.
 *******************************************************************************/
static void collectPpg(const PpgDebug &d) {
  if (!d.finger) return;
  decimSum += d.ir_view;
  if (++decimN < PPG_FS / CLOUD_PPG_FS) return;
  long v = lroundf(decimSum / decimN);
  decimSum = 0.0f;
  decimN = 0;
  if (v > 32767) v = 32767;
  if (v < -32768) v = -32768;
  if (tickPpgN < CLOUD_PPG_FS) tickPpg[tickPpgN++] = (int16_t)v;
}

/*******************************************************************************
 * @brief       Imprime el resultado de cada segundo como línea de log.
 *
 * @details     Sin dedo imprime «sin dedo»; si no, SpO2 y BPM ("--" si no son
 *              válidos), R, índice de perfusión, calidad y SATURADO si
 *              corresponde.
 *
 * @param       Entrada: o - salida del procesador PPG.
 *
 * @peripherals Internos: Serial/USB-CDC. Externos: Ninguno.
 *******************************************************************************/
static void printResult(const PpgOutput &o) {
  if (!o.finger) {
    DBG("PPG", "sin dedo");
    return;
  }
  char spo2[8] = "--", bpm[8] = "--";
  if (o.spo2_valid) snprintf(spo2, sizeof(spo2), "%.0f", o.spo2);
  if (o.bpm_valid) snprintf(bpm, sizeof(bpm), "%.0f", o.bpm);
  DBG("PPG", "SpO2=%s%% BPM=%s | R=%.3f PI=%.2f%% q=%.2f%s", spo2, bpm, o.r_ratio, o.perfusion_index, o.quality,
      o.saturated ? " | SATURADO" : "");
}

// ---- The measurement ------------------------------------------------------------------------------------

/*******************************************************************************
 * @brief       Empieza una medición: reinicia el juez y abre la sesión en la
 *              nube.
 *
 * @details     Reinicia la sesión, el decimado de la traza y la pantalla de
 *              medición. Si la nube está lista (LinkState::READY) pide abrir la
 *              sesión; si no, mide igual sin enviar. Pasa a MEASURING.
 *
 * @param       Entrada: cloudLinkState() - si hay nube lista para enviar.
 *              Salida:  session, measStartMs - sesión nueva y su instante de
 *                       inicio.
 *              Salida:  tickPpgN, decimSum, decimN, fingerShown - reiniciados.
 *              Salida:  state - MEASURING.
 *
 * @peripherals Internos: Serial/USB-CDC (log), bloque WiFi (vía la tarea de la
 *              nube).
 *              Externos: pantalla TFT, nube (Firebase).
 *******************************************************************************/
static void startMeasurement() {
  const bool toCloud = cloudLinkState() == LinkState::READY;
  ppg_session_init(&session);
  measStartMs = millis();
  tickPpgN = 0;
  decimSum = 0.0f;
  decimN = 0;
  fingerShown = true;
  displaySetMeasuring(0, 0);
  displaySetProgress(0);
  DBG("MEAS", "medicion iniciada (%s)", toCloud ? "con nube" : "SIN nube: no se envia");
  if (toCloud) cloudLinkStartSession();
  setState(State::MEASURING);
}

/*******************************************************************************
 * @brief       Cierra una medición buena: cierra la sesión en la nube y apaga
 *              el sensor.
 *
 * @details     Loguea el resultado y, si la sesión está abierta, pide a la nube
 *              cerrarla con SpO2, BPM y calidad. Manda el sensor a shutdown y
 *              pasa a CLOSING para esperar a la nube.
 *
 * @param       Entrada: session - resultado (spo2, bpm, quality, seconds,
 *                       valid_seconds).
 *              Salida:  closingToResult - true: termina en RESULT.
 *              Salida:  state - CLOSING.
 *
 * @peripherals Internos: I2C (Wire), Serial/USB-CDC, bloque WiFi (vía la tarea
 *              de la nube).
 *              Externos: MAX30102, nube (Firebase).
 *******************************************************************************/
static void finishMeasurement() {
  DBG("MEAS", "TERMINADA a los %u s (%u validos): SpO2=%.0f BPM=%.0f calidad=%.2f", session.seconds,
      session.valid_seconds, (double)session.spo2, (double)session.bpm, (double)session.quality);
  if (cloudLinkState() == LinkState::SESSION) cloudLinkFinish(session.spo2, session.bpm, session.quality);
  shutdownSensor();
  closingToResult = true;
  setState(State::CLOSING);
}

/*******************************************************************************
 * @brief       Descarta una medición inválida: borra la sesión de la nube y
 *              apaga el sensor.
 *
 * @details     Loguea el motivo, muestra «sin dedo», pide a la nube borrar la
 *              sesión si estaba abierta, manda el sensor a shutdown y pasa a
 *              CLOSING con destino ERROR_SCREEN.
 *
 * @param       Entrada: why - motivo del descarte, para el log.
 *              Salida:  fingerShown - false.
 *              Salida:  closingToResult - false: termina en ERROR_SCREEN.
 *              Salida:  state - CLOSING.
 *
 * @peripherals Internos: I2C (Wire), Serial/USB-CDC, bloque WiFi (vía la tarea
 *              de la nube).
 *              Externos: MAX30102, pantalla TFT, nube (Firebase).
 *******************************************************************************/
static void discardMeasurement(const char *why) {
  DBG("MEAS", "DESCARTADA a los %u s: %s", session.seconds, why);
  fingerShown = false;
  displaySetNoFinger();
  if (cloudLinkState() == LinkState::SESSION) cloudLinkAbort();
  shutdownSensor();
  closingToResult = false;
  setState(State::CLOSING);
}

/*******************************************************************************
 * @brief       Procesa cada segundo de la medición: tick a la nube, progreso y
 *              veredicto.
 *
 * @details     Pasa la salida al juez (ppg_session_push), actualiza la barra de
 *              progreso y, si sigue midiendo con la sesión abierta, arma y
 *              encola el tick `live` con la traza del segundo. Si el juez dice
 *              DONE o ABORTED, termina o descarta la medición.
 *
 * @param       Entrada: o - salida del procesador PPG del último segundo.
 *              Entrada: session, tickPpg[], measStartMs - juez, traza del
 *                       segundo e inicio.
 *              Salida:  tickPpgN - vuelve a 0: el segundo ya se entregó.
 *              Salida:  state - CLOSING si la medición terminó o se descartó.
 *
 * @peripherals Internos: I2C (Wire), Serial/USB-CDC, bloque WiFi (vía la tarea
 *              de la nube).
 *              Externos: MAX30102, pantalla TFT, nube (Firebase).
 *******************************************************************************/
static void onSecond(const PpgOutput &o) {
  const PpgSessionState verdict = ppg_session_push(&session, &o);
  displaySetProgress(ppg_session_progress(&session));

  if (verdict == PPG_SESSION_RUNNING && cloudLinkState() == LinkState::SESSION) {
    LiveTick t = {};
    t.elapsedMs = millis() - measStartMs;
    t.finger = o.finger;
    t.spo2Valid = o.spo2_valid;
    t.bpmValid = o.bpm_valid;
    t.spo2 = o.spo2;
    t.bpm = o.bpm;
    t.quality = o.quality;
    t.ppgCount = o.finger ? tickPpgN : 0;
    for (uint8_t i = 0; i < t.ppgCount; i++) t.ppg[i] = tickPpg[i];
    cloudLinkPushTick(t);
  }
  tickPpgN = 0;

  if (verdict == PPG_SESSION_DONE) {
    finishMeasurement();
  } else if (verdict == PPG_SESSION_ABORTED) {
    discardMeasurement(ppg_abort_text(session.reason));
  }
}

/*******************************************************************************
 * @brief       Lee el FIFO del sensor y pasa cada muestra por el pipeline.
 *
 * @details     Lee hasta 32 muestras con readFifo(). Cuenta los fallos I2C
 *              seguidos y registra desbordes y muestras corruptas. Cada muestra
 *              va a ppg_push() y al stream; al medir alimenta pantalla y nube,
 *              y cada segundo llama a onSecond().
 *
 * @param       Entrada: state - qué hacer con cada muestra (WAIT_FINGER o
 *                       MEASURING).
 *              Entrada: streamTakeGlitchRequest() - (solo PULSOX_TEST_HOOKS)
 *                       corromper la próxima.
 *              Salida:  i2cFails - fallos de lectura seguidos (0 si salió
 *                       bien).
 *              Salida:  fingerPresent, lastSample, sampleIndex, samplesInWindow
 *                       - última muestra.
 *              Salida:  livePiIr/Red, liveAcIr/Red - PI y AC del último
 *                       segundo, para la línea LIVE.
 *              Salida:  lastOverflow, lastCorrupt - contadores ya informados.
 *
 * @peripherals Internos: I2C (Wire), Serial/USB-CDC, bloque WiFi (vía
 *              onSecond() y la tarea de la nube).
 *              Externos: MAX30102, pantalla TFT, nube (Firebase).
 *******************************************************************************/
static void pollSensor() {
  PpgSample buf[32];
#ifdef PULSOX_TEST_HOOKS
  if (streamTakeGlitchRequest()) sensor.testCorruptNextSample();
#endif
  int n = sensor.readFifo(buf, sizeof(buf) / sizeof(buf[0]));
  if (n < 0) {
    i2cFails++;
    if (i2cFails == 1) {  // log only the first failure of a streak
      DBG("I2C", "fallo al leer el FIFO: err=%u (%s)", sensor.lastError(),
          Max30102::errorText(sensor.lastError()));
    }
    return;
  }
  i2cFails = 0;

  uint32_t overflow = sensor.overflowCount();
  if (overflow != lastOverflow) {
    lastOverflow = overflow;
    streamEvent(sampleIndex, "fifo_overflow", overflow);
    DBG("MAX", "el FIFO desbordo: %lu muestras perdidas en total", (unsigned long)overflow);
  }
  const uint32_t corrupt = sensor.corruptCount();
  if (corrupt != lastCorrupt) {  // the driver already replaced them: this is only to know how often the bus glitches
    lastCorrupt = corrupt;
    streamEvent(sampleIndex, "bad_sample", corrupt);
    DBG("MAX", "muestra corrupta reemplazada por la anterior (lectura I2C basura): %lu en total", (unsigned long)corrupt);
  }

  for (int i = 0; i < n; i++) {
    bool refreshed = ppg_push(&ppg, buf[i].red, buf[i].ir);
    const PpgDebug &d = *ppg_debug(&ppg);
    fingerPresent = d.finger;
    streamSample(sampleIndex, buf[i], d);
    if (state == State::MEASURING) {
      feedDisplay(d);
      collectPpg(d);
    }
    if (refreshed) {
      const PpgOutput &o = *ppg_output(&ppg);
      streamResult(sampleIndex, o);
      livePiIr = o.perfusion_index;
      livePiRed = o.perfusion_index_red;
      liveAcIr = o.ac_rms_ir;
      liveAcRed = o.ac_rms_red;
      printResult(o);
      if (state == State::MEASURING) {
        showResult(o);
        onSecond(o);  // may end the measurement: the state is no longer MEASURING
      }
    }
    sampleIndex++;
    lastSample = buf[i];
    if (state != State::MEASURING && state != State::WAIT_FINGER) break;  // the rest of the FIFO is of no use
  }
  samplesInWindow += n;
}

/*******************************************************************************
 * @brief       Imprime la línea LIVE, una vez por STATS_PERIOD_MS, con la salud
 *              del sensor.
 *
 * @details     Muestras por segundo, últimos RED e IR, PI y AC de cada canal,
 *              desbordes, muestras corruptas, fallos I2C y peor tiempo de
 *              pantalla. Luego pone a cero samplesInWindow y displayWorstUs.
 *
 * @param       Entrada: statsAt - instante del último resumen.
 *              Salida:  statsAt - avanza un período.
 *              Salida:  samplesInWindow, displayWorstUs - vuelven a 0.
 *
 * @peripherals Internos: Serial/USB-CDC. Externos: Ninguno.
 *******************************************************************************/
static void printStats() {
  if (millis() - statsAt < STATS_PERIOD_MS) return;
  statsAt += STATS_PERIOD_MS;
  DBG("LIVE", "%u muestras/s | red=%lu ir=%lu | PI ir=%.3f%% red=%.3f%% | AC ir=%.1f red=%.1f | overflows=%lu corruptas=%lu | fallos I2C=%u | pantalla max %lu us",
      samplesInWindow, (unsigned long)lastSample.red, (unsigned long)lastSample.ir,
      (double)livePiIr, (double)livePiRed, (double)liveAcIr, (double)liveAcRed,
      (unsigned long)sensor.overflowCount(), (unsigned long)sensor.corruptCount(), i2cFails,
      (unsigned long)displayWorstUs);
  samplesInWindow = 0;
  displayWorstUs = 0;
}

// ---- Sleep and the flow ----------------------------------------------------------------------------------

/*******************************************************************************
 * @brief       Espera a que la nube termine lo que se le pidió, con la pantalla
 *              viva.
 *
 * @details     Mientras cloudLinkBusy() y no pase el timeout, llama a
 *              updateDisplay() y espera 10 ms.
 *
 * @param       Entrada: timeoutMs - espera máxima, en milisegundos.
 *
 * @peripherals Internos: SPI (vía displayUpdate()). Externos: pantalla TFT.
 *******************************************************************************/
static void waitCloud(uint32_t timeoutMs) {
  const uint32_t t0 = millis();
  while (cloudLinkBusy() && millis() - t0 < timeoutMs) {
    updateDisplay();
    delay(10);
  }
}

/*******************************************************************************
 * @brief       Arranca el flujo (botón o primer arranque): pantalla
 *              «Conectando» y nube.
 *
 * @details     Anota awakeSince, muestra la barra de estado y la pantalla de
 *              conexión, pide a la nube conectarse en segundo plano y pasa a
 *              CONNECTING. El sensor no arranca todavía: solo importa con la
 *              nube resuelta.
 *
 * @param       Salida:  awakeSince, fingerShown - instante del despertar y
 *                       false.
 *              Salida:  state - CONNECTING.
 *
 * @peripherals Internos: bloque WiFi (vía la tarea de la nube).
 *              Externos: pantalla TFT, nube (Firebase).
 *******************************************************************************/
static void beginFlow() {
  awakeSince = millis();
  fingerShown = false;
  displaySetStatus(false, BATTERY_PLACEHOLDER_PCT);
  displaySetConnecting();
  cloudLinkConnect();  // in the background
  setState(State::CONNECTING);
}

/*******************************************************************************
 * @brief       Deja todo quieto y duerme.
 *
 * @details     Loguea, manda el sensor a shutdown (avisa si falla), libera el
 *              I2C, duerme la pantalla y llama a powerSleep(). Con deep sleep
 *              no vuelve; con light sleep y con el sueño simulado (env dev)
 *              vuelve al apretar el botón.
 *
 * @param       Entrada: awakeSince - para informar cuánto estuvo despierto.
 *
 * @peripherals Internos: I2C (Wire), SPI, GPIO, light/deep sleep,
 *              Serial/USB-CDC.
 *              Externos: MAX30102, pantalla TFT, botón, LED de estado.
 *******************************************************************************/
static void enterSleep() {
  DBG("PWR", "a dormir tras %lu s despierto", (unsigned long)((millis() - awakeSince) / 1000));
  if (shutdownSensor()) {
    DBG("PWR", "MAX30102 en shutdown");
  } else {
    DBG("PWR", "ATENCION: no se pudo apagar el MAX30102: err=%u (%s)", sensor.lastError(),
        Max30102::errorText(sensor.lastError()));
  }
  releaseWire();
  displaySleep();
  powerSleep();
}

/*******************************************************************************
 * @brief       Retoma el flujo al despertar del sueño.
 *
 * @details     Vuelve a empezar desde CONNECTING (beginFlow) y despierta la
 *              pantalla (displayWake). Lo llama goToSleep() al volver de light
 *              sleep o del sueño simulado, y appSetup() al arrancar (también
 *              tras un deep sleep).
 *
 * @param       Salida:  state - CONNECTING.
 *
 * @peripherals Internos: SPI, GPIO (backlight), Serial/USB-CDC, bloque WiFi
 *              (vía la tarea de la nube).
 *              Externos: pantalla TFT, nube (Firebase).
 *******************************************************************************/
static void wakeUp() {
  beginFlow();
  displayWake();
  DBG("WAKE", "despierto");
}

/*******************************************************************************
 * @brief       Cierra la conexión a la nube, duerme y retoma al despertar.
 *
 * @details     Loguea el motivo, desconecta la nube, espera hasta
 *              CLOUD_CLOSE_TIMEOUT_MS a que termine, duerme con enterSleep() y
 *              al volver reinicia el flujo con wakeUp().
 *
 * @param       Entrada: why - motivo de dormir, para el log.
 *
 * @peripherals Internos: I2C (Wire), SPI, GPIO, light/deep sleep,
 *              Serial/USB-CDC, bloque WiFi (vía la tarea de la nube).
 *              Externos: MAX30102, pantalla TFT, botón, LED de estado, nube
 *              (Firebase).
 *******************************************************************************/
static void goToSleep(const char *why) {
  DBG("APP", "a dormir: %s", why);
  cloudLinkDisconnect();
  waitCloud(CLOUD_CLOSE_TIMEOUT_MS);
  enterSleep();
  wakeUp();
}

/*******************************************************************************
 * @brief       Espera BOOT_WINDOW_MS tras un arranque en frío, con la pantalla
 *              en negro.
 *
 * @details     Es el margen para flashear o abrir el monitor antes de dormir.
 *              Mientras espera hace parpadear el LED de estado, atiende
 *              comandos seriales y mira el botón.
 *
 * @param       Salida:  retorno (bool) - true si se apretó el botón durante la
 *                       espera.
 *
 * @peripherals Internos: GPIO, Serial/USB-CDC. Externos: botón, LED de estado.
 *******************************************************************************/
static bool bootWindow() {
  DBG("BOOT", "arranque en frio: %d ms para flashear o abrir el monitor; el boton inicia", BOOT_WINDOW_MS);
  while (millis() < BOOT_WINDOW_MS) {
    if (buttonPressed()) return true;
    statusLedUpdate(false);
    streamPollCommands();
    delay(10);
  }
  return false;
}

// ---- The state machine ------------------------------------------------------------------------------------

/*******************************************************************************
 * @brief       Atiende una pulsación del botón según el estado.
 *
 * @details     En RESULT o ERROR_SCREEN empieza una medición nueva (beginFlow);
 *              en cualquier otro estado la ignora, porque hay una medición en
 *              curso.
 *
 * @param       Entrada: state - estado de la aplicación.
 *              Salida:  buttonPresses - cuenta una pulsación más.
 *
 * @peripherals Internos: Serial/USB-CDC, bloque WiFi (vía la tarea de la nube)
 *              (vía beginFlow()).
 *              Externos: botón (su pulsación), pantalla TFT, nube (Firebase).
 *******************************************************************************/
static void onButton() {
  buttonPresses++;
  if (state == State::RESULT || state == State::ERROR_SCREEN) {
    DBG("BTN", "pulsacion #%lu: nueva medicion", (unsigned long)buttonPresses);
    beginFlow();
  } else {
    DBG("BTN", "pulsacion #%lu (ignorada: hay una medicion en curso)", (unsigned long)buttonPresses);
  }
}

/*******************************************************************************
 * @brief       Avanza la máquina de estados de la aplicación.
 *
 * @details     CONNECTING pasa a WAIT_FINGER con la nube lista, sin nube o a
 *              CONNECT_MAX_MS, y arranca el sensor. WAIT_FINGER mide con dedo o
 *              duerme tras FINGER_WAIT_S. CLOSING espera a la nube y pasa a
 *              RESULT o ERROR_SCREEN, que duermen luego.
 *
 * @param       Entrada: state, stateSince - estado vigente y desde cuándo.
 *              Entrada: cloudLinkState(), fingerPresent, closingToResult,
 *                       session - datos que deciden la transición.
 *              Salida:  state - nuevo estado.
 *              Salida:  streaming - true si el sensor arrancó.
 *
 * @peripherals Internos: I2C (Wire), SPI, GPIO, light/deep sleep,
 *              Serial/USB-CDC, bloque WiFi (vía la tarea de la nube).
 *              Externos: MAX30102, pantalla TFT, botón, LED de estado, nube
 *              (Firebase).
 *******************************************************************************/
static void updateState() {
  const uint32_t now = millis();
  switch (state) {
    case State::CONNECTING: {
      const LinkState link = cloudLinkState();
      if (link == LinkState::READY || link == LinkState::OFFLINE || now - stateSince >= CONNECT_MAX_MS) {
        displaySetWaitFinger();
        streaming = startSensor();
        if (!streaming) DBG("APP", "sin sensor: reintento cada %d ms", SENSOR_RETRY_MS);
        setState(State::WAIT_FINGER);
      }
      break;
    }
    case State::WAIT_FINGER:
      if (streaming && fingerPresent) {
        startMeasurement();
      } else if (now - stateSince >= FINGER_WAIT_S * 1000UL) {
        goToSleep("nadie puso el dedo");
      }
      break;
    case State::MEASURING:
      break;  // driven by the sensor: onSecond()
    case State::CLOSING: {
      const bool timedOut = now - stateSince >= CLOUD_CLOSE_TIMEOUT_MS;
      if (cloudLinkBusy() && !timedOut) break;
      const bool sent = cloudLinkSent();
      cloudLinkDisconnect();  // in the background: the result is already on its way
      if (closingToResult) {
        DBG("MEAS", "resultado %s", sent ? "enviado a la nube" : "NO enviado");
        displaySetResult(roundi(session.spo2), roundi(session.bpm), sent);
        setState(State::RESULT);
      } else {
        setState(State::ERROR_SCREEN);
      }
      break;
    }
    case State::RESULT:
      if (now - stateSince >= RESULT_SHOW_S * 1000UL) goToSleep("resultado mostrado");
      break;
    case State::ERROR_SCREEN:
      if (now - stateSince >= ERROR_SHOW_S * 1000UL) goToSleep("error mostrado");
      break;
  }
}

/*******************************************************************************
 * @brief       Arranque del firmware: inicia los módulos y decide si duerme o
 *              empieza a medir.
 *
 * @details     Libera los pines de un deep sleep, inicia LED, Serial, pantalla,
 *              botón e I2C y crea la tarea de la nube (de menor prioridad que
 *              el loop). Si despertó por el temporizador o fue un arranque en
 *              frío sin botón, duerme; si no, sigue con wakeUp().
 *
 * @param       Entrada: powerWakeReason() - por qué arrancó el chip.
 *              Salida:  Serial, pantalla, botón, I2C - iniciados.
 *
 * @peripherals Internos: GPIO, I2C (Wire), SPI, Serial/USB-CDC, tareas
 *              FreeRTOS, bloque WiFi (vía la tarea de la nube), light/deep
 *              sleep.
 *              Externos: MAX30102, pantalla TFT, botón, LED de estado, nube
 *              (Firebase).
 *******************************************************************************/
void appSetup() {
  powerBegin();  // first: release the pins a deep sleep froze
  const Wake wake = powerWakeReason();
  const bool cold = wake == Wake::COLD_BOOT;
  statusLedBegin(cold);

  Serial.begin(115200);
  const uint32_t waitMs = cold ? SERIAL_WAIT_MS : SERIAL_WAIT_ON_WAKE_MS;
  uint32_t start = millis();
  while (!Serial && millis() - start < waitMs) delay(10);  // USB-CDC: wait for the monitor
  if (cold) delay(300);
  streamBegin();

  Serial.println();
  Serial.println("=== PulsOx firmware ===");
  printBootInfo();
  DBG("BOOT", "despierta por: %s", powerWakeText(wake));

  if (displayBegin()) {
    DBG("BOOT", "pantalla lista (buffer de 32 KB)");
  } else {
    DBG("BOOT", "no hay RAM para el buffer de la pantalla (32 KB): sigue sin pantalla");
  }

  buttonBegin();
  pinMode(PIN_INT_OX, INPUT_PULLUP);
  i2cPreCheck();
  ensureWire();

  // The cloud runs in a task of its own at a lower priority than this one: its TLS work only
  // gets the time the sensor polling and the screen leave free (loop() always ends in a
  // delay, so it does get some), and can never make the FIFO overflow.
  cloudLinkBegin();
  vTaskPrioritySet(nullptr, 2);

  if (wake == Wake::TIMER || (cold && !bootWindow())) {
    // The timer of a stuck button (deep sleep) is not a press: look again. Or a cold boot that nobody pressed.
    enterSleep();  // deep sleep: never returns; the others return on a press
  }
  wakeUp();
}

/*******************************************************************************
 * @brief       Una vuelta del loop: atiende botón, LED, comandos, pantalla,
 *              estado y sensor.
 *
 * @details     Solo en WAIT_FINGER y MEASURING lee el sensor (pollSensor):
 *              reintenta arrancarlo si no responde y lo da por perdido tras
 *              I2C_FAILS_BEFORE_LOST fallos. Siempre termina en un delay: la
 *              tarea de la nube corre con el tiempo que deja libre.
 *
 * @param       Entrada: state, streaming, i2cFails - estado de la app y salud
 *                       del sensor.
 *              Salida:  streaming - false si el sensor dejó de responder.
 *              Salida:  state - puede cambiar (máquina de estados).
 *
 * @peripherals Internos: GPIO, I2C (Wire), SPI, Serial/USB-CDC, bloque WiFi
 *              (vía la tarea de la nube), light/deep sleep.
 *              Externos: MAX30102, pantalla TFT, botón, LED de estado, nube
 *              (Firebase).
 *******************************************************************************/
void appLoop() {
  if (buttonPressed()) onButton();

  statusLedUpdate(streaming);
  streamPollCommands();
  updateDisplay();
  const LinkState link = cloudLinkState();
  displaySetStatus(link == LinkState::READY || link == LinkState::SESSION || link == LinkState::CLOSING,
                   BATTERY_PLACEHOLDER_PCT);
  updateState();

  // The sensor only runs while waiting for the finger and measuring. loop() always ends in a delay:
  // the cloud task runs on the time it leaves free.
  if (state != State::WAIT_FINGER && state != State::MEASURING) {
    delay(POLL_INTERVAL_MS);
    return;
  }
  if (!streaming) {
    static uint32_t lastTry = 0;
    if (state == State::MEASURING) {
      discardMeasurement("el sensor dejo de responder");
    } else if (millis() - lastTry >= SENSOR_RETRY_MS) {
      lastTry = millis();
      streaming = startSensor();
    }
    delay(POLL_INTERVAL_MS);
    return;
  }

  pollSensor();
  if (i2cFails >= I2C_FAILS_BEFORE_LOST) {
    DBG("I2C", "el MAX30102 dejo de responder (%u fallos seguidos)", i2cFails);
    streaming = false;
    return;
  }
  printStats();
  delay(POLL_INTERVAL_MS);
}
