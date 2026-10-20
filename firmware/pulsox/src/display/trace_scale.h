// Vertical scale of the pulse trace on the TFT (src/display/display.cpp). Plain C, no Arduino
// dependency, so test/host/trace_host.c can run it on the PC against recorded captures.
//
// The scale used to be the min and max of the whole trace window (2.5 s). A single glitch or a
// finger movement puts a spike of thousands of counts in a trace whose pulse is a few hundred:
// the scale opened to fit the spike and the real wave shrank to a flat line until the spike
// scrolled out of the window.
//
// Now the scale is the MEDIAN of the last TRACE_SCALE_BLOCKS seconds, each one summarised by
// its span (max - min) and its middle. A median ignores up to half of the blocks that are
// bad, so a spike or a movement is drawn clipped against the edge of the plot instead of
// shrinking the wave, while a real and lasting change of amplitude is followed in a few
// seconds.
//
// Per column of the trace (trace_scale_push), per frame (trace_scale_update).
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Columns of one block: 1 s of trace at DISPLAY_PPG_PX_PER_S (48 px/s). Long enough to hold at
// least one beat from 60 bpm up.
#ifndef TRACE_SCALE_BLOCK_COLS
#define TRACE_SCALE_BLOCK_COLS 48
#endif
// Blocks the median looks at.
#ifndef TRACE_SCALE_BLOCKS
#define TRACE_SCALE_BLOCKS 5
#endif
// Part of the distance to the target covered each frame (40 ms): about 0.2 s to settle.
#ifndef TRACE_SCALE_SMOOTH
#define TRACE_SCALE_SMOOTH 0.2f
#endif
// The scale is this much wider than the typical span, so that the beats a bit taller than the
// typical one still fit instead of being clipped flat at the edge of the plot. Measured on the
// captures with test/host/trace_host.c: 1.1 clips noisy captures too often (up to 3 % of the columns),
// 1.2 or more makes the wave about 10 % smaller than the old scale drew it.
#ifndef TRACE_SCALE_HEADROOM
#define TRACE_SCALE_HEADROOM 1.15f
#endif

typedef struct {
  float blk_lo, blk_hi;  // block in progress
  uint8_t blk_n;         // columns in it
  float span[TRACE_SCALE_BLOCKS], mid[TRACE_SCALE_BLOCKS];  // closed blocks, as a ring
  uint8_t head, count;
  float lo, hi;          // the scale being used
  bool valid;
} TraceScale;

// Forget everything: a new trace is starting.
void trace_scale_reset(TraceScale *s);

// One column of the trace (the value that is about to be drawn at the right edge).
void trace_scale_push(TraceScale *s, float column);

// The scale for this frame, in *lo and *hi. win_lo and win_hi are the min and max of the
// whole window, used only until the first block has closed.
void trace_scale_update(TraceScale *s, float win_lo, float win_hi, float *lo, float *hi);

#ifdef __cplusplus
}
#endif
