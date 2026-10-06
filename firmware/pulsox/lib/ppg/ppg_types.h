// Types shared between the sensor driver and the signal-processing pipeline.
// No Arduino dependency: this library builds on any C++ toolchain.
#pragma once
#include <stdint.h>

// One raw sample as read from the sensor: 18-bit ADC counts per LED.
struct PpgSample {
  uint32_t red;  // LED1
  uint32_t ir;   // LED2
};
