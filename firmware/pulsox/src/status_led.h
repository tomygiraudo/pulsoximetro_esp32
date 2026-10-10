// Heartbeat on the Super Mini's on-board LED (GPIO8): proves the firmware is
// running even when Serial shows nothing.
//   3 quick blinks at boot    firmware started (cold boot only: not when a button press wakes it)
//   fast blink (4 Hz)         sensor missing, retrying
//   slow blink (1 Hz)         sensor streaming
#pragma once

void statusLedBegin(bool bootBlinks = true);  // pin setup + the 3 boot blinks (blocks ~600 ms)
void statusLedUpdate(bool streaming);         // call on every loop() pass
void statusLedOff();                          // LED dark (before sleeping)
