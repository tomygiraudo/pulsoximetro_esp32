#include "button.h"
#include <Arduino.h>
#include "config.h"
#include "debug_log.h"

static bool stable = false;        // debounced level: true = down
static bool lastRaw = false;
static uint32_t rawChangedAt = 0;
static bool pending = false;       // a press nobody has read yet

static bool rawDown() { return digitalRead(PIN_BUTTON) == LOW; }

// Follows the pin; the level is accepted once it has stayed the same for BUTTON_DEBOUNCE_MS.
static void update() {
  const bool raw = rawDown();
  const uint32_t now = millis();
  if (raw != lastRaw) {
#if BUTTON_LOG_RAW
    DBG("BTN", "GPIO%d crudo: %s", PIN_BUTTON, raw ? "BAJO (apretado)" : "ALTO (suelto)");
#endif
    lastRaw = raw;
    rawChangedAt = now;
  }
  if (raw != stable && now - rawChangedAt >= BUTTON_DEBOUNCE_MS) {
    stable = raw;
    if (stable) pending = true;
  }
}

void buttonBegin() {
  // No internal pull-up: the button board has its own (R4). The chip's ~45 kOhm one would
  // form a divider with the board's series resistor (R5) and keep the pin from reaching a
  // low level when R5 is large (100 kOhm: ~2.3 V pressed, which still reads as high).
  pinMode(PIN_BUTTON, INPUT);
  stable = lastRaw = rawDown();  // already down: the press that woke us, not a new one
  rawChangedAt = millis();
  pending = false;
  DBG("BTN", "GPIO%d en reposo: %s", PIN_BUTTON, stable ? "BAJO (apretado)" : "ALTO (suelto)");
}

bool buttonPressed() {
  update();
  if (!pending) return false;
  pending = false;
  return true;
}

bool buttonDown() {
  update();
  return stable;
}

bool buttonWaitRelease(uint32_t timeoutMs) {
  const uint32_t start = millis();
  while (buttonDown()) {
    if (millis() - start >= timeoutMs) return false;
    delay(5);
  }
  return true;
}

void buttonInject() { pending = true; }
