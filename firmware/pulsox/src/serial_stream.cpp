#include "serial_stream.h"
#include <Arduino.h>
#include "button.h"
#include "debug_log.h"
#include "ppg_csv.h"

static StreamMode mode = StreamMode::SUMMARY;

// The timeout must NOT be 0: in the Arduino core's HWCDC::write(), 0 makes the "nobody is
// reading" countdown wrap around, and a write to a USB port that is open (or plugged into a
// charger) but not being read then waits until a reader shows up, freezing loop() (and the
// screen) for as long as that takes. With a small positive value the wait is bounded by about
// that many ms, after which the bytes are dropped until the host reads again.
void streamBegin() { Serial.setTxTimeoutMs(SERIAL_TX_TIMEOUT_MS); }

StreamMode streamMode() { return mode; }

// Printed every time CSV mode is requested, so a capture always starts with them.
static void printHeaders() {
  Serial.println("# D," PPG_CSV_D_HEADER);
  Serial.println("# R," PPG_CSV_R_HEADER);
  Serial.println("# E,n,event,value");
}

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
      default:  // CR / LF / anything else: ignored
        break;
    }
  }
}

void streamSample(uint32_t n, const PpgSample &s, const PpgDebug &d) {
  if (mode != StreamMode::CSV) return;
  char line[160];
  int len = ppg_csv_d(line, sizeof(line), n, s.red, s.ir, &d);
  if (len > 0 && len < (int)sizeof(line)) Serial.write((const uint8_t *)line, len);
}

void streamResult(uint32_t n, const PpgOutput &o) {
  if (mode != StreamMode::CSV) return;
  char line[128];
  int len = ppg_csv_r(line, sizeof(line), n, &o);
  if (len > 0 && len < (int)sizeof(line)) Serial.write((const uint8_t *)line, len);
}

void streamEvent(uint32_t n, const char *name, uint32_t value) {
  if (mode != StreamMode::CSV) return;
  Serial.printf("E,%lu,%s,%lu\n", (unsigned long)n, name, (unsigned long)value);
}
