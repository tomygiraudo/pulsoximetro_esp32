# Plan: sketch de prueba `cloud-test` (ESP32-C3 → Firebase RTDB)

> **Estado: planificado y aprobado el 2026-10-08. Todavía NO se implementó nada.**
> Este archivo es el plan completo, escrito para retomarlo desde otra sesión de Claude Code (por ejemplo, desde la tablet) sin el contexto de la conversación original. Para ejecutarlo: abrí el repo en la rama `firmware-pulsox` y pedí "ejecutá `firmware/cloud-test/PLAN.md`". Cuando esté implementado, este archivo se puede borrar (queda el `README.md` del sketch).

## Contexto

La web ya está en GitHub Pages y lee la medición de Firebase Realtime Database (fuente "Nube"). Las reglas están verificadas contra el servicio real (18/18 PASS) y `tools/cloud/fake_device.py` (que vive en `main`, ver el paso 0) hace desde la PC lo que tiene que hacer el ESP32. Decisión del usuario: **el ESP32 es solo un cliente de la base que escribe telemetría**. No tiene servidor WebSocket, no lee nada y no recibe comandos.

El firmware de esta rama todavía no tiene HTTP/HTTPS, ni algoritmo de SpO₂/BPM, ni lectura de batería. El único código WiFi está en `firmware/battery-test` (UDP). Este plan arma un **proyecto PlatformIO de prueba aparte**, `firmware/cloud-test/`, que porta `fake_device.py` al ESP32-C3. Incluye WiFi, TLS validado contra CAs raíz, login, keep-alive, renovación del token y una medición con **valores simulados** (sin sensor ni TFT). El objetivo es validar el contrato de nube completo en el hardware real antes de integrarlo al firmware `pulsox`.

Decisiones tomadas con el usuario: ciclo simulado completo; TLS con CAs raíz, sin `setInsecure()`.

Datos del proyecto Firebase (no son secretos, ya están en `main`, `web/js/cloud-config.js`): `databaseURL = https://pulsoximetro-esp-default-rtdb.firebaseio.com`, `deviceId = pulsox-4ba0e4ea46d7`.

## Cómo ejecutarlo desde una sesión sin la placa (tablet / Claude Code en la web)

- **Se puede hacer ahí**: el paso 0, escribir todo el código y los certificados, y compilar con `pio run` si el entorno tiene PlatformIO y red para bajar el toolchain (si no, dejar la compilación para la PC). Para compilar, crear un `include/secrets.h` con valores falsos a partir del `.example`; está en `.gitignore` y **nunca** se commitea. Los certificados raíz se bajan y se verifican con `openssl`.
- **Necesita la PC con la placa por USB**: el `secrets.h` real, el flasheo y las pruebas 2 a 8 de "Verificación".
- **Nunca pegar en la sesión** la contraseña del WiFi, la API key ni la contraseña del usuario de Firebase. Esos valores se cargan después en la PC, en `secrets.h` (son los mismos de `tools/cloud/.env`).
- Al terminar: commit y push a `firmware-pulsox`. Después, en la PC: `git pull`, crear `secrets.h`, flashear y verificar.

## Paso 0: preparar la rama

- Desde la base común (`8cb9199`), `firmware-pulsox` tenía 14 commits que `main` no tiene, y `main` tiene 11 que ella no.
- **Mergear `origin/main` en `firmware-pulsox`** trae el `PROTOCOL.md` vigente (sección "Transporte en la nube"), `tools/cloud/` (`fake_device.py`, `mock_rtdb.js`, `check_rules.py`, reglas) y la web actualizada. Hoy la rama tiene un `PROTOCOL.md` viejo, solo con WebSocket.
- Solo `README.md` cambió de los dos lados, así que es el único conflicto esperable. Se resuelve conservando ambas secciones.
- Hacer el commit del merge y seguir en esa rama.

## Estructura nueva: `firmware/cloud-test/`

