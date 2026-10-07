# pulsoximetro_esp32

Diseño open-source de un pulsioxímetro doméstico portátil realizado con un ESP32-C3 Super Mini y un sensor MAX30102.

## Estado del proyecto

- **Firmware (ESP32-C3 + MAX30102):** todavía no existe. `PROTOCOL.md` define el
  flujo de una medición y el formato de telemetría que el firmware deberá
  implementar (el armado de los paquetes de inicio, PPG y fin está pendiente).
- **Web app:** implementada en [`web/`](web/). Funciona hoy en modo demo (sin
  hardware) simulando una medición completa, y ya está lista para conectarse a
  un ESP32 real en cuanto exista el firmware: por la red local (WebSocket) o por
  la **nube** (Firebase), que además comparte el historial entre dispositivos.

## Web app

App mobile-first, instalable como PWA, con estética de wearable de consumo
(cálida, legible, nada clínica). Tres secciones, con navegación inferior:

| Sección | Qué muestra |
|---|---|
| **Historial** | Tendencia de SpO2 / frecuencia cardíaca (una magnitud por vez, rango 2 min / 10 min / 1 h) y las lecturas **agrupadas por medición**, con filtro por severidad. |
| **Dashboard** (inicio) | SpO2 en un gauge de anillo, frecuencia cardíaca, calidad de señal y la **señal PPG filtrada en vivo** (ventana de 6 s). Titular en lenguaje natural ("Todo en rango"). |
| **Ajustes** | Fuente de datos (**Nube** / Red local con la dirección del ESP32 / Demo), tema (**Claro** por defecto / Oscuro / Auto), umbrales de SpO2 y BPM, borrado del historial (no disponible en la nube). |

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

