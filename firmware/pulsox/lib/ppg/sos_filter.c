#include "sos_filter.h"

void sos_init(SosFilter *f, const SosSection *sec, int n) {
  f->sec = sec;
  f->n = n;
  for (int k = 0; k < SOS_MAX_SECTIONS; k++) f->z[k][0] = f->z[k][1] = 0.0;
}

void sos_prime(SosFilter *f, double x0) {
  double x = x0;
  for (int k = 0; k < f->n; k++) {
    const SosSection *s = &f->sec[k];
    // Constant input x -> constant output y = H(1) x; the states follow from the recurrence.
    double y = x * (s->b0 + s->b1 + s->b2) / (1.0 + s->a1 + s->a2);
    f->z[k][1] = s->b2 * x - s->a2 * y;
    f->z[k][0] = y - s->b0 * x;
    x = y;
  }
}

double sos_step(SosFilter *f, double x) {
  for (int k = 0; k < f->n; k++) {
    const SosSection *s = &f->sec[k];
    double *z = f->z[k];
    double y = s->b0 * x + z[0];
    z[0] = s->b1 * x - s->a1 * y + z[1];
    z[1] = s->b2 * x - s->a2 * y;
    x = y;
  }
  return x;
}
