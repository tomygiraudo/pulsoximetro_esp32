// Light-sleep: low-power state for the PulsOx PCB (ESP32-C3 Super Mini + MAX30102).
//
// Runs once and never gets out of it:
//   1. MAX30102: LEDs to 0 mA + shutdown mode (~1 uA), read back to confirm.
//   2. ESP32-C3: LIGHT sleep with no wakeup source, so it stays asleep indefinitely.
//
// Nothing here ever turns the sensor back on. It is powered from the PCB's 3V3 rail,
// not by the ESP32, so it stays in shutdown while the board has power; only
// flashing another firmware (or a RESET, which just repeats step 1) changes that.
//
// The ESP32-C3's native USB-Serial-JTAG is clock-gated in light sleep: once asleep
// the PC sees the port drop and Serial is unusable. To flash again, hold BOOT
// (GPIO9), press RESET and release BOOT; right after a plain RESET the port is
// alive for ~5 s (BOOT_WINDOW) before it goes to sleep again.
#include <Arduino.h>
#include <Wire.h>
#include <esp_sleep.h>
#include "max30102.h"  // driver of firmware/pulsox/lib (lib_extra_dirs in platformio.ini)

#define PIN_SDA_OX 3
#define PIN_SCL_OX 4

#define SERIAL_WAIT_MS 4000   // max wait for the monitor at boot (on battery there's no host: times out)
#define SERIAL_GRACE_MS 1500  // after a reset USB re-enumerates and the monitor reconnects late
#define SHUTDOWN_TRIES 3
#define SHUTDOWN_RETRY_MS 100

static Max30102 sensor;

// begin() probes the sensor and soft-resets it (clears whatever LEDs/mode were left
// running); its result is not used because shutdown() is what has to succeed, and it
// reports the I2C error itself.
static bool shutdownSensor() {
  Wire.begin(PIN_SDA_OX, PIN_SCL_OX);
  Wire.setClock(100000);
  sensor.begin(Wire);
  for (uint8_t i = 0; i < SHUTDOWN_TRIES; i++) {
    if (sensor.shutdown()) return true;
    delay(SHUTDOWN_RETRY_MS);
  }
  return false;
}

void setup() {
  Serial.begin(115200);
  uint32_t start = millis();
  while (!Serial && millis() - start < SERIAL_WAIT_MS) delay(10);  // USB-CDC: wait for the monitor
  if (Serial) delay(SERIAL_GRACE_MS);

  Serial.println("=== light-sleep ===");
  if (shutdownSensor()) {
    Serial.println("MAX30102: LEDs a 0 mA y en shutdown (confirmado por lectura)");
  } else {
    Serial.printf("MAX30102: NO se pudo apagar, err=%u (%s). Reviso soldadura / SDA=GPIO3 SCL=GPIO4\n",
                  sensor.lastError(), Max30102::errorText(sensor.lastError()));
  }
  Wire.end();  // nobody talks to the sensor again

  Serial.println("ESP32-C3: light sleep indefinido (sin fuente de wakeup). El USB se corta.");
  Serial.flush();
}

void loop() {
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  esp_light_sleep_start();  // no wakeup source: does not return. If it ever does, just sleep again.
}
