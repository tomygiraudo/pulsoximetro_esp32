// The PulsOx application. See app.h for the states. Orchestration only: the sensor driver
// lives in lib/max30102, the signal processing in lib/ppg (the pipeline and the judge of the
// measurement), the screens in src/display.cpp, the network in src/cloud_link.cpp and the
// sleep in src/power.cpp.
//
// While it measures it starts the MAX30102 at 100 samples/s, feeds every sample to the
// pipeline (finger, filters, SpO2, heart rate), shows the result and the pulse trace on the
// TFT, sends one `live` tick per second to Firebase, and prints one line per second with the
// result and the sensor health (samples/s, last RED/IR, FIFO overflows, slowest screen
// update). In CSV mode (send 'c') it also streams every sample with its filtered values, and
// the result, for tools/capture.py and tools/validate_ppg.py.
#include "app.h"
#include <Arduino.h>
#include <Wire.h>
#include "button.h"
#include "cloud_link.h"
#include "config.h"
#include "debug_log.h"
#include "display.h"
#include "i2c_diag.h"
#include "max30102.h"
#include "power.h"
#include "ppg_processor.h"
#include "ppg_session.h"
#include "serial_stream.h"
#include "status_led.h"

enum class State : uint8_t { CONNECTING, WAIT_FINGER, MEASURING, CLOSING, RESULT, ERROR_SCREEN };

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

static bool fingerPresent = false;   // the pipeline's verdict on the latest sample
static bool fingerShown = false;     // what the measuring screen says: measuring (true) or "no finger"

static bool closingToResult = false; // CLOSING: it ends in RESULT (true) or ERROR_SCREEN (false)

// One second of the pulse trace for the cloud: the 100 sps view trace averaged in pairs.
static_assert(PPG_FS % CLOUD_PPG_FS == 0, "CLOUD_PPG_FS has to divide the sensor rate");
static int16_t tickPpg[CLOUD_PPG_FS];
static uint8_t tickPpgN = 0;
static float decimSum = 0.0f;
static uint8_t decimN = 0;
static uint32_t measStartMs = 0;

static void setState(State s) {
  if (s != state) DBG("APP", "%s -> %s", stateName(state), stateName(s));
  state = s;
  stateSince = millis();
}

static int roundi(float v) { return (int)(v + 0.5f); }

// ---- I2C and the sensor --------------------------------------------------------------------------

static void ensureWire() {
  if (wireUp) return;
  Wire.begin(PIN_SDA_OX, PIN_SCL_OX);
  Wire.setClock(I2C_CLOCK_HZ);
  wireUp = true;
}

static void releaseWire() {
  if (!wireUp) return;
  Wire.end();
  wireUp = false;
}

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

// The sensor is powered from the PCB's 3V3 rail, not by the ESP32: it keeps its state across
// an ESP32 reset, so it is always sent to shutdown explicitly. begin() soft-resets it first
// (also what makes shutdown() usable: it gives the driver the bus).
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

// Every sample, right after ppg_push(). The screen follows the finger sample by sample (the
// pipeline only refreshes its result once per second) and draws the trace of the IR through
// the wide display filter (ir_view), which has the heartbeat pointing up and keeps the dicrotic
// notch that the measuring filter removes.
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

// Once per second, with the pipeline result. Invalid values show as "--".
static void showResult(const PpgOutput &o) {
  if (!fingerShown) return;  // the result may still describe the finger that was just removed
  displaySetMeasuring(o.spo2_valid ? roundi(o.spo2) : 0, o.bpm_valid ? roundi(o.bpm) : 0);
}

static void updateDisplay() {
  uint32_t t0 = micros();
  displayUpdate();
  uint32_t dt = micros() - t0;
  if (dt > displayWorstUs) displayWorstUs = dt;
}

// Every sample: the cloud gets the same trace the screen draws (ir_view), at CLOUD_PPG_FS.
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

// The once-per-second result of the pipeline, as a log line.
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

// The judge says it is over and good: the session is closed in the cloud and the sensor rests.
static void finishMeasurement() {
  DBG("MEAS", "TERMINADA a los %u s (%u validos): SpO2=%.0f BPM=%.0f calidad=%.2f", session.seconds,
      session.valid_seconds, (double)session.spo2, (double)session.bpm, (double)session.quality);
  if (cloudLinkState() == LinkState::SESSION) cloudLinkFinish(session.spo2, session.bpm, session.quality);
  shutdownSensor();
  closingToResult = true;
  setState(State::CLOSING);
}

// Invalid (finger away, erratic signal...): the session is deleted from the cloud, "no finger".
static void discardMeasurement(const char *why) {
  DBG("MEAS", "DESCARTADA a los %u s: %s", session.seconds, why);
  fingerShown = false;
  displaySetNoFinger();
  if (cloudLinkState() == LinkState::SESSION) cloudLinkAbort();
  shutdownSensor();
  closingToResult = false;
  setState(State::CLOSING);
}

// Once per second while measuring: the tick for the cloud, the progress bar, the verdict.
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

static void printStats() {
  if (millis() - statsAt < STATS_PERIOD_MS) return;
  statsAt += STATS_PERIOD_MS;
  DBG("LIVE", "%u muestras/s | red=%lu ir=%lu | overflows=%lu corruptas=%lu | fallos I2C=%u | pantalla max %lu us",
      samplesInWindow, (unsigned long)lastSample.red, (unsigned long)lastSample.ir,
      (unsigned long)sensor.overflowCount(), (unsigned long)sensor.corruptCount(), i2cFails,
      (unsigned long)displayWorstUs);
  samplesInWindow = 0;
  displayWorstUs = 0;
}

// ---- Sleep and the flow ----------------------------------------------------------------------------------

// Waits (keeping the screen alive) until the cloud has finished what it was told to do.
static void waitCloud(uint32_t timeoutMs) {
  const uint32_t t0 = millis();
  while (cloudLinkBusy() && millis() - t0 < timeoutMs) {
    updateDisplay();
    delay(10);
  }
}

// From the button (or the first boot): everything starts from the "connecting" screen. The
// sensor is not started yet: it only matters once the cloud is settled.
static void beginFlow() {
  awakeSince = millis();
  fingerShown = false;
  displaySetStatus(false, BATTERY_PLACEHOLDER_PCT);
  displaySetConnecting();
  cloudLinkConnect();  // in the background
  setState(State::CONNECTING);
}

// Everything quiet, then sleep. With deep sleep it does not return; in the simulated sleep
// (env dev) it returns when the button is pressed.
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

// Back from the simulated sleep (env dev); with deep sleep, setup() runs this after a reboot.
static void wakeUp() {
  beginFlow();
  displayWake();
  DBG("WAKE", "despierto");
}

static void goToSleep(const char *why) {
  DBG("APP", "a dormir: %s", why);
  cloudLinkDisconnect();
  waitCloud(CLOUD_CLOSE_TIMEOUT_MS);
  enterSleep();
  wakeUp();
}

// Waits out BOOT_WINDOW_MS after a cold boot (time to flash or open the monitor), on a black screen.
// True if the button was pressed meanwhile.
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

static void onButton() {
  buttonPresses++;
  if (state == State::RESULT || state == State::ERROR_SCREEN) {
    DBG("BTN", "pulsacion #%lu: nueva medicion", (unsigned long)buttonPresses);
    beginFlow();
  } else {
    DBG("BTN", "pulsacion #%lu (ignorada: hay una medicion en curso)", (unsigned long)buttonPresses);
  }
}

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

  if (cold && !bootWindow()) {
    enterSleep();  // deep sleep: never returns; the simulated one returns on a press
  }
  wakeUp();
}

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
