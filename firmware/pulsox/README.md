# PulsOx (firmware del pulsioxímetro)

Firmware real del dispositivo: ESP32-C3 Super Mini + MAX30102 + pantalla TFT
ST7735 128×128, con PlatformIO y el framework Arduino. Mide SpO₂ y pulso, los
muestra en la pantalla y los manda a Firebase Realtime Database
([`PROTOCOL.md`](../../PROTOCOL.md), "Transporte en la nube") para que la web los
vea. Reúne lo que se probó por separado en `peripherals-test` (sensor y
pantalla), `cloud-test` (nube) y `battery-test` / `light-sleep` (ahorro de
energía).

## Cómo se usa

El dispositivo **vive dormido** (ESP32 en deep sleep, MAX30102 en shutdown,
pantalla apagada). **Un botón lo despierta**:

```
botón ─► CONECTANDO ─► ESPERA_DEDO ─► MIDIENDO ─┬─► RESULTADO (2 min) ─► deep sleep
         WiFi + login   "Colocá el dedo"  45-60 s │
                        30 s sin dedo: dormir     └─► ERROR "Dedo no encontrado" (2 min) ─► deep sleep
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
   está en `.gitignore`: no se commitea. La red tiene que ser de **2,4 GHz**.
2. Compilá y subí, con la placa por USB (en Windows, PlatformIO a veces elige un COM
   Bluetooth: indicá el puerto con `--upload-port`):

```
pio run -e esp32-c3-supermini -t upload        # el firmware real, con deep sleep
pio run -e dev -t upload                        # el mismo, con el sueño simulado
```

### Entornos

| Entorno | Para qué |
|---|---|
| `esp32-c3-supermini` | **El firmware real.** Deep sleep de verdad: el USB se corta mientras duerme. |
| `dev` | Igual, pero el "sueño" es simulado: la CPU y el USB siguen, así se ve el log y se puede subir en cualquier momento. Despierta con el botón o con `b` por serie. Loguea cada flanco de GPIO0 e incluye los comandos de prueba (`g`). No sirve para medir consumo. |
| `dev-offline` | `dev` sin nube: nunca conecta. Prueba el camino "medir sin enviar" sin apagar el router. |
| `ui-demo` | Recorre las 12 pantallas con datos falsos, sin sensor ni WiFi. |

### Subir con el chip dormido

En deep sleep el puerto USB desaparece. Tras un RESET (o al conectar la placa) hay
una **ventana de 5 s** antes de dormir: subí dentro de ella. También sirve
mantener BOOT (GPIO9), apretar RESET y soltar BOOT.

### La placa del botón

El botón (SW1 → GPIO0, activo en bajo) está en una placa externa, con un pull-up a
3V3 (R4) y una resistencia en serie (R5) más un capacitor (C7) hacia GPIO0.
**R5 tiene que ser de 4,7 kΩ o menos.** El SDK de Arduino trae activada
`CONFIG_ESP_SLEEP_GPIO_ENABLE_INTERNAL_RESISTORS`, que enciende el pull-up interno
(~45 kΩ) del pin de despertar al entrar en deep sleep y no se puede apagar desde el
proyecto. Con R5 = 100 kΩ el pin apretado solo baja a ~2,3 V, que se lee como alto,
y el deep sleep real no despertaría. Con R5 ≤ 4,7 kΩ baja a ~0,3 V. El antirrebote
lo hace el software (30 ms).

## Pantalla

Las pantallas siguen el diseño de `design-system/Pulsioxímetro WiFi · TFT 128×128.html`
(fuente Barlow Semi Condensed y los íconos, rasterizados a máscaras de 4 bits).
`src/display_assets.h` es **generado**: no se edita a mano, se cambia
`tools/gen_display_assets.py` y se vuelve a correr (necesita `pillow`, `fonttools` y
`brotli`; en un entorno virtual aparte, no hace falta instalarlos en PlatformIO).
Los umbrales de alerta son los del diseño: ≥ 96 % normal, 93–95 % precaución,
< 93 % crítico.

## Qué hay en cada carpeta

| Ruta | Qué es |
|---|---|
| `src/app.cpp` | La máquina de estados de arriba. `main.cpp` solo llama a `appSetup()` / `appLoop()`. |
| `src/cloud.cpp`, `src/cloud_link.cpp` | El cliente de Firebase (REST + TLS, de `cloud-test`) y la tarea que lo corre sin frenar el sensor. |
| `src/power.cpp`, `src/button.cpp` | Deep sleep (retención de pines, wakeup por GPIO0) y el botón. |
| `src/display.cpp`, `lib/tft_st7735` | Las pantallas y el controlador. |
| `lib/max30102` | Driver del sensor (I2C, FIFO). |
| `lib/ppg` | El procesamiento, en C puro: filtros, SpO₂, pulso, calidad (`ppg_processor`) y el juez de la medición (`ppg_session`). |
| `include/config.h` | **Todos los parámetros**: pines, sensor, filtros, tiempos, umbrales. |
| `captures/` | Capturas crudas del sensor, para ajustar y validar el algoritmo. |
| `tools/` | Herramientas de PC (ver abajo). |

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

## Herramientas (`tools/`)

| | |
|---|---|
| `capture.py` | Graba el CSV del dispositivo (modo `c`). `%USERPROFILE%\.platformio\penv\Scripts\python.exe tools\capture.py --port COM7 --seconds 60 --label dedo_quieto` |
| `ppg_host.c` | Corre el pipeline en la PC sobre una captura. |
| `session_host.c` | Corre el pipeline y el juez de la medición sobre una captura, con simulaciones de dedo retirado (`--finger-off S`), saturación (`--saturate S L`), ruido (`--noise S P`) y captura cortada (`--cut S`). Se compila con cualquier compilador de C99 (`python -m ziglang cc` sirve). |
| `validate_ppg.py` | Compara el pipeline en C con scipy y con `procesamiento.py`. |
| `gen_ppg_coefs.py` | Genera `lib/ppg/ppg_coefs.h` desde `config.h`. Hay que correrlo si cambia la frecuencia de muestreo o un filtro. |
| `gen_display_assets.py` | Genera `src/display_assets.h`. |

## Limitaciones conocidas

- **El bus I2C del sensor es frágil.** Con cables largos o finos, mover el módulo o
  tocar los cables puede dar ráfagas de lecturas basura. El driver descarta las
  muestras con los seis bytes en `0xFF` (el glitch típico) y la línea `[LIVE]` las
  cuenta, pero una ráfaga larga igual deja la señal plana hasta que pasa. Lo de
  fondo es de hardware (cables cortos o trenzados, desacople).
- Sin medición de batería: la barra de la pantalla muestra un valor fijo
  (`BATTERY_PLACEHOLDER_PCT`) y `info.battery_pct` se omite.
- El deep sleep real depende de la placa del botón (arriba). Sin R5 corregida, solo
  funciona el entorno `dev`.
