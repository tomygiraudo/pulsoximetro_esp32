// Battery-autonomy test for the ESP32-C3 Super Mini.
//
// One-shot cycle: boot -> connect WiFi -> send UDP "hello world" packets with
// the device's own IP for SEND_DURATION_MS -> deep sleep FOREVER (no timer
// wakeup set, so it only wakes up again on a manual reset/power cycle).
//
// Intended use: flash it, confirm the packets arrive, then disconnect the
// 5V/USB supply and measure the battery voltage with a multimeter right
// away, then again after ~24h to see how much it sagged under pure deep
// sleep current (no WiFi, no periodic wakeups in between).
//
// IMPORTANT: for the measurement to mean anything, the board must be
// powered ONLY by the battery during the test — not by USB.

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include "secrets.h"

#define WIFI_CONNECT_TIMEOUT_MS 10000
#define SEND_DURATION_MS 5000   // keep sending packets for this long after connecting
#define SEND_INTERVAL_MS 500    // one packet every this many ms during that window

static void goToSleepForever() {
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_OFF);
  Serial.flush();
  esp_deep_sleep_start(); // no wakeup source enabled -> sleeps until reset
}

void setup() {
  Serial.begin(115200);
  delay(100); // let USB-CDC settle before the first print, if USB is attached

  uint32_t start = millis();
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_CONNECT_TIMEOUT_MS) {
    delay(100);
  }

  if (WiFi.status() == WL_CONNECTED) {
    String ip = WiFi.localIP().toString();
    WiFiUDP udp;

    uint32_t sendStart = millis();
    uint32_t seq = 0;
    while (millis() - sendStart < SEND_DURATION_MS) {
      char payload[80];
      snprintf(payload, sizeof(payload), "hello world from %s #%lu", ip.c_str(),
               (unsigned long)seq++);

      udp.beginPacket(UDP_TARGET_IP, UDP_TARGET_PORT);
      udp.write((const uint8_t *)payload, strlen(payload));
      udp.endPacket();

      Serial.printf("Sent: %s\n", payload);
      delay(SEND_INTERVAL_MS);
    }
  } else {
    Serial.printf("WiFi connect timed out after %lums, sleeping without sending\n",
                  (unsigned long)(millis() - start));
  }

  Serial.println("Going to deep sleep forever now.");
  goToSleepForever();
}

void loop() {
  // Never reached.
}
