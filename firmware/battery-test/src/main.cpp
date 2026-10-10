// Battery-autonomy test for the ESP32-C3 Super Mini.
//
// One-shot cycle, in this order:
//   1. MAX30102 self-test (before WiFi): reset -> LEDs ON in SpO2 mode -> read a
//      couple of samples -> LEDs OFF and back to shutdown (~1 uA).
//   2. WiFi: connect and send UDP "hello world" packets for SEND_DURATION_MS.
//   3. Deep sleep FOREVER (no wakeup source: only a reset / power cycle wakes it).
//
// The MAX30102 is powered from the PCB's 3V3 rail, not by the ESP32, so it keeps
// its state (e.g. LEDs on) across ESP32 resets and deep sleep. That's why it is
// explicitly put in shutdown at boot: otherwise it would keep burning its idle
// current and skew the measurement. Pins are the ones used in peripherals-test.
//
// Intended use: flash it, confirm the packets arrive, then disconnect the 5V/USB
// supply and measure the battery voltage with a multimeter right away, then again
// after ~24h to see how much it sagged under pure deep sleep current.
//
// IMPORTANT: for the measurement to mean anything, the board must be powered ONLY
// by the battery during the test — not by USB. And set MAX_TEST and
// DEBUG_STAY_AWAKE to 0 first.
//
// The ESP32-C3's USB-Serial-JTAG is powered down in deep sleep, so the serial
// monitor drops when the board sleeps: nothing can be printed after that point.

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <Wire.h>
#include "secrets.h"

// ---- Config -----------------------------------------------------------------
// Both flags can also be set from the command line, e.g. -DMAX_TEST=0.
#ifndef MAX_TEST
#define MAX_TEST 0          // 1: run the LED/sample self-test at boot (0: only shut the sensor down)
#endif
#ifndef DEBUG_STAY_AWAKE
#define DEBUG_STAY_AWAKE 0  // 1: don't deep-sleep, keep USB up and print the sensor state every second
#endif

#define SERIAL_WAIT_MS 4000   // max wait for the monitor at boot (on battery there's no host: times out)
#define SERIAL_GRACE_MS 1500  // extra time once it's attached: after a reset USB re-enumerates and the
                              // monitor reconnects late, so the first lines would be lost

#define TEST_LEDS_ON_MS 3000      // how long the LEDs stay lit during the self-test
#define TEST_SAMPLES 2            // samples that must be read (and be valid) during the self-test
#define TEST_SAMPLE_TIMEOUT_MS 1000

#define WIFI_CONNECT_TIMEOUT_MS 10000
#define SEND_DURATION_MS 5000     // keep sending packets for this long after connecting
#define SEND_INTERVAL_MS 500      // one packet every this many ms during that window

// ---- MAX30102 pins and registers --------------------------------------------
#define PIN_SDA_OX 3
#define PIN_SCL_OX 4

#define MAX_ADDR 0x57
#define REG_FIFO_WR_PTR 0x04      // WR_PTR, OVF_COUNTER, RD_PTR are consecutive
#define REG_FIFO_DATA 0x07
#define REG_MODE_CONFIG 0x09
#define REG_SPO2_CONFIG 0x0A
#define REG_LED1_PA 0x0C          // RED
#define REG_LED2_PA 0x0D          // IR

#define MODE_SHDN 0x80            // shutdown: registers kept, LEDs/ADC off
#define MODE_RESET 0x40           // soft reset, bit clears itself when done
#define MODE_SPO2 0x03            // RED + IR pulsing
#define SPO2_CONFIG_ON 0x27       // ADC 4096 nA, 100 sps, 411 us pulse (same as peripherals-test)
#define LED_PA_ON 0x7F            // 127 * 0.2 mA = ~25 mA, bright enough to see clearly

// ---- I2C helpers ------------------------------------------------------------
// Observed on this board: the first I2C transaction after Wire.begin() fails and
// the following ones work (cause not identified). Every access is therefore
// retried on the SAME bus session, never re-initialising Wire between tries.
#define I2C_TRIES 5
#define I2C_RETRY_MS 5
#define I2C_ERR_SHORT_READ 0xF0   // requestFrom() returned fewer bytes than asked

static uint8_t lastI2cErr = 0;    // error of the last failed attempt

static const char *i2cErrText(uint8_t e) {
  switch (e) {
    case 0: return "ok";
    case 2: return "transaction failed (NACK / no answer)";
    case 3: return "NACK on data";
    case 4: return "bus error";
    case 5: return "timeout, bus stuck";
    case I2C_ERR_SHORT_READ: return "short read";
    default: return "other";
  }
}

