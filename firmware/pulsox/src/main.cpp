// PulsOx firmware entry point. Orchestration only: the sensor driver lives in
// lib/max30102, the signal processing in lib/ppg.
//
// Current stage: raw acquisition. Starts the MAX30102 at 100 samples/s, prints
// one summary line per second (samples/s, last RED/IR, FIFO overflows) and, in
// CSV mode (send 'c'), streams every raw sample for tools/capture.py.
#include <Arduino.h>
#include <Wire.h>
#include "config.h"
#include "debug_log.h"
#include "i2c_diag.h"
#include "max30102.h"
#include "serial_stream.h"
#include "status_led.h"

static Max30102 sensor;
static const Max30102Config SENSOR_CFG = {SENSOR_FIFO_CONFIG, SENSOR_SPO2_CONFIG,
                                          SENSOR_LED_RED_PA, SENSOR_LED_IR_PA};

static bool streaming = false;
static uint8_t i2cFails = 0;

static uint32_t sampleIndex = 0;     // samples since the sensor was started
static uint32_t lastOverflow = 0;
static PpgSample lastSample = {0, 0};
static uint16_t samplesInWindow = 0;
static uint32_t statsAt = 0;

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
  statsAt = millis();
  DBG("MAX", "sensor configurado: %.0f muestras/s, LEDs RED=0x%02X IR=0x%02X", SENSOR_FS_HZ,
      SENSOR_LED_RED_PA, SENSOR_LED_IR_PA);
  return true;
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
    streamSample(sampleIndex++, buf[i]);
    lastSample = buf[i];
  }
  samplesInWindow += n;
}

static void printStats() {
  if (millis() - statsAt < STATS_PERIOD_MS) return;
  statsAt += STATS_PERIOD_MS;
  DBG("LIVE", "%u muestras/s | red=%lu ir=%lu | overflows=%lu | fallos I2C=%u", samplesInWindow,
      (unsigned long)lastSample.red, (unsigned long)lastSample.ir,
      (unsigned long)sensor.overflowCount(), i2cFails);
  samplesInWindow = 0;
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
    return;
  }
  printStats();
  delay(POLL_INTERVAL_MS);
}
