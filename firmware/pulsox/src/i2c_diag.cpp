#include "i2c_diag.h"
#include <Arduino.h>
#include <Wire.h>
#include "config.h"
#include "debug_log.h"
#include "max30102.h"
#include "max30102_regs.h"

static void dbgLines(const char *when) {
  DBG("I2C", "%s: SDA(GPIO%d)=%d SCL(GPIO%d)=%d INT(GPIO%d)=%d", when, PIN_SDA_OX,
      digitalRead(PIN_SDA_OX), PIN_SCL_OX, digitalRead(PIN_SCL_OX), PIN_INT_OX,
      digitalRead(PIN_INT_OX));
}

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
