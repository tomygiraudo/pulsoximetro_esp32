// PulsOx firmware entry point. Orchestration only: the sensor driver lives in
// lib/max30102, the signal processing in lib/ppg, the screens in src/display.cpp.
//
// Current stage: sensor -> signal processing -> TFT -> cloud, with the button and deep
// sleep. The device lives asleep (ESP32 in deep sleep, MAX30102 in shutdown, TFT in
// sleep-in); the button wakes it. While awake it connects to the cloud (src/cloud_link.cpp,
// a task of its own), starts the MAX30102 at 100 samples/s, feeds every sample to the
// pipeline (finger, filters, SpO2, heart rate), shows the result and the pulse trace on the
// TFT, sends one `live` tick per second to Firebase while it measures, and prints one line
// per second with the result and the sensor health (samples/s, last RED/IR, FIFO overflows,
// slowest screen update). In CSV mode (send 'c') it also streams every sample with its
// filtered values, and the result, for tools/capture.py and tools/validate_ppg.py.
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
#include "serial_stream.h"
#include "status_led.h"

static Max30102 sensor;
static PpgProcessor ppg;
static const Max30102Config SENSOR_CFG = {SENSOR_FIFO_CONFIG, SENSOR_SPO2_CONFIG,
                                          SENSOR_LED_RED_PA, SENSOR_LED_IR_PA};

static bool streaming = false;
static uint8_t i2cFails = 0;

static uint32_t sampleIndex = 0;     // samples since the sensor was started
static uint32_t lastOverflow = 0;
static PpgSample lastSample = {0, 0};
static uint16_t samplesInWindow = 0;
static uint32_t statsAt = 0;

static bool fingerShown = false;  // what the screen says: measuring (true) or "no finger"
static uint32_t displayWorstUs = 0;  // slowest displayUpdate() since the last LIVE line

static uint32_t awakeSince = 0;   // millis() of the last wakeUp()
static uint32_t buttonPresses = 0;

// ---- The measurement (PROVISIONAL, stage 2: until the state machine of stage 5) ------------
// WAIT_FINGER until the cloud is ready (or has failed) and a finger is on; then RUNNING for
// MEAS_MAX_S, with a tick to the cloud every second; the end (or the finger leaving) closes
// the session and goes back to sleep.
enum class Meas : uint8_t { WAIT_FINGER, RUNNING };
static Meas meas = Meas::WAIT_FINGER;
static uint32_t measStartMs = 0;
static uint32_t linkResolvedAt = 0;  // millis() when the cloud left CONNECTING, 0 = not yet
static uint32_t fingerLostAt = 0;    // millis() when the finger went away during RUNNING, 0 = present
static PpgOutput lastOutput = {};    // the latest once-per-second result

// One second of the pulse trace for the cloud: the 100 sps view trace averaged in pairs.
static_assert(PPG_FS % CLOUD_PPG_FS == 0, "CLOUD_PPG_FS has to divide the sensor rate");
static int16_t tickPpg[CLOUD_PPG_FS];
static uint8_t tickPpgN = 0;
static float decimSum = 0.0f;
static uint8_t decimN = 0;

// The sensor was (re)started or lost: nothing to measure, so the "no finger" screen. It is
// the only error screen the design has; it also covers a missing sensor.
static void showNoFinger() {
  fingerShown = false;
  displaySetNoFinger();
}

// Every sample, right after ppg_push(). The screen follows the finger sample by sample
// (the pipeline only refreshes its result once per second) and draws the trace of the IR
// through the wide display filter (ir_view), which has the heartbeat pointing up and keeps
// the dicrotic notch that the measuring filter removes.
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
  displaySetMeasuring(o.spo2_valid ? (int)(o.spo2 + 0.5f) : 0, o.bpm_valid ? (int)(o.bpm + 0.5f) : 0);
}

static void updateDisplay() {
  uint32_t t0 = micros();
  displayUpdate();
  uint32_t dt = micros() - t0;
  if (dt > displayWorstUs) displayWorstUs = dt;
}

static bool startSensor() {
  static bool diagnosed = false;  // run the I2C diagnosis once per failure streak, not on every retry
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
  samplesInWindow = 0;
  ppg_init(&ppg);
  showNoFinger();
  statsAt = millis();
  DBG("MAX", "sensor configurado: %.0f muestras/s, LEDs RED=0x%02X IR=0x%02X", SENSOR_FS_HZ,
      SENSOR_LED_RED_PA, SENSOR_LED_IR_PA);
  return true;
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
  DBG("PPG", "SpO2=%s%% BPM=%s | R=%.3f PI=%.2f%%%s", spo2, bpm, o.r_ratio, o.perfusion_index,
      o.saturated ? " | SATURADO" : "");
}

