# pulsoximetro_esp32

Diseño open-source de un pulsioxímetro doméstico portátil realizado con un ESP32-C3 Super Mini y un sensor MAX30102.

## Estado del proyecto

- **Firmware (ESP32-C3 + MAX30102):** todavía no existe. `PROTOCOL.md` define el
  formato de telemetría que el firmware deberá implementar.
- **Web app / dashboard:** implementada en [`web/`](web/). Funciona hoy en modo
  demo (sin hardware) generando lecturas simuladas, y ya está lista para
  conectarse a un ESP32 real en cuanto exista el firmware.

## Web app — dashboard de oximetría

Dashboard en tiempo real, mobile-first, para visualizar SpO2 y frecuencia
cardíaca recibidas del ESP32 por WebSocket.

- **Lecturas grandes y legibles**, coloreadas según el estado fisiológico:
  verde (normal), amarillo (precaución) y rojo (peligro) — siempre con ícono y
  texto además del color, no solo color.
- **Historial** de lecturas con filtro por severidad.
- **Gráficos de líneas** de SpO2 y frecuencia cardíaca en el tiempo, con
  selector de rango (2 min / 10 min / 1 h) y zonas de color según umbral.
- **Modo demo** integrado: simula telemetría realista (incluyendo episodios de
  precaución/peligro) para poder probar y mostrar el dashboard sin el
  hardware armado todavía.
- Instalable como PWA ("Agregar a pantalla de inicio") en el celular.
- Sin build step: HTML/CSS/JS plano, pensado para eventualmente poder
  servirse directo desde la memoria flash del ESP32 (LittleFS/SPIFFS) además
  de poder alojarse en cualquier hosting estático.

### Cómo probarla

```bash
cd web
python -m http.server 8000
# abrir http://localhost:8000 en el navegador (o desde el celular, en la misma red, http://<ip-de-tu-pc>:8000)
```

Al abrir por primera vez arranca en **modo demo** automáticamente. Desde el
ícono de ajustes (⚙) podés:

- Cargar la URL WebSocket del ESP32 (`ws://<ip-del-esp32>/ws`) y conectarte
  cuando el firmware exista.
- Ajustar los umbrales de SpO2/BPM que definen verde/amarillo/rojo.
- Cambiar el tema (auto/claro/oscuro).

### Estructura

```
web/
  index.html
  css/
    tokens.css     tokens de diseño (colores, tipografía, espaciado)
    styles.css     estilos de componentes
  js/
    protocol.js    parseo de paquetes + clasificación de estado fisiológico
    simulator.js   generador de telemetría para el modo demo
    connection.js  WebSocket real + reconexión / modo demo
    history.js     historial persistido (localStorage) con throttling
    charts.js      gráficos de líneas (Chart.js) con bandas de zona
    icons.js       íconos SVG (sin emojis)
    app.js         orquestación de la UI
  manifest.webmanifest, sw.js   PWA
```

## Protocolo de telemetría

Ver [`PROTOCOL.md`](PROTOCOL.md): define los mensajes JSON (`hello`,
`telemetry`, `status`) que el firmware del ESP32 debe enviar por WebSocket
para que el dashboard funcione. El simulador de demo (`web/js/simulator.js`)
implementa exactamente este contrato, así que sirve como referencia
ejecutable además del documento.

## Sistema de diseño

Ver [`design-system/pulsox-esp32/`](design-system/pulsox-esp32/) (generado con
`ui-ux-pro-max`): tokens de color/tipografía/espaciado y especificación de
componentes (tarjetas KPI, gráficos, historial) usados por el dashboard.

## Próximos pasos

- Firmware ESP32-C3: lectura del MAX30102, cálculo de SpO2/BPM, servidor
  WebSocket implementando `PROTOCOL.md`.
- Conectar la web app al dispositivo real y validar el modo `live` end-to-end.
