// The wake / start button (SW1 on the external board, GPIO0, active low), debounced in
// software on top of the board's RC. Polled: call buttonPressed() on every loop() pass.
//
// A button that is already down when buttonBegin() runs is the press that woke the chip
// from deep sleep: it is not reported as a press, the button has to be released first.
#pragma once
#include <stdint.h>

// Pin setup, as a plain input (the pull-up is on the button board). Without that board the
// pin floats and reads random levels.
void buttonBegin();

// True once per press, after the debounce time. Call on every loop() pass.
bool buttonPressed();

// The debounced level, true while the button is held down.
bool buttonDown();

// Blocks until the button is released or timeoutMs passes. True if it was released.
bool buttonWaitRelease(uint32_t timeoutMs);

// Makes the next buttonPressed() return true, as if the button had been pressed. Wired to
// the serial command 'b' (serial_stream.cpp), to test without touching the hardware.
void buttonInject();