Sigue el molde de `firmware/battery-test/`: proyecto PlatformIO completo, README en español, `secrets.h` ignorado.

```
firmware/cloud-test/
  platformio.ini            copia del env de battery-test (esp32-c3-devkitm-1, arduino, USB-CDC); sin lib_deps
  .gitignore                .pio/  include/secrets.h
  include/secrets.h.example WIFI_SSID, WIFI_PASSWORD, FIREBASE_API_KEY, FIREBASE_EMAIL, FIREBASE_PASSWORD
  include/root_ca.h         PEM concatenado: GTS Root R1 + GTS Root R4
  src/cloud.h / cloud.cpp   WiFi + HTTPS + Auth + escrituras a la base (lo reutilizable para pulsox)
  src/fake_vitals.h / .cpp  generador de SpO2/BPM/PPG (port de Vitals de fake_device.py)
  src/main.cpp              ciclo de medición, planificador de 1 Hz, comandos por Serial, métricas
  README.md                 setup, flasheo y checklist de pruebas
```

`WiFi.h`, `WiFiClientSecure` y `HTTPClient` vienen con el core Arduino-ESP32 2.0.17 (platform espressif32, el mismo que usan los otros proyectos). No hace falta ArduinoJson: el JSON se arma con `snprintf` como en `battery-test`, y de las respuestas se extraen pocos campos con un helper mínimo.

## Diseño por archivo

### `include/root_ca.h`: TLS con dos raíces
Verificado con `openssl s_client` (2026-10-08): los hosts usan **cadenas distintas**.
- `pulsoximetro-esp-default-rtdb.firebaseio.com`: WR1 → **GTS Root R1** (RSA). El SAN incluye `*.firebaseio.com`.
- `identitytoolkit.googleapis.com` y `securetoken.googleapis.com`: WE2 → **GTS Root R4** (ECDSA).

Los dos certificados se ponen concatenados en un solo PEM y se cargan con `WiFiClientSecure::setCACert()`, que acepta varios certificados. Se bajan del repositorio oficial `pki.goog` y la huella SHA-256 (`openssl x509 -noout -fingerprint -sha256`) se verifica contra la que publica `pki.goog` y contra el almacén de certificados del sistema antes de fijarlos. No hace falta NTP: el core no valida fechas por defecto. Si el handshake fallara por la validez del certificado, se agrega `configTime()` como plan B.

### `src/cloud.h/.cpp`: cliente de la base (port de `Rest` / `Auth` / `Db` de `tools/cloud/fake_device.py:68-189`)
- `bool wifiConnect(uint32_t timeoutMs)`: `WIFI_STA`, `setAutoReconnect(true)` y un log con IP y RSSI. Sale del patrón de `firmware/battery-test/src/main.cpp:244-256`.
- La clase `CloudDb`:
  - **Conexión a la base persistente**: un `WiFiClientSecure` miembro y `HTTPClient` con `setReuse(true)`. Cada request hace `http.begin(client, DB_HOST, 443, uri, true)` y después `end()`, que deja la conexión abierta si se leyó la respuesta completa. Antes de cada request se mira `client.connected()` y se cuenta `tlsConnects` para comprobar que hay un solo handshake.
  - **Auth en una conexión aparte y efímera**: es otro host y se usa solo al arrancar y cada ~50 min.
    - `login()` hace `POST /v1/accounts:signInWithPassword?key=` y guarda `idToken`, `refreshToken`, `expiresIn` y `localId`.
    - `refresh()` hace `POST securetoken /v1/token?key=` con un formulario `grant_type=refresh_token&refresh_token=` y guarda `id_token`, `refresh_token` y `expires_in`.
    - Renueva a `expires − min(300 s, ttl/2)`, como `fake_device.py:137-152`. Si el refresh falla, hace un login nuevo.
  - `put/patch/del(path, json)` agregan `?auth=<idToken>&print=silent`. `post(path, json, String* pushKey)` extrae `"name"` de la respuesta.
  - **401 → `invalidate()` + login + un solo reintento**, como `fake_device.py:165-177`. Un `.validate` incumplido también devuelve 401, así que no se reintenta más de una vez.
  - Cada llamada devuelve el código HTTP (negativo si falla el transporte) y registra la latencia en `stats`: ok/fallas, latencia media y máxima, `tlsConnects` y `ESP.getMinFreeHeap()`.
  - Timeouts de 5 s (`setTimeout`). Un tick que falla se descarta y no hay cola, como pide `PROTOCOL.md` ("Fallos de red").
  - Helpers: `jsonEscape()` para email y contraseña, y `jsonField(body, key)` para leer un string de la respuesta. Ahí entran `idToken`, `name` y `error.message`, que se usa en los logs.
  - **Nunca se loguean el token, la contraseña ni la API key**: solo longitudes y el `uid`.

