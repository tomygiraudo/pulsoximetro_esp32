#include "serial_stream.h"
#include <Arduino.h>
#include "debug_log.h"

static StreamMode mode = StreamMode::SUMMARY;

void streamBegin() { Serial.setTxTimeoutMs(0); }

StreamMode streamMode() { return mode; }

// Printed every time CSV mode is requested, so a capture always starts with them.
static void printHeaders() {
  Serial.println("# D,n,red,ir");
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
      default:  // CR / LF / anything else: ignored
        break;
    }
  }
}

void streamSample(uint32_t n, const PpgSample &s) {
  if (mode != StreamMode::CSV) return;
  Serial.printf("D,%lu,%lu,%lu\n", (unsigned long)n, (unsigned long)s.red, (unsigned long)s.ir);
}

void streamEvent(uint32_t n, const char *name, uint32_t value) {
  if (mode != StreamMode::CSV) return;
  Serial.printf("E,%lu,%s,%lu\n", (unsigned long)n, name, (unsigned long)value);
}
