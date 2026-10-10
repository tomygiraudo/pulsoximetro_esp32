// =============================================================================
//  tft_st7735.h
//  Pantalla TFT de 1.44" (128 x 128 px, controlador ST7735S) de la PCB PulsOx.
// =============================================================================
//
//  QUÉ ES
//  Un único archivo (solo .h) con todo lo necesario para usar la pantalla en
//  cualquier firmware de este repositorio:
//    1. La configuración (pines, orden de init, velocidad del SPI, calibración
//       de la imagen). Todo se puede pisar desde el config.h del firmware o con
//       -D en build_flags, porque cada valor está protegido con #ifndef.
//    2. La secuencia de inicialización, validada en la placa.
//    3. Unas pocas funciones de ayuda (texto y gráfico de una señal).
//    4. Este instructivo para armar pantallas y gráficos.
//
//  ESTADO DE VALIDACIÓN
//  La secuencia de init (tftBegin), el SPI por hardware, los pines y el ajuste
//  del origen (TFT_X_ADJUST / TFT_Y_ADJUST) están validados en hardware, con el
//  mismo código que corre en firmware/peripherals-test. Las funciones tftPrintAt
//  y la clase TftTrace se escribieron a partir de ese código y COMPILAN, pero
//  todavía no se probaron en la placa. Más detalles en
//  firmware/peripherals-test/TFT.md.
//
// -----------------------------------------------------------------------------
//  1. CÓMO USARLO EN UN FIRMWARE
// -----------------------------------------------------------------------------
//  a) Esta carpeta (lib/tft_st7735/) ya está donde PlatformIO busca librerías
//     propias; no hay que registrar nada.
//
//  b) En el platformio.ini del firmware agregar las dos librerías de Adafruit
//     (este archivo las necesita, pero PlatformIO no las descarga solo):
//
//         lib_deps =
//             adafruit/Adafruit GFX Library@^1.12.6
//             adafruit/Adafruit ST7735 and ST7789 Library@^1.11.0
//
//  c) Si el firmware ya define estos pines u otros valores en su config.h,
//     incluir config.h ANTES que este archivo: lo que ya esté definido se respeta.
//
//  d) Ejemplo mínimo (main.cpp):
//
//         #include <Arduino.h>
//         #include "tft_st7735.h"
//
//         void setup() {
//           tftBegin();                                  // inicializa la pantalla
//           tftScreen().drawRect(0, 0, TFT_W, TFT_H, ST77XX_WHITE);  // marco de 1 px
//           tftPrintAt(8, 8, "PulsOx", 2);               // texto tamaño 2
//         }
//
//         void loop() {
//           static uint32_t contador = 0;                // se incrementa en cada vuelta
//           char texto[16];                              // buffer del número a mostrar
//           snprintf(texto, sizeof(texto), "%6lu", (unsigned long)contador++);
//           tftPrintAt(8, 40, texto, 2);                 // ancho fijo: no deja restos
//           delay(100);
//         }
//
//  e) Llamar a tftBegin() UNA sola vez, después de que arranque Arduino (en
//     setup(), no en un constructor global).
//
// -----------------------------------------------------------------------------
//  2. INSTRUCTIVO PARA ARMAR LOS GRÁFICOS
// -----------------------------------------------------------------------------
//  Todo lo de la librería Adafruit GFX está disponible a través de tftScreen().
//  Lo que sigue es lo que más se usa.
//
//  2.1 Coordenadas
//    - La pantalla tiene 128 x 128 píxeles (TFT_W x TFT_H).
//    - El origen (0, 0) es la esquina SUPERIOR IZQUIERDA. x crece hacia la
//      derecha e y crece hacia ABAJO. El píxel de abajo a la derecha es (127, 127).
//    - Lo que se dibuja fuera de la pantalla se recorta, no da error.
//
//  2.2 Colores (RGB565, 16 bits por píxel)
//    - Un color es un número de 16 bits: 5 bits de rojo, 6 de verde y 5 de azul.
//    - Constantes listas: ST77XX_BLACK, ST77XX_WHITE, ST77XX_RED, ST77XX_GREEN,
//      ST77XX_BLUE, ST77XX_CYAN, ST77XX_MAGENTA, ST77XX_YELLOW, ST77XX_ORANGE,
//      y TFT_GRAY (definida más abajo).
//    - Para un color propio:  uint16_t c = tftScreen().color565(r, g, b);
//      con r, g, b entre 0 y 255.  Ejemplo: color565(255, 128, 0) es naranja.
//
//  2.3 Texto
//    - La fuente por defecto ocupa una celda de 6 x 8 px por carácter a tamaño 1.
//      A tamaño n la celda es de (6n) x (8n) px. En 128 px caben:
//          tamaño 1: 21 caracteres por línea, 16 líneas
//          tamaño 2: 10 caracteres por línea,  8 líneas
//          tamaño 3:  7 caracteres por línea,  5 líneas
//    - tftPrintAt() escribe con FONDO OPACO: pinta el fondo de cada carácter, así
//      que el texto nuevo tapa al anterior sin borrar la pantalla antes. Eso evita
//      el parpadeo. Condición: el texto nuevo debe ocupar al menos tantos
//      caracteres como el anterior. Para números usar ancho fijo ("%6lu") o
//      rellenar con espacios; si no, quedan restos del valor viejo.
//    - Un texto que se pasa del borde derecho salta a la línea siguiente. Para
//      cortarlo en cambio:  tftScreen().setTextWrap(false);
//    - Para centrar un texto: tftScreen().getTextBounds(texto, 0, 0, &x1, &y1, &w, &h)
//      devuelve el ancho y alto que ocupará, y de ahí se calcula la posición.
//
//  2.4 Formas (todas con coordenadas en píxeles y un color RGB565)
//      drawPixel(x, y, color)                       un punto
//      drawLine(x0, y0, x1, y1, color)              una línea
//      drawFastHLine(x, y, largo, color)            línea horizontal (más rápida)
//      drawFastVLine(x, y, largo, color)            línea vertical (más rápida)
//      drawRect(x, y, ancho, alto, color)           rectángulo (solo el borde)
//      fillRect(x, y, ancho, alto, color)           rectángulo relleno
//      drawRoundRect / fillRoundRect                igual, con esquinas redondeadas
//      drawCircle(x, y, radio, color)               círculo (solo el borde)
//      fillCircle(x, y, radio, color)               círculo relleno
//      drawTriangle / fillTriangle                  triángulos
//      fillScreen(color)                            toda la pantalla de un color
//
//  2.5 Gráficos sin parpadeo
//    - NO hacer fillScreen() y redibujar todo en cada ciclo: se ve parpadear.
//    - Texto que cambia: usar tftPrintAt() (fondo opaco), como se explicó arriba.
//    - Una señal que se mueve (por ejemplo la onda del pulso): dibujarla primero
//      en un lienzo en RAM y mandarlo a la pantalla de una sola vez. La clase
//      TftTrace hace exactamente eso:
//
//          TftTrace onda(0, 56, TFT_W, 72);   // x, y, ancho, alto de la zona
//          ...
//          onda.push(valorNuevo);              // en cada muestra que llega
//          ...
//          onda.draw();                        // cada ~100 ms, no en cada muestra
//
//      Cuesta ancho x alto x 2 bytes de RAM (128 x 72 -> 18 KB). Si falta memoria,
//      usar una zona más chica.
//
//  2.6 Diagramación de la pantalla
//    - Pensar la pantalla como zonas fijas y repintar solo la que cambió. La
//      diagramación validada en peripherals-test (modo en vivo) es:
//          y =  0..15   valor 1, texto tamaño 2   (ej. "IR  123456")
//          y = 18..33   valor 2, texto tamaño 2   (ej. "RED 123456")
//          y = 36..51   estado,  texto tamaño 2   (ej. "DEDO" / "SIN DEDO")
//          y = 54       línea separadora gris (drawFastHLine)
//          y = 56..127  gráfico de la señal (TftTrace de 128 x 72)
//    - Dibujar los elementos que nunca cambian (separadores, rótulos) una sola
//      vez, después de tftBegin(), y no en cada vuelta del loop().
//
//  2.7 Rendimiento y reglas
//    - Las funciones de dibujo BLOQUEAN hasta terminar de mandar los datos por
//      SPI. Estimado teórico a 10 MHz, no medido: 128 x 72 px ≈ 15 ms y la
//      pantalla completa ≈ 26 ms.
//    - Si el firmware lee un sensor con FIFO (el MAX30102 de pulsox guarda 320 ms
//      de datos a 100 muestras/s), mantener el refresco de pantalla en 100 ms o
//      más y no bloquear el loop() más tiempo que el que aguanta el FIFO.
//    - Dibujar siempre desde el loop() o una tarea, nunca desde una interrupción.
//    - Usar el SPI por hardware (el que configura tftBegin). El SPI por software
//      es demasiado lento para esto.
//
//  2.8 Calibrar la imagen si cambia el módulo
//    - Dibujar un marco de 1 px:  tftScreen().drawRect(0, 0, TFT_W, TFT_H, ST77XX_WHITE);
//      Las cuatro líneas deben verse completas y no debe haber ruido en los bordes.
//    - Franja de ruido abajo (y falta la línea de arriba): subir TFT_Y_ADJUST.
//      Franja a la derecha: subir TFT_X_ADJUST. Si empeora, usar valores negativos.
//    - Los valores actuales (-2, +29) son de ESTE módulo. Con otro módulo, o con
//      una rotación distinta de 0, hay que recalibrar.
//
//  2.9 Qué todavía no está resuelto
//    - Light sleep / deep sleep con la pantalla: no probado. La idea es apagar el
//      backlight con tftBacklight(false) y poner el controlador en reposo con
//      tftScreen().enableSleep(true) antes de dormir; falta validarlo y medir el
//      consumo.
//    - SPI por encima de 10 MHz durante el dibujado: no probado.
//
// =============================================================================

