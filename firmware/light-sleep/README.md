# light-sleep

Deja la placa en bajo consumo y ahí se queda. Al bootear, una sola vez:

1. **MAX30102**: corriente de los LEDs a 0 mA (`LED1_PA`/`LED2_PA` = 0) y bit
   `SHDN` en `MODE_CONFIG` (~1 µA). Se relee todo para confirmar que quedó así
   (hasta 3 intentos).
2. **ESP32-C3**: **light sleep** (`esp_light_sleep_start()`) **sin ninguna fuente
   de wakeup**, o sea indefinido. No es deep sleep: no hay timer ni botón que lo
   despierte.

El sketch nunca vuelve a encender el sensor. Como el MAX30102 se alimenta del riel
de 3V3 de la PCB (no del ESP32), queda en shutdown mientras la placa tenga
alimentación; solo se vuelve a encender si flasheás otro firmware que lo haga. Un
RESET no lo enciende: el sketch arranca de nuevo y lo vuelve a apagar.

El driver es el de [`firmware/pulsox/lib`](../pulsox/lib) (`lib_extra_dirs` en
`platformio.ini`), no una copia.

## Uso

```powershell
cd firmware/light-sleep
pio run -t upload
pio device monitor -p COM7
```

(`pio.exe` está en `%USERPROFILE%\.platformio\penv\Scripts`; el monitor sin `-p`
puede abrir un COM Bluetooth, mirá el puerto con `pio device list`.)

En el monitor tiene que salir:

```
=== light-sleep ===
MAX30102: LEDs a 0 mA y en shutdown (confirmado por lectura)
ESP32-C3: light sleep indefinido (sin fuente de wakeup). El USB se corta.
```

Si en vez de eso sale `NO se pudo apagar, err=...`, el MAX30102 no contestó por
I2C (revisar soldadura, SDA=GPIO3 / SCL=GPIO4). El ESP32 duerme igual.

## USB y volver a flashear

En light sleep el reloj del USB-Serial-JTAG del ESP32-C3 se apaga: después del
último mensaje la PC ve el puerto como desconectado y no se puede leer ni escribir
por Serial
([doc de Espressif](https://docs.espressif.com/projects/esp-idf/en/stable/esp32c3/api-guides/usb-serial-jtag-console.html)).
Por eso **para flashear otro firmware con la placa dormida** hay que:

1. Mantener apretado **BOOT** (GPIO9).
2. Tocar **RESET**.
3. Soltar BOOT y correr `pio run -t upload`.

Sin tocar BOOT: tras un RESET el puerto vive ~5 s (espera al monitor + 1,5 s) antes de
volver a dormir, que alcanza para un upload si lo lanzás en ese momento.
