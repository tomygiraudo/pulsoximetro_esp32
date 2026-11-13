#include "sos_filter.h"

/*******************************************************************************
 * @brief       Asocia el filtro a sus secciones y borra su estado.
 *
 * @details     Guarda el puntero a las n secciones (no las copia: deben vivir
 *              más que el filtro) y pone a cero el estado de todas las
 *              secciones.
 *
 * @param       Entrada: sec - secciones biquad (coeficientes).
 *              Entrada: n - cantidad de secciones en uso.
 *              Salida:  f - filtro enlazado y con estado en cero.
 *
 * @peripherals Ninguno.
 *******************************************************************************/
void sos_init(SosFilter *f, const SosSection *sec, int n) {
  f->sec = sec;
  f->n = n;
  for (int k = 0; k < SOS_MAX_SECTIONS; k++) f->z[k][0] = f->z[k][1] = 0.0;
}

/*******************************************************************************
 * @brief       Precarga el estado como si el filtro hubiera recibido x0 desde
 *              siempre.
 *
 * @details     Por sección calcula la salida de régimen y = x * H(1) y de ahí
 *              los estados (equivale a sosfilt_zi * x0 de scipy), así la
 *              primera muestra no produce un transitorio de escalón.
 *
 * @param       Entrada: x0 - valor constante de entrada.
 *              Salida:  f - estado de cada sección, precargado.
 *
 * @peripherals Ninguno.
 *******************************************************************************/
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

/*******************************************************************************
 * @brief       Pasa una muestra por la cascada de secciones y devuelve la
 *              salida.
 *
 * @details     Cada sección es un biquad en forma directa II transpuesta; la
 *              salida de una es la entrada de la siguiente.
 *
 * @param       Entrada: x - muestra de entrada.
 *              Salida:  f - actualiza el estado de cada sección.
 *              Salida:  retorno (double) - muestra filtrada.
 *
 * @peripherals Ninguno.
 *******************************************************************************/
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
