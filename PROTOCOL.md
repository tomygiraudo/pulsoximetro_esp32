# Protocolo de telemetría — PulsOx ESP32

Este documento define el formato de los paquetes que el firmware del ESP32-C3
debe enviar a la web app para que el dashboard funcione. Como todavía no existe
firmware, este formato es el contrato a implementar del lado del dispositivo.

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

## 2. `telemetry` — lectura periódica

El mensaje principal. Se espera aproximadamente **1 vez por segundo** (el
algoritmo de SpO2/BPM del MAX30102 ya promedia internamente sobre varios
segundos de señal PPG; no hace falta enviar muestras crudas a más frecuencia).

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
  lectura", muestra el estado "Colocá el dedo en el sensor" y no lo agrega al
  historial.

## 3. `status` — eventos y errores

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

## Umbrales fisiológicos usados por el dashboard

Estos umbrales son los que la web app usa por defecto para colorear el
estado (verde / amarillo / rojo). Son guías generales de bienestar, **no**
un dispositivo médico certificado, y son configurables desde el panel de
ajustes del dashboard (se guardan en `localStorage`, no se envían al ESP32).

| Parámetro | Verde (normal) | Amarillo (precaución) | Rojo (peligro) |
|---|---|---|---|
| SpO2 | ≥ 95% | 90–94% | < 90% |
| BPM (reposo) | 60–100 | 50–59 o 101–120 | < 50 o > 120 |

## Ejemplo de sesión

```
→ (cliente abre ws://192.168.1.42/ws)
← {"type":"hello","device_id":"pulsox-esp32-01","fw_version":"0.1.0","sensor":"MAX30102","sample_rate_hz":1}
← {"type":"telemetry","seq":1,"uptime_ms":1000,"finger_detected":false,"spo2":null,"spo2_valid":false,"bpm":null,"bpm_valid":false,"signal_quality":0.0}
← {"type":"telemetry","seq":2,"uptime_ms":2000,"finger_detected":true,"spo2":96.8,"spo2_valid":false,"bpm":81,"bpm_valid":false,"signal_quality":0.31}
← {"type":"telemetry","seq":3,"uptime_ms":3000,"finger_detected":true,"spo2":97.2,"spo2_valid":true,"bpm":74,"bpm_valid":true,"signal_quality":0.79}
```

## Implementación de referencia (cliente)

El parseo y la validación de estos mensajes están en
[`web/js/protocol.js`](web/js/protocol.js). El simulador de demo en
[`web/js/simulator.js`](web/js/simulator.js) genera paquetes `telemetry` que
respetan exactamente este formato, así que sirve como referencia ejecutable
del contrato además de este documento.