### `src/fake_vitals.h/.cpp`: port de `Vitals` y `ppg_pulse` (`fake_device.py:195-258`)
- Paseo aleatorio de SpO₂ y BPM con ruido gaussiano (Box-Muller sobre `esp_random()`).
- Escenarios `normal` y `mixed`, y unos ticks sin dedo al empezar.
- `ppgBatch()` genera `FS_HZ` enteros (1 s de señal) con el pico sistólico hacia arriba.

### `src/main.cpp`: ciclo de medición (port de `run_measurement` y `main`, `fake_device.py:268-405`)
- **Configuración** con `#ifndef`, que se puede pisar con `-D`:
  - `DB_HOST "pulsoximetro-esp-default-rtdb.firebaseio.com"`
  - `DEVICE_ID "pulsox-4ba0e4ea46d7"`
  - `FW_VERSION "cloud-test-0.1.0"`
  - `MEASUREMENT_S 60`, `CYCLES 1`, `GAP_S 10`, `FS_HZ 50`, `READING_EVERY_S 5`
  - `FAKE_BATTERY_PCT 78` (-1 la omite)
  - `SCENARIO_MIXED 0`, `CRASH_TEST 0`
  - `FORCE_REFRESH_AFTER_S 0`, para probar la renovación sin esperar 50 min.
- **Arranque**:
  1. `waitForMonitor()`, igual que en `battery-test`.
  2. WiFi.
  3. Login. Se imprime el `uid` para compararlo con el de `tools/cloud/database.rules.json:9`.
  4. `PUT info {device_id, fw_version, sensor:"MAX30102 (simulado)", battery_pct, updated:SV}`.
  5. `DELETE live`.
- **Medición**:
  - Al empezar: `POST sessions {startedAt:SV}` → `sid`.
  - Un planificador por `millis()` hace `nextTick += 1000`. Si se atrasó más de un tick, se resincroniza y se cuentan los ticks salteados.
  - En cada tick:
    - Hace `PUT live`. Lleva `session_id`, `seq`, `ts:SV`, `elapsed_ms`, `finger_detected`, `spo2`/`bpm` con sus `*_valid`, `signal_quality`, `battery_pct`, `fs` y `ppg` en CSV; `spo2`, `bpm` y `ppg` se omiten sin dedo.
    - Cada `READING_EVERY_S`, si la lectura es válida, hace `POST sessions/<sid>/readings`.
  - Con WiFi caído se saltea el request.
  - Buffer de 1 KB: el `live` ronda 0,6 KB, y la regla permite `ppg` de hasta 2000 caracteres.
