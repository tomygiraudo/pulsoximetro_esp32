#include "debug/serial_stream.h"
#include <Arduino.h>
#include "board/button.h"
#include "debug/debug_log.h"
#include "ppg_csv.h"

static StreamMode mode = StreamMode::SUMMARY;
static bool glitchRequested = false;

/*******************************************************************************
 * @brief       Acota cuánto puede bloquear una escritura por Serial.
 *
 * @details     Fija el timeout de transmisión en SERIAL_TX_TIMEOUT_MS. Nunca 0:
 *              en HWCDC::write() un 0 hace que escribir a un USB abierto sin
 *              lector espere hasta que aparezca uno, congelando el loop y la
 *              pantalla. Con un valor chico se descartan bytes.
 *
 * @param       Entrada: SERIAL_TX_TIMEOUT_MS - espera máxima de una escritura,
 *                       en ms.
 *
 * @peripherals Internos: Serial/USB-CDC. Externos: Ninguno.
 *******************************************************************************/
void streamBegin() { Serial.setTxTimeoutMs(SERIAL_TX_TIMEOUT_MS); }

/*******************************************************************************
 * @brief       Devuelve el modo de salida de datos vigente (resumen o CSV).
 *
 * @details     Devuelve mode, que cambian los comandos 's' y 'c'.
 *
 * @param       Entrada: mode - modo vigente.
 *              Salida:  retorno (StreamMode) - SUMMARY o CSV.
 *
 * @peripherals Ninguno.
 *******************************************************************************/
StreamMode streamMode() { return mode; }

/*******************************************************************************
 * @brief       Informa, una sola vez, si el host pidió corromper la próxima
 *              muestra.
 *
 * @details     Devuelve glitchRequested y lo borra; lo activa el comando 'g'.
 *              Solo las compilaciones de prueba (PULSOX_TEST_HOOKS) actúan
 *              sobre él.
 *
 * @param       Entrada: glitchRequested - pedido pendiente.
 *              Salida:  retorno (bool) - true si había un pedido.
 *              Salida:  glitchRequested - false.
 *
 * @peripherals Ninguno.
 *******************************************************************************/
bool streamTakeGlitchRequest() {
  const bool requested = glitchRequested;
  glitchRequested = false;
  return requested;
}

/*******************************************************************************
 * @brief       Imprime las cabeceras del CSV con los nombres de las columnas.
 *
 * @details     Se imprimen cada vez que se pide el modo CSV, así toda captura
 *              empieza con ellas: líneas "# D,", "# R," y "# E,".
 *
 * @param       Ninguno.
 *
 * @peripherals Internos: Serial/USB-CDC. Externos: Ninguno.
 *******************************************************************************/
static void printHeaders() {
  Serial.println("# D," PPG_CSV_D_HEADER);
  Serial.println("# R," PPG_CSV_R_HEADER);
  Serial.println("# E,n,event,value");
}

/*******************************************************************************
 * @brief       Lee y ejecuta los comandos de un carácter que manda el host por
 *              Serial.
 *
 * @details     'c' modo CSV (imprime las cabeceras), 's' modo resumen, 'b'
 *              simula el botón y 'g' pide corromper la próxima muestra; CR, LF
 *              y cualquier otro se ignoran. Llamar en cada vuelta del loop().
 *
 * @param       Entrada: Serial - caracteres recibidos del host.
 *              Salida:  mode, glitchRequested - modo y pedido de glitch.
 *
 * @peripherals Internos: Serial/USB-CDC. Externos: botón (simulado con 'b').
 *******************************************************************************/
void streamPollCommands() {
  while (Serial.available()) {
    switch (Serial.read()) {
      case 'c':
        mode = StreamMode::CSV;
        printHeaders();
        DBG("CMD", "modo CSV (captura)");
        break;
      case 's':
        mode = StreamMode::SUMMARY;
        DBG("CMD", "modo resumen");
        break;
      case 'b':
        buttonInject();
        DBG("CMD", "boton simulado");
        break;
      case 'g':
        glitchRequested = true;
        DBG("CMD", "glitch pedido (solo hace algo en las compilaciones de prueba)");
        break;
      default:  // CR / LF / anything else: ignored
        break;
    }
  }
}

/*******************************************************************************
 * @brief       Imprime la línea CSV "D," de una muestra (solo en modo CSV).
 *
 * @details     Arma la línea con ppg_csv_d() y la escribe por Serial; si no
 *              entra en el buffer de 160 bytes la descarta.
 *
 * @param       Entrada: n - índice de muestra.
 *              Entrada: s, d - muestra cruda y valores intermedios del
 *                       procesador PPG.
 *              Entrada: mode - solo imprime en modo CSV.
 *
 * @peripherals Internos: Serial/USB-CDC. Externos: Ninguno.
 *******************************************************************************/
void streamSample(uint32_t n, const PpgSample &s, const PpgDebug &d) {
  if (mode != StreamMode::CSV) return;
  char line[160];
  int len = ppg_csv_d(line, sizeof(line), n, s.red, s.ir, &d);
  if (len > 0 && len < (int)sizeof(line)) Serial.write((const uint8_t *)line, len);
}

/*******************************************************************************
 * @brief       Imprime la línea CSV "R," con el resultado de un segundo (solo
 *              en modo CSV).
 *
 * @details     Arma la línea con ppg_csv_r() y la escribe por Serial; si no
 *              entra en el buffer de 128 bytes la descarta.
 *
 * @param       Entrada: n - índice de muestra.
 *              Entrada: o - salida del procesador PPG.
 *              Entrada: mode - solo imprime en modo CSV.
 *
 * @peripherals Internos: Serial/USB-CDC. Externos: Ninguno.
 *******************************************************************************/
void streamResult(uint32_t n, const PpgOutput &o) {
  if (mode != StreamMode::CSV) return;
  char line[128];
  int len = ppg_csv_r(line, sizeof(line), n, &o);
  if (len > 0 && len < (int)sizeof(line)) Serial.write((const uint8_t *)line, len);
}

/*******************************************************************************
 * @brief       Imprime una línea CSV "E," con un evento de la captura (solo en
 *              modo CSV).
 *
 * @details     Escribe n, nombre y valor del evento, por ejemplo fifo_overflow
 *              o bad_sample.
 *
 * @param       Entrada: n - índice de muestra.
 *              Entrada: name, value - nombre del evento y su valor.
 *              Entrada: mode - solo imprime en modo CSV.
 *
 * @peripherals Internos: Serial/USB-CDC. Externos: Ninguno.
 *******************************************************************************/
void streamEvent(uint32_t n, const char *name, uint32_t value) {
  if (mode != StreamMode::CSV) return;
  Serial.printf("E,%lu,%s,%lu\n", (unsigned long)n, name, (unsigned long)value);
}
