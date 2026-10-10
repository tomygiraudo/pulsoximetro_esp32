#include "status_led.h"
#include <Arduino.h>
#include "config.h"

static void ledSet(bool on) { digitalWrite(PIN_LED, on ? LED_ON_LEVEL : !LED_ON_LEVEL); }

void statusLedBegin(bool bootBlinks) {
  pinMode(PIN_LED, OUTPUT);
  ledSet(false);
  if (!bootBlinks) return;
  for (uint8_t i = 0; i < 3; i++) {
    ledSet(true);
    delay(80);
    ledSet(false);
    delay(120);
  }
}

void statusLedUpdate(bool streaming) {
  static uint32_t last = 0;
  static bool on = false;
  if (millis() - last < (streaming ? LED_PERIOD_LIVE_MS : LED_PERIOD_IDLE_MS)) return;
  last = millis();
  on = !on;
  ledSet(on);
}

void statusLedOff() { ledSet(false); }
