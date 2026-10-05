# Test de periféricos (MAX30102 + TFT ST7735S)

Firmware de bring-up para la PCB (`PCB/Sensor_ctrl_pwr_pcb`) con el ESP32-C3
Super Mini. **No es el firmware final de PulsOx**: solo verifica que el
sensor responda por I2C y que la pantalla responda por SPI, y después deja
corriendo una lectura en vivo del sensor.

## Pines (del esquemático)

| Señal | GPIO | Uso |
|---|---|---|
| SDA-OX | 3 | I2C SDA del MAX30102 |
| SCL-OX | 4 | I2C SCL del MAX30102 |
| INT-OX | 1 | INT del MAX30102 (open-drain, activo en bajo) |
| SCK_TFT | 6 | SPI clock |
| SDA_TFT | 7 | SPI MOSI (en el ST7735S es el SDA bidireccional) |
| CS_TFT | 21 | SPI chip select |
| DATA_SEL_TFT | 10 | D/C (A0) |
| RST_TFT | 20 | Reset del TFT |
| BACKLIGHT_TFT | 5 | Backlight (pin LED del módulo) |
| SW1 | 0 | Pulsador (este test no lo usa) |

Según `PCB/Esquematico V2.pdf` / `Sensor_ctrl_pwr_pcb.kicad_sch`. Si cambiás el
esquemático, actualizá los `#define PIN_*` al principio de `src/main.cpp`.

## Setup

Con PlatformIO instalado (extensión de VSCode o CLI):

```bash
cd firmware/peripherals-test
pio run -t upload    # flashea por USB
pio device monitor   # ver el resultado de los tests por Serial
```

El Super Mini usa el USB-CDC nativo del ESP32-C3: el firmware espera hasta 4 s
a que abras el monitor antes de empezar los tests, así no te perdés el
output. Si igual se te pasó, apretá reset (o desenchufá y volvé a enchufar).

Si tu placa no la reconoce como `esp32-c3-devkitm-1`, probá con `lolin_c3_mini`
en `platformio.ini` (igual que en `battery-test`).

## Probar solo el sensor (sin TFT conectado)

La pantalla viene **deshabilitada por defecto** (`#define ENABLE_TFT 0` al
principio de `src/main.cpp`). En ese modo no se compila nada del TFT: no se
tocan los pines de la pantalla, el test 5 queda como `N/D` y todo se reporta
solo por Serial (incluido el modo en vivo con `ir:<n>,red:<n>`).

Cuando conectes la pantalla, poné `ENABLE_TFT` en `1` (o compilá con
`-DENABLE_TFT=1`) y reflasheá.

## Qué hace

Al arrancar corre estos tests y reporta `PASS` / `FAIL` / `N/D` por Serial y en
la pantalla (esta última solo con `ENABLE_TFT 1`):

1. **I2C scan**: lista todo lo que responde en el bus y espera encontrar `0x57`.
2. **MAX PART_ID**: lee `PART_ID` (esperado `0x15`) y hace un soft reset.
3. **MAX temp die**: dispara una conversión de temperatura y la lee (valida
   escritura y lectura de registros; debería dar algo cercano a la temperatura
   ambiente + unos grados).
4. **MAX pin INT**: habilita la interrupción de fin de conversión de
   temperatura y verifica que GPIO1 esté en alto en reposo, baje con la
   interrupción, y vuelva a subir al limpiarla.
5. **LCD readback**: reset por hardware, init (`initR`) y lectura del registro
   `RDDPM` del ST7735S antes y después de la init (esperado `0x08` → `0x9C`).
   Que cambie a `0x9C` prueba que los comandos del SPI llegaron al controlador.

Después de los tests corre el **patrón visual** del TFT: pantalla completa en
rojo, verde, azul y blanco (cada una con su nombre escrito), un marco de 1 px
en el borde y un parpadeo del backlight. Al final muestra el resumen en la
pantalla durante 3 s.

### Modo en vivo

Si el MAX30102 pasó los tests 1 y 2, pasa a leer el sensor (SpO2 mode, 25
muestras/s, LEDs a ~7 mA):

- **Pantalla**: valores IR y RED, `DEDO` / `SIN DEDO` (umbral IR > 50 000) y un
  gráfico de IR autoescalado (~5 s de historia).
- **Serial**: una línea `ir:<n>,red:<n>` por muestra, para el serial plotter.

Si el MAX30102 no respondió, se queda en el resumen y reintenta detectarlo
cada 2 s: podés mover cables o retocar soldaduras sin volver a flashear.
Lo mismo si deja de responder en medio de la lectura en vivo.

## Debug: ¿está vivo el ESP32?

**LED integrado (GPIO8)**, funciona aunque el Serial no muestre nada:

| LED | Significado |
|---|---|
| 3 parpadeos rápidos al encender | El firmware arrancó |
| Fijo encendido | Dentro de `setup()` (esperando el monitor serie hasta 4 s, o corriendo tests) |
| Parpadeo rápido (4 Hz) | En `loop()`, MAX30102 sin detectar, reintentando |
| Parpadeo lento (1 Hz) | En `loop()`, leyendo el sensor en vivo |

Si no ves ni los 3 parpadeos: el ESP32 no está corriendo (modo boot, sin
alimentación, o el LED de tu placa es activo en alto → invertí `LED_ON_LEVEL`).

**Serial**: cada línea de debug es `[uptime ms][TAG] mensaje`.

