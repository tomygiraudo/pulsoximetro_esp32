# Cloud test (ESP32-C3 → Firebase Realtime Database)

Sketch de prueba para el ESP32-C3 Super Mini. Hace desde la placa lo mismo que
[`tools/cloud/fake_device.py`](../../tools/cloud/fake_device.py) hace desde la
PC: una medición **con valores simulados** (sin sensor ni TFT) escrita por REST
a la base, para validar el contrato de nube de
[`PROTOCOL.md`](../../PROTOCOL.md) ("Transporte en la nube") en el hardware real
antes de integrarlo al firmware `pulsox`.

El ESP32 es **solo un cliente de la base que escribe telemetría**: no tiene
servidor WebSocket, no lee nada y no recibe comandos. La web
(<https://tomygiraudo.github.io/pulsoximetro_esp32/>, fuente "Nube") es el otro
cliente de la misma base.

## Qué hace

1. **WiFi** (STA, auto-reconnect) y **login** con email/contraseña
   (`signInWithPassword`). Imprime el `uid`: tiene que ser el de
   `tools/cloud/database.rules.json` (el único que puede escribir).
2. `PUT info` y `DELETE live` (limpia el `live` que deja un corte a mitad de medición).
3. **Medición** (`MEASUREMENT_S`, 60 s por defecto):
   `POST sessions` → `sid`; un `PUT live` por segundo (SpO₂, BPM, calidad y 1 s de
   PPG en CSV); cada 5 s con lectura válida un `POST sessions/<sid>/readings`.
4. **Cierre**, en el orden de `PROTOCOL.md` (8, 9, 10): `PATCH sessions/<sid>
   {endedAt}` → `PATCH info` → `DELETE live`.
5. Queda **despierto con USB** esperando comandos por Serial. Deep sleep está
   fuera de esta prueba.

Detalles que se quieren comprobar:

- **TLS validado** contra las CAs raíz de Google (`include/root_ca.h`: GTS Root R1
  para `firebaseio.com`, GTS Root R4 para `googleapis.com`), sin `setInsecure()`.
- **Una sola conexión TLS** a la base (`HTTPClient::setReuse(true)`): el contador
  `tlsConnects` tiene que quedar en 1. La autenticación usa otra conexión,
  efímera, solo al arrancar y cada ~55 min.
- **Renovación del token** (vence a la hora) antes de que venza; ante un `401`,
  login nuevo y **un solo** reintento.
- Un tick que falla se **descarta** (sin cola); si un request tarda más de un
  tick, se salteán los segundos perdidos en vez de hacer una ráfaga.

## Setup

1. Copiá `include/secrets.h.example` a `include/secrets.h` y completá el WiFi y
   las credenciales de Firebase (`secrets.h` está en `.gitignore`, no se
   commitea). Email, contraseña y API key son los mismos de `tools/cloud/.env`.
2. Con PlatformIO instalado (extensión de VSCode o CLI):
   ```bash
   cd firmware/cloud-test
   pio run -t upload    # flashea por USB
   pio device monitor   # logs por Serial
   ```
   Si tu placa no la reconoce como `esp32-c3-devkitm-1`, probá cambiando el
   `board` en `platformio.ini` (algunos clones "Super Mini" andan mejor con
   `lolin_c3_mini`).

El sketch espera hasta 15 s a que abras el monitor serie (USB-CDC) antes de
empezar, así no te perdés el log de arranque.

## Configuración

Todo se puede pisar con `-D` en `build_flags` (valores por defecto entre paréntesis):

| Flag | Qué hace |
|---|---|
| `DB_HOST`, `DEVICE_ID` | Base y dispositivo (`pulsoximetro-esp-default-rtdb.firebaseio.com`, `pulsox-4ba0e4ea46d7`, los de `web/js/cloud-config.js`). |
| `MEASUREMENT_S` (60), `CYCLES` (1), `GAP_S` (10) | Duración, cantidad de mediciones automáticas (0 = esperar el comando `m`) y reposo entre ellas. |
| `FS_HZ` (50), `READING_EVERY_S` (5) | Frecuencia del PPG simulado y período de las lecturas del historial. |
| `FAKE_BATTERY_PCT` (78) | Batería informada; `-1` omite el campo. |
| `SCENARIO_MIXED` (0) | `1`: episodios aleatorios de SpO₂ baja / BPM anormal. |
| `CRASH_TEST` (0) | `1`: no cierra la medición (simula un corte de energía). |
| `FORCE_REFRESH_AFTER_S` (0) | `>0`: renueva el token a esos segundos del login, para probar sin esperar ~55 min. |
| `NTP_SYNC` (0) | `1`: sincroniza el reloj antes de TLS. Plan B si el handshake fallara por las fechas del certificado (el core no las valida por defecto). |

Ejemplo: `build_flags = ... -DFORCE_REFRESH_AFTER_S=60 -DMEASUREMENT_S=180`.

## Comandos por Serial

| Comando | Acción |
|---|---|
| `m` | Nueva medición |
| `e` | Terminar la actual (con cierre normal) |
| `c` | Corte: deja de escribir **sin cerrar** (`live` y la sesión quedan abiertos) |
| `r` | Forzar la renovación del token |
| `v` | Escritura inválida (`spo2=500`): hay que ver un 401 sin bucle |
| `s` | Estadísticas (ok/fallos, latencia, `tlsConnects`, logins, heap) |
| `?` | Ayuda |

## Qué mirar en el log

Cada línea empieza con el uptime en ms y un tag de 4 letras.

```
[   1530][BOOT] cloud-test cloud-test-0.1.0 | dispositivo pulsox-4ba0e4ea46d7 | base ...
[   3012][WIFI] conectado: IP 192.168.x.x RSSI -52 dBm
[   4980][AUTH] login ok (uid QD54Dm...) en 1850 ms
[   5700][BOOT] PUT info -> 204
[   5910][BOOT] DELETE live -> 204
[   6400][MEAS] medicion iniciada: sesion -Nxxxxxxxx (60 s, 50 Hz)
[  16410][LIVE] seq 10 | SpO2 97.4 BPM 74 | ticks OK 10/10 ... | PUT live media 85 max 140 ms
[  16411][LIVE] lecturas OK 2 fallo 0 | tlsConnects 1 | heap libre ... minimo ...
```

- El `uid` coincide con `tools/cloud/database.rules.json`.
- `tlsConnects 1` durante toda la medición: la conexión se reutiliza.
- `ticks OK` ≥ 57 de 60 y latencia media de `PUT live` muy por debajo de 1 s.
- El heap mínimo no baja medición tras medición (si baja, hay una fuga).
- Errores de login: `login rechazado: HTTP 400 INVALID_LOGIN_CREDENTIALS` (email o
  contraseña), `API key not valid` (API key). Un error de certificado en el
  handshake (`-1`/`CONNECTION_REFUSED` en el primer request) apunta a las CAs de
  `root_ca.h` o a la fecha: probá `-DNTP_SYNC=1`.
- Nunca se loguean el token, la contraseña ni la API key: solo longitudes y el `uid`.

## Checklist de verificación (en el hardware, en este orden)

1. **Compila**: `pio run` (revisá el uso de flash y RAM).
2. **Arranque**: conecta al WiFi; el login da OK con el `uid` de las reglas;
   `PUT info` y `DELETE live` devuelven 200 o 204.
3. **Medición de 60 s**: abrí la web en "Nube". Pasa de "Listo para medir" a la
   medición en vivo (SpO₂, BPM y traza PPG). Ticks OK ≥ 57/60, latencia media de
   `PUT` muy por debajo de 1 s, `tlsConnects` = 1 y heap mínimo estable.
4. **Cierre**: el Historial muestra la sesión de ~1 min y `live` queda borrado:
   `curl https://pulsoximetro-esp-default-rtdb.firebaseio.com/devices/pulsox-4ba0e4ea46d7/live.json`
   → `null`.
5. **Corte** (`c`, o desenchufar): la web cierra la medición a los ~10 s. Al
   resetear la placa, el `DELETE live` del arranque limpia el nodo.
6. **WiFi caído**: apagá el hotspot ~20 s a mitad de la medición. Los ticks
   fallan (`sin WiFi`) sin colgar la placa y se reanudan solos; la web cierra la
   medición y la reabre.
7. **Token**: con `-DFORCE_REFRESH_AFTER_S=60` (o el comando `r`) aparece "token
   renovado" y los `PUT` siguientes dan OK. Si se puede, una corrida de más de
   60 min confirma la renovación natural y que el heap no pierde memoria.
8. **Regla incumplida** (`v`): 401 → login nuevo → reintento → 401 → tick
   descartado. El siguiente tick da OK y no hay bucle.

## Para integrarlo al firmware real

`src/cloud.h/.cpp` es la parte reutilizable (WiFi + HTTPS + auth + escrituras).
`src/fake_vitals.*` se reemplaza por el pipeline real del MAX30102. Quedan fuera
de esta prueba, y van en el plan de integración: el botón GPIO0, el deep sleep
entre mediciones y la lectura de batería (la PCB no cablea ningún ADC a la
batería: GPIO2, GPIO8 y GPIO9 están libres; o se omite `battery_pct` o se agrega
un divisor).
