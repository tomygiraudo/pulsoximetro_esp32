// Debug logging on Serial. Every line starts with the uptime in ms and a tag, so
// a frozen or rebooting board is obvious from the log. Lines begin with '[',
// which keeps them apart from the data lines of the CSV stream ("D," / "R," / "E,").
#pragma once
#include <Arduino.h>

#define DBG(tag, fmt, ...) \
  Serial.printf("[%7lu][%-4s] " fmt "\n", (unsigned long)millis(), tag, ##__VA_ARGS__)

// Reset reason, chip, heap and build time: tells a crash or brownout from a normal boot.
void printBootInfo();
