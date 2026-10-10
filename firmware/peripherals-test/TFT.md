# Pantalla TFT ST7735S 128×128: guía de integración

Referencia para llevar la pantalla a otro firmware (por ejemplo `firmware/pulsox`).
La secuencia de init y los valores de esta guía están **validados en hardware**
(PCB V2 + ESP32-C3 Super Mini, 2026-10-07): SPI por hardware, patrón visual
completo y gráfico en vivo funcionando. El código de referencia es
[`src/main.cpp`](src/main.cpp) de este proyecto; el diagnóstico paso a paso está
en el [README](README.md).

> **Librería lista para usar:**
> [`firmware/pulsox/lib/tft_st7735/tft_st7735.h`](../pulsox/lib/tft_st7735/tft_st7735.h).
> Es un único `.h` con toda la configuración de esta guía, la secuencia de init,
> funciones de texto y de gráfico de señal, y un instructivo para armar pantallas
> (comentado en español). Para `firmware/pulsox` conviene usarla en lugar de
> copiar el bloque de §4. La secuencia de init y los ajustes son los validados
> acá; `tftPrintAt` y `TftTrace` compilan pero todavía no se probaron en la placa.

## 1. Resumen

| | |
|---|---|
| Módulo | TFT 1.44" 128×128, controlador ST7735S, 8 pines |
| Bus | SPI por hardware, **solo escritura** (el módulo no tiene MISO) |
| Librerías | `Adafruit GFX Library` 1.12.6 + `Adafruit ST7735 and ST7789 Library` 1.11.0 (trae `Adafruit BusIO` 1.17.4) |
| Plataforma | PlatformIO `espressif32` 7.0.1, framework Arduino-ESP32 2.0.17, placa `esp32-c3-devkitm-1` |
| Init | `initR(INITR_144GREENTAB)` |
| Origen de la imagen | desplazado con `nudgeOrigin(-2, +29)` (ver §3) |

Los números de versión son los que estaban instalados cuando se validó. En el
`platformio.ini` las librerías están con `^` y la plataforma sin fijar: si algo
cambia al actualizar, fijá estas versiones.

## 2. Hardware

Pinout del conector **J2** de la PCB (según `Esquematico V2.pdf` /
`Sensor_ctrl_pwr_pcb.kicad_sch`). El módulo tiene que tener el mismo orden:
`VCC GND CS RESET A0 SDA SCK LED`.

| J2 | Señal del módulo | Señal PCB | GPIO | Notas |
|---|---|---|---|---|
| 1 | VCC | +3V3 | | 3.3 V, no sale de un GPIO |
| 2 | GND | GND | | |
| 3 | CS | CS_TFT | **21** | CS manejado por software por la librería |
| 4 | RESET | RST_TFT | **20** | la librería hace el reset por hardware en `initR()` |
| 5 | A0 (DC) | DATA_SEL_TFT | **10** | |
| 6 | SDA (MOSI) | SDA_TFT | **7** | MOSI del SPI |
| 7 | SCK | SCK_TFT | **6** | clock del SPI |
| 8 | LED | BACKLIGHT_TFT | **5** | backlight, **activo en alto** |

- **GPIO20/21 son los pines UART0** del ESP32-C3. Con `-DARDUINO_USB_CDC_ON_BOOT=1`
  el Serial va por USB y no los usa, pero el bootloader ROM puede escribir por
  GPIO21 (CS) al arrancar. Durante las pruebas no se reportó basura en la pantalla
  al encender; si aparece, es lo primero a sospechar.
- Ninguno de estos pines choca con los de `firmware/pulsox/include/config.h`
  (I2C en 3/4, INT en 1, LED en 8).
- GPIO5 y GPIO1 pueden despertar de deep sleep, GPIO6/7/10/20/21 no. No importa
  para el SPI, sí para decidir qué hacer con el backlight al dormir (§7).

## 3. Configuración validada

| Parámetro | Valor | Por qué |
|---|---|---|
| `SPI.begin(sck, miso, mosi, ss)` | `(6, -1, 7, -1)` | sin MISO ni CS por hardware |
| Reloj durante `initR()` | 32 MHz (default fijo de la librería) | funciona en esta placa |
| `setSPISpeed()` | `10000000` (10 MHz), **después** de `initR()` | `initR()` pisa cualquier velocidad anterior |
| `setRotation()` | `0` | |
| `nudgeOrigin(dx, dy)` | `(-2, +29)`, **después** de `setRotation()` | ver abajo |
| Backlight | GPIO5 en `HIGH` | |

