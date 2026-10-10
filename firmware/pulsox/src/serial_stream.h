// Data output on Serial, with the mode selected by sending one character:
//   's'  summary only: the human-readable "[uptime][TAG]" lines (default)
//   'c'  CSV capture: one "D," line per sample and one "R," line per second (the
//        result of the pipeline), "E," lines for events. Starts with "# D,..." /
//        "# R,..." / "# E,..." header lines naming the columns, which is what
//        tools/capture.py records. The D and R formats live in lib/ppg/ppg_csv.h.
//   'b'  presses the button (buttonInject), to test the wake / start flow without the hardware
//   'g'  corrupts the next sample read (test builds only), to test the driver's integrity check
// Data lines start with 'D,' / 'R,' / 'E,' and log lines with '[', so a parser can
// tell them apart without any framing.
#pragma once
#include <stdint.h>
#include "ppg_processor.h"
#include "ppg_types.h"

enum class StreamMode : uint8_t { SUMMARY, CSV };

// Bounds how long a Serial write can block (SERIAL_TX_TIMEOUT_MS), so the loop never
// stalls when no monitor is reading (bytes are dropped instead; the sample index `n`
// exposes any gap).
void streamBegin();

// Reads the single-character commands sent by the host. Call on every loop() pass.
void streamPollCommands();

StreamMode streamMode();

// True once after the host sent 'g': the next sample read from the sensor is to be corrupted, to
// test that the driver catches it (only test builds, PULSOX_TEST_HOOKS, act on it).
bool streamTakeGlitchRequest();

// One raw sample and what the pipeline made of it. `n` counts samples since the
// sensor was started.
void streamSample(uint32_t n, const PpgSample &s, const PpgDebug &d);

// The pipeline result, once per second.
void streamResult(uint32_t n, const PpgOutput &o);

// Something worth marking in the capture (e.g. "fifo_overflow").
void streamEvent(uint32_t n, const char *name, uint32_t value);
