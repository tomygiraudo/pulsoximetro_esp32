# Pulsoximetro portátil WI-FI

Diseño open-source de un pulsioxímetro doméstico portátil con conexión WI-FI para la materia de instrumentación Biomédica, realizado con un ESP32-C3 Super mini, un sensor de Fotoplestimografía (PPG) MAX30102, un circuito de carga y control de batería, una baterpia de 3.7 V y 230 mAh y una pantalla TFT 128x128 px con controlador STF775S.
<div align="center"> 
<img  width="580" height="548" alt="Gemini_Generated_Image_x7xrnwx7xrnwx7xr" src="https://github.com/user-attachments/assets/d1a2941a-a398-41fd-8816-c9f2f306fcf5" />
</div>



Detalles:

- **Ciclo de medición:** el dashboard espera en "Listo para medir" hasta que el
  ESP32 manda el paquete de inicio (se presionó el botón de medición); recién
  entonces aparecen las lecturas, el cronómetro y la traza PPG. Al terminar la
  medición el dashboard deja 2 minutos el resultado final (SpO₂, BPM y la señal)
  antes de volver a "Listo para medir"; si se empieza otra medición antes, la
  reemplaza.
- **Firmware** El firmware se encuentra en firmware/pulsox, tiene integrado todo lo hecho en los distintos tests de las otras carpetas.
- **Credenciales** Para que el pulsoxímetro se conecte a la aplicación y a la base de datos, se debe clonar el codigo de la base de datos y alojarlo en un proyecto firebase local. Por ultimo crear un archivo .env con las credenciales de la red wifi y las API's correspondientes para establecer la conexión con la base de datos.


## Procesamiento de Señal y diseño de filtros en Python
https://colab.research.google.com/drive/18P-XyyEuqM189sJWGMnf9TlH9UD-HeR_?usp=sharing 

<img width="1589" height="990" alt="image" src="https://github.com/user-attachments/assets/2c3757df-b4d7-4383-8362-4da494ee0da2" />
Links útiles del fabricante del MAX30102: 
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
<img width="400" height="400" alt="Pulsoximetro_resorte_integrado v18" src="https://github.com/user-attachments/assets/9b4e9e05-b3e6-4bce-935d-9d0769c873d7" />
<img width="400" height="400" alt="Pulsoximetro_resorte_integrado v18" src="https://github.com/user-attachments/assets/42292ed9-1998-4dac-8590-836d851efa5f" />
<img width="400" height="400" alt="Pulsoximetro_resorte_integrado v18" src="https://github.com/user-attachments/assets/98b387b2-90e0-47c4-b829-ec0f01a29e5d" />



## Telemetría del ESP32

El ESP32-C3 envía cada medición por **HTTPS (REST) a Firebase Realtime Database**. La web app la muestra en vivo y la guarda en el historial.

- **Conexión:** al presionar el botón, el ESP32 se conecta al WiFi, inicia sesión en Firebase y mantiene una sola conexión TLS abierta durante la medición.
- **En vivo (1 Hz):** cada segundo reemplaza `devices/<id>/live` con SpO₂, BPM, calidad de señal, estado del dedo y la traza PPG (50 muestras/s). Al terminar, borra `live`.
- **Historial:** una medición válida (45–60 s) guarda una única lectura final en `sessions/<sid>`. El valor es la mediana de los últimos 20 s válidos. Si la medición se descarta (dedo retirado o señal errática), la sesión se borra.
- **Credenciales:** WiFi, API key y usuario del dispositivo van en `firmware/pulsox/include/secrets.h`. El archivo se crea a partir de `secrets.h.example` y no se sube al repo.

Detalle de nodos, llamadas REST y reglas de seguridad en [`PROTOCOL.md`](PROTOCOL.md). Firmware en [`firmware/pulsox`](firmware/pulsox).


## Sistema de diseño y tipografías para pantalla y app web 

Ver [`design-system/pulsox-esp32/`](design-system/pulsox-esp32/): paleta,
tipografía, formas y especificación de componentes (gauge de SpO2, tarjeta de
frecuencia cardíaca, traza PPG, barra de navegación, historial) usados por la
web app.
<div align="center">
  <img position=center width="338" height="747" alt="image" src="https://github.com/user-attachments/assets/b3259339-e40b-495a-b335-278f26770e1f" /> 
</div>
<div align="center">
     <img width="300" height="300" alt="image" src="https://github.com/user-attachments/assets/9c6ffa2e-40d0-4aed-a427-e95c24bd181f" /> <img width="300" height="300" alt="image" src="https://github.com/user-attachments/assets/416e92ef-37c2-4a64-9126-c4b70426730d" /> <img width="300" height="300" alt="image" src="https://github.com/user-attachments/assets/866001e8-2c0b-4c04-95fa-910bc8762985" />                 
  </div>

## Próximos pasos

- Comparar mediciones con un patrón y realizar una calibración o informe de resultados.

