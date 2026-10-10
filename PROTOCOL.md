# Protocolo de telemetría — PulsOx ESP32

Este documento define el formato de los paquetes que el firmware del ESP32-C3
debe enviar a la web app para que el dashboard funcione. Como todavía no existe
firmware, este formato es el contrato a implementar del lado del dispositivo.

> **Estado del documento.** El flujo de una medición y la semántica de cada
> paquete están definidos. El **armado de los paquetes nuevos**
> (`measurement_start`, `ppg` y `measurement_end`: campos, tipos, unidades,
> frecuencia) queda **PENDIENTE** — ver [Pendientes](#pendientes). Los paquetes
> `hello`, `telemetry` y `status` ya están cerrados.

## Transporte

**WebSocket**, con el ESP32 actuando como servidor en la red WiFi local.

```
ws://<ip-del-esp32>/ws
```

Se eligió WebSocket sobre BLE (Web Bluetooth) porque Web Bluetooth no funciona
en iOS/Safari, mientras que WebSocket sobre WiFi funciona en cualquier
navegador móvil moderno (Android e iOS) sin pairing previo. El ESP32 puede
operar en modo AP (crea su propia red `PulsOx-XXXX`) o en modo STA (se une a
la WiFi de casa) — el dashboard solo necesita la IP.

> **Transporte alternativo: la nube.** Un navegador bloquea `ws://` desde una
> página `https://` (contenido mixto), y el historial en `localStorage` no se
> comparte entre dispositivos. Por eso existe un segundo transporte en el que el
> ESP32 escribe en una Realtime Database de Firebase y la web la lee (ver
> [Transporte en la nube](#transporte-en-la-nube-firebase-realtime-database)).
> Los paquetes son los mismos y el WebSocket LAN sigue funcionando.

Todos los mensajes son JSON, un objeto por frame de texto. El campo `type`
identifica el tipo de mensaje.

## Ciclo de una medición

El dispositivo **no transmite lecturas mientras está en reposo**. Una medición
empieza cuando el usuario presiona el botón de medición del dispositivo:

```
ESP32                                                         Web app
  │  (el cliente abre la conexión)                               │
  │ ── hello ──────────────────────────────────────────────────► │  dashboard: "Listo para medir"
  │                                                              │
  │  (se presiona el botón de medición)                          │
  │ ── measurement_start ──────────────────────────────────────► │  abre la medición, arranca el cronómetro
  │ ── telemetry (≈ 1 Hz) ─────────────────────────────────────► │  SpO2, BPM, calidad, batería
  │ ── ppg (lotes de muestras) ────────────────────────────────► │  traza PPG en vivo
  │      …                                                       │
  │ ── measurement_end ────────────────────────────────────────► │  cierra la medición (ver pendientes)
```

- La web app asocia todas las lecturas recibidas entre `measurement_start` y el
  fin de la medición a **una misma medición**; el historial se agrupa así.
- `telemetry` y `ppg` solo se envían **dentro de** una medición.
- Si se cierra la conexión durante una medición, la web app la da por terminada.

## 1. `hello` — identificación del dispositivo

Se envía **una vez**, inmediatamente después de que el cliente abre la
conexión.

```json
{
  "type": "hello",
  "device_id": "pulsox-esp32-01",
  "fw_version": "0.1.0",
  "sensor": "MAX30102",
  "sample_rate_hz": 1
}
```

| Campo | Tipo | Descripción |
|---|---|---|
| `device_id` | string | Identificador único del dispositivo (p. ej. derivado de la MAC). |
| `fw_version` | string | Versión del firmware, para diagnóstico. |
| `sensor` | string | Sensor físico usado. |
| `sample_rate_hz` | number | Frecuencia aproximada de envío de `telemetry`. |

## 2. `measurement_start` — inicio de una medición

> **Estado: ⏳ PENDIENTE — armado del paquete a definir.**

Lo envía el ESP32 **una vez**, cuando se presiona el botón de medición. Es el
primer paquete de la medición: hasta que llega, la app muestra el dashboard en
espera ("Listo para medir", valores en "—", traza PPG vacía).

**Qué hace la app con este paquete:** abre una medición nueva (si había una
abierta, la cierra), pone el cronómetro en 00:00, limpia la traza PPG y crea el
grupo correspondiente en el historial.

**Supuesto provisional del cliente y del simulador** (no es el formato
definitivo): la app solo necesita el `type`. El identificador y la hora de
inicio de la medición los asigna ella misma al recibir el paquete; cualquier
otro campo se ignora. El simulador envía:

```json
{ "type": "measurement_start", "session_id": "sim-1700000000000", "uptime_ms": 5120 }
```

En el [transporte en la nube](#transporte-en-la-nube-firebase-realtime-database)
la web sintetiza este paquete con dos campos que la app **solo usa en ese
transporte**: `session_id` (la push key de `sessions/<sid>`, para que la medición
abierta coincida con el historial compartido) y `started_at` (ms epoch, para que
el cronómetro de quien se une a mitad de medición sea correcto).

## 3. `telemetry` — lectura periódica

El paquete de lecturas. Se espera aproximadamente **1 vez por segundo**, y solo
dentro de una medición (el algoritmo de SpO2/BPM del MAX30102 ya promedia
internamente sobre varios segundos de señal PPG).

```json
{
  "type": "telemetry",
  "seq": 4821,
  "uptime_ms": 182340,
  "finger_detected": true,
  "spo2": 97.4,
  "spo2_valid": true,
  "bpm": 72,
  "bpm_valid": true,
  "signal_quality": 0.86,
  "battery_pct": 78
}
```

| Campo | Tipo | Rango / notas |
|---|---|---|
| `seq` | integer | Contador incremental (detecta paquetes perdidos). |
| `uptime_ms` | integer | Milisegundos desde el boot del ESP32 (no hay RTC; el timestamp absoluto lo agrega el cliente al recibir). |
| `finger_detected` | boolean | `false` si el sensor no detecta dedo (IR por debajo de umbral). Si es `false`, `spo2`/`bpm` deben ignorarse aunque vengan presentes. |
| `spo2` | number \| null | Saturación de oxígeno en %, 0–100. `null` si no hay lectura válida. |
| `spo2_valid` | boolean | `true` solo si el algoritmo considera la lectura confiable (suficientes ciclos de pulso, sin saturación de señal). |
| `bpm` | number \| null | Frecuencia cardíaca en latidos por minuto. `null` si no hay lectura válida. |
| `bpm_valid` | boolean | Igual que `spo2_valid` pero para BPM. |
| `signal_quality` | number | 0.0–1.0, estimación de calidad de señal PPG (perfusion index normalizado o similar). Se usa para mostrar la barra de calidad de señal. |
| `battery_pct` | integer \| omitido | Opcional. Si el dispositivo no tiene medición de batería, se omite el campo por completo (no enviar `null`). |

Notas de diseño:
- Los campos `*_valid` existen porque el algoritmo de SpO2 del MAX30102 puede
  producir números incluso cuando la señal es mala; separarlos evita que el
  dashboard muestre un valor "creíble" que en realidad es ruido.
- El dashboard trata un paquete con `finger_detected: false` como "sin
  lectura": muestra "Colocá el dedo en el sensor", borra la traza PPG y no
  agrega el paquete al historial.
- Una lectura inválida aislada **no** borra el valor mostrado: la app conserva
  el último valor válido hasta 5 s (el gráfico de tendencia sí registra el
  corte).

## 4. `ppg` — señal PPG

> **Estado: ⏳ PENDIENTE — armado del paquete a definir.**

Transporta la señal fotopletismográfica **ya filtrada y acondicionada** por el
firmware, para dibujarla en vivo en el dashboard debajo de la frecuencia
cardíaca. La app **no filtra ni procesa**: solo la dibuja.

**Qué necesita la app de este paquete:**
- Muestras numéricas en orden temporal, con polaridad tal que el **pico
  sistólico apunte hacia arriba**. La escala es libre: la app autoescala el eje
  vertical, así que no hace falta normalizar ni mandar unidades.
- La frecuencia de muestreo, para ubicar cada muestra en el eje de tiempo.
- Las muestras llegan **en lotes** (un frame = varias muestras): mandar una
  muestra por frame a decenas de Hz satura el servidor WebSocket del ESP32-C3.

**Qué hace la app:** mantiene una ventana deslizante de 6 s, dibujada con un
retardo de ~0,3 s respecto de "ahora" para que los lotes se rellenen de forma
fluida. Mientras no hay dedo (según `telemetry`), no dibuja la traza.

**Supuesto provisional del cliente y del simulador** (no es el formato
definitivo): `fs` en Hz y `samples` como arreglo de números; el simulador usa
`fs = 50` y manda 10 muestras cada 200 ms.

```json
{ "type": "ppg", "fs": 50, "samples": [112, 340, 655, 801, 742, 590, 331, 40, -120, -210] }
```

## 5. `measurement_end` — fin de una medición

> **Estado: ⏳ PENDIENTE — falta decidir si existe como paquete y su armado.**

Marcaría el fin de la medición (el usuario la detiene o el firmware la corta por
duración máxima). Al recibirlo la app cierra la medición, deja de mostrar
lecturas y vuelve a "Listo para medir"; el grupo del historial queda con su
duración.

**Supuesto provisional del cliente y del simulador:** la app solo necesita el
`type`. El simulador envía `{ "type": "measurement_end", "uptime_ms": 65210 }`.

## 6. `status` — eventos y errores

Mensaje esporádico para comunicar cambios de estado que no son una lectura.

```json
{
  "type": "status",
  "level": "error",
  "code": "sensor_not_found",
  "message": "No se detecta el MAX30102 en el bus I2C"
}
```

| Campo | Tipo | Valores |
|---|---|---|
| `level` | string | `"info"` \| `"warning"` \| `"error"` |
| `code` | string | Código estable para lógica del cliente (`sensor_not_found`, `low_battery`, `wifi_reconnected`, etc.). |
| `message` | string | Texto legible, se muestra tal cual en la UI como fallback. |

## Convenciones generales

- Codificación: JSON UTF-8 en frames de texto WebSocket (no binario).
- El servidor (ESP32) puede cerrar y reabrir la conexión libremente; el
  cliente reintenta con backoff exponencial (ver `web/js/connection.js`).
- No se espera que el cliente envíe telemetría al servidor; el canal es
  unidireccional (ESP32 → navegador). Un futuro `ping`/`pong` de aplicación no
  es necesario porque el protocolo WebSocket ya tiene ping/pong nativo.
- Todos los valores numéricos van sin comillas (JSON number), nunca como
  string.
- Un `type` desconocido se descarta sin error (compatibilidad hacia adelante).

## Umbrales fisiológicos usados por el dashboard

Estos umbrales son los que la web app usa por defecto para colorear el
estado (verde / amarillo / rojo). Son guías generales de bienestar, **no**
un dispositivo médico certificado, y son configurables desde Ajustes
(se guardan en `localStorage`, no se envían al ESP32).

| Parámetro | Verde (normal) | Amarillo (precaución) | Rojo (peligro) |
|---|---|---|---|
| SpO2 | ≥ 95% | 90–94% | < 90% |
| BPM (reposo) | 60–100 | 50–59 o 101–120 | < 50 o > 120 |

## Ejemplo de sesión

Los paquetes marcados con ⏳ tienen el armado pendiente; se muestran con el
supuesto provisional del cliente.

```
→ (cliente abre ws://192.168.1.42/ws)
← {"type":"hello","device_id":"pulsox-esp32-01","fw_version":"0.1.0","sensor":"MAX30102","sample_rate_hz":1}
      … (reposo: el dashboard muestra "Listo para medir") …
      … (se presiona el botón de medición) …
← ⏳ {"type":"measurement_start"}
← {"type":"telemetry","seq":1,"uptime_ms":1000,"finger_detected":false,"spo2":null,"spo2_valid":false,"bpm":null,"bpm_valid":false,"signal_quality":0.0}
← {"type":"telemetry","seq":2,"uptime_ms":2000,"finger_detected":true,"spo2":96.8,"spo2_valid":false,"bpm":81,"bpm_valid":false,"signal_quality":0.31}
← ⏳ {"type":"ppg","fs":50,"samples":[…10 muestras…]}
← {"type":"telemetry","seq":3,"uptime_ms":3000,"finger_detected":true,"spo2":97.2,"spo2_valid":true,"bpm":74,"bpm_valid":true,"signal_quality":0.79}
← ⏳ {"type":"ppg","fs":50,"samples":[…10 muestras…]}
      …
← ⏳ {"type":"measurement_end"}
```

## Implementación de referencia (cliente)

El parseo y la validación de estos mensajes están en
[`web/js/protocol.js`](web/js/protocol.js); la traza PPG se dibuja en
[`web/js/ppg.js`](web/js/ppg.js). El simulador de demo en
[`web/js/simulator.js`](web/js/simulator.js) genera el ciclo completo
(`hello` → `measurement_start` → `telemetry` + `ppg` → `measurement_end`), así
que sirve como referencia ejecutable además de este documento. El cliente es
**permisivo** con los paquetes pendientes (solo exige el `type` y, para `ppg`,
el arreglo numérico `samples`), de modo que el armado definitivo se puede
decidir sin romper la app.

Si llegan `telemetry` o `ppg` sin un `measurement_start` previo (p. ej. el
cliente se reconecta con una medición en curso), la app abre una medición
implícita en lugar de descartar los datos.

## Transporte en la nube (Firebase Realtime Database)

Segundo transporte, para que **cualquiera con la URL de la web** vea la medición
en vivo y el historial desde cualquier red, sin conocer la IP del ESP32 ni estar
en su WiFi. GitHub Pages es hosting estático (no guarda datos ni acepta
conexiones) y el ESP32 no puede ser cliente de la *página*; lo que sí se puede es
que **los dos sean clientes del mismo servicio en la nube**:

```
ESP32 ──HTTPS (REST, con login)──►  Firebase Realtime DB  ◄──REST + streaming (EventSource), lectura pública──  Web (Pages, cualquier celular/PC)
```

- **Solo el dispositivo escribe.** La lectura es pública (sin login); la
  escritura la permite únicamente el `uid` del usuario del dispositivo
  ([`tools/cloud/database.rules.json`](tools/cloud/database.rules.json)).
- **La medición se inicia solo con el botón del dispositivo**; la app no manda
  comandos. Por eso el ESP32 puede dormir en deep sleep entre mediciones: no
  tiene que estar escuchando.
- Se guardan **valores crudos**. El estado (normal / precaución / peligro) lo
  calcula cada cliente con *sus* umbrales.
- Las horas son del servidor (`{".sv":"timestamp"}`, abreviado `SV` abajo): el
  ESP32 no tiene RTC ni necesita NTP.
- Los paquetes que entiende la app son los mismos de las secciones 1–6; este
  transporte solo cambia cómo viajan (ver [cómo lo interpreta la web](#cómo-lo-interpreta-la-web)).

### Nodos

```
/devices/<deviceId>/
  info      { device_id, fw_version, sensor, battery_pct, updated }      al arrancar y al terminar cada medición
  live      { session_id, seq, ts, elapsed_ms, finger_detected, spo2,    PUT a 1 Hz solo durante la medición;
              spo2_valid, bpm, bpm_valid, signal_quality, battery_pct,   se borra al terminar
              fs, ppg:"112,340,…" }
  sessions/<sid>  { startedAt, endedAt, readings/<pushKey> { ts, spo2, bpm, quality } }
                                                                          sid = push key (orden cronológico);
                                                                          readings: UNA sola, el resultado, al terminar
```

`<deviceId>` es la "llave" de la URL de lectura (`web/js/cloud-config.js` y
`PULSOX_DEVICE_ID`): un valor no obvio y sin datos personales.

**`live`** (se reemplaza completo con `PUT` cada segundo; la base no guarda
`null`, así que un campo sin valor **se omite**):

| Campo | Tipo | Notas |
|---|---|---|
| `session_id` | string | Push key de la sesión (`sessions/<sid>`). |
| `seq` | integer | Contador de la medición (1, 2, 3…). Cada `seq` nuevo es un "tick" para la web. |
| `ts` | number | `SV`. |
| `elapsed_ms` | integer | Milisegundos desde que **empezó la medición** (dedo detectado y conexión resuelta; no desde que se apretó el botón: antes hay unos segundos de conexión), medidos por el dispositivo. La web calcula con esto el inicio de la medición (quien entra a mitad ve el cronómetro correcto sin fiarse de relojes). |
| `finger_detected` | boolean | Igual que en `telemetry`. |
| `spo2`, `bpm` | number | Solo si hay dedo; ver `telemetry`. |
| `spo2_valid`, `bpm_valid` | boolean | Igual que en `telemetry`. |
| `signal_quality` | number | 0.0–1.0. |
| `battery_pct` | integer | Se omite si no se mide. |
| `fs` | integer | Frecuencia de muestreo del PPG en Hz. El firmware manda 50 (la señal de 100 sps, promediada de a dos). |
| `ppg` | string | `fs` muestras enteras (1 s de señal) separadas por comas, ya filtradas, pico sistólico hacia arriba. Se omite sin dedo. Una hoja atómica (~250 B a 50 Hz) en vez de un arreglo, y un solo request por segundo con la telemetría incluida. |

**`info`**: `device_id`, `fw_version`, `sensor` (strings, como en `hello`),
`battery_pct` (se actualiza al terminar cada medición: es la batería "en
reposo") y `updated` (`SV`).

**`sessions/<sid>`**: `startedAt` (`SV`, lo escribe el `POST` que abre la
medición), `endedAt` (`SV`, al terminar) y **una sola** lectura,
`readings/<pushKey>` con `{ ts (SV), spo2, bpm, quality }`: el resultado de la
medición (la mediana de los segundos estables de los últimos 20 s). El historial
no lleva las lecturas intermedias: vienen de un filtro que todavía se está
asentando (la primera suele dar valores bajos) y no sirven como registro; lo que
se ve evolucionar en vivo es `live`. Una sesión sin lectura es una fila vacía en
el historial, así que si la lectura final no se puede escribir la sesión se
borra (ver "Medición descartada").

### Llamadas REST del dispositivo

`<db>` es el `databaseURL` (`https://<proyecto>-default-rtdb.firebaseio.com`) y
`<auth>` el `idToken` del usuario del dispositivo (`?auth=<idToken>`). Las
escrituras que no necesitan respuesta llevan `&print=silent` (la base contesta
`204 No Content`).

| # | Cuándo | Llamada | Cuerpo | Respuesta |
|---|---|---|---|---|
| 1 | Al arrancar | `POST https://identitytoolkit.googleapis.com/v1/accounts:signInWithPassword?key=<API_KEY>` | `{"email","password","returnSecureToken":true}` | `idToken`, `refreshToken`, `expiresIn: "3600"`, `localId` |
| 2 | Antes de que venza el token | `POST https://securetoken.googleapis.com/v1/token?key=<API_KEY>` | formulario `grant_type=refresh_token&refresh_token=…` | `id_token`, `refresh_token`, `expires_in: "3600"` |
| 3 | Al arrancar | `PUT <db>/devices/<id>/info.json?auth=<auth>` | `info` completo | — |
| 4 | Al arrancar | `DELETE <db>/devices/<id>/live.json?auth=<auth>` | — | — |
| 5 | Botón de medición | `POST <db>/devices/<id>/sessions.json?auth=<auth>` | `{"startedAt": SV}` | `{"name": "<sid>"}` |
| 6 | Cada segundo | `PUT <db>/devices/<id>/live.json?auth=<auth>` | `live` completo | — |
| 7 | Al terminar (medición válida) | `POST <db>/devices/<id>/sessions/<sid>/readings.json?auth=<auth>` | `{"ts": SV, "spo2", "bpm", "quality"}` | `{"name": …}` |
| 8 | Al terminar | `PATCH <db>/devices/<id>/sessions/<sid>.json?auth=<auth>` | `{"endedAt": SV}` | — |
| 9 | Al terminar | `PATCH <db>/devices/<id>/info.json?auth=<auth>` | `{"battery_pct", "updated": SV}` (el firmware actual no mide batería: solo `updated`) | — |
| 10 | Al terminar | `DELETE <db>/devices/<id>/live.json?auth=<auth>` | — | — |
| 11 | Medición descartada | `DELETE <db>/devices/<id>/sessions/<sid>.json?auth=<auth>` y después la 10 | — | — |

Notas para el firmware (la especificación ejecutable es
[`tools/cloud/fake_device.py`](tools/cloud/fake_device.py)):

- **Orden de cierre: 8, 9 y 10 al final.** Borrar `live` es la señal de
  "medición terminada" para los visores; si va antes, el historial que refrescan
  podría no tener todavía `endedAt`.
- **Conexión persistente.** Un handshake TLS por cada PUT de 1 Hz costaría
  ~1 s: mantener una sola conexión HTTPS abierta (keep-alive;
  `HTTPClient::setReuse(true)` en el ESP32) y reabrirla si el servidor la cierra.
- **Token.** Vence a la hora (`expiresIn: 3600`). Renovarlo con margen (p. ej. a
  los 50 min con la llamada 2); ante un `401` hacer un login nuevo (llamada 1) y
  reintentar una vez. Un `401` también lo devuelve una regla incumplida
  (`.validate`, escritura de otro usuario): la API REST responde `401
  Permission denied` ante cualquier violación de reglas.
- **Fallos de red.** Un tick que no se pudo escribir se descarta (el siguiente
  llega en 1 s); no acumular una cola. El historial se arma con la lectura
  final (llamada 7), que no necesita 1 Hz: se reintenta 3 veces porque sin ella
  la sesión queda vacía.
- **Medición descartada** (dedo retirado, señal errática, lecturas
  insuficientes, sensor saturado): llamadas 11 y 10, en ese orden. No queda nada
  de esa medición en el historial y los visores ven `live` desaparecer como en
  una medición normal.
- **Sin nube** (sin WiFi, login rechazado): el dispositivo mide igual y muestra
  el resultado como "NO ENVIADO"; no guarda nada para mandar después.
- **Arranque y deep sleep.** Al arrancar: llamadas 3 y 4 (4 limpia el `live` que
  deja un corte de energía a mitad de medición). Antes de dormir, la medición
  tiene que estar cerrada (8–10).
- **Credenciales.** Email/contraseña y API key van en un archivo que no se
  versiona (en el firmware, un header ignorado por git; en las herramientas,
  `tools/cloud/.env`). La API key web de Firebase identifica al proyecto, no
  autoriza nada por sí sola.
- **Certificado.** Validar el certificado TLS contra la CA raíz que usan los
  servidores de Google (verificarla desde el navegador antes de fijarla en el
  firmware) en lugar de `setInsecure()`.
- **Volumen.** Un PUT de `live` ronda los 0,5–0,6 KB: una medición de 1 h son
  ~2 MB de subida más ~720 lecturas de ~70 B.

### Cómo lo interpreta la web

`web/js/cloud.js` (`CloudSource`) abre un `EventSource` a
`<db>/devices/<id>/live.json` (la base manda `put` / `patch` con `{path, data}`
y un `keep-alive` cada ~30 s; si antepone un `307`, el navegador lo sigue solo),
mantiene una copia local de `live` y **sintetiza los mismos frames del
WebSocket**, de modo que `app.js` no distingue el transporte:

| Cambio en la base | Frame que recibe la app |
|---|---|
| `GET info.json` al abrir y al terminar cada medición | `hello` (`device_id`, `fw_version`, `sensor`, `sample_rate_hz: 1` y, si hay, `battery_pct`: la batería en reposo) |
| Aparece `live` o cambia su `session_id` | `measurement_start` con `session_id` y `started_at` (ms epoch = hora local de recepción − `elapsed_ms`; son los dos campos que la app usa de este paquete solo en este transporte) |
| Cada `seq` nuevo | `telemetry` y, si hay `ppg`, `ppg` (`fs` y `samples`, en ese orden) |
| `live` se borra | `measurement_end` |
| ~10 s sin un `seq` nuevo con la medición abierta | `measurement_end` (dispositivo apagado o sin WiFi a mitad); si vuelve a escribir, la medición se reabre |

Más detalles del comportamiento:

- Un `live` que ya está en la base al conectar **no se cree hasta que cambia**
  (llega otro `seq` en 3 s): si el dispositivo perdió energía a mitad de una
  medición, ese nodo queda para siempre y no debe mostrarse como una medición en
  curso. Un `live` que aparece con el stream abierto arranca sin espera.
- Sin ningún evento (ni `keep-alive`) durante 65 s se reabre el stream; ante
  `cancel` / `auth_revoked` o un error HTTP se reintenta con backoff
  (1 s … 15 s); al volver a la pestaña (`visibilitychange`; iOS corta las
  conexiones en segundo plano) se reabre y se re-sincroniza.
- Las muestras PPG llegan en lotes de 1 s: la traza se dibuja con ~1,5 s de
  demora respecto del dedo (300 ms en el transporte LAN). La pantalla TFT del
  dispositivo es inmediata.
- **Historial** (`CloudHistory`): `GET <db>/devices/<id>/sessions.json?orderBy="startedAt"&limitToLast=30`
  (requiere el `.indexOn` de las reglas). Recalcula el estado con los umbrales del
  visor y aplica el mismo adelgazado que el historial local (primera lectura,
  cada cambio de estado y una cada 20 s). Una sesión sin `endedAt` que no es la
  que se está midiendo se muestra terminada en su última lectura (no "En curso").
  Se refresca al abrir Historial, al terminar una medición y cada 10 s mientras
  haya una medición abierta con el Historial a la vista. Es de solo lectura:
  "Borrar historial" se deshabilita en esta fuente.
- La app que se sirve desde GitHub Pages (HTTPS) puede usar este transporte
  porque `fetch` / `EventSource` hacia la base también son HTTPS; el WebSocket
  LAN (`ws://`) queda bloqueado por el navegador en una página HTTPS.

### Límites y privacidad

- **Datos de salud en un servicio de lectura pública.** Para un trabajo práctico
  alcanza con un `deviceId` no obvio y sin datos personales; **no sirve como
  producto** (sin autenticación de lectores, sin borrado por el usuario).
- Plan sin costo de Firebase: 100 conexiones simultáneas (cada visor abierto es
  una) y cuotas de almacenamiento y descarga; el volumen por medición es de
  KB, pero conviene mirar el uso en la consola.
- Las reglas se verifican contra el servicio real con
  [`tools/cloud/check_rules.py`](tools/cloud/check_rules.py).

## Pendientes

Armado de los paquetes y decisiones asociadas, a cerrar junto con el firmware:

- [ ] **`measurement_start`**: campos (identificador de medición, base de
      `uptime_ms`, configuración del sensor, batería inicial, etc.).
- [ ] **`ppg`**: campos y tipos (`fs`, `seq`, `samples` como entero o float,
      timestamp/`uptime_ms` del primer sample), tamaño de lote y periodo de
      envío, polaridad y escala de la señal ya acondicionada, y verificar que el
      servidor WebSocket del ESP32-C3 sostiene el caudal.
- [ ] **Frecuencia de muestreo** de la señal PPG que se transmite (puede ser
      menor que la del ADC del MAX30102).
- [ ] **`measurement_end`**: ¿existe como paquete?, ¿lo manda el firmware al
      soltar el botón / por duración máxima / por inactividad?, ¿con motivo y
      resumen?
- [x] **Botón presionado durante una medición**: se **ignora**; una medición
      siempre termina antes de poder empezar otra. Con el resultado (o el error)
      en pantalla, el botón empieza una medición nueva.
- [x] **Sin dedo**: durante `finger_detected: false` el firmware omite `ppg`,
      `spo2` y `bpm` del `live`. Un dedo retirado, además, descarta la medición
      (llamadas 11 y 10).
- [ ] **Reconexión a mitad de medición**: ¿`hello` informa si hay una medición
      en curso (y hace cuánto empezó) para no depender de la medición
      implícita?
- [ ] **Batería** fuera de una medición: por WebSocket hoy solo viaja en
      `telemetry`; ¿se agrega a `hello`/`measurement_start` para mostrarla en
      reposo? (En la nube ya está resuelto: `info.battery_pct`, que la web
      entrega como `hello.battery_pct`; la app lo acepta en cualquier `hello`.)
- [x] **Cliente del ESP32 para la nube**: WiFi STA + TLS + login + keep-alive,
      integrado con el botón (GPIO0) y el deep sleep en
      [`firmware/pulsox`](firmware/pulsox/README.md) (`src/cloud.cpp`,
      `src/cloud_link.cpp`); sigue [`tools/cloud/fake_device.py`](tools/cloud/fake_device.py).
      Verificado en la placa en el entorno `dev` (sueño simulado); el deep sleep
      real depende de la placa del botón (ver el README del firmware).
