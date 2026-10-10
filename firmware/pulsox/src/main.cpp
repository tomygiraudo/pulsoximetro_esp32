// PulsOx firmware entry point. Orchestration only: the sensor driver lives in
// lib/max30102, the signal processing in lib/ppg, the screens in src/display.cpp.
//
// Current stage: sensor -> signal processing -> TFT, no WiFi. Starts the MAX30102
// at 100 samples/s, feeds every sample to the pipeline (finger, filters, SpO2,
// heart rate), shows the result and the pulse trace on the TFT and prints one line
// per second with the result and the sensor health (samples/s, last RED/IR, FIFO
// overflows, slowest screen update). In CSV mode (send 'c') it also streams every
// sample with its filtered values, and the result, for tools/capture.py and
// tools/validate_ppg.py.
#include <Arduino.h>
#include <Wire.h>
#include "config.h"
#include "debug_log.h"
#include "display.h"
#include "i2c_diag.h"
#include "max30102.h"
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
    if (refreshed) {
      streamResult(sampleIndex, *ppg_output(&ppg));
      printResult(*ppg_output(&ppg));
      showResult(*ppg_output(&ppg));
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

void setup() {
  statusLedBegin();

  Serial.begin(115200);
  uint32_t start = millis();
  while (!Serial && millis() - start < SERIAL_WAIT_MS) delay(10);  // USB-CDC: wait for the monitor
  delay(300);
  streamBegin();

  Serial.println();
  Serial.println("=== PulsOx firmware ===");
  printBootInfo();

  if (displayBegin()) {
    displaySetStatus(false, BATTERY_PLACEHOLDER_PCT);
    DBG("BOOT", "pantalla lista (buffer de 32 KB)");
  } else {
    DBG("BOOT", "no hay RAM para el buffer de la pantalla (32 KB): sigue sin pantalla");
  }

  pinMode(PIN_INT_OX, INPUT_PULLUP);
  i2cPreCheck();
  Wire.begin(PIN_SDA_OX, PIN_SCL_OX);
  Wire.setClock(I2C_CLOCK_HZ);

  streaming = startSensor();
  if (!streaming) DBG("BOOT", "sin sensor: reintento cada %d ms", SENSOR_RETRY_MS);
}

void loop() {
  statusLedUpdate(streaming);
  streamPollCommands();
  updateDisplay();  // also while the sensor is missing: the "no finger" icon is animated

  if (!streaming) {
    static uint32_t lastTry = 0;
    if (millis() - lastTry < SENSOR_RETRY_MS) return;
    lastTry = millis();
    streaming = startSensor();
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
