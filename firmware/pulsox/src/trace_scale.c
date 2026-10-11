#include "trace_scale.h"
#include <string.h>

// Median of v[0..n) (n > 0), on a copy.
static float median(const float *v, int n) {
  float s[TRACE_SCALE_BLOCKS];
  for (int i = 0; i < n; i++) s[i] = v[i];
  for (int i = 1; i < n; i++) {  // insertion sort
    float x = s[i];
    int j = i - 1;
    while (j >= 0 && s[j] > x) {
      s[j + 1] = s[j];
      j--;
    }
    s[j + 1] = x;
  }
  return (n & 1) ? s[n / 2] : 0.5f * (s[n / 2 - 1] + s[n / 2]);
}

void trace_scale_reset(TraceScale *s) { memset(s, 0, sizeof(*s)); }

void trace_scale_push(TraceScale *s, float column) {
  if (s->blk_n == 0) {
    s->blk_lo = column;
    s->blk_hi = column;
  } else {
    if (column < s->blk_lo) s->blk_lo = column;
    if (column > s->blk_hi) s->blk_hi = column;
  }
  if (++s->blk_n < TRACE_SCALE_BLOCK_COLS) return;

  s->span[s->head] = s->blk_hi - s->blk_lo;
  s->mid[s->head] = 0.5f * (s->blk_hi + s->blk_lo);
  s->head = (uint8_t)((s->head + 1) % TRACE_SCALE_BLOCKS);
  if (s->count < TRACE_SCALE_BLOCKS) s->count++;
  s->blk_n = 0;
}

void trace_scale_update(TraceScale *s, float win_lo, float win_hi, float *lo, float *hi) {
  if (s->count == 0) {
    // Not one second of trace yet: the whole window, as is, so that nothing is left out.
    s->lo = win_lo;
    s->hi = win_hi;
    s->valid = true;
  } else {
    const float span = median(s->span, s->count) * TRACE_SCALE_HEADROOM;
    const float mid = median(s->mid, s->count);
    const float target_lo = mid - 0.5f * span, target_hi = mid + 0.5f * span;
    if (!s->valid) {
      s->lo = target_lo;
      s->hi = target_hi;
      s->valid = true;
    } else {
      s->lo += (target_lo - s->lo) * TRACE_SCALE_SMOOTH;
      s->hi += (target_hi - s->hi) * TRACE_SCALE_SMOOTH;
    }
  }
  *lo = s->lo;
  *hi = s->hi;
}