static bool maxWrite(uint8_t reg, uint8_t val) {
  for (uint8_t i = 0; i < I2C_TRIES; i++) {
    Wire.beginTransmission(MAX_ADDR);
    Wire.write(reg);
    Wire.write(val);
    lastI2cErr = Wire.endTransmission();
    if (lastI2cErr == 0) return true;
    delay(I2C_RETRY_MS);
  }
  return false;
}

static bool maxReadBytes(uint8_t reg, uint8_t *buf, size_t n) {
  for (uint8_t i = 0; i < I2C_TRIES; i++) {
    Wire.beginTransmission(MAX_ADDR);
    Wire.write(reg);
    lastI2cErr = Wire.endTransmission(false);
    if (lastI2cErr == 0) {
      if (Wire.requestFrom((uint8_t)MAX_ADDR, n) == n) {
        for (size_t b = 0; b < n; b++) buf[b] = Wire.read();
        return true;
      }
      lastI2cErr = I2C_ERR_SHORT_READ;
    }
    delay(I2C_RETRY_MS);
  }
  return false;
}

static bool maxRead(uint8_t reg, uint8_t &val) { return maxReadBytes(reg, &val, 1); }

// ---- MAX30102 control -------------------------------------------------------
static void maxBegin() {
  Wire.begin(PIN_SDA_OX, PIN_SCL_OX);
  Wire.setClock(100000);
  Wire.beginTransmission(MAX_ADDR); // throwaway probe: absorbs the first (failing) transaction
  Wire.endTransmission();
}

static bool maxReset() {
  if (!maxWrite(REG_MODE_CONFIG, MODE_RESET)) return false;
  uint32_t start = millis();
  uint8_t mode;
  while (millis() - start < 100) {
    if (maxRead(REG_MODE_CONFIG, mode) && !(mode & MODE_RESET)) return true;
    delay(1);
  }
  return false;
}

static bool maxLedsOn() {
  return maxWrite(REG_SPO2_CONFIG, SPO2_CONFIG_ON) && maxWrite(REG_LED1_PA, LED_PA_ON) &&
         maxWrite(REG_LED2_PA, LED_PA_ON) && maxWrite(REG_MODE_CONFIG, MODE_SPO2);
}

struct MaxState {
  uint8_t mode = 0xFF, led1 = 0xFF, led2 = 0xFF;
};

static bool maxReadState(MaxState &s) {
  return maxRead(REG_MODE_CONFIG, s.mode) && maxRead(REG_LED1_PA, s.led1) &&
         maxRead(REG_LED2_PA, s.led2);
}

// LED currents to 0, then the SHDN bit; reads everything back and reports it.
// Returns true only if the sensor is confirmed in shutdown with the LEDs at 0.
static bool maxShutdown() {
  bool wrote = maxWrite(REG_LED1_PA, 0x00) && maxWrite(REG_LED2_PA, 0x00) &&
               maxWrite(REG_MODE_CONFIG, MODE_SHDN);
  MaxState s;
  bool read = wrote && maxReadState(s);
  bool ok = read && (s.mode & MODE_SHDN) && s.led1 == 0 && s.led2 == 0;
  if (ok) {
    Serial.printf("MAX30102 shutdown OK: MODE_CONFIG=0x%02X LED1_PA=0x%02X LED2_PA=0x%02X\n", s.mode,
                  s.led1, s.led2);
  } else {
    Serial.printf("MAX30102 shutdown FAILED: %s, MODE_CONFIG=0x%02X LED1_PA=0x%02X LED2_PA=0x%02X, "
                  "I2C err=%u (%s)\n",
                  wrote ? "readback mismatch" : "write failed", s.mode, s.led1, s.led2, lastI2cErr,
                  i2cErrText(lastI2cErr));
  }
  return ok;
}

struct MaxSample {
  uint32_t red, ir;
};

// Reads up to `want` samples from the FIFO (SpO2 mode: RED then IR, 18 bit each).
// Returns how many it got before the timeout.
static uint8_t maxReadSamples(MaxSample *out, uint8_t want, uint32_t timeoutMs) {
  uint8_t got = 0;
  uint32_t start = millis();
  while (got < want && millis() - start < timeoutMs) {
    uint8_t ptr[3]; // WR_PTR, OVF_COUNTER, RD_PTR
    if (maxReadBytes(REG_FIFO_WR_PTR, ptr, 3)) {
      uint8_t pending = (ptr[0] - ptr[2]) & 0x1F;
      for (uint8_t i = 0; i < pending && got < want; i++) {
        uint8_t s[6];
        if (!maxReadBytes(REG_FIFO_DATA, s, 6)) break;
        out[got].red = (((uint32_t)s[0] << 16) | ((uint32_t)s[1] << 8) | s[2]) & 0x3FFFF;
        out[got].ir = (((uint32_t)s[3] << 16) | ((uint32_t)s[4] << 8) | s[5]) & 0x3FFFF;
        got++;
      }
    }
    delay(10);
  }
  return got;
}

