// TFT screens (128x128), drawn from design-system/Pulsioxímetro WiFi · TFT 128×128.html.
// A passive view: the caller says what to show and this module draws it, animates it
// and sends to the panel only the parts that changed. It does not read the sensor, run
// the PPG pipeline or touch any other module.
//
//   measuring, SpO2 normal   >= 96 %   green figures and trace
//   measuring, caution       93-95 %   amber, steady border, "SpO2 BAJA" banner
//   measuring, critical      <  93 %   red, 1 Hz blinking border, "¡SpO2 CRÍTICA!" banner
//   no finger                          "ERROR / Dedo no encontrado"
//
// Wiring it in, in main.cpp: displayBegin() once in setup(); displaySetMeasuring() or
// displaySetNoFinger() whenever the result changes; displayPushPpg() with every PPG
// sample; displayUpdate() on every loop() pass (cheap: it paces itself).
#pragma once
#include <stdint.h>

// SpO2 thresholds of the design (sheet D), in %. Override from config.h or with -D.
#ifndef DISPLAY_SPO2_NORMAL_MIN
#define DISPLAY_SPO2_NORMAL_MIN 96   // at or above: NORMAL
#endif
#ifndef DISPLAY_SPO2_CAUTION_MIN
#define DISPLAY_SPO2_CAUTION_MIN 93  // at or above: CAUTION; below: CRITICAL
#endif

// Trace. The design sweeps at 48 px/s (period in px = 2880 / BPM).
#ifndef DISPLAY_PPG_PX_PER_S
#define DISPLAY_PPG_PX_PER_S 48.0f
#endif
// Rate at which displayPushPpg() is called (samples per second).
#ifndef DISPLAY_PPG_INPUT_HZ
#define DISPLAY_PPG_INPUT_HZ 100.0f
#endif
// Smallest vertical range of the trace, in the units of the samples. Keeps noise from
// being blown up to the full height when there is no pulse.
#ifndef DISPLAY_PPG_MIN_SPAN
#define DISPLAY_PPG_MIN_SPAN 64.0f
#endif

enum class DisplayAlert : uint8_t { NORMAL, CAUTION, CRITICAL };

// Alert level for a SpO2 reading (the thresholds above). A reading <= 0 means "no
// value yet" and counts as NORMAL.
DisplayAlert displayAlertFor(int spo2);

// Initializes the panel (tftBegin) and allocates the frame buffer (32 KB). Call once,
// from setup(). Returns false if there was not enough RAM; every other call is then a
// no-op. Starts on the "no finger" screen.
bool displayBegin();

// Status bar: WiFi icon (grey when not connected) and battery (0-100 %).
void displaySetStatus(bool wifiConnected, uint8_t batteryPct);

// Measuring screen. spo2 in % and bpm; a value <= 0 shows "--" (not available yet, as
// while the filters settle) and, if spo2 is the missing one, no alert.
void displaySetMeasuring(int spo2, int bpm);

// "Dedo no encontrado" error screen.
void displaySetNoFinger();

// One PPG sample, oriented so that the heartbeat points up (invert the raw IR). Call at
// DISPLAY_PPG_INPUT_HZ. Cheap: it only feeds the trace history.
void displayPushPpg(float sample);

// Runs the animations (heart, trace, blinking border) and sends what changed to the
// panel. Call on every loop() pass.
void displayUpdate();

// Low power. displaySleep() turns the backlight off and puts the controller in display-off
// + sleep-in; the frame buffer is kept. displayWake() brings the controller back, redraws
// the whole screen and only then turns the backlight on. While asleep displayUpdate() does
// nothing. Both are no-ops when the panel is not up.
void displaySleep();
void displayWake();

// Freezes (hold = true) or releases the lines that must keep their level in deep sleep:
// backlight off, CS and RESET high, so the panel stays asleep and dark. Release them first
// thing after waking, before displayBegin() touches the pins.
void displayHoldPins(bool hold);