Al abrir por primera vez arranca en **modo demo** automáticamente (o en **Nube**,
si `web/js/cloud-config.js` tiene `databaseURL`). Tocá **Simular botón de
medición** para empezar una medición. Desde **Ajustes** podés elegir la fuente:
**Red local** (URL WebSocket del ESP32, `ws://<ip-del-esp32>/ws`, solo si la app
se abre por `http`), **Nube** (ver [Nube (Firebase)](#nube-firebase)) o **Demo**.

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
    connection.js  WebSocket real + reconexión / fuente Nube / modo demo
    history.js     historial por mediciones, persistido (localStorage)
    cloud.js       fuente "Nube": stream de Firebase -> frames del protocolo, e historial compartido
    cloud-config.js  databaseURL + deviceId de la fuente Nube (vacío = no se ofrece)
    charts.js      gráficos de tendencia (Chart.js) con bandas de zona
    ppg.js         traza PPG en vivo (canvas, ventana deslizante)
    icons.js       íconos SVG (sin emojis)
    app.js         vistas, ciclo de medición y orquestación de la UI
  manifest.webmanifest, sw.js   PWA
  assets/icons/    íconos de la app (generate_icons.py los regenera)
tools/cloud/       backend en la nube y herramientas
  database.rules.json   reglas de la Realtime Database (pegar en la consola)
  fake_device.py        dispositivo simulado: lo que hará el ESP32 (solo stdlib)
  check_rules.py        verifica las reglas contra el servicio real
  mock_rtdb.js          mock local de la base y de Auth, para probar sin Firebase
  .env.example          credenciales (copiar a .env, que está en .gitignore)
tests/             pruebas de Node (`node --test`)
```

## Nube (Firebase)

Un navegador bloquea `ws://` desde una página `https://` (GitHub Pages es HTTPS)
y el historial en `localStorage` es de un solo navegador. La fuente **Nube**
resuelve las dos cosas: el ESP32 y la web son clientes de la misma Realtime
Database de Firebase, así que **cualquiera con la URL** ve la medición en vivo y
el mismo historial, desde cualquier red.

```
ESP32 ──HTTPS (REST, con login)──►  Firebase Realtime DB  ◄──REST + streaming (EventSource), lectura pública──  Web
```

Solo el dispositivo escribe; la medición se inicia únicamente con el botón del
dispositivo (la app no manda comandos, así el ESP32 puede dormir entre
mediciones). Detalle del modelo de datos y de las llamadas del dispositivo en
[`PROTOCOL.md`](PROTOCOL.md#transporte-en-la-nube-firebase-realtime-database).
El cliente del ESP32 todavía no existe: `tools/cloud/fake_device.py` hace lo
mismo por REST y sirve para probar todo (y como especificación del firmware).

Mientras `databaseURL` esté vacío en `web/js/cloud-config.js` la opción "Nube" no
se ofrece y la app funciona como siempre (Red local / Demo).

### Configurar Firebase (unos 5 minutos)

1. En la [consola de Firebase](https://console.firebase.google.com/) creá un
   proyecto (se puede desactivar Google Analytics).
2. **Realtime Database** → *Crear base de datos* → elegí la ubicación → *Modo
   bloqueado*. Copiá la URL que aparece arriba de la pestaña *Datos*
   (`https://<proyecto>-default-rtdb.firebaseio.com`, o con `.<region>.firebasedatabase.app`
   si elegiste otra región).
3. **Authentication** → *Comenzar* → *Método de acceso* → habilitá
   **Correo electrónico/contraseña**. En *Users* → *Agregar usuario* creá el
   usuario **del dispositivo** (el email puede ser inventado, p. ej.
   `pulsox-device@example.com`; contraseña larga y única). Copiá su **UID de usuario**.
4. **Reglas**: en [`tools/cloud/database.rules.json`](tools/cloud/database.rules.json)
   reemplazá `REEMPLAZAR_UID_DEL_DISPOSITIVO` por ese UID, pegá el contenido en
   *Realtime Database → Reglas* y *Publicar*. (Lectura pública de
   `devices/<id>`, escritura solo para ese UID, validación de rangos y formas, e
   índice por `startedAt`: sin él la consulta del historial falla.)
5. **Credenciales para las herramientas**: *Configuración del proyecto →
   General → Clave de API web*. Copiá `tools/cloud/.env.example` a
   `tools/cloud/.env` (está en `.gitignore`) y completá `FIREBASE_DATABASE_URL`,
   `FIREBASE_API_KEY`, `FIREBASE_EMAIL`, `FIREBASE_PASSWORD` y `PULSOX_DEVICE_ID`.
   **La contraseña y la API key van solo en ese archivo: nunca en el repositorio,
   en un issue ni en un chat.**
6. **Web**: en `web/js/cloud-config.js` poné `databaseURL` (el del paso 2) y
   `deviceId` (el mismo que `PULSOX_DEVICE_ID`). No es un secreto, pero el
   `deviceId` es la "llave" de la URL de lectura pública: dejá uno no obvio y sin
   datos personales.
7. **Verificar las reglas** contra el servicio real:

   ```bash
   python tools/cloud/check_rules.py
   ```

   Debe terminar con todas las comprobaciones en `PASS` (lectura anónima 200,
   escritura anónima y de otro usuario denegada, `.validate` rechazando valores
   inválidos, consulta del historial 200). Crea y borra un usuario descartable
   (`--no-signup` lo omite) y escribe solo bajo un `deviceId` de prueba.
8. **Probar con el dispositivo simulado** y la web local:

   ```bash
   python tools/cloud/fake_device.py --duration 90        # lo que hará el ESP32
   cd web && python -m http.server 8000                   # otra terminal
   # http://localhost:8000 → Ajustes → Nube
   ```

   El dashboard pasa de "Listo para medir" a la medición en vivo (SpO₂, BPM y
   traza PPG). Con la misma URL desde un celular se ve la misma medición y el
   mismo historial. `--cycles 3 --scenario mixed` encadena mediciones con
   valores de precaución/peligro; `--crash` corta a mitad como un apagón (a los
   ~10 s la web cierra la medición y el historial la muestra con su duración).
9. **Publicar**: al mergear a `main`, el workflow `deploy-pages.yml` despliega
   `web/` en GitHub Pages. Abrí la página desde allí (HTTPS) con la consola del
   navegador abierta: no debe haber errores de contenido mixto ni de CORS.

Límites a tener en cuenta: son **datos de salud en un servicio de lectura
pública** (para el trabajo práctico alcanza con un `deviceId` no obvio; no sirve
como producto), el plan sin costo admite 100 conexiones simultáneas (cada visor
abierto es una) y la traza PPG llega con ~1,5 s de demora respecto del dedo (la
pantalla del dispositivo es inmediata).

### Probar sin Firebase (mock local)

Para desarrollar sin proyecto ni credenciales, `tools/cloud/mock_rtdb.js` emula
la API REST y el streaming de la Realtime Database (con la redirección 307 y
CORS) y los endpoints de Auth, aplicando las mismas
`tools/cloud/database.rules.json`. No reemplaza la verificación contra el
servicio real (paso 7):

```bash
node tools/cloud/mock_rtdb.js --port 9000 --redirect-port 9001
python tools/cloud/fake_device.py --mock http://localhost:9000 --duration 60
python tools/cloud/check_rules.py --mock http://localhost:9000
# y en web/js/cloud-config.js (sin commitear): databaseURL "http://localhost:9000", el deviceId de PULSOX_DEVICE_ID
```

### Pruebas automáticas

```bash
node --test
```

Cubre la lógica pura de `web/js/cloud.js` con eventos de streaming sintéticos
(aplicación de `put`/`patch`, frames que se sintetizan, watchdog de 10 s,
reconexión, historial y adelgazado) y, contra el mock con el `EventSource` real
de Node, la redirección 307, el corte del stream y la consulta del historial.
Lo que solo puede comprobarse con Firebase y un navegador real (CORS y 307 del
servicio, contenido mixto en Pages) está en los pasos 7 y 9.

## Protocolo de telemetría

Ver [`PROTOCOL.md`](PROTOCOL.md): flujo de una medición (`hello` →
`measurement_start` → `telemetry` + `ppg` → fin) y formato de los mensajes JSON
que el firmware del ESP32 debe enviar por WebSocket, y el modelo de datos y las
llamadas REST del transporte en la nube. El armado de
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
- Cliente de la nube en el ESP32 (WiFi STA + TLS + login + keep-alive, integrado
  con deep sleep y el botón), siguiendo `tools/cloud/fake_device.py`.
- Conectar la web app al dispositivo real y validar el modo `live` / Nube end-to-end.