**Por qué `nudgeOrigin(-2, +29)`.** El ST7735 tiene una RAM más grande que la
pantalla visible y la librería asume dónde empieza la parte visible. Para
`INITR_144GREENTAB` en rotación 0 asume la columna 2 y la fila 3. Este módulo
muestra la ventana en la **columna 0, fila 32** (`2-2 = 0`, `3+29 = 32`); con los
valores por defecto quedaba una franja de ruido abajo, que era RAM sin escribir.
Es un dato **del módulo, no del firmware**: si cambiás de módulo hay que
recalibrarlo (ver §6.4).

`nudgeOrigin` es una subclase mínima porque `_xstart` / `_ystart` son miembros
protegidos de la librería y `setRotation()` los recalcula, así que el ajuste va
siempre después.

## 4. Cómo integrarlo en otro firmware

1. **`platformio.ini`**:
   ```ini
   lib_deps =
       adafruit/Adafruit GFX Library@^1.12.6
       adafruit/Adafruit ST7735 and ST7789 Library@^1.11.0
   ```
2. **Pines y ajustes** en el `config.h` del firmware destino (misma tabla que §2 y §3).
3. **Código mínimo.** Este bloque compila tal cual en un proyecto con la
   configuración de este README (flash 252 KB, RAM 14 KB, sin warnings) y repite
   la secuencia validada de `testLcd()`:

   ```cpp
   #include <Arduino.h>
   #include <SPI.h>
   #include <Adafruit_GFX.h>
   #include <Adafruit_ST7735.h>

   // Pins (PCB/Sensor_ctrl_pwr_pcb, Esquematico V2)
   #define PIN_SCK_TFT 6   // SPI clock
   #define PIN_SDA_TFT 7   // SPI MOSI (the module's SDA)
   #define PIN_CS_TFT 21
   #define PIN_DC_TFT 10   // A0
   #define PIN_RST_TFT 20
   #define PIN_BL_TFT 5    // backlight (LED pin), active high

   #define TFT_W 128
   #define TFT_H 128
   #define TFT_INIT_TAB INITR_144GREENTAB
   #define TFT_SPI_HZ 10000000
   #define TFT_X_ADJUST -2   // effective origin in controller RAM = (0, 32)
   #define TFT_Y_ADJUST 29

   // Adafruit_ST7735 plus a way to move the image origin (the offsets are protected).
   class TftPanel : public Adafruit_ST7735 {
    public:
     using Adafruit_ST7735::Adafruit_ST7735;
     // Call AFTER setRotation(), which recomputes the origin from the library's offsets.
     void nudgeOrigin(int8_t dx, int8_t dy) {
       _xstart += dx;
       _ystart += dy;
     }
   };

   static TftPanel tft(&SPI, PIN_CS_TFT, PIN_DC_TFT, PIN_RST_TFT);

   static void tftBegin() {
     pinMode(PIN_BL_TFT, OUTPUT);
     digitalWrite(PIN_BL_TFT, HIGH);
     SPI.begin(PIN_SCK_TFT, -1, PIN_SDA_TFT, -1);  // no MISO, no hardware CS
     tft.initR(TFT_INIT_TAB);                      // init commands go out at 32 MHz
     tft.setSPISpeed(TFT_SPI_HZ);                  // drawing at 10 MHz
     tft.setRotation(0);
     tft.nudgeOrigin(TFT_X_ADJUST, TFT_Y_ADJUST);
     tft.fillScreen(ST77XX_BLACK);
   }

   void setup() {
     tftBegin();
     tft.drawRect(0, 0, TFT_W, TFT_H, ST77XX_WHITE);   // 1 px frame: all 4 edges must show
     tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
     tft.setTextSize(2);
     tft.setCursor(8, 8);
     tft.print("PulsOx");
   }

   void loop() {
     static uint32_t n = 0;
     tft.setCursor(8, 40);
     tft.printf("%lu   ", (unsigned long)n++);
     delay(100);
   }
   ```
   Compilar no es lo mismo que probarlo: este bloque en sí no se flasheó, pero
   cada llamada y su orden son los de `testLcd()`, que sí corre en la placa.