- **Cierre**, en el orden de `PROTOCOL.md` (8, 9, 10): `PATCH sessions/<sid> {endedAt}` → `PATCH info {battery_pct, updated}` → `DELETE live`. Con `CRASH_TEST` se omite.
- **Al terminar los ciclos**: queda despierto y con USB, esperando comandos. Deep sleep queda fuera de esta prueba.
- **Comandos por Serial**, para probar a mano:

  | Comando | Acción |
  |---|---|
  | `m` | Nueva medición |
  | `e` | Terminar la actual |
  | `c` | Corte: deja de escribir sin cerrar |
  | `r` | Forzar la renovación del token |
  | `v` | Escritura inválida (`spo2=500`), para ver que el 401 no entra en bucle |
  | `s` | Mostrar estadísticas |

- **Logs** con el formato `DBG(tag, fmt, ...)` de `firmware/pulsox/src/debug_log.h:7-8`, copiado localmente. Cada 10 ticks se imprime un resumen con ok/total, latencia media y máxima, `tlsConnects` y el heap libre y mínimo.

### `README.md`
En el estilo de `firmware/battery-test/README.md`. Cubre:
- Copiar `secrets.h.example` a `secrets.h`. Email, contraseña y API key son los mismos de `tools/cloud/.env`.
- `pio run -t upload` y `pio device monitor`.
- Qué log esperar.
- El checklist de verificación de abajo.

## Verificación (en el hardware, en este orden)

1. **Compila**: `pio run` en `firmware/cloud-test`, revisando el uso de flash y RAM.
2. **Arranque**: conecta al WiFi (IP y RSSI). El login responde OK con el `uid` igual al de las reglas. `PUT info` y `DELETE live` devuelven 200 o 204.
3. **Medición de 60 s**: abrir `https://tomygiraudo.github.io/pulsoximetro_esp32/` en Nube. Debe pasar de "Listo para medir" a la medición en vivo (SpO₂, BPM y traza PPG). En los logs:
   - 57 o más ticks OK de 60.
   - Latencia media de `PUT` muy por debajo de 1 s.
   - **`tlsConnects` = 1**: la conexión se reutiliza.
   - Heap mínimo estable.
4. **Cierre**: el Historial muestra la sesión de ~1 min y `live` queda borrado. Se puede chequear con `curl <db>/devices/<id>/live.json` → `null`.
5. **Corte** (`c` o desenchufar): la web cierra la medición a los ~10 s. Al resetear la placa, el `DELETE live` del arranque limpia el nodo.
6. **WiFi caído**: apagar el hotspot ~20 s a mitad de la medición. Los ticks fallan sin colgar la placa y se reanudan solos. La web cierra la medición y la reabre.
7. **Token**: con `-DFORCE_REFRESH_AFTER_S=60` o el comando `r` debe aparecer "token renovado" y los `PUT` siguientes deben dar OK. Si se puede, una corrida de más de 60 min confirma la renovación natural y que el heap no pierde memoria.
8. **Regla incumplida** (`v`): 401 → login nuevo → reintento → 401 → tick descartado. El siguiente tick da OK y no hay bucle.

## Fuera de alcance (pendientes que encontró la exploración)

- **Sensor real y algoritmo de SpO₂/BPM**: no existen todavía. En `firmware/pulsox/include/config.h:34-84` solo están los parámetros. La integración se hace después, reemplazando `fake_vitals` por el pipeline real.
- **Batería**: la PCB no cablea ningún ADC a la batería. GPIO2, GPIO8 y GPIO9 están libres, y `CHARGING`/`COMPLETE` del TP4056 no llegan al ESP32. El firmware real tendrá que omitir `battery_pct` o agregar un divisor (decisión de hardware).
- **Botón GPIO0 y deep sleep**: ningún código usa el botón y `pulsox` no duerme. El ciclo "botón → medición → cierre → dormir" va en el plan de integración.
- **Limpieza de docs**: quitar el WebSocket/"Red local" de `PROTOCOL.md`, del README y de la app (por ejemplo `firmware/battery-test/README.md:19-23`, que menciona el servidor WebSocket), según la decisión de que el ESP32 sea solo cliente.
- Ruta contra el mock local por `http://`: no hace falta, la base real ya está verificada.
