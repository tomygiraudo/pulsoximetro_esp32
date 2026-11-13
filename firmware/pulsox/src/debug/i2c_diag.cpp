#include "debug/i2c_diag.h"
#include <Arduino.h>
#include <Wire.h>
#include "config.h"
#include "debug/debug_log.h"
#include "max30102.h"
#include "max30102_regs.h"

/*******************************************************************************
 * @brief       Imprime el nivel de las líneas SDA, SCL e INT del sensor.
 *
 * @details     Lee los tres pines como entrada digital y los muestra con la
 *              etiqueta recibida.
 *
 * @param       Entrada: when - texto que identifica el momento del diagnóstico.
 *
 * @peripherals Internos: GPIO (lectura de SDA, SCL e INT), Serial/USB-CDC.
 *              Externos: MAX30102 (líneas del bus e INT).
 *******************************************************************************/
static void dbgLines(const char *when) {
  DBG("I2C", "%s: SDA(GPIO%d)=%d SCL(GPIO%d)=%d INT(GPIO%d)=%d", when, PIN_SDA_OX,
      digitalRead(PIN_SDA_OX), PIN_SCL_OX, digitalRead(PIN_SCL_OX), PIN_INT_OX,
      digitalRead(PIN_INT_OX));
}

/*******************************************************************************
 * @brief       Revisa los pull-ups del bus I2C antes de iniciar Wire.
 *
 * @details     Lee SDA y SCL como entrada simple y con pull-up interno, y
 *              distingue: bus libre (los dos en alto), falta de pull-up
 *              externo, o línea sostenida en bajo (corto o pull-ups a 1.8 V).
 *              Debe correr antes de Wire.begin().
 *
 * @param       Salida:  pines SDA y SCL - quedan como entrada simple.
 *
 * @peripherals Internos: GPIO (SDA y SCL), Serial/USB-CDC.
 *              Externos: MAX30102 (pull-ups del módulo).
 *******************************************************************************/
void i2cPreCheck() {
  pinMode(PIN_SDA_OX, INPUT);
  pinMode(PIN_SCL_OX, INPUT);
  delay(2);
  bool sda = digitalRead(PIN_SDA_OX), scl = digitalRead(PIN_SCL_OX);
  pinMode(PIN_SDA_OX, INPUT_PULLUP);
  pinMode(PIN_SCL_OX, INPUT_PULLUP);
  delay(2);
  bool sdaPu = digitalRead(PIN_SDA_OX), sclPu = digitalRead(PIN_SCL_OX);
  pinMode(PIN_SDA_OX, INPUT);
  pinMode(PIN_SCL_OX, INPUT);
  DBG("I2C", "pre-check sin pull-up interno: SDA=%d SCL=%d | con pull-up interno: SDA=%d SCL=%d",
      sda, scl, sdaPu, sclPu);
  if (sda && scl) {
    DBG("I2C", "ok: las dos lineas estan en alto (hay pull-ups externos, bus libre)");
  } else if (sdaPu && sclPu) {
    DBG("I2C", "ATENCION: linea/s en bajo sin pull-up interno -> no hay pull-up externo "
               "(modulo sin alimentar, o pista/soldadura cortada)");
  } else {
    DBG("I2C", "ATENCION: %s en bajo incluso con pull-up interno -> corto a GND, algo la "
               "sujeta en bajo, o el modulo tiene pull-ups a 1.8V (el C3 necesita ~2.5V para leer 1)",
        (!sdaPu && !sclPu) ? "SDA y SCL" : (!sdaPu ? "SDA" : "SCL"));
  }
}

/*******************************************************************************
 * @brief       Diagnostica el bus I2C cuando el MAX30102 no responde.
 *
 * @details     Sondea 0x57 con STOP (el error crudo dice si alguien da ACK a la
 *              dirección), escanea 0x08 a 0x77 contando dispositivos y
 *              timeouts, e imprime SDA, SCL e INT.
 *
 * @param       Ninguno.
 *
 * @peripherals Internos: I2C (Wire), GPIO, Serial/USB-CDC.
 *              Externos: MAX30102 y cualquier otro dispositivo del bus.
 *******************************************************************************/
void i2cDiagnose() {
  // Probe with a STOP: unlike the register read (repeated start), this error is
  // the plain answer of the address phase.
  Wire.beginTransmission(max30102_reg::ADDR);
  uint8_t probe = Wire.endTransmission();
  DBG("I2C", "probe 0x%02X con STOP: err=%u (%s)", max30102_reg::ADDR, probe,
      Max30102::errorText(probe));

  uint8_t count = 0, timeouts = 0;
  for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
    Wire.beginTransmission(addr);
    uint8_t err = Wire.endTransmission();
    if (err == 0) {
      DBG("I2C", "scan: responde 0x%02X", addr);
      count++;
    } else if (err == 5) {
      timeouts++;
    }
  }
  DBG("I2C", "scan 0x08-0x77: %u dispositivo/s, %u timeout/s%s", count, timeouts,
      timeouts ? " -> el bus se traba (SDA/SCL en bajo)" : "");
  dbgLines("tras el scan");
}
