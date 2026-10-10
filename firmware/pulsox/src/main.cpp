// PulsOx firmware entry point. The whole application is in src/app.cpp (see app.h for the
// states); the sensor driver lives in lib/max30102, the signal processing in lib/ppg, the
// screens in src/display.cpp, the network in src/cloud_link.cpp and the sleep in src/power.cpp.
#include <Arduino.h>
#include "app.h"

void setup() { appSetup(); }

void loop() { appLoop(); }
