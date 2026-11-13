#include "board/button.h"
#include <Arduino.h>
#include "config.h"
#include "debug/debug_log.h"

static bool stable = false;        // debounced level: true = down
static bool lastRaw = false;
static uint32_t rawChangedAt = 0;
static bool pending = false;       // a press nobody has read yet

/*******************************************************************************
 * @brief       Lee el pin del botón sin ningún filtro.
 *
 * @details     true si GPIO0 está en bajo (el botón es activo en bajo).
 *
 * @param       Salida:  retorno (bool) - true = apretado.
 *
 * @peripherals Internos: GPIO (PIN_BUTTON). Externos: botón.
 *******************************************************************************/
static bool rawDown() { return digitalRead(PIN_BUTTON) == LOW; }

/*******************************************************************************
 * @brief       Sigue el pin del botón y valida el nivel con antirrebote por
 *              software.
 *
 * @details     Acepta un nivel nuevo cuando se mantuvo igual
 *              BUTTON_DEBOUNCE_MS; si es de apretado deja una pulsación
 *              pendiente. Con BUTTON_LOG_RAW loguea cada flanco crudo.
 *
 * @param       Entrada: rawDown() - nivel crudo del pin.
 *              Salida:  stable, lastRaw, rawChangedAt, pending - estado del
 *                       antirrebote.
 *
 * @peripherals Internos: GPIO (PIN_BUTTON), Serial/USB-CDC (log, con
 *              BUTTON_LOG_RAW).
 *              Externos: botón.
 *******************************************************************************/
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

/*******************************************************************************
 * @brief       Configura el pin del botón como entrada, sin pull-up interno.
 *
 * @details     El pull-up está en la placa del botón: el interno (~45 kOhm)
 *              formaría un divisor con R5 y el pin no llegaría a nivel bajo. Un
 *              botón ya apretado al llamar es la pulsación que despertó al
 *              chip: no cuenta como nueva.
 *
 * @param       Salida:  stable, lastRaw, rawChangedAt, pending - estado inicial
 *                       del antirrebote.
 *
 * @peripherals Internos: GPIO (PIN_BUTTON), Serial/USB-CDC (log).
 *              Externos: botón.
 *******************************************************************************/
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

/*******************************************************************************
 * @brief       Informa una sola vez por pulsación, ya sin rebotes.
 *
 * @details     Actualiza el antirrebote y devuelve true una vez por pulsación
 *              (consume pending). Llamar en cada vuelta del loop().
 *
 * @param       Salida:  retorno (bool) - true si hubo una pulsación nueva.
 *              Salida:  pending - se borra al informarla.
 *
 * @peripherals Internos: GPIO (PIN_BUTTON). Externos: botón.
 *******************************************************************************/
bool buttonPressed() {
  update();
  if (!pending) return false;
  pending = false;
  return true;
}

/*******************************************************************************
 * @brief       Devuelve el nivel del botón ya filtrado.
 *
 * @details     Actualiza el antirrebote y devuelve el nivel estable: true
 *              mientras está apretado.
 *
 * @param       Salida:  retorno (bool) - true = apretado (nivel estable).
 *
 * @peripherals Internos: GPIO (PIN_BUTTON). Externos: botón.
 *******************************************************************************/
bool buttonDown() {
  update();
  return stable;
}

/*******************************************************************************
 * @brief       Espera a que se suelte el botón, con un tiempo máximo.
 *
 * @details     Bloquea (de a 5 ms) hasta que buttonDown() dé false o pase el
 *              timeout.
 *
 * @param       Entrada: timeoutMs - espera máxima, en milisegundos.
 *              Salida:  retorno (bool) - true si se soltó; false si venció el
 *                       timeout.
 *
 * @peripherals Internos: GPIO (PIN_BUTTON). Externos: botón.
 *******************************************************************************/
bool buttonWaitRelease(uint32_t timeoutMs) {
  const uint32_t start = millis();
  while (buttonDown()) {
    if (millis() - start >= timeoutMs) return false;
    delay(5);
  }
  return true;
}

/*******************************************************************************
 * @brief       Simula una pulsación del botón.
 *
 * @details     Marca pending: el próximo buttonPressed() devuelve true. Lo usa
 *              el comando serial 'b' para probar sin hardware.
 *
 * @param       Salida:  pending - pasa a true.
 *
 * @peripherals Ninguno.
 *******************************************************************************/
void buttonInject() { pending = true; }