4. **Para un firmware con sensor en tiempo real** (como `firmware/pulsox`, que
   lee el FIFO del MAX30102 cada 10 ms y el FIFO guarda 320 ms de datos):
   - **Usá SPI por hardware.** El SPI por software es demasiado lento para esto
     (estimado: una pantalla completa lleva del orden de medio segundo, más que
     el FIFO; no se midió, pero con SPI por hardware refrescó claramente más
     rápido).
   - **Dibujá solo lo que cambia.** Para texto, `setTextColor(fg, bg)` con fondo
     opaco repinta sin borrar antes y no parpadea. Para gráficos, dibujá en un
     `GFXcanvas16` fuera de pantalla y mandalo de una con `drawRGBBitmap()`, como
     hace `drawLive()` (el canvas de 128×72 ocupa 18 KB de RAM).
   - **Presupuesto de tiempo** (cálculo teórico a 10 MHz y 16 bits por píxel, no
     medido): 128×72 px ≈ 15 ms, pantalla completa ≈ 26 ms. Dibujar cada 100 ms
     (`LIVE_REFRESH_MS`) deja margen de sobra respecto de los 320 ms del FIFO.
     Las transferencias bloquean: no las hagas desde una interrupción.

## 5. Qué NO copiar

Esto existe solo para el bring-up, no hace falta en el firmware final:

- **La lectura por bit-bang de `RDDID` / `RDDPM`** (`tftReadReg`) y el
  `SPI.end()` / `SPI.begin()` de `testLcd()`. Este módulo no devuelve datos de
  lectura (siempre `0x00`), así que `LCD readback` queda en `N/D` y la
  verificación es visual.
- **`tftPinCheck()`, `TFT_PIN_TEST` y `TFT_USE_SOFT_SPI`**: herramientas para
  diagnosticar una PCB nueva (ver §6 y el README).
- **`visualPattern()`**: opcional. Es útil para validar un módulo nuevo.

## 6. Lecciones aprendidas

1. **El backlight prendido no prueba nada del SPI.** El pin LED lo maneja GPIO5
   por separado, así que el backlight puede prender con el controlador sin VCC o
   sin recibir datos. Con la pantalla en blanco y el backlight prendido, no lo
   tomes como señal de que el bus anda.
2. **El problema real fue un corto entre SCK y SDA (GPIO6 y GPIO7)**, que ya
   se eliminó. Síntoma: pantalla en blanco con backlight, igual con SPI por
   hardware y por software. Se encontró con `TFT_PIN_TEST 2`, que sube un pin por
   vez: con SCK en alto, SDA daba ~1.77 V (y al revés). El DRC de KiCad no marcó
   nada, así que el diseño estaba bien y el defecto era de fabricación o
   soldadura. El lugar exacto del corto no quedó registrado; el sospechoso más
   probable son los pads SMD de U3, separados por solo 0.74 mm y tapados por el
   Super Mini. Si vuelve a pasar, empezá por ahí.
3. **El init a 32 MHz funciona.** La librería lo manda a 32 MHz y el datasheet del
   ST7735S habla de ~15 MHz, pero con la placa bien soldada anda y el SPI por
   hardware es más rápido y más fluido que el de software. `TFT_USE_SOFT_SPI` quedó
   solo como herramienta para separar "problema de reloj" de "problema de
   cableado".
4. **Calibrar el origen con el marco de 1 px.** Si ves una franja de ruido en un
   borde y falta la línea del marco en el borde opuesto, es el offset de RAM. El
   ruido abajo se corrige subiendo `TFT_Y_ADJUST`; a un costado, `TFT_X_ADJUST`; si
   empeora, usá negativos. En este módulo el valor final fue Y = +29 (no unas
   pocas filas, sino casi toda la diferencia hasta la fila 32).
5. **`[E] spiAttachMISO(): SPI Does not have default pins on ESP32C3!`** en el
   Serial es inofensivo: el módulo no tiene MISO (`-1`) y el core lo avisa. No
   afecta la escritura.
6. **No definas macros llamadas `TFT_SOFT_SPI`, `TFT_HARD_SPI` ni `TFT_PARALLEL`.**
   La librería de Adafruit las define dentro de su `.cpp` y chocan con un `-D`
   tuyo. Por eso el flag del proyecto se llama `TFT_USE_SOFT_SPI`.

## 7. Pendiente / no probado

- **Light sleep / deep sleep con la pantalla.** No se probó. Antes de dormir
  habría que apagar el backlight (GPIO5 en `LOW`), y la librería trae
  `tft.enableSleep(true)` para el controlador. Falta medir si alcanza y cuánto
  consume.
- **Consumo** de la pantalla y del backlight: no medido.
- **SPI por encima de 10 MHz** durante el dibujo: no probado.
- **Orientaciones distintas de 0.** Los offsets de la librería cambian con la
  rotación; si usás otra, hay que recalibrar `nudgeOrigin`.
- **Módulos distintos.** Los offsets (§3) son de este módulo en particular.
