# Battery autonomy test

Sketch de prueba para el ESP32-C3 Super Mini. Un solo ciclo, en este orden:

1. **Self-test del MAX30102** (antes del WiFi): reset, prende los LEDs en modo
   SpO2, lee un par de muestras (RED/IR, válidas = distintas de 0), apaga los
   LEDs y lo deja en **shutdown** de bajo consumo. Imprime `PASS` o `FAIL`.
2. **WiFi**: conecta y manda paquetes UDP ("hello world" + su propia IP)
   durante unos segundos.
3. `deep sleep` **para siempre** (sin timer de wakeup — solo despierta de
   nuevo con un reset manual o power cycle).

El MAX30102 sigue alimentado desde el riel de 3V3 aunque el ESP32 se resetee o
esté en deep sleep, y conserva su estado (por ejemplo, LEDs prendidos desde
`peripherals-test`). Por eso se lo deja explícitamente en shutdown (bit `SHDN`,
`0x80` en `MODE_CONFIG`, registro `0x09`) por I2C (SDA=GPIO3, SCL=GPIO4, los
mismos pines que `peripherals-test`): en shutdown consume ~1 µA en vez de su
corriente de reposo normal.

No es el firmware final de PulsOx (ese va a correr un servidor WebSocket
siempre activo, ver [`PROTOCOL.md`](../../PROTOCOL.md), lo cual es
incompatible con deep sleep). Este sketch es solo para medir cuánto se
descarga la batería en reposo (deep sleep puro, sin WiFi ni wakeups
periódicos de por medio).

## Setup

1. Copiá `include/secrets.h.example` a `include/secrets.h` y completá tu
   WiFi y la IP de tu PC (`secrets.h` está en `.gitignore`, no se commitea).
2. Con PlatformIO instalado (extensión de VSCode o CLI):
   ```bash
   cd firmware/battery-test
   pio run -t upload   # flashea por USB
   pio device monitor   # opcional, para ver los logs por Serial mientras está por USB
   ```
   Si tu placa no la reconoce como `esp32-c3-devkitm-1`, probá cambiando el
   `board` en `platformio.ini` (algunos clones "Super Mini" andan mejor con
   `lolin_c3_mini`).

## Cómo correr el test

1. En tu PC, en la misma red WiFi, corré el listener para confirmar que el
   paquete llega:
   ```bash
   cd firmware/battery-test/tools
   python3 listener.py
   ```
2. Reseteá la placa (o desconectala y volvé a conectarla) y confirmá en el
   listener que llegó el `hello world from <ip>`. Después de eso la placa
   ya está dormida para siempre — no va a mandar nada más.
3. **Desconectá el USB/5V de la placa** y medí la tensión de la batería con
   el multímetro en ese instante (anotá el valor y la hora).
4. Al otro día, medí la tensión de nuevo con la placa todavía alimentada
   solo a batería (sin haberla resetear en el medio).

### Flags de debug y monitor serie

Dos flags al principio de `src/main.cpp` (también se pueden pasar con
`-D...`). Los dos vienen en **1**; para la medición real de batería ponelos en **0**:

- `MAX_TEST`: corre el self-test de LEDs/muestras al bootear. En 0 solo deja el
  sensor en shutdown.
- `DEBUG_STAY_AWAKE`: no entra en deep sleep; mantiene el USB y imprime los
  registros del sensor cada segundo, para ver que siga en shutdown después del WiFi.

En deep sleep el USB-Serial-JTAG del ESP32-C3 se apaga, la PC ve la placa como
"desenchufada" y el monitor serie se cae: **desde ahí no se puede imprimir nada**
(limitación del chip, no un bug). Además, tras un reset el USB se re-enumera y el
monitor reconecta tarde, por eso el sketch espera a que lo abras y suma 1,5 s de
margen antes de empezar.

```powershell
pio run -t upload
pio device monitor -p COM7
```

Qué mirar en el log:

- `MAX30102 self-test PASS` → los LEDs prendieron, se leyeron muestras válidas y
  quedó en shutdown (`MODE_CONFIG=0x80 LED1_PA=0x00 LED2_PA=0x00`). Si el LED
  físico sigue prendido con eso, no es el LED de medición sino un indicador del
  módulo, que no se apaga por software.
- `FAIL` indica cuál de los tres pasos falló (`LEDs on`, `samples valid`, `shutdown`).
  `I2C err=2` es un fallo genérico de la transacción (sin respuesta); `err=5`, bus trabado.

### Sobre qué esperar de la medición

El consumo en deep sleep del ESP32-C3 suele ser de **pocas decenas de µA**.
En 24h eso es un consumo de batería del orden de 0.5 mAh o menos — en una
batería LiPo típica (varios cientos de mAh) esa caída de tensión puede ser
demasiado chica para verse con un multímetro común, porque la curva de
descarga de una LiPo es bastante plana en reposo y además la tensión varía
un poco con la temperatura. Si el número que te interesa es la corriente de
deep sleep en sí (no solo "aguantó 24h sin problema"), lo más confiable es
medir **corriente** en serie entre la batería y la placa (multímetro en
rango µA/mA, o un medidor USB con esa función) en el momento en que ya está
dormida, en vez de comparar voltaje antes/después.

## Ajustar el sketch

- El payload es texto plano armado a mano: `"hello world from <ip>"`. Si
  después querés volver a mandar paquetes periódicos (para medir el ciclo
  completo wake/transmit/sleep en vez de solo el reposo), la versión
  anterior de este sketch usaba `esp_sleep_enable_timer_wakeup()` antes de
  `esp_deep_sleep_start()` — se puede volver a agregar fácil.
- `WIFI_CONNECT_TIMEOUT_MS` (10s por defecto): si no conecta en ese tiempo,
  se va a dormir igual sin mandar nada, para no gastar batería reintentando
  indefinidamente.
