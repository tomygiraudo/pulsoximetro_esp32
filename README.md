# pulsoximetro_esp32

Diseño open-source de un pulsioxímetro doméstico portátil realizado con un ESP32-C3 Super Mini y un sensor MAX30102.

## Estado del proyecto

- **Firmware (ESP32-C3 + MAX30102):** todavía no existe. `PROTOCOL.md` define el
  flujo de una medición y el formato de telemetría que el firmware deberá
  implementar (el armado de los paquetes de inicio, PPG y fin está pendiente).
- **Web app:** implementada en [`web/`](web/). Funciona hoy en modo demo (sin
  hardware) simulando una medición completa, y ya está lista para conectarse a
  un ESP32 real en cuanto exista el firmware.

## Web app

App mobile-first, instalable como PWA, con estética de wearable de consumo
(cálida, legible, nada clínica). Tres secciones, con navegación inferior:

| Sección | Qué muestra |
|---|---|
| **Historial** | Tendencia de SpO2 / frecuencia cardíaca (una magnitud por vez, rango 2 min / 10 min / 1 h) y las lecturas **agrupadas por medición**, con filtro por severidad. |
| **Dashboard** (inicio) | SpO2 en un gauge de anillo, frecuencia cardíaca, calidad de señal y la **señal PPG filtrada en vivo** (ventana de 6 s). Titular en lenguaje natural ("Todo en rango"). |
| **Ajustes** | Dirección del ESP32, modo demo, tema (**Claro** por defecto / Oscuro / Auto), umbrales de SpO2 y BPM, borrado del historial. |

Detalles:

- **Ciclo de medición:** el dashboard espera en "Listo para medir" hasta que el
  ESP32 manda el paquete de inicio (se presionó el botón de medición); recién
  entonces aparecen las lecturas, el cronómetro y la traza PPG.
- **Estado nunca solo por color:** verde / amarillo / rojo siempre van con ícono
  y texto.
- **Modo demo** integrado: simula el ciclo completo (botón "Simular botón de
  medición" en el dashboard), con episodios de precaución/peligro y pérdida de
  dedo.
- Sin build step: HTML/CSS/JS plano, pensado para poder servirse directo desde
  la memoria flash del ESP32 (LittleFS/SPIFFS) además de cualquier hosting
  estático.

### Cómo probarla

```bash
cd web
python -m http.server 8000
# abrir http://localhost:8000 en el navegador (o desde el celular, en la misma red, http://<ip-de-tu-pc>:8000)
```

Al abrir por primera vez arranca en **modo demo** automáticamente. Tocá
**Simular botón de medición** para empezar una medición. Desde **Ajustes** podés
cargar la URL WebSocket del ESP32 (`ws://<ip-del-esp32>/ws`) y conectarte cuando
el firmware exista.

### Estructura

```
web/
  index.html
  css/
    tokens.css     tokens de diseño (colores, tipografía, radios); tema claro por defecto
    styles.css     estilos de componentes y barra de navegación inferior
  js/
    protocol.js    parseo de paquetes + clasificación de estado fisiológico
    simulator.js   generador de mediciones para el modo demo
    connection.js  WebSocket real + reconexión / modo demo
    history.js     historial por mediciones, persistido (localStorage)
    charts.js      gráficos de tendencia (Chart.js) con bandas de zona
    ppg.js         traza PPG en vivo (canvas, ventana deslizante)
    icons.js       íconos SVG (sin emojis)
    app.js         vistas, ciclo de medición y orquestación de la UI
  manifest.webmanifest, sw.js   PWA
  assets/icons/    íconos de la app (generate_icons.py los regenera)
```

## Protocolo de telemetría

Ver [`PROTOCOL.md`](PROTOCOL.md): flujo de una medición (`hello` →
`measurement_start` → `telemetry` + `ppg` → fin) y formato de los mensajes JSON
que el firmware del ESP32 debe enviar por WebSocket. El armado de
`measurement_start`, `ppg` y `measurement_end` está marcado como **pendiente**.
El simulador (`web/js/simulator.js`) implementa el ciclo completo con un
formato provisional, así que sirve como referencia ejecutable además del
documento.

## Sistema de diseño

Ver [`design-system/pulsox-esp32/`](design-system/pulsox-esp32/): paleta,
tipografía, formas y especificación de componentes (gauge de SpO2, tarjeta de
frecuencia cardíaca, traza PPG, barra de navegación, historial) usados por la
web app.

## Próximos pasos

- Cerrar el armado de los paquetes pendientes de `PROTOCOL.md`
  (`measurement_start`, `ppg`, `measurement_end`).
- Firmware ESP32-C3: lectura del MAX30102, cálculo de SpO2/BPM, acondicionamiento
  de la señal PPG, servidor WebSocket implementando `PROTOCOL.md`.
- Conectar la web app al dispositivo real y validar el modo `live` end-to-end.
