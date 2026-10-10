// UI demo: steps through every TFT screen with fake data, to see them on the board
// without the sensor. Only built in the `ui-demo` environment, which replaces main.cpp
// with this setup()/loop():
//
//   pio run -e ui-demo -t upload
//
// Serial prints which screen is up and, every second, the longest displayUpdate() so
// far: it has to stay well under the 320 ms the MAX30102's FIFO holds.
#ifdef PULSOX_DISPLAY_DEMO
#include <Arduino.h>
#include <math.h>
#include "config.h"
#include "debug_log.h"
#include "display.h"

struct DemoStep {
  const char *name;
  int spo2;       // <= 0: no value yet
  int bpm;
  bool noFinger;
  bool wifi;
  uint8_t battery;
};

static const DemoStep STEPS[] = {
    {"A  midiendo, SpO2 normal (98 %, 72 BPM)", 98, 72, false, true, 82},
    {"B  precaucion (94 %, 90 BPM)", 94, 90, false, true, 82},
    {"C  critica (89 %, 120 BPM)", 89, 120, false, true, 82},
    {"E  dedo no encontrado", 0, 0, true, true, 82},
    {"midiendo, todavia sin valores", 0, 0, false, true, 82},
    {"A  con WiFi desconectado y bateria al 15 %", 98, 72, false, false, 15},
};
static const int STEP_COUNT = sizeof(STEPS) / sizeof(STEPS[0]);
static const uint32_t STEP_MS = 6000;
static const uint32_t SAMPLE_MS = (uint32_t)(1000.0f / SENSOR_FS_HZ);

static int step = 0;
static uint32_t stepAt = 0;
static uint32_t nextSample = 0;
static float pulsePhase = 0.0f;  // 0-1 within the fake heartbeat
static uint32_t worstUs = 0;
static uint32_t reportAt = 0;

static void applyStep() {
  const DemoStep &s = STEPS[step];
  DBG("UI", "%s", s.name);
  displaySetStatus(s.wifi, s.battery);
  if (s.noFinger) {
    displaySetNoFinger();
  } else {
    displaySetMeasuring(s.spo2, s.bpm);
  }
  stepAt = millis();
}

// One PPG sample of a fake pulse: a systolic peak and a smaller dicrotic bump, pointing
// up, on a DC level like the one the real signal has before filtering.
static float fakeSample(float phase) {
  auto bump = [](float x, float center, float width) {
    float d = (x - center) / width;
    return expf(-d * d);
  };
  return 20000.0f + 1000.0f * (bump(phase, 0.14f, 0.045f) + 0.38f * bump(phase, 0.36f, 0.07f));
}

void setup() {
  Serial.begin(115200);
  uint32_t start = millis();
  while (!Serial && millis() - start < SERIAL_WAIT_MS) delay(10);  // USB-CDC: wait for the monitor
  delay(300);
  Serial.println();
  Serial.println("=== PulsOx UI demo ===");

  if (!displayBegin()) {
    DBG("UI", "no hay RAM para el buffer de la pantalla (32 KB)");
    return;
  }
  applyStep();
  nextSample = millis();
  reportAt = millis() + 1000;
}

void loop() {
  uint32_t now = millis();
  if (now - stepAt >= STEP_MS) {
    step = (step + 1) % STEP_COUNT;
    applyStep();
  }

  // The fake sensor, at the real sample rate. The beat rate follows the screen's BPM
  // (60 while there is none).
  const int bpm = STEPS[step].bpm > 0 ? STEPS[step].bpm : 60;
  for (int i = 0; i < 10 && (int32_t)(now - nextSample) >= 0; i++) {
    pulsePhase += bpm / 60.0f / SENSOR_FS_HZ;
    pulsePhase -= floorf(pulsePhase);
    displayPushPpg(fakeSample(pulsePhase));
    nextSample += SAMPLE_MS;
  }

  uint32_t t0 = micros();
  displayUpdate();
  uint32_t dt = micros() - t0;
  if (dt > worstUs) worstUs = dt;

  if (now >= reportAt) {
    DBG("UI", "displayUpdate: peor caso %lu us en el ultimo segundo", (unsigned long)worstUs);
    worstUs = 0;
    reportAt = now + 1000;
  }
  delay(5);
}
#endif  // PULSOX_DISPLAY_DEMO
