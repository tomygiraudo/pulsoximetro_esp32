// Light sleep or deep sleep (POWER_SLEEP_MODE in config.h) and the wake-up button.
//
// The device spends its life asleep: ESP32-C3 in light or deep sleep, MAX30102 in shutdown, TFT in
// sleep-in with the backlight off. GPIO0 (the button, active low) wakes it. A light-sleep wakeup
// resumes right where it was (powerSleep() returns). A deep-sleep
// wakeup is a reboot: setup() runs again, and powerWakeReason() says why.
//
// POWER_SLEEP_SIM (env `dev`) simulates the sleep: powerSleep() just waits for the button
// (or the serial command 'b') and returns, with the CPU and the USB port up, so the log can
// be followed and the firmware flashed at any time.
#pragma once

enum class Wake : unsigned char {
  COLD_BOOT,  // power-on, RESET pin, flashing, crash: anything but a deep-sleep wakeup
  BUTTON,     // GPIO0 went low
  TIMER,      // the stuck-button fallback of powerSleep()
};

// First thing in setup(): releases the pin holds a deep sleep left (see powerSleep()), so
// the rest of the setup can drive those pins.
void powerBegin();

Wake powerWakeReason();
const char *powerWakeText(Wake w);

// Everything that must be quiet is quiet (sensor shut down, panel in sleep, see main.cpp);
// this last step freezes the pins that would float, arms GPIO0 and sleeps. It waits for a
// button that is still held (a wakeup by level would fire at once). With deep sleep it
// does not return. With light sleep and in the simulated sleep it returns when the button is
// pressed (that press is not reported again by buttonPressed()).
void powerSleep();
