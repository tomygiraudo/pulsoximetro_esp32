# PulsOx (firmware del pulsioxímetro)

Firmware real del dispositivo: ESP32-C3 Super Mini + MAX30102 + pantalla TFT
ST7735 128×128, con PlatformIO y el framework Arduino. Mide SpO₂ y pulso, los
muestra en la pantalla y los manda a Firebase Realtime Database
([`PROTOCOL.md`](../../PROTOCOL.md), "Transporte en la nube") para que la web los
vea. Reúne lo que se probó por separado en `peripherals-test` (sensor y
pantalla), `cloud-test` (nube) y `battery-test` / `light-sleep` (ahorro de
energía).

## Cómo se usa

El dispositivo **vive dormido** (ESP32 en light sleep, MAX30102 en shutdown,
pantalla apagada). **Un botón lo despierta**:

```
botón ─► CONECTANDO ─► ESPERA_DEDO ─► MIDIENDO ─┬─► RESULTADO (2 min) ─► a dormir
         WiFi + login   "Colocá el dedo"  45-60 s │
                        30 s sin dedo: dormir     └─► ERROR "Dedo no encontrado" (2 min) ─► a dormir
```

- **Conectando**: WiFi y login a Firebase, en una tarea aparte. El sensor todavía
  no está encendido.
- **Colocá el dedo**: se enciende el sensor. Con el dedo puesto empieza la medición.
  Si en 30 s nadie lo pone, duerme.
- **Midiendo**: SpO₂, pulso y traza en la pantalla, y un `live` por segundo a la
  nube. **El botón se ignora**: una medición siempre termina antes de empezar otra.
  Termina a los 45 s si hay lecturas estables; si no, sigue hasta los 60 s.
