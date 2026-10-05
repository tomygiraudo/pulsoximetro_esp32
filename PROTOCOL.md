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
- [ ] **Botón presionado durante una medición**: ¿la detiene o la reinicia?
- [ ] **Sin dedo**: confirmar que durante `finger_detected: false` el firmware
      no envía `ppg` (hoy la app lo ignora después de la primera `telemetry`).
- [ ] **Reconexión a mitad de medición**: ¿`hello` informa si hay una medición
      en curso (y hace cuánto empezó) para no depender de la medición
      implícita?
- [ ] **Batería** fuera de una medición: hoy solo viaja en `telemetry`; ¿se
      agrega a `hello`/`measurement_start` para mostrarla en reposo?
