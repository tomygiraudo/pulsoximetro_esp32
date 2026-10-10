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

void powerSleep() {
  const bool released = buttonWaitRelease(BUTTON_RELEASE_WAIT_MS);
  if (!released) {
    DBG("PWR", "el boton sigue apretado tras %d ms: duermo con temporizador de %d s", BUTTON_RELEASE_WAIT_MS,
        BUTTON_STUCK_RETRY_S);
  }
  statusLedOff();

#if POWER_DEEP_SLEEP
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
#else
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
#endif
}
