#include "ppg_csv.h"
#include <stdio.h>

int ppg_csv_d(char *buf, size_t size, uint32_t n, uint32_t red, uint32_t ir, const PpgDebug *d) {
  return snprintf(buf, size, "D,%lu,%lu,%lu,%.3f,%.3f,%.2f,%.2f,%.3f,%.3f,%d,%d\n", (unsigned long)n,
                  (unsigned long)red, (unsigned long)ir, (double)d->red_ac, (double)d->ir_ac,
                  (double)d->red_dc, (double)d->ir_dc, (double)d->ir_view, (double)d->beat_thr,
                  d->beat ? 1 : 0, d->finger ? 1 : 0);
}

int ppg_csv_r(char *buf, size_t size, uint32_t n, const PpgOutput *o) {
  return snprintf(buf, size, "R,%lu,%d,%.0f,%d,%.2f,%d,%.5f,%.4f,%d,%.3f\n", (unsigned long)n,
                  o->finger ? 1 : 0, (double)o->spo2, o->spo2_valid ? 1 : 0, (double)o->bpm,
                  o->bpm_valid ? 1 : 0, (double)o->r_ratio, (double)o->perfusion_index,
                  o->saturated ? 1 : 0, (double)o->quality);
}