#pragma once

#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>

// =============================================================================
//  CONFIGURACIÓN
//  Cada valor se puede pisar desde el config.h del firmware (incluido antes que
//  este archivo) o con -D en build_flags, porque está protegido con #ifndef.
// =============================================================================

// ---- Pines (PCB/Sensor_ctrl_pwr_pcb, esquemático V2, conector J2) ----------

#ifndef PIN_SCK_TFT
#define PIN_SCK_TFT 6   // Reloj del SPI (SCK del módulo, J2 pin 7).
#endif
#ifndef PIN_SDA_TFT
#define PIN_SDA_TFT 7   // Datos del SPI (MOSI; es el SDA del módulo, J2 pin 6).
#endif
#ifndef PIN_CS_TFT
#define PIN_CS_TFT 21   // Chip select (J2 pin 3). La librería lo maneja por software.
#endif
#ifndef PIN_DC_TFT
#define PIN_DC_TFT 10   // Dato/comando, llamado A0 en el módulo (J2 pin 5).
#endif
#ifndef PIN_RST_TFT
#define PIN_RST_TFT 20  // Reset por hardware del controlador (J2 pin 4).
#endif
#ifndef PIN_BL_TFT
#define PIN_BL_TFT 5    // Backlight, pin LED del módulo (J2 pin 8).
#endif