// Every sample: the cloud gets the same trace the screen draws (ir_view), at CLOUD_PPG_FS.
static void collectPpg(const PpgDebug &d) {
  if (meas != Meas::RUNNING || !d.finger) return;
  decimSum += d.ir_view;
  if (++decimN < PPG_FS / CLOUD_PPG_FS) return;
  long v = lroundf(decimSum / decimN);
  decimSum = 0.0f;
  decimN = 0;
  if (v > 32767) v = 32767;
  if (v < -32768) v = -32768;
  if (tickPpgN < CLOUD_PPG_FS) tickPpg[tickPpgN++] = (int16_t)v;
}

// Once per second, with the pipeline result: the tick for the cloud.
static void onSecond(const PpgOutput &o) {
  lastOutput = o;
  if (meas == Meas::RUNNING && cloudLinkState() == LinkState::SESSION) {
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
}

static void pollSensor() {
  PpgSample buf[32];
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

  for (int i = 0; i < n; i++) {
    bool refreshed = ppg_push(&ppg, buf[i].red, buf[i].ir);
    streamSample(sampleIndex, buf[i], *ppg_debug(&ppg));
    feedDisplay(*ppg_debug(&ppg));
    collectPpg(*ppg_debug(&ppg));
    if (refreshed) {
      streamResult(sampleIndex, *ppg_output(&ppg));
      printResult(*ppg_output(&ppg));
      showResult(*ppg_output(&ppg));
      onSecond(*ppg_output(&ppg));
    }
    sampleIndex++;
    lastSample = buf[i];
  }
  samplesInWindow += n;
}

static void printStats() {
  if (millis() - statsAt < STATS_PERIOD_MS) return;
  statsAt += STATS_PERIOD_MS;
  DBG("LIVE", "%u muestras/s | red=%lu ir=%lu | overflows=%lu | fallos I2C=%u | pantalla max %lu us",
      samplesInWindow, (unsigned long)lastSample.red, (unsigned long)lastSample.ir,
      (unsigned long)sensor.overflowCount(), i2cFails, (unsigned long)displayWorstUs);
  samplesInWindow = 0;
  displayWorstUs = 0;
}

// The sensor is powered from the PCB's 3V3 rail, not by the ESP32: it keeps its state across
// an ESP32 reset, so it is always sent to shutdown explicitly. begin() soft-resets it first
// (also what makes shutdown() usable: it gives the driver the bus).
static bool shutdownSensor() {
  sensor.begin(Wire);  // its result does not matter: shutdown() is what has to succeed
  for (uint8_t i = 0; i < 3; i++) {
    if (sensor.shutdown()) return true;
    delay(100);
  }
  return false;
}

// Everything quiet, then sleep. With deep sleep it does not return; in the simulated sleep
// (env dev) it returns when the button is pressed.
static void enterSleep() {
  DBG("PWR", "a dormir tras %lu s despierto", (unsigned long)((millis() - awakeSince) / 1000));
  streaming = false;
  if (shutdownSensor()) {
    DBG("PWR", "MAX30102 en shutdown");
  } else {
    DBG("PWR", "ATENCION: no se pudo apagar el MAX30102: err=%u (%s)", sensor.lastError(),
        Max30102::errorText(sensor.lastError()));
  }
  Wire.end();
  displaySleep();
  powerSleep();
}

// Brings the screen and the sensor up for a measurement. The screen first and the sensor last:
// waking the panel blocks for ~150 ms, and a sensor that is already sampling would overflow its
// 320 ms FIFO before the first poll. The screen wakes straight into "no finger", not into the
// one it went to sleep on.
static void wakeUp() {
  awakeSince = millis();
  meas = Meas::WAIT_FINGER;
  linkResolvedAt = 0;
  fingerLostAt = 0;
  cloudLinkConnect();  // in the background, while the sensor starts
  showNoFinger();
  displayWake();
  Wire.begin(PIN_SDA_OX, PIN_SCL_OX);
  Wire.setClock(I2C_CLOCK_HZ);
  streaming = startSensor();
  if (!streaming) DBG("WAKE", "sin sensor: reintento cada %d ms", SENSOR_RETRY_MS);
  DBG("WAKE", "despierto: sensor %s", streaming ? "listo" : "NO responde");
}

// Waits (keeping the screen alive) until the cloud has finished what it was told to do.
static void waitCloud(uint32_t timeoutMs) {
  const uint32_t t0 = millis();
  while (cloudLinkBusy() && millis() - t0 < timeoutMs) {
    updateDisplay();
    delay(10);
  }
}

// The measurement is over: close (or discard) the session, switch the WiFi off and sleep.
// PROVISIONAL: stage 5 shows the result for 2 minutes (or the error screen) before this.
static void endMeasurement(bool discard) {
  if (cloudLinkState() == LinkState::SESSION) {
    if (discard) {
      cloudLinkAbort();
    } else {
      cloudLinkFinish(lastOutput.spo2, lastOutput.bpm, lastOutput.quality);
    }
  }
  waitCloud(CLOUD_CLOSE_TIMEOUT_MS);
  const bool sent = cloudLinkSent();
  cloudLinkDisconnect();
  waitCloud(2000);
  DBG("MEAS", "medicion %s tras %lu s: SpO2=%.0f BPM=%.0f calidad=%.2f | %s", discard ? "DESCARTADA" : "terminada",
      (unsigned long)((millis() - measStartMs) / 1000), (double)lastOutput.spo2, (double)lastOutput.bpm,
      (double)lastOutput.quality, sent ? "enviada a la nube" : "NO enviada");
  enterSleep();
  wakeUp();
}

static void startMeasurement(bool toCloud) {
  meas = Meas::RUNNING;
  measStartMs = millis();
  fingerLostAt = 0;
  tickPpgN = 0;
  decimSum = 0.0f;
  decimN = 0;
  DBG("MEAS", "medicion iniciada (%s)", toCloud ? "con nube" : "SIN nube: no se envia");
  if (toCloud) cloudLinkStartSession();
}

static void updateMeasurement() {
  const LinkState link = cloudLinkState();
  const bool linkResolved = link == LinkState::READY || link == LinkState::OFFLINE || link == LinkState::SESSION;
  if (linkResolved && linkResolvedAt == 0) linkResolvedAt = millis();
  const uint32_t now = millis();

  if (meas == Meas::WAIT_FINGER) {
    if (fingerShown && linkResolved) {
      startMeasurement(link != LinkState::OFFLINE);
    } else if ((linkResolvedAt != 0 && now - linkResolvedAt >= FINGER_WAIT_S * 1000UL) ||
               now - awakeSince >= CONNECT_MAX_MS + FINGER_WAIT_S * 1000UL) {
      DBG("MEAS", "nadie puso el dedo en %d s", FINGER_WAIT_S);
      measStartMs = now;
      endMeasurement(true);
    }
    return;
  }

  // RUNNING
  if (fingerShown) {
    fingerLostAt = 0;
  } else if (fingerLostAt == 0) {
    fingerLostAt = now;
  } else if (now - fingerLostAt >= MEAS_FINGER_LOST_S * 1000UL) {
    DBG("MEAS", "el dedo se retiro");
    endMeasurement(true);
    return;
  }
  if (now - measStartMs >= MEAS_MAX_S * 1000UL) endMeasurement(false);
}

// Waits out BOOT_WINDOW_MS after a cold boot (time to flash or open the monitor) with the
// "no finger" screen up. True if the button was pressed meanwhile.
static bool bootWindow() {
  DBG("BOOT", "arranque en frio: %d ms para flashear o abrir el monitor; el boton inicia", BOOT_WINDOW_MS);
  while (millis() < BOOT_WINDOW_MS) {
    if (buttonPressed()) return true;
    statusLedUpdate(false);
    streamPollCommands();
    updateDisplay();
    delay(10);
  }
  return false;
}

void setup() {
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
    displaySetStatus(false, BATTERY_PLACEHOLDER_PCT);
    DBG("BOOT", "pantalla lista (buffer de 32 KB)");
  } else {
    DBG("BOOT", "no hay RAM para el buffer de la pantalla (32 KB): sigue sin pantalla");
  }

  buttonBegin();
  pinMode(PIN_INT_OX, INPUT_PULLUP);
  i2cPreCheck();
  Wire.begin(PIN_SDA_OX, PIN_SCL_OX);
  Wire.setClock(I2C_CLOCK_HZ);

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

void loop() {
  if (buttonPressed()) {
    buttonPresses++;
    DBG("BTN", "pulsacion #%lu%s", (unsigned long)buttonPresses,
        meas == Meas::RUNNING ? " (ignorada: hay una medicion en curso)" : "");
  }

  statusLedUpdate(streaming);
  streamPollCommands();
  updateDisplay();  // also while the sensor is missing: the "no finger" icon is animated
  const LinkState link = cloudLinkState();
  displaySetStatus(link == LinkState::READY || link == LinkState::SESSION || link == LinkState::CLOSING,
                   BATTERY_PLACEHOLDER_PCT);
  updateMeasurement();

  if (!streaming) {
    static uint32_t lastTry = 0;
    if (millis() - lastTry >= SENSOR_RETRY_MS) {
      lastTry = millis();
      streaming = startSensor();
    }
    delay(POLL_INTERVAL_MS);  // always yield: the cloud task runs on the time loop() leaves free
    return;
  }

  pollSensor();
  if (i2cFails >= I2C_FAILS_BEFORE_LOST) {
    DBG("I2C", "el MAX30102 dejo de responder (%u fallos seguidos), reintentando", i2cFails);
    streaming = false;
    showNoFinger();
    return;
  }
  printStats();
  delay(POLL_INTERVAL_MS);
}
