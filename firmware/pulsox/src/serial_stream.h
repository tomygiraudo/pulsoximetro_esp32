// Data output on Serial, with the mode selected by sending one character:
//   's'  summary only: the human-readable "[uptime][TAG]" lines (default)
//   'c'  CSV capture: one "D," line per sample, "E," lines for events. Starts
//        with "# D,..." / "# E,..." header lines naming the columns, which is
//        what tools/capture.py records.
// Data lines start with 'D,' / 'E,' and log lines with '[', so a parser can
// tell them apart without any framing.
#pragma once
#include <stdint.h>
#include "ppg_types.h"

enum class StreamMode : uint8_t { SUMMARY, CSV };

// Makes Serial writes non-blocking, so the loop never stalls when no monitor
// is attached (bytes are dropped instead; the sample index `n` exposes any gap).
void streamBegin();

// Reads the single-character commands sent by the host. Call on every loop() pass.
void streamPollCommands();

StreamMode streamMode();

// One raw sample. `n` counts samples since the sensor was started.
void streamSample(uint32_t n, const PpgSample &s);

// Something worth marking in the capture (e.g. "fifo_overflow").
void streamEvent(uint32_t n, const char *name, uint32_t value);