#ifndef TFT_BL_ON
#define TFT_BL_ON HIGH  // Nivel que ENCIENDE el backlight (HIGH = activo en alto).
#endif
#ifndef TFT_BL_ON_AT_BEGIN
#define TFT_BL_ON_AT_BEGIN 1  // 1: tftBegin() enciende el backlight al empezar (secuencia validada).
                              // 0: lo deja apagado; quien llama lo enciende con tftBacklight(true)
                              // cuando ya dibujó algo (evita el destello de la RAM del controlador).
#endif

// ---- Pantalla ---------------------------------------------------------------

#ifndef TFT_W
#define TFT_W 128       // Ancho de la pantalla en píxeles (con rotación 0).
#endif
#ifndef TFT_H
#define TFT_H 128       // Alto de la pantalla en píxeles (con rotación 0).
#endif
#ifndef TFT_INIT_TAB
#define TFT_INIT_TAB INITR_144GREENTAB  // Variante de init de la librería para 1.44" 128x128.
#endif
#ifndef TFT_ROTATION
#define TFT_ROTATION 0  // Orientación 0..3. Los ajustes de abajo solo están calibrados para 0.
#endif

// ---- SPI --------------------------------------------------------------------

#ifndef TFT_SPI_HZ
#define TFT_SPI_HZ 10000000  // Velocidad del SPI al DIBUJAR (10 MHz). El init sale a 32 MHz fijos.
#endif

