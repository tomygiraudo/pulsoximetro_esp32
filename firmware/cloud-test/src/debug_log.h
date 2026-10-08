// Debug logging on Serial, same format as firmware/pulsox/src/debug_log.h: every
// line starts with the uptime in ms and a 4-char tag, so a frozen or rebooting
// board is obvious from the log.
#pragma once
#include <Arduino.h>

#define DBG(tag, fmt, ...) \
  Serial.printf("[%7lu][%-4s] " fmt "\n", (unsigned long)millis(), tag, ##__VA_ARGS__)
