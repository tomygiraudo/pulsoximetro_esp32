#include "debug/debug_log.h"
#include <esp_system.h>

/*******************************************************************************
 * @brief       Traduce la causa del último reset a un texto en español.
 *
 * @details     Para el log de arranque: encendido, pin RESET, software, CRASH,
 *              watchdogs, salida de deep sleep, BROWNOUT; cualquier otra da
 *              "otra / desconocida".
 *
 * @param       Entrada: r - causa del reset (esp_reset_reason_t).
 *              Salida:  retorno (const char *) - texto de la causa.
 *
 * @peripherals Ninguno.
 *******************************************************************************/
static const char *resetReasonText(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON: return "encendido (power-on)";
    case ESP_RST_EXT: return "pin RESET";
    case ESP_RST_SW: return "reinicio por software";
    case ESP_RST_PANIC: return "CRASH (panic / excepcion)";
    case ESP_RST_INT_WDT: return "watchdog de interrupciones";
    case ESP_RST_TASK_WDT: return "watchdog de tarea";
    case ESP_RST_WDT: return "watchdog";
    case ESP_RST_DEEPSLEEP: return "salida de deep sleep";
    case ESP_RST_BROWNOUT: return "BROWNOUT (alimentacion insuficiente)";
    default: return "otra / desconocida";
  }
}

/*******************************************************************************
 * @brief       Imprime la causa del reset, el chip, el heap libre y la hora de
 *              compilación.
 *
 * @details     Permite distinguir un cuelgue (panic, watchdog) o un brownout de
 *              un arranque normal. Una línea de log por dato.
 *
 * @param       Ninguno.
 *
 * @peripherals Internos: Serial/USB-CDC. Externos: Ninguno.
 *******************************************************************************/
void printBootInfo() {
  esp_reset_reason_t rr = esp_reset_reason();
  DBG("BOOT", "build %s %s", __DATE__, __TIME__);
  DBG("BOOT", "causa del reset: %s (%d)", resetReasonText(rr), (int)rr);
  DBG("BOOT", "chip %s rev %d, %u MHz, heap libre %u B", ESP.getChipModel(),
      (int)ESP.getChipRevision(), (unsigned)ESP.getCpuFreqMHz(), (unsigned)ESP.getFreeHeap());
}