// ---- Calibración de la imagen (propia de ESTE módulo) -----------------------
// La RAM del ST7735 es más grande que la pantalla visible; la librería asume que
// la parte visible empieza en (columna 2, fila 3) y este módulo empieza en (0, 32).
// Estos dos valores son la diferencia: 2 + (-2) = 0 y 3 + 29 = 32.

#ifndef TFT_X_ADJUST
#define TFT_X_ADJUST -2  // Corrimiento del origen en columnas (negativo = hacia la izquierda).
#endif
#ifndef TFT_Y_ADJUST
#define TFT_Y_ADJUST 29  // Corrimiento del origen en filas (positivo = baja la imagen).
#endif

// ---- Colores extra ----------------------------------------------------------
// Los demás colores (ST77XX_*) los trae la librería de Adafruit.

#ifndef TFT_GRAY
#define TFT_GRAY 0x7BEF  // Gris medio en RGB565, para separadores y texto secundario.
#endif

// =============================================================================
//  PANTALLA
// =============================================================================

// Pantalla ST7735 de Adafruit más la posibilidad de mover el origen de la imagen.
// Hereda todos los métodos de Adafruit_ST7735 / Adafruit_GFX (texto, formas, etc.).
class TftPanel : public Adafruit_ST7735 {
 public:
  // Reutiliza los constructores de Adafruit_ST7735 (se usa el de SPI por hardware).
  using Adafruit_ST7735::Adafruit_ST7735;

  // Mueve el origen de la imagen dentro de la RAM del controlador.
  //   dx: columnas a sumar al origen (negativo = izquierda).
  //   dy: filas a sumar al origen (positivo = baja la imagen).
  // Hay que llamarlo DESPUÉS de setRotation(), que recalcula el origen con los
  // valores que trae la librería. Los campos _xstart y _ystart son protegidos, por
  // eso hace falta esta subclase.
  void nudgeOrigin(int8_t dx, int8_t dy) {
    _xstart += dx;  // Origen horizontal ya con la rotación aplicada, más el corrimiento.
    _ystart += dy;  // Origen vertical ya con la rotación aplicada, más el corrimiento.
  }
};

// Devuelve LA pantalla (siempre la misma, en cualquier archivo .cpp que incluya
// este .h). Se crea la primera vez que se la pide. Con este objeto se usan todas
// las funciones de dibujo:  tftScreen().fillCircle(64, 64, 10, ST77XX_RED);
inline TftPanel &tftScreen() {
  // Única instancia de la pantalla: SPI por hardware, CS, DC y RST por GPIO.
  static TftPanel panel(&SPI, PIN_CS_TFT, PIN_DC_TFT, PIN_RST_TFT);
  return panel;
}

