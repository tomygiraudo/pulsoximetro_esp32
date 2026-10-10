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

enum class Kind : uint8_t { MEASURING, NO_FINGER, CONNECTING, WAIT_FINGER, RESULT };

struct DemoStep {
  const char *name;
  Kind kind;
  int spo2;       // <= 0: no value yet
  int bpm;
  bool wifi;
  uint8_t battery;
  int progress;   // measuring: 0-100, -1 = no bar
  bool sent;      // result: reached the cloud
};

static const DemoStep STEPS[] = {
    {"conectando", Kind::CONNECTING, 0, 0, false, 82, -1, false},
    {"coloca el dedo", Kind::WAIT_FINGER, 0, 0, true, 82, -1, false},
    {"midiendo, todavia sin valores (progreso 8 %)", Kind::MEASURING, 0, 0, true, 82, 8, false},
    {"A  midiendo, SpO2 normal (98 %, 72 BPM), progreso 40 %", Kind::MEASURING, 98, 72, true, 82, 40, false},
    {"B  precaucion (94 %, 90 BPM), progreso 62 %", Kind::MEASURING, 94, 90, true, 82, 62, false},
    {"C  critica (89 %, 120 BPM), progreso 90 %", Kind::MEASURING, 89, 120, true, 82, 90, false},
    {"resultado normal, enviado", Kind::RESULT, 98, 72, true, 82, -1, true},
    {"resultado normal, NO enviado", Kind::RESULT, 97, 68, false, 82, -1, false},
    {"resultado precaucion, enviado", Kind::RESULT, 94, 90, true, 82, -1, true},
    {"resultado critico, NO enviado", Kind::RESULT, 89, 120, false, 82, -1, false},
    {"E  dedo no encontrado", Kind::NO_FINGER, 0, 0, true, 82, -1, false},
    {"A  con WiFi desconectado y bateria al 15 %", Kind::MEASURING, 98, 72, false, 15, 25, false},
};
static const int STEP_COUNT = sizeof(STEPS) / sizeof(STEPS[0]);
static const uint32_t STEP_MS = 5000;
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
  switch (s.kind) {
    case Kind::CONNECTING: displaySetConnecting(); break;
    case Kind::WAIT_FINGER: displaySetWaitFinger(); break;
    case Kind::NO_FINGER: displaySetNoFinger(); break;
    case Kind::MEASURING:
      displaySetMeasuring(s.spo2, s.bpm);
      displaySetProgress(s.progress);
      break;
    case Kind::RESULT: displaySetResult(s.spo2, s.bpm, s.sent); break;
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