- `BOOT`: causa del último reset (power-on, pin RESET, **BROWNOUT**, **CRASH**),
  si había monitor conectado, chip/heap, y marcas de `setup()` / `loop()`.
- `I2C`: nivel de SDA / SCL / INT, y en cada fallo el código crudo de
  `Wire.endTransmission()`:
  - `err=2` NACK en la dirección: nadie responde en 0x57 (alimentación,
    soldadura, dirección).
  - `err=5` timeout: SDA o SCL trabadas en bajo (corto o pull-up a 1.8 V).
- Antes de iniciar el I2C hace un **pre-check de las líneas**: con el módulo
  alimentado, SDA y SCL deben leer `1` sin pull-up interno. Distingue
  "no hay pull-up externo" de "línea sujeta en bajo".
- Mientras el sensor falta: una línea cada 2 s con el error I2C y los niveles
  SDA/SCL/INT (así, aunque abras el monitor tarde, ves el estado actual).
- En vivo: una línea por segundo con `muestras/s` (esperado ~25), IR/RED,
  punteros del FIFO y un diagnóstico (`SIN MUESTRAS NUEVAS`, `IR y RED en 0`,
  `dedo detectado` / `sin dedo`).

## Qué esperar / troubleshooting

**Serial esperado** con todo bien:

```
[PASS] I2C scan 0x57  0x57 responde (1 dispositivo/s en el bus)
[PASS] MAX PART_ID    PART_ID=0x15 REV_ID=0x..., soft reset ok
[PASS] MAX temp die   2x.xx C
[PASS] MAX pin INT    reposo alto=si, baja con int=si, flag=si, se libera=si
RDDPM  = 0x08 tras reset (esperado 0x08), 0x9C tras initR (esperado 0x9C)
[PASS] LCD readback   RDDPM 0x08 -> 0x9C, el controlador recibio la init por SPI
```

| Síntoma | Causa probable |
|---|---|
| `I2C scan` no encuentra `0x57` | Soldadura de SDA/SCL, o los **pull-ups del módulo MAX30102 van a 1.8 V**: el ESP32-C3 necesita ≥ ~2.5 V para leer un "1". Muchos módulos tienen un jumper/resistencias para pasar los pull-ups a 3V3. |
| Aparece otra dirección en el scan | El módulo no es un MAX30102, o el bus tiene otro dispositivo. |
| `PART_ID` distinto de `0x15` | Módulo clon o lectura corrupta (probar bajar a 100 kHz, que ya es lo que usa el test). |
| `pin INT` falla con "reposo alto=NO" | INT en bajo desde antes: línea en corto a GND o sin pull-up. El test activa el pull-up interno del ESP32, pero el módulo tiene que dejar el pin libre. |
| `pin INT` falla con "baja con int=NO" | INT no llega a GPIO1 (pista/soldadura). |
| `LCD readback` da `N/D` | El módulo no devuelve datos de lectura (algunos no conectan el SDA al pin de salida del controlador). No es un error: **mirar la pantalla**. |
| `LCD readback` da `FAIL` con `RDDPM` igual antes y después | La lectura anda pero las escrituras no llegan: revisar SCK/SDA/CS/DC. |
| Pantalla en blanco pero el test pasa | Backlight: probar invertir `BL_ON` en `main.cpp` (si el módulo tiene transistor inversor). |
| Los colores del patrón no coinciden con el nombre (rojo se ve azul) | Cambiar `TFT_INIT_TAB` a `INITR_BLACKTAB` en `main.cpp`. |
| El marco de 1 px se corta en algún borde o la imagen está corrida | Offsets de columna/fila del módulo: probar otra constante de `TFT_INIT_TAB` (`INITR_144GREENTAB`, `INITR_BLACKTAB`, `INITR_GREENTAB`). |

### Notas

- El `RDDPM` se lee por bit-bang con GPIOs (el módulo no tiene MISO, la lectura
  sale por el mismo SDA): por eso el test libera el bus SPI (`SPI.end()`), lee,
  y lo vuelve a iniciar. Los valores `0x08` / `0x9C` son los del datasheet del
  ST7735; si tu clon responde otra cosa consistente, mirá el valor crudo en
  el Serial antes de darlo por fallado.
- La librería de Adafruit manda los comandos de init con el clock por defecto
  de ella (32 MHz pedidos, el ESP32-C3 baja a lo que pueda); después del
  `initR` el test lo baja a 10 MHz (`TFT_SPI_HZ`), que es más seguro para el
  dibujado. Si la init falla de forma intermitente, sospechá del cableado antes
  que del software.
- **GPIO21 (CS_TFT) y GPIO20 (RST_TFT)** son los pines UART0 del ESP32-C3
  (`U0TXD` / `U0RXD`). Con `USB_CDC_ON_BOOT` no se usan para el Serial, pero el
  bootloader ROM puede escribir logs por GPIO21 al arrancar, lo que mueve el
  CS del TFT unos instantes. Debería ser inofensivo porque en ese momento
  el SCK no se mueve y el test resetea y re-inicializa la pantalla; si ves
  basura en la pantalla recién al encender, es lo primero a sospechar.
- **Deep sleep**: el ESP32-C3 solo despierta por GPIO0–GPIO5. En este
  esquemático SW1 (GPIO0) y INT-OX (GPIO1) sí caen en ese rango, así que ambos
  sirven como fuente de wake-up en el firmware final.
