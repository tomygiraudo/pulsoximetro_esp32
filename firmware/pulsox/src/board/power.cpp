#include "board/power.h"
#include <Arduino.h>
#include <driver/gpio.h>
#include <esp_sleep.h>
#include "board/button.h"
#include "config.h"
#include "debug/debug_log.h"
#include "display/display.h"
#include "debug/serial_stream.h"
#include "board/status_led.h"

/*******************************************************************************
 * @brief       Libera los pines que un deep sleep dejó retenidos.
 *
 * @details     Primera llamada de setup(): quita la retención global de deep
 *              sleep, la del LED de estado y la de los pines de la pantalla,
 *              para poder manejarlos.
 *
 * @param       Ninguno.
 *
 * @peripherals Internos: GPIO (retención de pines), light/deep sleep.
 *              Externos: LED de estado, pantalla TFT (backlight, CS y RST).
 *******************************************************************************/
void powerBegin() {
  gpio_deep_sleep_hold_dis();
  gpio_hold_dis((gpio_num_t)PIN_LED);
  displayHoldPins(false);
}

/*******************************************************************************
 * @brief       Dice por qué despertó el chip.
 *
 * @details     Traduce esp_sleep_get_wakeup_cause(): GPIO = botón, temporizador
 *              = el reintento de un botón trabado, cualquier otra causa =
 *              arranque en frío.
 *
 * @param       Salida:  retorno (Wake) - BUTTON, TIMER o COLD_BOOT.
 *
 * @peripherals Internos: light/deep sleep (causa de despertar).
 *              Externos: Ninguno.
 *******************************************************************************/
Wake powerWakeReason() {
  switch (esp_sleep_get_wakeup_cause()) {
    case ESP_SLEEP_WAKEUP_GPIO: return Wake::BUTTON;
    case ESP_SLEEP_WAKEUP_TIMER: return Wake::TIMER;
    default: return Wake::COLD_BOOT;
  }
}

/*******************************************************************************
 * @brief       Devuelve el motivo del despertar como texto.
 *
 * @details     Para los logs: botón (GPIO0), temporizador (botón trabado) o
 *              arranque en frío.
 *
 * @param       Entrada: w - motivo del despertar.
 *              Salida:  retorno (const char *) - texto del motivo.
 *
 * @peripherals Ninguno.
 *******************************************************************************/
const char *powerWakeText(Wake w) {
  switch (w) {
    case Wake::BUTTON: return "boton (GPIO0)";
    case Wake::TIMER: return "temporizador (boton trabado)";
    default: return "arranque en frio";
  }
}

/*******************************************************************************
 * @brief       Prepara lo común a todos los modos de sueño: botón suelto y LED
 *              apagado.
 *
 * @details     Espera hasta BUTTON_RELEASE_WAIT_MS a que suelten el botón (un
 *              despertar por nivel dispararía enseguida) y apaga el LED de
 *              estado. Si sigue apretado, el sueño será con temporizador
 *              (BUTTON_STUCK_RETRY_S) para volver a mirar.
 *
 * @param       Salida:  retorno (bool) - true si el botón estaba suelto; false
 *                       si sigue apretado.
 *
 * @peripherals Internos: GPIO, Serial/USB-CDC (log).
 *              Externos: botón, LED de estado.
 *******************************************************************************/
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

/*******************************************************************************
 * @brief       Versión POWER_SLEEP_LIGHT (por defecto): duerme en light sleep
 *              hasta que el botón despierte.
 *
 * @details     Retiene los pines de pantalla y LED, arma el despertar por nivel
 *              bajo de GPIO0 (o un temporizador si el botón estaba trabado) y
 *              entra en light sleep: la RAM se conserva y vuelve a este punto.
 *              Si despertó el temporizador repite el ciclo.
 *
 * @param       Entrada: PIN_BUTTON, BUTTON_STUCK_RETRY_S - pin de despertar y
 *                       reintento del botón trabado.
 *              Salida:  retorno (void) - vuelve al despertar por el botón, sin
 *                       reportar esa pulsación.
 *
 * @peripherals Internos: light sleep, GPIO (wakeup y retención), temporizador,
 *              Serial/USB-CDC.
 *              Externos: botón, pantalla TFT (pines retenidos), LED de estado.
 *******************************************************************************/
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

/*******************************************************************************
 * @brief       Versión POWER_SLEEP_DEEP: deep sleep (~5 uA); despertar =
 *              reinicio del chip.
 *
 * @details     Igual que la versión light, pero con deep sleep: además de los
 *              pines retiene los GPIO globalmente y arma el despertar de GPIO0
 *              por nivel bajo. No vuelve: al despertar corre setup() otra vez.
 *              Necesita R5 <= ~3 kOhm en la placa del botón.
 *
 * @param       Entrada: PIN_BUTTON, BUTTON_STUCK_RETRY_S - pin de despertar y
 *                       reintento del botón trabado.
 *              Salida:  retorno (void) - no vuelve: el chip se reinicia al
 *                       despertar.
 *
 * @peripherals Internos: deep sleep, GPIO (wakeup y retención), temporizador,
 *              Serial/USB-CDC.
 *              Externos: botón, pantalla TFT (pines retenidos), LED de estado.
 *******************************************************************************/
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

/*******************************************************************************
 * @brief       Versión POWER_SLEEP_SIM (env dev): sueño simulado, el CPU y el
 *              USB siguen activos.
 *
 * @details     Prepara el sueño y descarta una pulsación pendiente; espera
 *              atendiendo comandos seriales hasta que el botón o el comando 'b'
 *              lo despierten. Con BUTTON_LOG_RAW loguea el nivel de GPIO0 cada
 *              2 s.
 *
 * @param       Salida:  retorno (void) - vuelve cuando se aprieta el botón (o
 *                       llega 'b').
 *
 * @peripherals Internos: GPIO (botón), Serial/USB-CDC.
 *              Externos: botón, LED de estado (se apaga).
 *******************************************************************************/
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