- **Resultado**: los valores quedan 2 minutos (SpO₂, pulso, "ENVIADO" o "NO
  ENVIADO"), con el sensor y el WiFi apagados. El botón mide de nuevo.
- **Error**: si el dedo se retira, la señal es errática, el sensor se satura o no
  hay lecturas suficientes, se muestra "Dedo no encontrado", **se borra la sesión
  de la nube** y no queda nada en el historial. El botón mide de nuevo.
- Sin WiFi o con el login rechazado, **mide igual** y el resultado dice "NO
  ENVIADO". No guarda nada para mandarlo después.
- En el historial de la web queda **una sola lectura por medición**: el resultado
  (la mediana de los segundos estables de los últimos 20 s). Lo que evoluciona
  en vivo viaja en `live`.

## Setup

1. Copiá `include/secrets.h.example` a `include/secrets.h` y completá el WiFi y las
   credenciales de Firebase (mismos valores que `tools/cloud/.env`). `secrets.h`
   está en `.gitignore`: no se commitea. Las redes tienen que ser de **2,4 GHz**.
2. **Varias redes.** `WIFI_NETWORKS` es una lista en **orden de preferencia**:

   ```c
   #define WIFI_NETWORKS \
     {"red-de-casa", "clave-casa"}, \
     {"hotspot-del-celu", "clave-celu"}
   ```

   Prueba las redes **en el orden de la lista** (no la de más señal) y se queda con la
   primera que conecta. Una red que el chip no encuentra se descarta a los ~2,5 s, lo
   que tarda su búsqueda del nombre, y no tras el timeout de 10 s; el total está
   topado en 25 s. Si ninguna conecta, la medición sigue sin nube ("NO ENVIADO").
   Probar cada red directo, sin escanear antes, es lo que da el mejor tiempo (un
   escaneo completo cuesta ~6 s) y hace que las redes ocultas también anden. El
   formato viejo, `WIFI_SSID` y `WIFI_PASSWORD`, sigue andando si no definís
   `WIFI_NETWORKS`. Las claves se compilan adentro del firmware: cambiar la lista
   implica **volver a subirlo**. El log dice la posición de la red ("red 2 de 3"),
   nunca su nombre. Poné primero la que más usás: cada red anterior que no esté a
   tu alcance suma ~2,5 s a la conexión.
3. Compilá y subí, con la placa por USB (en Windows, PlatformIO a veces elige un COM
   Bluetooth: indicá el puerto con `--upload-port`):

```
pio run -e esp32-c3-supermini -t upload        # el firmware real, con light sleep
pio run -e dev -t upload                        # el mismo, con el sueño simulado
```

### Entornos

| Entorno | Para qué |
|---|---|
| `esp32-c3-supermini` | **El firmware real.** Duerme en **light sleep** (`POWER_SLEEP_MODE`): ~130 µA en el chip, la RAM se conserva y despierta en milisegundos. El USB se corta mientras duerme y **no vuelve** al despertar (ver abajo). |
| `deep-sleep` | Igual, pero en deep sleep (~5 µA en el chip). **Solo despierta si R5 de la placa del botón es de ~3 kΩ o menos** (ver abajo). |
| `dev` | Igual, pero el "sueño" es simulado: la CPU y el USB siguen, así se ve el log y se puede subir en cualquier momento. Despierta con el botón o con `b` por serie. Loguea cada flanco de GPIO0 e incluye los comandos de prueba (`g`). No sirve para medir consumo. |
| `dev-offline` | `dev` sin nube: nunca conecta. Prueba el camino "medir sin enviar" sin apagar el router. |
| `ui-demo` | Recorre las 12 pantallas con datos falsos, sin sensor ni WiFi. |

### Subir con el chip dormido

Mientras duerme, el puerto USB desaparece, y **en light sleep Windows tampoco lo
vuelve a reconocer al despertar** (el controlador USB no se re-enumera): no hay log
por serie hasta el próximo RESET. Para ver el log usá el entorno `dev`. Para subir
firmware: tras un RESET (o al conectar la placa) hay una **ventana de 5 s** antes de
dormir, subí dentro de ella. También sirve mantener BOOT (GPIO9), apretar RESET y
soltar BOOT.

### La placa del botón

El botón (SW1 → GPIO0, activo en bajo) está en una placa externa, con un pull-up a
3V3 (R4) y una resistencia en serie (R5) más un capacitor (C7) hacia GPIO0. El
antirrebote lo hace el software (30 ms).

- **En light sleep (lo predeterminado) R5 puede ser la de 100 kΩ.** El pin se
  configura como entrada **sin** pull-up interno, así que al apretar baja a ~0 V.
  Verificado en la placa con R5 = 100 kΩ, C7 = 100 nF.
- **En deep sleep, R5 tiene que ser de ~3 kΩ o menos.** El SDK de Arduino trae
  activada `CONFIG_ESP_SLEEP_GPIO_ENABLE_INTERNAL_RESISTORS`, que enciende el
  pull-up interno del pin de despertar al entrar en deep sleep y no se puede apagar
  desde el proyecto. Ese pull-up forma un divisor con R5 y el pin apretado tiene
  que bajar de 0,825 V (el 25 % de 3,3 V) para leerse como "bajo". Con 100 kΩ solo
  baja a ~2,8 V (medido) y no despertaría. Dos resistencias de 6,8 kΩ en paralelo
  sobre R5 darían ~0,5 V. Sin probar en deep sleep real.

## Pantalla

Las pantallas siguen el diseño de `design-system/Pulsioxímetro WiFi · TFT 128×128.html`
(fuente Barlow Semi Condensed y los íconos, rasterizados a máscaras de 4 bits).
`src/display/display_assets.h` es **generado**: no se edita a mano, se cambia
`tools/gen_display_assets.py` y se vuelve a correr (necesita `pillow`, `fonttools` y
`brotli`; en un entorno virtual aparte, no hace falta instalarlos en PlatformIO).
Los umbrales de alerta son los del diseño: ≥ 96 % normal, 93–95 % precaución,
< 93 % crítico.

## Qué hay en cada carpeta

En `src/` solo está `main.cpp` suelto; el resto va en una carpeta por lo que hace.

| Ruta | Qué es |
|---|---|
| `src/main.cpp` | El punto de entrada: `setup()` / `loop()` solo llaman a `appSetup()` / `appLoop()`. |
| `src/app/` | **La capa de aplicación.** `app.cpp` es la máquina de estados de arriba y orquesta todo lo demás. |
| `src/display/` | Las pantallas TFT: `display.cpp`, la escala de la traza (`trace_scale.c`) y `display_assets.h` (generado). Usa el controlador de `lib/tft_st7735`. |
| `src/cloud/` | La nube: el cliente de Firebase (`cloud.cpp`, REST + TLS, de `cloud-test`) y la tarea que lo corre sin frenar el sensor (`cloud_link.cpp`). |
| `src/board/` | La placa: el botón (`button.cpp`), el LED de latido (`status_led.cpp`) y el sueño (`power.cpp`: light / deep sleep, retención de pines, wakeup por GPIO0). |
| `src/debug/` | El puerto serie y el diagnóstico: el log `DBG()` (`debug_log.cpp`), el modo CSV y los comandos (`serial_stream.cpp`) y el diagnóstico del bus I2C (`i2c_diag.cpp`). |
| `lib/max30102` | Driver del sensor (I2C, FIFO). |
| `lib/ppg` | El procesamiento, en C puro: filtros, SpO₂, pulso, calidad (`ppg_processor`) y el juez de la medición (`ppg_session`). |
| `lib/tft_st7735` | El controlador de la pantalla. |
| `include/config.h` | **Todos los parámetros**: pines, sensor, filtros, tiempos, umbrales. |
| `test/` | Lo que no es del firmware real: el demo de pantallas (`test/demo/`) y las pruebas de PC (`test/host/`). Ver abajo. |
| `tools/` | Utilidades de PC: grabar capturas y generar archivos (ver abajo). |
| `captures/` | Capturas crudas del sensor, para ajustar y validar el algoritmo. |

**Includes.** Los headers de `src/` se incluyen con su carpeta, también el propio
(`#include "cloud/cloud_link.h"`): PlatformIO ya pone `src/` en el include path. Los de
`lib/` e `include/` van sin carpeta (`"config.h"`, `"ppg_processor.h"`).

## Parámetros que se pueden ajustar

Todos en `include/config.h`, comentados. Los de la medición son puntos de partida
ajustados con las capturas de `captures/`:

- `MEAS_MIN_S` / `MEAS_MAX_S` (45 / 60 s), `MEAS_MIN_VALID_S`, `MEAS_QUALITY_MIN`: cuándo termina una medición.
- `MEAS_INVALID_MAX_S`, `MEAS_SATURATED_MAX_S`, `MEAS_FINGER_LOST_S`: cuándo se descarta.
- `QUALITY_PI_GOOD_PCT`, `QUALITY_PI_MAX_PCT`: escala de la calidad de señal.
- `RESULT_SHOW_S`, `ERROR_SHOW_S`, `FINGER_WAIT_S`, `BOOT_WINDOW_MS`: tiempos de pantalla y de espera.

**SpO₂ sin calibrar.** Usa la tabla de referencia de Maxim, sin calibrar contra un
oxímetro de referencia para este sensor y esta carcasa: los valores son
indicativos, no médicos.

## Por serie (115200)

Cada línea de log empieza con el uptime y una etiqueta (`[APP ]`, `[MEAS]`, `[CLOU]`,
`[MAX ]`, `[PWR ]`, `[BTN ]`, `[LIVE]`...). Comandos de un carácter:

| | |
|---|---|
| `c` | modo CSV: una línea `D,` por muestra y una `R,` por segundo, para `tools/capture.py` |
| `s` | vuelve al modo resumen |
| `b` | simula el botón |
| `g` | corrompe la próxima muestra del sensor (solo en `dev`, para probar el driver) |

La línea `[LIVE]` de cada segundo da las muestras/s (tienen que ser ~100), el último
RED/IR, los desbordes del FIFO, las **muestras corruptas** que el driver reemplazó,
los fallos de I2C y el refresco de pantalla más lento. Si aparece `SATURADO` con el
valor crudo muy por debajo del tope (262143), es un glitch del bus.

## Utilidades (`tools/`)

| | |
|---|---|
| `capture.py` | Graba el CSV del dispositivo (modo `c`). `%USERPROFILE%\.platformio\penv\Scripts\python.exe tools\capture.py --port COM7 --seconds 60 --label dedo_quieto` |
| `procesamiento.py` | El script de referencia (offline, con `filtfilt` y FFT) contra el que se compara el pipeline en C. |
| `gen_ppg_coefs.py` | Genera `lib/ppg/ppg_coefs.h` desde `config.h`. Hay que correrlo si cambia la frecuencia de muestreo o un filtro. |
| `gen_display_assets.py` | Genera `src/display/display_assets.h`. |

## Demo y pruebas (`test/`)

Nada de esto entra en el firmware real. El demo se carga en la placa con su propio entorno
(`ui-demo`); las pruebas corren en la PC sobre las capturas de `captures/`.

| | |
|---|---|
| `demo/display_demo.cpp` | El demo de pantallas (entorno `ui-demo`): recorre las 12 pantallas con datos falsos, sin sensor ni WiFi. |
| `host/ppg_host.c` | Corre el pipeline de `lib/ppg` en la PC sobre una captura. |
| `host/session_host.c` | Corre el pipeline y el juez de la medición sobre una captura, con simulaciones de dedo retirado (`--finger-off S`), saturación (`--saturate S L`), ruido (`--noise S P`) y captura cortada (`--cut S`). Se compila con cualquier compilador de C99 (`python -m ziglang cc` sirve). |
| `host/trace_host.c` | Compara la escala vertical de la traza de la pantalla (`src/display/trace_scale.c`) con la que había antes, sobre una captura: altura con la que se dibuja el último segundo, cuadros aplanados y columnas recortadas. Simula un golpe (`--spike S`) o un cambio de presión del dedo (`--press S PCT [LEN]`). Se compila con `python -m ziglang cc -O2 -std=c11 -Iinclude -Ilib/ppg -Isrc test/host/trace_host.c src/display/trace_scale.c lib/ppg/ppg_processor.c lib/ppg/sos_filter.c -lm -o trace_host`. |
| `host/validate_ppg.py` | Compara el pipeline en C con scipy y con `tools/procesamiento.py`. |

## Limitaciones conocidas

- **El bus I2C del sensor es frágil.** Con cables largos o finos, mover el módulo o
  tocar los cables puede dar ráfagas de lecturas basura. El driver descarta las
  muestras con los seis bytes en `0xFF` (el glitch típico) y la línea `[LIVE]` las
  cuenta, pero una ráfaga larga igual deja la señal plana hasta que pasa. Lo de
  fondo es de hardware (cables cortos o trenzados, desacople).
- Sin medición de batería: la barra de la pantalla muestra un valor fijo
  (`BATTERY_PLACEHOLDER_PCT`) y `info.battery_pct` se omite.
- **Consumo sin medir.** Los ~130 µA (light sleep) y ~5 µA (deep sleep) son los
  valores típicos del datasheet del chip; el total depende del resto de la placa
  (regulador, módulo TFT, LED de la Super Mini). Se mide con el multímetro en serie
  con la batería.
- **Sin USB después del primer sueño** (arriba): para depurar, entorno `dev`.
- No se probó el corte de WiFi a mitad de una medición, ni el deep sleep real.
