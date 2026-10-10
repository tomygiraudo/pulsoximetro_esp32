// Text format of the PPG stream, shared by the firmware (src/serial_stream.cpp, over
// Serial) and by the host tool (tools/ppg_host.c, to files), so both produce the
// same CSV and tools/validate_ppg.py reads either.
//
//   D  one line per sample
//   R  one line per second, with the result of the pipeline
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "ppg_processor.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PPG_CSV_D_HEADER "n,red,ir,red_ac,ir_ac,red_dc,ir_dc,ir_view,beat_thr,beat,finger"
#define PPG_CSV_R_HEADER "n,finger,spo2,spo2_valid,bpm,bpm_valid,r,pi,saturated,quality"

// Each writes "D,..." / "R,..." plus '\n' into buf. Returns the length, or a value
// >= size if it did not fit (as snprintf does).
int ppg_csv_d(char *buf, size_t size, uint32_t n, uint32_t red, uint32_t ir, const PpgDebug *d);
int ppg_csv_r(char *buf, size_t size, uint32_t n, const PpgOutput *o);

#ifdef __cplusplus
}
#endif