// Inicializa la pantalla con la secuencia validada en la placa. Llamar UNA vez,
// desde setup(). Deja la pantalla en negro y el backlight encendido (o apagado si
// TFT_BL_ON_AT_BEGIN es 0).
// Secuencia: backlight -> SPI -> initR (a 32 MHz) -> baja a 10 MHz -> rotación ->
// calibración del origen -> pantalla en negro.
inline void tftBegin() {
  pinMode(PIN_BL_TFT, OUTPUT);                  // El pin del backlight es una salida.
  digitalWrite(PIN_BL_TFT, TFT_BL_ON_AT_BEGIN ? TFT_BL_ON : !TFT_BL_ON);  // Backlight al empezar.
  SPI.begin(PIN_SCK_TFT, -1, PIN_SDA_TFT, -1);  // Sin MISO ni CS por hardware. El aviso
                                                // "SPI Does not have default pins" es inofensivo.
  TftPanel &pantalla = tftScreen();             // La pantalla a inicializar.
  pantalla.initR(TFT_INIT_TAB);                 // Resetea el controlador y manda el init (32 MHz).
  pantalla.setSPISpeed(TFT_SPI_HZ);             // De acá en adelante se dibuja a TFT_SPI_HZ.
  pantalla.setRotation(TFT_ROTATION);           // Orientación de la imagen.
  pantalla.nudgeOrigin(TFT_X_ADJUST, TFT_Y_ADJUST);  // Centra la imagen (siempre después de setRotation).
  pantalla.fillScreen(ST77XX_BLACK);            // Empieza en negro.
}

// Enciende o apaga el backlight (la pantalla sigue funcionando, solo se ve negra).
//   encendido: true = prender, false = apagar.
inline void tftBacklight(bool encendido) {
  // Si TFT_BL_ON es HIGH, encender es HIGH; si fuera activo en bajo, se invierte.
  digitalWrite(PIN_BL_TFT, encendido ? TFT_BL_ON : !TFT_BL_ON);
}

// Escribe un texto con fondo opaco (no parpadea; ver 2.3 del instructivo).
//   x, y:   posición de la esquina superior izquierda del texto, en píxeles.
//   texto:  cadena terminada en cero.
//   tamano: factor de escala de la fuente (1 = 6x8 px por carácter, 2 = 12x16, ...).
//   color:  color de las letras (RGB565).
//   fondo:  color del fondo de cada carácter (RGB565).
inline void tftPrintAt(int16_t x, int16_t y, const char *texto, uint8_t tamano = 1,
                       uint16_t color = ST77XX_WHITE, uint16_t fondo = ST77XX_BLACK) {
  TftPanel &pantalla = tftScreen();     // La pantalla donde se escribe.
  pantalla.setTextSize(tamano);         // Escala de la fuente.
  pantalla.setTextColor(color, fondo);  // Letras y fondo opaco.
  pantalla.setCursor(x, y);             // Dónde empieza el texto.
  pantalla.print(texto);                // Escribe.
}

// =============================================================================
//  GRÁFICO DE UNA SEÑAL
// =============================================================================

// Gráfico de una señal que se desplaza de derecha a izquierda, con autoescala.
// Guarda las últimas "ancho" muestras (una por columna de píxeles), las dibuja en
// un lienzo en RAM y las manda a la pantalla de una sola vez (sin parpadeo).
//
// Uso:
//     TftTrace onda(0, 56, TFT_W, 72);   // zona: x, y, ancho, alto
//     onda.push(muestra);                // por cada muestra nueva (barato)
//     onda.draw();                       // cada ~100 ms (manda ~18 KB por SPI)
//
// La escala vertical se ajusta sola al mínimo y máximo de lo que hay en el
// historial. Si la señal casi no varía, se mantiene un rango mínimo (spanMin)
// para que el ruido no se agrande hasta ocupar todo el alto.
class TftTrace {
 public:
  // Crea el gráfico. Reserva ancho x alto x 2 bytes de RAM para el lienzo.
  //   x, y:    esquina superior izquierda de la zona en la pantalla, en píxeles.
  //   ancho:   ancho de la zona; también es la cantidad de muestras que se
  //            guardan (entre 1 y TFT_W; si es mayor se recorta a TFT_W).
  //   alto:    alto de la zona en píxeles.
  //   color:   color de la línea (RGB565).
  //   spanMin: rango vertical mínimo, en las mismas unidades que las muestras.
  TftTrace(int16_t x, int16_t y, int16_t ancho, int16_t alto, uint16_t color = ST77XX_CYAN,
           float spanMin = 256.0f)
      : x_(x),
        y_(y),
        w_(ancho > TFT_W ? TFT_W : ancho),
        h_(alto),
        color_(color),
        spanMin_(spanMin),
        lienzo_(ancho > TFT_W ? TFT_W : ancho, alto) {}