// LEDs ON -> read TEST_SAMPLES samples -> LEDs OFF and back to shutdown.
// A sample is valid if the read worked and neither channel is 0 (a dead LED or a
// dead ADC would read 0). Always leaves the sensor in shutdown if the bus works.
static bool maxSelfTest() {
  Serial.println("MAX30102 self-test: LEDs ON (SpO2) -> read samples -> LEDs OFF + shutdown");

  bool ledsOn = maxReset() && maxLedsOn();
  Serial.printf("  LEDs ON: %s\n", ledsOn ? "ok" : "FAILED");

  MaxSample samples[TEST_SAMPLES];
  uint8_t got = ledsOn ? maxReadSamples(samples, TEST_SAMPLES, TEST_SAMPLE_TIMEOUT_MS) : 0;
  bool valid = got == TEST_SAMPLES;
  for (uint8_t i = 0; i < got; i++) {
    Serial.printf("  sample %u: RED=%lu IR=%lu\n", i, (unsigned long)samples[i].red,
                  (unsigned long)samples[i].ir);
    valid = valid && samples[i].red > 0 && samples[i].ir > 0;
  }
  if (got < TEST_SAMPLES) Serial.printf("  only %u/%u samples read\n", got, TEST_SAMPLES);

  if (ledsOn) delay(TEST_LEDS_ON_MS); // keep the LEDs lit long enough to see them
  bool shutdown = maxShutdown();

  bool pass = ledsOn && valid && shutdown;
  Serial.printf("MAX30102 self-test %s (LEDs on: %s, samples valid: %s, shutdown: %s)\n",
                pass ? "PASS" : "FAIL", ledsOn ? "yes" : "NO", valid ? "yes" : "NO",
                shutdown ? "yes" : "NO");
  return pass;
}

// Step 1: runs before WiFi. Whatever happens, the sensor ends up in shutdown.
static void maxBootRoutine() {
  maxBegin();
#if MAX_TEST
  maxSelfTest(); // ends with maxShutdown()
#else
  maxShutdown();
#endif
  Wire.end();
}

// ---- WiFi / UDP -------------------------------------------------------------
// Step 2: connect and send packets with the device's own IP for SEND_DURATION_MS.
static void sendPackets() {
  uint32_t start = millis();
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_CONNECT_TIMEOUT_MS) {
    delay(100);
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.printf("WiFi connect timed out after %lums, sleeping without sending\n",
                  (unsigned long)(millis() - start));
    return;
  }

  String ip = WiFi.localIP().toString();
  WiFiUDP udp;
  uint32_t sendStart = millis();
  uint32_t seq = 0;
  while (millis() - sendStart < SEND_DURATION_MS) {
    char payload[80];
    snprintf(payload, sizeof(payload), "hello world from %s #%lu", ip.c_str(), (unsigned long)seq++);

    udp.beginPacket(UDP_TARGET_IP, UDP_TARGET_PORT);
    udp.write((const uint8_t *)payload, strlen(payload));
    udp.endPacket();

    Serial.printf("Sent: %s\n", payload);
    delay(SEND_INTERVAL_MS);
  }
}

// ---- Sleep ------------------------------------------------------------------
#if DEBUG_STAY_AWAKE
// Instead of sleeping: keeps USB alive and prints the sensor registers every
// second, to check it stays in shutdown (MODE_CONFIG=0x80, LEDs 0) after the WiFi.
static void debugWatchSensor() {
  Serial.println("DEBUG_STAY_AWAKE: NOT sleeping, watching the MAX30102 (check the LED)");
  maxBegin();
  for (;;) {
    MaxState s;
    bool ok = maxReadState(s);
    Serial.printf("MAX30102 watch: read %s, MODE_CONFIG=0x%02X LED1_PA=0x%02X LED2_PA=0x%02X\n",
                  ok ? "ok" : "FAILED", s.mode, s.led1, s.led2);
    delay(1000);
  }
}
#endif

// Step 3: no wakeup source enabled -> sleeps until a reset.
static void goToSleepForever() {
  Serial.println("Going to deep sleep forever now.");
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_OFF);
#if DEBUG_STAY_AWAKE
  debugWatchSensor(); // never returns
#endif
  Serial.flush();
  esp_deep_sleep_start();
}

// ---- Arduino entry points ---------------------------------------------------
static void waitForMonitor() {
  Serial.begin(115200);
  uint32_t start = millis();
  while (!Serial && millis() - start < SERIAL_WAIT_MS) delay(10); // USB-CDC: wait for the monitor
  if (Serial) delay(SERIAL_GRACE_MS);
}

void setup() {
  waitForMonitor();
  maxBootRoutine();
  sendPackets();
  goToSleepForever();
}

void loop() {
  // Never reached.
}
