#include "power.h"
#include <Arduino.h>
#include <driver/gpio.h>
#include <esp_sleep.h>
#include "button.h"
#include "config.h"
#include "debug_log.h"
#include "display.h"
#include "serial_stream.h"
#include "status_led.h"

void powerBegin() {
  gpio_deep_sleep_hold_dis();
  gpio_hold_dis((gpio_num_t)PIN_LED);
  displayHoldPins(false);
}

Wake powerWakeReason() {
  switch (esp_sleep_get_wakeup_cause()) {
    case ESP_SLEEP_WAKEUP_GPIO: return Wake::BUTTON;
    case ESP_SLEEP_WAKEUP_TIMER: return Wake::TIMER;
    default: return Wake::COLD_BOOT;
  }
}

const char *powerWakeText(Wake w) {
  switch (w) {
    case Wake::BUTTON: return "boton (GPIO0)";
    case Wake::TIMER: return "temporizador (boton trabado)";
    default: return "arranque en frio";
  }
}

// What is common to every mode: the button must be up (a wakeup by level would fire at once), and
// the LED dark. False if the button is still down after BUTTON_RELEASE_WAIT_MS: then the sleep is
// on a timer, to look again, not on the button.
static bool prepareToSleep() {
  const bool released = buttonWaitRelease(BUTTON_RELEASE_WAIT_MS);
  if (!released) {
    DBG("PWR", "el boton sigue apretado tras %d ms: duermo con temporizador de %d s", BUTTON_RELEASE_WAIT_MS,
        BUTTON_STUCK_RETRY_S);
  }
  statusLedOff();
  return released;
}

#if POWER_SLEEP_MODE == POWER_SLEEP_LIGHT

void powerSleep() {
  for (;;) {  // loops only if the button was stuck: the timer wakes it to look again
    const bool released = prepareToSleep();
    // The pins keep their level while the CPU sleeps; the hold makes sure of it for the ones that must
    // stay quiet (backlight off, panel not selected, LED off).
    displayHoldPins(true);
    gpio_hold_en((gpio_num_t)PIN_LED);
    if (released) {
      gpio_wakeup_enable((gpio_num_t)PIN_BUTTON, GPIO_INTR_LOW_LEVEL);  // a plain input: no internal pull-up
      esp_sleep_enable_gpio_wakeup();
    } else {
      esp_sleep_enable_timer_wakeup((uint64_t)BUTTON_STUCK_RETRY_S * 1000000ULL);
    }
    DBG("PWR", "light sleep (el USB se corta); el boton GPIO%d despierta", PIN_BUTTON);
    Serial.flush();
    esp_light_sleep_start();  // returns when it wakes
    const esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    gpio_wakeup_disable((gpio_num_t)PIN_BUTTON);
    displayHoldPins(false);
    gpio_hold_dis((gpio_num_t)PIN_LED);
    if (cause == ESP_SLEEP_WAKEUP_TIMER) continue;  // it was only the timer of a stuck button
    buttonBegin();  // the press that woke it is still down: it is not a new one
    DBG("PWR", "despierta del light sleep");
    return;
  }
}

#elif POWER_SLEEP_MODE == POWER_SLEEP_DEEP

void powerSleep() {
  const bool released = prepareToSleep();
  displayHoldPins(true);
  gpio_hold_en((gpio_num_t)PIN_LED);
  gpio_deep_sleep_hold_en();
  if (released) {
    esp_deep_sleep_enable_gpio_wakeup(1ULL << PIN_BUTTON, ESP_GPIO_WAKEUP_GPIO_LOW);
  } else {
    esp_sleep_enable_timer_wakeup((uint64_t)BUTTON_STUCK_RETRY_S * 1000000ULL);
  }
  DBG("PWR", "deep sleep (el USB se corta); el boton GPIO%d despierta", PIN_BUTTON);
  Serial.flush();
  esp_deep_sleep_start();
}

#else  // POWER_SLEEP_SIM

void powerSleep() {
  prepareToSleep();
  DBG("PWR", "sueno SIMULADO (env dev): USB y CPU siguen; el boton o 'b' despiertan");
  buttonPressed();  // forget a press that happened while going to sleep
  while (!buttonPressed()) {
    streamPollCommands();
#if BUTTON_LOG_RAW
    static uint32_t lastLevelLog = 0;
    if (millis() - lastLevelLog >= 2000) {
      lastLevelLog = millis();
      DBG("BTN", "dormido, GPIO%d = %d (1 = suelto)", PIN_BUTTON, digitalRead(PIN_BUTTON));
    }
#endif
    delay(10);
  }
  DBG("PWR", "despierta");
}

#endif