  // Agrega una muestra nueva (la más vieja se descarta). La primera muestra
  // llena todo el historial con su valor, para que el gráfico empiece plano y no
  // en cero.
  //   valor: la muestra, en cualquier unidad (cuentas del sensor, señal filtrada...).
  void push(float valor) {
    if (!cargado_) {                                          // Primera muestra desde el inicio o reset().
      for (uint16_t i = 0; i < w_; i++) historial_[i] = valor;  // i: columna del historial.
      cargado_ = true;
    }
    historial_[cabeza_] = valor;       // Pisa la muestra más vieja.
    cabeza_ = (cabeza_ + 1) % w_;      // La próxima más vieja es la siguiente.
  }

  // Olvida el historial. La próxima muestra vuelve a llenarlo con su valor.
  void reset() {
    cargado_ = false;  // Marca el historial como vacío.
    cabeza_ = 0;       // Vuelve al principio del buffer circular.
  }

  // Redibuja el gráfico en la pantalla. No hace nada si todavía no hay muestras
  // o si no alcanzó la RAM para el lienzo. Bloquea mientras manda los datos.
  void draw() {
    if (!cargado_ || lienzo_.getBuffer() == nullptr) return;

    float minimo = historial_[0];  // Muestra más chica del historial.
    float maximo = historial_[0];  // Muestra más grande del historial.
    for (uint16_t i = 1; i < w_; i++) {  // i: columna del historial.
      if (historial_[i] < minimo) minimo = historial_[i];
      if (historial_[i] > maximo) maximo = historial_[i];
    }
    if (maximo - minimo < spanMin_) {  // Señal casi plana: forzar un rango mínimo.
      float medio = (maximo + minimo) / 2.0f;  // Centro del rango, para dejar la señal en el medio.
      minimo = medio - spanMin_ / 2.0f;
      maximo = minimo + spanMin_;
    }

    lienzo_.fillScreen(ST77XX_BLACK);  // Borra el lienzo (la pantalla real no se toca todavía).
    int16_t yAnterior = 0;             // Fila de la muestra anterior, para unir con una línea.
    for (uint16_t col = 0; col < w_; col++) {  // col: columna del lienzo, de izquierda a derecha.
      // La muestra más vieja (cabeza_) va a la izquierda y la más nueva a la derecha.
      float valor = historial_[(cabeza_ + col) % w_];  // Muestra que corresponde a esta columna.
      // Fila 0 es arriba: el valor máximo va arriba y el mínimo abajo.
      int16_t fila = (h_ - 1) - (int16_t)(((valor - minimo) * (h_ - 1)) / (maximo - minimo));
      if (fila < 0) fila = 0;              // Por redondeo, nunca salir del lienzo.
      if (fila > h_ - 1) fila = h_ - 1;
      if (col > 0) lienzo_.drawLine(col - 1, yAnterior, col, fila, color_);
      yAnterior = fila;
    }
    // Manda el lienzo completo a la pantalla en una sola transferencia.
    tftScreen().drawRGBBitmap(x_, y_, lienzo_.getBuffer(), w_, h_);
  }

 private:
  int16_t x_;             // Posición horizontal de la zona en la pantalla.
  int16_t y_;             // Posición vertical de la zona en la pantalla.
  int16_t w_;             // Ancho de la zona = cantidad de muestras guardadas.
  int16_t h_;             // Alto de la zona.
  uint16_t color_;        // Color de la línea.
  float spanMin_;         // Rango vertical mínimo (evita agrandar el ruido).
  GFXcanvas16 lienzo_;    // Lienzo en RAM donde se arma el dibujo antes de enviarlo.
  float historial_[TFT_W];  // Últimas muestras (buffer circular, una por columna).
  uint16_t cabeza_ = 0;   // Posición de la muestra más vieja, que es la próxima en pisarse.
  bool cargado_ = false;  // true cuando el historial ya tiene muestras.
};
