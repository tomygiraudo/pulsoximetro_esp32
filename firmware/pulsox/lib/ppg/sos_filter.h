// Cascade of second-order sections (biquads), direct form II transposed, in
// double: the same structure and arithmetic as scipy.signal.sosfilt. Double rather
// than float because the 0.2 Hz low-pass at 100 Hz has poles very close to the unit
// circle, where float loses accuracy; the ESP32-C3 has no FPU, but at 100 samples/s
// the cost is small either way.
// Plain C, no Arduino dependency.
#pragma once
#ifdef __cplusplus
extern "C" {
#endif

#define SOS_MAX_SECTIONS 4

// One section, a0 normalized to 1: y = b0 x + z0; z0 = b1 x - a1 y + z1; z1 = b2 x - a2 y.
typedef struct {
  double b0, b1, b2, a1, a2;
} SosSection;

typedef struct {
  const SosSection *sec;
  int n;
  double z[SOS_MAX_SECTIONS][2];
} SosFilter;

// Binds the filter to `n` sections (not copied: they must outlive the filter) and
// clears its state.
void sos_init(SosFilter *f, const SosSection *sec, int n);

// Sets the state the filter would have after being fed `x0` forever (scipy's
// sosfilt_zi * x0), so the first sample does not produce a step transient.
void sos_prime(SosFilter *f, double x0);

double sos_step(SosFilter *f, double x);

#ifdef __cplusplus
}
#endif
