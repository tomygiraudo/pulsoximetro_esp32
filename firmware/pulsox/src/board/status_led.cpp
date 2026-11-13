#include "board/status_led.h"
#include <Arduino.h>
#include "config.h"

/*******************************************************************************
 * @brief       Enciende o apaga el LED de estado.
 *
 * @details     Escribe en PIN_LED el nivel LED_ON_LEVEL (activo en bajo en el
 *              Super Mini) o su inverso.
 *
 * @param       Entrada: on - true = encendido.
 *
 * @peripherals Internos: GPIO (PIN_LED). Externos: LED de estado.
 *******************************************************************************/
static void ledSet(bool on) { digitalWrite(PIN_LED, on ? LED_ON_LEVEL : !LED_ON_LEVEL); }

/*******************************************************************************
 * @brief       Configura el LED de estado y, si se pide, hace los 3 destellos
 *              de arranque.
 *
 * @details     Pone el pin como salida con el LED apagado. Con bootBlinks hace
 *              3 destellos (80 ms encendido, 120 ms apagado) y bloquea unos 600
 *              ms.
 *
 * @param       Entrada: bootBlinks - true en un arranque en frío.
 *
 * @peripherals Internos: GPIO (PIN_LED). Externos: LED de estado.
 *******************************************************************************/
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

/*******************************************************************************
 * @brief       Hace parpadear el LED de estado, sin bloquear.
 *
 * @details     Alterna el LED cada LED_PERIOD_LIVE_MS si el sensor está
 *              midiendo (parpadeo lento) o cada LED_PERIOD_IDLE_MS si falta
 *              (rápido). Llamar en cada vuelta del loop().
 *
 * @param       Entrada: streaming - true si el sensor está muestreando.
 *              Salida:  LED - cambia de estado al cumplirse el período.
 *
 * @peripherals Internos: GPIO (PIN_LED). Externos: LED de estado.
 *******************************************************************************/
void statusLedUpdate(bool streaming) {
  static uint32_t last = 0;
  static bool on = false;
  if (millis() - last < (streaming ? LED_PERIOD_LIVE_MS : LED_PERIOD_IDLE_MS)) return;
  last = millis();
  on = !on;
  ledSet(on);
}

/*******************************************************************************
 * @brief       Apaga el LED de estado (antes de dormir).
 *
 * @details     Llama a ledSet(false).
 *
 * @param       Ninguno.
 *
 * @peripherals Internos: GPIO (PIN_LED). Externos: LED de estado.
 *******************************************************************************/
void statusLedOff() { ledSet(false); }
