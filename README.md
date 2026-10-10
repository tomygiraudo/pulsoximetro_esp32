# Pulsoximetro portátil WI-FI

Diseño open-source de un pulsioxímetro doméstico portátil con conexión WI-FI para la materia de instrumentación Biomédica, realizado con un ESP32-C3 Super mini, un sensor de Fotoplestimografía (PPG) MAX30102, un circuito de carga y control de batería, una baterpia de 3.7 V y 230 mAh y una pantalla TFT 128x128 px con controlador STF775S.

Detalles:

- **Ciclo de medición:** el dashboard espera en "Listo para medir" hasta que el
  ESP32 manda el paquete de inicio (se presionó el botón de medición); recién
  entonces aparecen las lecturas, el cronómetro y la traza PPG.
- **Firmware** El firmware se encuentra en firmware/pulsox, tiene integrado todo lo hecho en los distintos tests de las otras carpetas.
- **Credenciales** Para que el pulsoxímetro se conecte a la aplicación y a la base de datos, se debe clonar el codigo de la base de datos y alojarlo en un proyecto firebase local. Por ultimo crear un archivo .env con las credenciales de la red wifi y las API's correspondientes para establecer la conexión con la base de datos.


## Procesamiento de Señal y diseño de filtros en Python
https://colab.research.google.com/drive/18P-XyyEuqM189sJWGMnf9TlH9UD-HeR_?usp=sharing 

<img width="1589" height="990" alt="image" src="https://github.com/user-attachments/assets/2c3757df-b4d7-4383-8362-4da494ee0da2" />
Links utiles: 
https://www.analog.com/en/resources/reference-designs/maxrefdes117.html
https://www.analog.com/en/resources/technical-articles/how-to-design-a-better-pulse-oximeter.html 
https://www.analog.com/en/resources/technical-articles/guidelines-for-spo2-measurement--maxim-integrated.html

## Hardware
El esquemático del pulsioxímetro y todos sus componentes necesarios se detallan en PCB/Esquematico_V2.pdf/. Los archivos de diseño de la PCB se encuentran dentro de la misma carpeta. 
<img width="1723" height="892" alt="image" src="https://github.com/user-attachments/assets/23c1932e-2213-488d-a6c8-ed485b74c286" />
<img width="1083" height="537" alt="image" src="https://github.com/user-attachments/assets/7d759c62-28bc-4e04-b2f0-b33a57980b0e" />

## Impresión de Carcasa
Los archivos STL para la impresión de la carcasa se encuentran en Carcasa/. Las partes fueron impresas en PETG para una mayor resistencia. 
**Parámetros de Impresión**
-  Altura de capa: 0,16 mm
-  Perímetros: 5 * 0,4 mm
-  Relleno: 50% o superior
<img width="632" height="632" alt="Pulsoximetro_resorte_integrado v18" src="https://github.com/user-attachments/assets/9b4e9e05-b3e6-4bce-935d-9d0769c873d7" />
<img width="632" height="632" alt="Pulsoximetro_resorte_integrado v18" src="https://github.com/user-attachments/assets/42292ed9-1998-4dac-8590-836d851efa5f" />
<img width="632" height="632" alt="Pulsoximetro_resorte_integrado v18" src="https://github.com/user-attachments/assets/98b387b2-90e0-47c4-b829-ec0f01a29e5d" />



## Protocolo de telemetría

Ver [`PROTOCOL.md`](PROTOCOL.md): flujo de una medición (`hello` →
`measurement_start` → `telemetry` + `ppg` → fin) y formato de los mensajes JSON
que el firmware del ESP32 debe enviar por WebSocket, y el modelo de datos y las
llamadas REST del transporte en la nube. El armado de
`measurement_start`, `ppg` y `measurement_end` está marcado como **pendiente**.
El simulador (`web/js/simulator.js`) implementa el ciclo completo con un
formato provisional, así que sirve como referencia ejecutable además del
documento.

## Sistema de diseño y tipografías para pantalla y app web 

Ver [`design-system/pulsox-esp32/`](design-system/pulsox-esp32/): paleta,
tipografía, formas y especificación de componentes (gauge de SpO2, tarjeta de
frecuencia cardíaca, traza PPG, barra de navegación, historial) usados por la
web app.

<img position=center width="338" height="747" alt="image" src="https://github.com/user-attachments/assets/b3259339-e40b-495a-b335-278f26770e1f" /> 

<img width="300" height="300" alt="image" src="https://github.com/user-attachments/assets/9c6ffa2e-40d0-4aed-a427-e95c24bd181f" /> <img width="300" height="300" alt="image" src="https://github.com/user-attachments/assets/416e92ef-37c2-4a64-9126-c4b70426730d" /> <img width="300" height="300" alt="image" src="https://github.com/user-attachments/assets/866001e8-2c0b-4c04-95fa-910bc8762985" />






## Próximos pasos

- Cerrar el armado de los paquetes pendientes de `PROTOCOL.md`
  (`measurement_start`, `ppg`, `measurement_end`).
- Firmware ESP32-C3: lectura del MAX30102, cálculo de SpO2/BPM integrado en el ESP32 C3.
- Cliente de la nube en el ESP32 (WiFi STA + TLS + login + keep-alive, integrado
  con deep sleep y el botón).
- Conectar la web app al dispositivo real y validar el modo `live` / Nube end-to-end.

