// Peripheral bring-up test for the PulsOx PCB (ESP32-C3 Super Mini).
//
// Checks, in this order, and reports PASS / FAIL / N/D for each one on Serial
// and on the TFT:
//   1. I2C   - scan finds the MAX30102 (0x57)
//   2. I2C   - PART_ID register reads 0x15, soft reset completes
//   3. I2C   - die temperature conversion works (write + read of registers)
//   4. GPIO  - INT pin goes low on an interrupt and is released when cleared
//   5. SPI   - the ST7735S reports sleep-out + display-on after initR()
//              (read back through a bit-banged read, see tftReadReg)
// then runs a visual pattern on the TFT (colors / border / backlight) and
// finally shows live IR / RED readings from the MAX30102 on the TFT + Serial.
//
// The TFT side is compiled out unless ENABLE_TFT is 1 (see below): with it off,
// test 5 is reported as N/D, the TFT pins are never touched and everything is
// reported on Serial only.
//
// Debug aids (so a silent board can still be told apart from a dead one):
//   - On-board LED (GPIO8): 3 quick blinks at boot = firmware started, even with
//     no Serial. Then it blinks fast (4 Hz) while the MAX30102 is missing and
//     slowly (1 Hz) while it is streaming. Solid on = still inside setup().
//   - Every "[uptime][TAG]" line on Serial carries the uptime in ms. Boot info
//     (reset reason, brownout, heap), I2C line levels, and the raw I2C error
//     code on every failure.
//   - While the sensor is missing, one line every 2 s with the I2C error and
//     the SDA/SCL/INT levels; in live mode one stats line per second.
//
// Pins are the ones in PCB/Sensor_ctrl_pwr_pcb (KiCad schematic, "Esquematico V2"):
//   SDA-OX  GPIO3   I2C SDA (MAX30102)
//   SCL-OX  GPIO4   I2C SCL (MAX30102)
//   INT-OX  GPIO1   MAX30102 INT (open-drain, active low)
//   SCK_TFT GPIO6   SPI clock
//   SDA_TFT GPIO7   SPI MOSI (bidirectional SDA on the ST7735S)
//   CS_TFT  GPIO21  SPI chip select
//   DATA_SEL_TFT GPIO10  D/C (A0)
//   RST_TFT GPIO20  TFT hardware reset
//   BACKLIGHT_TFT GPIO5  TFT backlight (LED pin of the module)
//   SW1     GPIO0   push button (not used by this test)
//
// Live mode also streams "ir:<n>,red:<n>" lines, so the values can be
// plotted with any serial plotter.

// Set to 1 once the TFT is wired up. Can also be overridden with -DENABLE_TFT=1.
#ifndef ENABLE_TFT
#define ENABLE_TFT 0
#endif

#include <Arduino.h>
#include <Wire.h>
#include <esp_system.h>
#include <stdarg.h>
#if ENABLE_TFT
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#endif

// ---- Pins -----------------------------------------------------------------
#define PIN_SDA_OX 3
#define PIN_SCL_OX 4
#define PIN_INT_OX 1
#define PIN_SCK_TFT 6
#define PIN_SDA_TFT 7
#define PIN_CS_TFT 21
#define PIN_DC_TFT 10
#define PIN_RST_TFT 20
#define PIN_BL_TFT 5

#define BL_ON HIGH   // flip to LOW if the backlight turns out to be active-low

// ESP32-C3 Super Mini on-board LED, not wired to anything on the PCB (V2).
#define PIN_LED 8
#define LED_ON_LEVEL LOW          // active-low on the Super Mini; flip if it looks inverted
#define LED_PERIOD_IDLE_MS 125    // toggle period while the sensor is missing (fast blink)
#define LED_PERIOD_LIVE_MS 500    // toggle period while streaming (slow blink)

// ---- TFT ------------------------------------------------------------------
#define TFT_W 128
#define TFT_H 128
#define TFT_INIT_TAB INITR_144GREENTAB  // 1.44" 128x128; try INITR_BLACKTAB if colors/offsets look off
#define TFT_SPI_HZ 10000000
#define COLOR_GRAY 0x7BEF

// ST7735 commands used for the readback test
#define TFT_CMD_RDDID 0x04   // 3 bytes: manufacturer / driver version / driver id
#define TFT_CMD_RDDPM 0x0A   // power mode: 0x08 after reset, 0x9C after sleep-out + display on
#define TFT_RDDPM_AFTER_RESET 0x08
#define TFT_RDDPM_AFTER_INIT 0x9C

// ---- MAX30102 -------------------------------------------------------------
#define MAX_ADDR 0x57
#define MAX_PART_ID_EXPECTED 0x15

#define REG_INT_STATUS_1 0x00
#define REG_INT_STATUS_2 0x01
#define REG_INT_ENABLE_2 0x03
#define REG_FIFO_WR_PTR 0x04
#define REG_FIFO_OVF 0x05
#define REG_FIFO_RD_PTR 0x06
#define REG_FIFO_DATA 0x07
#define REG_FIFO_CONFIG 0x08
#define REG_MODE_CONFIG 0x09
#define REG_SPO2_CONFIG 0x0A
#define REG_LED1_PA 0x0C   // RED
#define REG_LED2_PA 0x0D   // IR
#define REG_TEMP_INT 0x1F
#define REG_TEMP_FRAC 0x20
#define REG_TEMP_CONFIG 0x21
#define REG_REV_ID 0xFE
#define REG_PART_ID 0xFF

#define MODE_RESET 0x40
#define MODE_SPO2 0x03
#define INT2_DIE_TEMP_RDY 0x02

// Live-mode sensor setup: 4-sample averaging at 100 sps -> 25 samples/s,
// ADC range 4096 nA, 411 us pulse (18 bit), LEDs ~7 mA.
#define LIVE_FIFO_CONFIG 0x50   // SMP_AVE=4, rollover on
#define LIVE_SPO2_CONFIG 0x27   // ADC_RGE=4096, SR=100, LED_PW=411us
#define LIVE_LED_PA 0x24        // 36 * 0.2 mA

#define FINGER_IR_THRESHOLD 50000
#define LIVE_REFRESH_MS 100
#define HIST_N TFT_W            // one plot column per stored sample
#define PLOT_Y 56
#define PLOT_H 72
#define PLOT_MIN_SPAN 256       // so sensor noise isn't blown up to full scale

#define RETRY_INTERVAL_MS 2000
#define I2C_FAILS_BEFORE_LOST 10

#if ENABLE_TFT
#define SUMMARY_HOLD_MS 3000    // time to read the summary on the TFT before going live
#else
#define SUMMARY_HOLD_MS 0
#endif

// ---- Test bookkeeping -------------------------------------------------------
enum TestId : uint8_t { T_I2C_SCAN, T_PART_ID, T_DIE_TEMP, T_INT_PIN, T_LCD_READBACK, T_COUNT };
enum Result : uint8_t { R_PENDING, R_PASS, R_FAIL, R_NA };

static const char *TEST_LABELS[T_COUNT] = {
    "I2C scan 0x57", "MAX PART_ID", "MAX temp die", "MAX pin INT", "LCD readback"};
static Result results[T_COUNT];

static float dieTempC = 0;

#if ENABLE_TFT
static uint8_t tftRddpmBefore = 0, tftRddpmAfter = 0;
static uint8_t tftId[3] = {0, 0, 0};

static Adafruit_ST7735 tft(&SPI, PIN_CS_TFT, PIN_DC_TFT, PIN_RST_TFT);
static GFXcanvas16 plot(TFT_W, PLOT_H);
#endif

static const char *resultText(Result r) {
  switch (r) {
    case R_PASS: return "PASS";
    case R_FAIL: return "FAIL";
    case R_NA: return "N/D";
    default: return "...";
  }
}

#if ENABLE_TFT
static uint16_t resultColor(Result r) {
  switch (r) {
    case R_PASS: return ST77XX_GREEN;
    case R_FAIL: return ST77XX_RED;
    case R_NA: return ST77XX_YELLOW;
    default: return COLOR_GRAY;
  }
}
#endif

static void report(TestId id, Result r, const char *fmt, ...) {
  char detail[200];
  va_list args;
  va_start(args, fmt);
  vsnprintf(detail, sizeof(detail), fmt, args);
  va_end(args);
  results[id] = r;
  Serial.printf("[%-4s] %-14s %s\n", resultText(r), TEST_LABELS[id], detail);
}

// ---- Debug helpers ----------------------------------------------------------
// Every DBG line starts with the uptime in ms and a tag, so a frozen or
// rebooting board is obvious from the log.
#define DBG(tag, fmt, ...) \
  Serial.printf("[%7lu][%-4s] " fmt "\n", (unsigned long)millis(), tag, ##__VA_ARGS__)

#define I2C_ERR_SHORT_READ 0xF0 // requestFrom() returned fewer bytes than asked
static uint8_t lastI2cErr = 0;  // last error seen by maxWrite / maxReadBytes

static void ledSet(bool on) { digitalWrite(PIN_LED, on ? LED_ON_LEVEL : !LED_ON_LEVEL); }

// Wire.endTransmission() return values (Arduino-ESP32), plus our own short read.
static const char *i2cErrText(uint8_t e) {
  switch (e) {
    case 0: return "ok";
    case 1: return "datos demasiado largos";
    case 2: return "NACK en la direccion (nadie responde)";
    case 3: return "NACK en los datos";
    case 4: return "error del bus";
    case 5: return "timeout (SDA/SCL trabadas?)";
    case I2C_ERR_SHORT_READ: return "lectura incompleta";
    default: return "desconocido";
  }
}

static const char *resetReasonText(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON: return "encendido (power-on)";
    case ESP_RST_EXT: return "pin RESET";
    case ESP_RST_SW: return "reinicio por software";
    case ESP_RST_PANIC: return "CRASH (panic / excepcion)";
    case ESP_RST_INT_WDT: return "watchdog de interrupciones";
    case ESP_RST_TASK_WDT: return "watchdog de tarea";
    case ESP_RST_WDT: return "watchdog";
    case ESP_RST_DEEPSLEEP: return "salida de deep sleep";
    case ESP_RST_BROWNOUT: return "BROWNOUT (alimentacion insuficiente)";
    default: return "otra / desconocida";
  }
}

static void printBootInfo() {
  esp_reset_reason_t rr = esp_reset_reason();
  DBG("BOOT", "build %s %s, TFT %s", __DATE__, __TIME__, ENABLE_TFT ? "habilitado" : "deshabilitado");
  DBG("BOOT", "causa del reset: %s (%d)", resetReasonText(rr), (int)rr);
  DBG("BOOT", "chip %s rev %d, %u MHz, heap libre %u B", ESP.getChipModel(),
      (int)ESP.getChipRevision(), (unsigned)ESP.getCpuFreqMHz(), (unsigned)ESP.getFreeHeap());
}

// Current level of the I2C lines and INT. Safe to call any time (read-only).
static void dbgI2cLines(const char *when) {
  DBG("I2C", "%s: SDA(GPIO%d)=%d SCL(GPIO%d)=%d INT(GPIO%d)=%d", when, PIN_SDA_OX,
      digitalRead(PIN_SDA_OX), PIN_SCL_OX, digitalRead(PIN_SCL_OX), PIN_INT_OX,
      digitalRead(PIN_INT_OX));
}

// Run once BEFORE Wire.begin(): tells "no pull-up" apart from "line held low".
// Both lines should read 1 with the module powered (it has its own pull-ups).
static void i2cPreCheck() {
  pinMode(PIN_SDA_OX, INPUT);
  pinMode(PIN_SCL_OX, INPUT);
  delay(2);
  bool sda = digitalRead(PIN_SDA_OX), scl = digitalRead(PIN_SCL_OX);
  pinMode(PIN_SDA_OX, INPUT_PULLUP);
  pinMode(PIN_SCL_OX, INPUT_PULLUP);
  delay(2);
  bool sdaPu = digitalRead(PIN_SDA_OX), sclPu = digitalRead(PIN_SCL_OX);
  pinMode(PIN_SDA_OX, INPUT);
  pinMode(PIN_SCL_OX, INPUT);
  DBG("I2C", "pre-check sin pull-up interno: SDA=%d SCL=%d | con pull-up interno: SDA=%d SCL=%d",
      sda, scl, sdaPu, sclPu);
  if (sda && scl) {
    DBG("I2C", "ok: las dos lineas estan en alto (hay pull-ups externos, bus libre)");
  } else if (sdaPu && sclPu) {
    DBG("I2C", "ATENCION: linea/s en bajo sin pull-up interno -> no hay pull-up externo "
               "(modulo sin alimentar, o pista/soldadura cortada)");
  } else {
    DBG("I2C", "ATENCION: %s en bajo incluso con pull-up interno -> corto a GND, algo la "
               "sujeta en bajo, o el modulo tiene pull-ups a 1.8V (el C3 necesita ~2.5V para leer 1)",
        (!sdaPu && !sclPu) ? "SDA y SCL" : (!sdaPu ? "SDA" : "SCL"));
  }
}

// ---- MAX30102 register access ----------------------------------------------
static bool maxWrite(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(MAX_ADDR);
  Wire.write(reg);
  Wire.write(val);
  lastI2cErr = Wire.endTransmission();
  return lastI2cErr == 0;
}

static bool maxReadBytes(uint8_t reg, uint8_t *buf, size_t n) {
  Wire.beginTransmission(MAX_ADDR);
  Wire.write(reg);
  lastI2cErr = Wire.endTransmission(false);
  if (lastI2cErr != 0) return false;
  if (Wire.requestFrom((uint8_t)MAX_ADDR, n) != n) {
    lastI2cErr = I2C_ERR_SHORT_READ;
    return false;
  }
  for (size_t i = 0; i < n; i++) buf[i] = Wire.read();
  return true;
}

static bool maxRead(uint8_t reg, uint8_t &val) { return maxReadBytes(reg, &val, 1); }

// Address-only probe; returns the raw endTransmission() code (0 = ACK).
static uint8_t maxProbe() {
  Wire.beginTransmission(MAX_ADDR);
  return Wire.endTransmission();
}

// Reading the status registers clears them (and releases INT).
static void maxClearInterrupts() {
  uint8_t dummy;
  maxRead(REG_INT_STATUS_1, dummy);
  maxRead(REG_INT_STATUS_2, dummy);
}

static bool maxSoftReset() {
  if (!maxWrite(REG_MODE_CONFIG, MODE_RESET)) return false;
  uint32_t start = millis();
  uint8_t mode;
  while (millis() - start < 100) {
    if (maxRead(REG_MODE_CONFIG, mode) && !(mode & MODE_RESET)) {
      maxClearInterrupts(); // PWR_RDY is set again after a reset and holds INT low
      return true;
    }
    delay(1);
  }
  return false;
}

// ---- MAX30102 tests -----------------------------------------------------------
static void testI2cScan() {
  Serial.println("I2C scan (0x08-0x77):");
  bool foundMax = false;
  uint8_t count = 0, timeouts = 0, maxErr = 0;
  for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
    Wire.beginTransmission(addr);
    uint8_t err = Wire.endTransmission();
    if (err == 0) {
      Serial.printf("  found 0x%02X\n", addr);
      count++;
      if (addr == MAX_ADDR) foundMax = true;
    } else if (err == 5) {
      timeouts++;
    }
    if (addr == MAX_ADDR) maxErr = err;
  }
  DBG("I2C", "scan terminado: %u respuesta/s, %u timeout/s%s", count, timeouts,
      timeouts ? " -> el bus se traba (SDA/SCL en bajo)" : "");
  dbgI2cLines("tras el scan");
  if (foundMax) {
    report(T_I2C_SCAN, R_PASS, "0x57 responde (%u dispositivo/s en el bus)", count);
  } else {
    report(T_I2C_SCAN, R_FAIL,
           "0x57 no responde, err=%u (%s), %u dispositivo/s. Revisar soldadura, SDA/SCL y "
           "pull-ups a 3V3 (algunos modulos los traen a 1.8V)",
           maxErr, i2cErrText(maxErr), count);
  }
}

static void testPartId() {
  uint8_t part = 0, rev = 0;
  if (!maxRead(REG_PART_ID, part)) {
    report(T_PART_ID, R_FAIL, "sin respuesta al leer PART_ID, err=%u (%s)", lastI2cErr,
           i2cErrText(lastI2cErr));
    return;
  }
  maxRead(REG_REV_ID, rev);
  if (part != MAX_PART_ID_EXPECTED) {
    report(T_PART_ID, R_FAIL, "PART_ID=0x%02X (esperado 0x%02X) REV_ID=0x%02X", part,
           MAX_PART_ID_EXPECTED, rev);
    return;
  }
  if (!maxSoftReset()) {
    report(T_PART_ID, R_FAIL, "PART_ID ok (0x%02X) pero el soft reset no termina", part);
    return;
  }
  report(T_PART_ID, R_PASS, "PART_ID=0x%02X REV_ID=0x%02X, soft reset ok", part, rev);
}

static void testDieTemp() {
  uint8_t cfg = 0, tInt = 0, tFrac = 0;
  if (!maxWrite(REG_INT_ENABLE_2, 0x00) || !maxWrite(REG_TEMP_CONFIG, 0x01)) {
    report(T_DIE_TEMP, R_FAIL, "no se pudo escribir TEMP_CONFIG, err=%u (%s)", lastI2cErr,
           i2cErrText(lastI2cErr));
    return;
  }
  uint32_t start = millis();
  bool done = false;
  while (millis() - start < 100) {
    if (maxRead(REG_TEMP_CONFIG, cfg) && !(cfg & 0x01)) {
      done = true;
      break;
    }
    delay(2);
  }
  if (!done) {
    report(T_DIE_TEMP, R_FAIL, "la conversion de temperatura no termina");
    return;
  }
  if (!maxRead(REG_TEMP_INT, tInt) || !maxRead(REG_TEMP_FRAC, tFrac)) {
    report(T_DIE_TEMP, R_FAIL, "no se pudo leer TINT/TFRAC, err=%u (%s)", lastI2cErr,
           i2cErrText(lastI2cErr));
    return;
  }
  dieTempC = (int8_t)tInt + (tFrac & 0x0F) * 0.0625f;
  maxClearInterrupts();
  if (dieTempC < 10.0f || dieTempC > 60.0f) {
    report(T_DIE_TEMP, R_FAIL, "temperatura fuera de rango: %.2f C", dieTempC);
    return;
  }
  report(T_DIE_TEMP, R_PASS, "%.2f C", dieTempC);
}

static void testIntPin() {
  pinMode(PIN_INT_OX, INPUT_PULLUP);
  maxClearInterrupts();
  if (!maxWrite(REG_INT_ENABLE_2, INT2_DIE_TEMP_RDY)) {
    report(T_INT_PIN, R_FAIL, "no se pudo habilitar DIE_TEMP_RDY");
    return;
  }

  bool idleHigh = digitalRead(PIN_INT_OX) == HIGH;
  maxWrite(REG_TEMP_CONFIG, 0x01);

  bool wentLow = false;
  uint32_t start = millis();
  while (millis() - start < 100) {
    if (digitalRead(PIN_INT_OX) == LOW) {
      wentLow = true;
      break;
    }
    delay(1);
  }

  uint8_t status2 = 0;
  maxRead(REG_INT_STATUS_2, status2); // clears the flag, INT should be released
  bool flagSet = status2 & INT2_DIE_TEMP_RDY;
  delay(1);
  bool released = digitalRead(PIN_INT_OX) == HIGH;
  maxWrite(REG_INT_ENABLE_2, 0x00);

  bool ok = idleHigh && wentLow && flagSet && released;
  report(T_INT_PIN, ok ? R_PASS : R_FAIL, "reposo alto=%s, baja con int=%s, flag=%s, se libera=%s",
         idleHigh ? "si" : "NO", wentLow ? "si" : "NO", flagSet ? "si" : "NO",
         released ? "si" : "NO");
}

static void skipTest(TestId id, const char *reason) { report(id, R_NA, "omitido: %s", reason); }

static void runMaxTests() {
  for (uint8_t i = T_I2C_SCAN; i <= T_INT_PIN; i++) results[i] = R_PENDING;
  testI2cScan();
  if (results[T_I2C_SCAN] != R_PASS) {
    skipTest(T_PART_ID, "el MAX30102 no responde");
    skipTest(T_DIE_TEMP, "el MAX30102 no responde");
    skipTest(T_INT_PIN, "el MAX30102 no responde");
    return;
  }
  testPartId();
  if (results[T_PART_ID] != R_PASS) {
    skipTest(T_DIE_TEMP, "PART_ID incorrecto");
    skipTest(T_INT_PIN, "PART_ID incorrecto");
    return;
  }
  testDieTemp();
  testIntPin();
}

static bool maxUsable() { return results[T_I2C_SCAN] == R_PASS && results[T_PART_ID] == R_PASS; }

#if ENABLE_TFT
// ---- TFT readback (bit-banged) ---------------------------------------------
// The ST7735S shares one bidirectional SDA line for writes and reads, and the
// module has no MISO wire, so reads can't go through the SPI peripheral. This
// is done with plain GPIOs while the SPI bus is released: send the command
// byte with D/C low, release SDA, give one dummy clock, then clock the data out
// with D/C high. SPI mode 0 (idle low, MOSI latched on the rising edge).
static void tftBitWriteByte(uint8_t b) {
  for (uint8_t i = 0; i < 8; i++) {
    digitalWrite(PIN_SCK_TFT, LOW);
    digitalWrite(PIN_SDA_TFT, (b & 0x80) ? HIGH : LOW);
    delayMicroseconds(2);
    digitalWrite(PIN_SCK_TFT, HIGH);
    delayMicroseconds(2);
    b <<= 1;
  }
}

static void tftReadReg(uint8_t cmd, uint8_t *out, uint8_t n) {
  pinMode(PIN_CS_TFT, OUTPUT);
  pinMode(PIN_DC_TFT, OUTPUT);
  pinMode(PIN_SCK_TFT, OUTPUT);
  pinMode(PIN_SDA_TFT, OUTPUT);

  digitalWrite(PIN_SCK_TFT, LOW);
  digitalWrite(PIN_CS_TFT, LOW);
  digitalWrite(PIN_DC_TFT, LOW);
  tftBitWriteByte(cmd);

  digitalWrite(PIN_SCK_TFT, LOW);
  digitalWrite(PIN_DC_TFT, HIGH);
  pinMode(PIN_SDA_TFT, INPUT);

  // dummy clock cycle
  delayMicroseconds(2);
  digitalWrite(PIN_SCK_TFT, HIGH);
  delayMicroseconds(2);
  digitalWrite(PIN_SCK_TFT, LOW);

  for (uint8_t i = 0; i < n; i++) {
    uint8_t v = 0;
    for (uint8_t bit = 0; bit < 8; bit++) {
      delayMicroseconds(2); // data changes on the falling edge, sample before the rising one
      v = (v << 1) | (digitalRead(PIN_SDA_TFT) ? 1 : 0);
      digitalWrite(PIN_SCK_TFT, HIGH);
      delayMicroseconds(2);
      digitalWrite(PIN_SCK_TFT, LOW);
    }
    out[i] = v;
  }

  digitalWrite(PIN_CS_TFT, HIGH);
  pinMode(PIN_SDA_TFT, OUTPUT);
}

static void tftHardwareReset() {
  pinMode(PIN_RST_TFT, OUTPUT);
  digitalWrite(PIN_RST_TFT, HIGH);
  delay(5);
  digitalWrite(PIN_RST_TFT, LOW);
  delay(10);
  digitalWrite(PIN_RST_TFT, HIGH);
  delay(120);
}

static bool looksReadable(uint8_t v) { return v != 0x00 && v != 0xFF; }

static void testLcd() {
  tftHardwareReset();
  tftReadReg(TFT_CMD_RDDPM, &tftRddpmBefore, 1);
  tftReadReg(TFT_CMD_RDDID, tftId, 3);

  SPI.begin(PIN_SCK_TFT, -1, PIN_SDA_TFT, -1); // no MISO, no hardware CS
  tft.initR(TFT_INIT_TAB);
  tft.setSPISpeed(TFT_SPI_HZ);
  tft.setRotation(0);

  // Release the bus to read back what initR() wrote, then take it again.
  SPI.end();
  tftReadReg(TFT_CMD_RDDPM, &tftRddpmAfter, 1);
  SPI.begin(PIN_SCK_TFT, -1, PIN_SDA_TFT, -1);

  Serial.printf("RDDID  = %02X %02X %02X (ST7735S suele dar 7C 89 F0, los clones varian)\n",
                tftId[0], tftId[1], tftId[2]);
  Serial.printf("RDDPM  = 0x%02X tras reset (esperado 0x%02X), 0x%02X tras initR (esperado 0x%02X)\n",
                tftRddpmBefore, TFT_RDDPM_AFTER_RESET, tftRddpmAfter, TFT_RDDPM_AFTER_INIT);

  bool readable = looksReadable(tftRddpmBefore) || looksReadable(tftRddpmAfter);
  if (tftRddpmAfter == TFT_RDDPM_AFTER_INIT) {
    report(T_LCD_READBACK, R_PASS, "RDDPM 0x%02X -> 0x%02X, el controlador recibio la init por SPI",
           tftRddpmBefore, tftRddpmAfter);
  } else if (!readable) {
    report(T_LCD_READBACK, R_NA,
           "el modulo no devuelve datos de lectura (0x%02X/0x%02X), verificar visualmente",
           tftRddpmBefore, tftRddpmAfter);
  } else {
    report(T_LCD_READBACK, R_FAIL, "RDDPM 0x%02X -> 0x%02X, esperado 0x%02X tras la init",
           tftRddpmBefore, tftRddpmAfter, TFT_RDDPM_AFTER_INIT);
  }
}

// ---- TFT visual test ---------------------------------------------------------
static void drawCentered(const char *text, uint8_t size, uint16_t color) {
  int16_t x1, y1;
  uint16_t w, h;
  tft.setTextSize(size);
  tft.setTextColor(color);
  tft.getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
  tft.setCursor((TFT_W - w) / 2 - x1, (TFT_H - h) / 2 - y1);
  tft.print(text);
}

static void visualPattern() {
  struct Step {
    uint16_t fill;
    const char *name;
    uint16_t text;
  };
  const Step steps[] = {{ST77XX_RED, "ROJO", ST77XX_WHITE},
                        {ST77XX_GREEN, "VERDE", ST77XX_BLACK},
                        {ST77XX_BLUE, "AZUL", ST77XX_WHITE},
                        {ST77XX_WHITE, "BLANCO", ST77XX_BLACK}};
  for (const Step &s : steps) {
    tft.fillScreen(s.fill);
    drawCentered(s.name, 2, s.text); // the label must match the color shown
    delay(500);
  }

  tft.fillScreen(ST77XX_BLACK);
  tft.drawRect(0, 0, TFT_W, TFT_H, ST77XX_WHITE); // all 4 edges must be visible
  drawCentered("MARCO 1px", 1, ST77XX_WHITE);
  delay(1000);

  digitalWrite(PIN_BL_TFT, !BL_ON);
  delay(250);
  digitalWrite(PIN_BL_TFT, BL_ON);
  delay(250);
}

static void drawSummary() {
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_CYAN);
  tft.setCursor(0, 0);
  tft.print("TEST PERIFERICOS");
  tft.drawFastHLine(0, 10, TFT_W, COLOR_GRAY);

  for (uint8_t i = 0; i < T_COUNT; i++) {
    int16_t y = 16 + i * 14;
    const char *txt = resultText(results[i]);
    tft.setTextColor(ST77XX_WHITE);
    tft.setCursor(0, y);
    tft.print(TEST_LABELS[i]);
    tft.setTextColor(resultColor(results[i]));
    tft.setCursor(TFT_W - 6 * strlen(txt), y);
    tft.print(txt);
  }

  char buf[32];
  tft.setTextColor(COLOR_GRAY);
  snprintf(buf, sizeof(buf), "Temp die: %.1f C", dieTempC);
  tft.setCursor(0, 94);
  tft.print(buf);
  snprintf(buf, sizeof(buf), "RDDPM: %02X -> %02X", tftRddpmBefore, tftRddpmAfter);
  tft.setCursor(0, 104);
  tft.print(buf);
  snprintf(buf, sizeof(buf), "RDDID: %02X %02X %02X", tftId[0], tftId[1], tftId[2]);
  tft.setCursor(0, 114);
  tft.print(buf);
}
#else
static void drawSummary() {}
#endif

static void printSummary() {
  Serial.println("---- Resumen ----");
  for (uint8_t i = 0; i < T_COUNT; i++) {
    Serial.printf("  [%-4s] %s\n", resultText(results[i]), TEST_LABELS[i]);
  }
  Serial.println("-----------------");
}

// ---- Live mode ---------------------------------------------------------------
static bool live = false;
#if ENABLE_TFT
static uint32_t hist[HIST_N];
static uint16_t histHead = 0;
static bool histPrimed = false;
#endif
static uint32_t lastIr = 0, lastRed = 0;
static uint32_t lastDraw = 0;
static uint8_t i2cFails = 0;

// Live-mode debug counters, reported once per second by liveStats().
static uint32_t statsAt = 0;
static uint16_t samplesInWindow = 0;
static uint8_t fifoWr = 0, fifoOvf = 0, fifoRd = 0;

static bool startLive() {
  Wire.setClock(400000);
  bool ok = maxWrite(REG_INT_ENABLE_2, 0x00) && maxWrite(REG_FIFO_CONFIG, LIVE_FIFO_CONFIG) &&
            maxWrite(REG_SPO2_CONFIG, LIVE_SPO2_CONFIG) && maxWrite(REG_LED1_PA, LIVE_LED_PA) &&
            maxWrite(REG_LED2_PA, LIVE_LED_PA) && maxWrite(REG_FIFO_WR_PTR, 0) &&
            maxWrite(REG_FIFO_OVF, 0) && maxWrite(REG_FIFO_RD_PTR, 0) &&
            maxWrite(REG_MODE_CONFIG, MODE_SPO2);
  if (!ok) {
    DBG("LIVE", "no se pudo configurar el MAX30102 para la lectura en vivo: err=%u (%s)",
        lastI2cErr, i2cErrText(lastI2cErr));
    Wire.setClock(100000);
    return false;
  }

  i2cFails = 0;
  lastDraw = 0;
  statsAt = millis();
  samplesInWindow = 0;
  lastIr = lastRed = 0;
#if ENABLE_TFT
  histPrimed = false;
  tft.fillScreen(ST77XX_BLACK);
  tft.drawFastHLine(0, PLOT_Y - 2, TFT_W, COLOR_GRAY);
#endif
  live = true;
  DBG("LIVE", "sensor configurado (SpO2, 100 sps / 4 = ~25 muestras/s esperadas, I2C a 400 kHz)");
  Serial.println("Modo en vivo: ir:<n>,red:<n>");
  return true;
}

static void pushSample(uint32_t ir, uint32_t red) {
#if ENABLE_TFT
  if (!histPrimed) { // start the plot flat at the first value instead of at 0
    for (uint16_t i = 0; i < HIST_N; i++) hist[i] = ir;
    histPrimed = true;
  }
  hist[histHead] = ir;
  histHead = (histHead + 1) % HIST_N;
#endif
  lastIr = ir;
  lastRed = red;
  samplesInWindow++;
  Serial.printf("ir:%lu,red:%lu\n", (unsigned long)ir, (unsigned long)red);
}

static void pollFifo() {
  uint8_t ptrs[3]; // WR_PTR, OVF_COUNTER, RD_PTR are consecutive registers
  if (!maxReadBytes(REG_FIFO_WR_PTR, ptrs, 3)) {
    i2cFails++;
    if (i2cFails == 1) { // log only the first failure of a streak, not all of them
      DBG("I2C", "fallo al leer los punteros del FIFO: err=%u (%s)", lastI2cErr,
          i2cErrText(lastI2cErr));
    }
    return;
  }
  i2cFails = 0;
  fifoWr = ptrs[0];
  fifoOvf = ptrs[1];
  fifoRd = ptrs[2];

  uint8_t pending = (uint8_t)(ptrs[0] - ptrs[2]) & 0x1F;
  if (pending == 0 && ptrs[1] != 0) pending = 32; // FIFO overran and wrapped

  while (pending > 0) {
    uint8_t chunk = pending > 16 ? 16 : pending;
    uint8_t buf[16 * 6];
    if (!maxReadBytes(REG_FIFO_DATA, buf, chunk * 6)) {
      i2cFails++;
      return;
    }
    for (uint8_t i = 0; i < chunk; i++) {
      const uint8_t *s = &buf[i * 6]; // RED (LED1) first, then IR (LED2), 18 bit each
      uint32_t red = (((uint32_t)s[0] << 16) | ((uint32_t)s[1] << 8) | s[2]) & 0x3FFFF;
      uint32_t ir = (((uint32_t)s[3] << 16) | ((uint32_t)s[4] << 8) | s[5]) & 0x3FFFF;
      pushSample(ir, red);
    }
    pending -= chunk;
  }
}

#if ENABLE_TFT
static void drawLive() {
  char buf[16];
  bool finger = lastIr > FINGER_IR_THRESHOLD;

  tft.setTextSize(2);
  tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
  snprintf(buf, sizeof(buf), "IR  %6lu", (unsigned long)lastIr);
  tft.setCursor(0, 0);
  tft.print(buf);
  snprintf(buf, sizeof(buf), "RED %6lu", (unsigned long)lastRed);
  tft.setCursor(0, 18);
  tft.print(buf);
  tft.setTextColor(finger ? ST77XX_GREEN : ST77XX_YELLOW, ST77XX_BLACK);
  tft.setCursor(0, 36);
  tft.print(finger ? "DEDO    " : "SIN DEDO");

  // Autoscaled IR trace, drawn off-screen and pushed in one go to avoid flicker.
  int32_t mn = hist[0], mx = hist[0];
  for (uint16_t i = 1; i < HIST_N; i++) {
    if ((int32_t)hist[i] < mn) mn = hist[i];
    if ((int32_t)hist[i] > mx) mx = hist[i];
  }
  if (mx - mn < PLOT_MIN_SPAN) {
    int32_t mid = (mx + mn) / 2;
    mn = mid - PLOT_MIN_SPAN / 2;
    mx = mn + PLOT_MIN_SPAN;
  }
  plot.fillScreen(ST77XX_BLACK);
  int16_t prevY = 0;
  for (uint16_t x = 0; x < HIST_N; x++) {
    int32_t v = hist[(histHead + x) % HIST_N];
    int16_t y = (PLOT_H - 1) - (int16_t)(((v - mn) * (PLOT_H - 1)) / (mx - mn));
    if (x > 0) plot.drawLine(x - 1, prevY, x, y, ST77XX_CYAN);
    prevY = y;
  }
  tft.drawRGBBitmap(0, PLOT_Y, plot.getBuffer(), TFT_W, PLOT_H);
}
#else
static void drawLive() {}
#endif

// One line per second while streaming: tells "no samples" from "samples but no finger".
static void liveStats() {
  if (millis() - statsAt < 1000) return;
  statsAt = millis();
  const char *hint = samplesInWindow == 0 ? "SIN MUESTRAS NUEVAS (FIFO vacio)"
                     : (lastIr == 0 && lastRed == 0) ? "IR y RED en 0 (LEDs sin senal)"
                     : (lastIr > FINGER_IR_THRESHOLD) ? "dedo detectado"
                                                      : "sin dedo";
  DBG("LIVE", "%u muestras/s | ir=%lu red=%lu | FIFO wr=%u rd=%u ovf=%u | fallos I2C=%u | INT=%d | %s",
      samplesInWindow, (unsigned long)lastIr, (unsigned long)lastRed, fifoWr, fifoRd, fifoOvf,
      i2cFails, digitalRead(PIN_INT_OX), hint);
  samplesInWindow = 0;
}

static void loseLive() {
  live = false;
  Wire.setClock(100000);
  results[T_I2C_SCAN] = R_FAIL;
  results[T_PART_ID] = results[T_DIE_TEMP] = results[T_INT_PIN] = R_NA;
  DBG("I2C", "el MAX30102 dejo de responder (%u fallos seguidos), ultimo error: %u (%s)",
      i2cFails, lastI2cErr, i2cErrText(lastI2cErr));
  dbgI2cLines("al perder el sensor");
  Serial.println("Reintentando cada 2 s...");
  drawSummary();
}

// Fast blink while the sensor is missing, slow blink while streaming.
static void ledHeartbeat() {
  static uint32_t last = 0;
  static bool on = false;
  if (millis() - last < (live ? LED_PERIOD_LIVE_MS : LED_PERIOD_IDLE_MS)) return;
  last = millis();
  on = !on;
  ledSet(on);
}

// ---- Arduino entry points -----------------------------------------------------
void setup() {
  // 3 quick blinks first: proves the firmware started even if Serial never shows up.
  pinMode(PIN_LED, OUTPUT);
  for (uint8_t i = 0; i < 3; i++) {
    ledSet(true);
    delay(80);
    ledSet(false);
    delay(120);
  }
  ledSet(true); // solid on while setup() runs

  Serial.begin(115200);
  uint32_t start = millis();
  while (!Serial && millis() - start < 4000) delay(10); // USB-CDC: wait for the monitor to attach
  uint32_t waited = millis() - start;
  bool monitorAttached = Serial;
  delay(300);

  Serial.println();
  Serial.printf("=== PulsOx - test de perifericos (MAX30102%s) ===\n",
                ENABLE_TFT ? " + TFT ST7735S" : ", TFT deshabilitado");
  DBG("BOOT", "setup() iniciado. Monitor serie %s (espera: %lu ms)",
      monitorAttached ? "conectado" : "NO detectado (si lo abris ahora, apreta RESET)",
      (unsigned long)waited);
  printBootInfo();

#if ENABLE_TFT
  pinMode(PIN_BL_TFT, OUTPUT);
  digitalWrite(PIN_BL_TFT, BL_ON);
#endif

  pinMode(PIN_INT_OX, INPUT_PULLUP);
  i2cPreCheck();
  bool wireOk = Wire.begin(PIN_SDA_OX, PIN_SCL_OX);
  Wire.setClock(100000);
  DBG("I2C", "Wire.begin(SDA=GPIO%d, SCL=GPIO%d) -> %s, 100 kHz", PIN_SDA_OX, PIN_SCL_OX,
      wireOk ? "ok" : "ERROR");
  dbgI2cLines("con el bus iniciado");

  DBG("BOOT", "corriendo tests del MAX30102...");
  runMaxTests();

#if ENABLE_TFT
  testLcd();
  visualPattern();
#else
  skipTest(T_LCD_READBACK, "TFT deshabilitado (ENABLE_TFT=0 en main.cpp)");
#endif
  drawSummary();
  printSummary();
  delay(SUMMARY_HOLD_MS);

  if (maxUsable()) {
    startLive();
  } else {
    DBG("BOOT", "sin sensor utilizable: LED en parpadeo rapido, reintento cada %d ms", RETRY_INTERVAL_MS);
  }
  DBG("BOOT", "setup() terminado");
}

void loop() {
  static bool firstLoop = true;
  if (firstLoop) {
    firstLoop = false;
    DBG("BOOT", "entrando a loop()");
  }
  ledHeartbeat();

  if (!live) {
    static uint32_t lastTry = 0;
    static uint32_t attempts = 0;
    if (millis() - lastTry < RETRY_INTERVAL_MS) return;
    lastTry = millis();
    attempts++;

    uint8_t err = maxProbe();
    if (err != 0) {
      DBG("I2C", "reintento #%lu: 0x57 sin respuesta, err=%u (%s) | SDA=%d SCL=%d INT=%d | heap %u B",
          (unsigned long)attempts, err, i2cErrText(err), digitalRead(PIN_SDA_OX),
          digitalRead(PIN_SCL_OX), digitalRead(PIN_INT_OX), (unsigned)ESP.getFreeHeap());
      return;
    }

    DBG("I2C", "0x57 detectado en el reintento #%lu, repitiendo tests...", (unsigned long)attempts);
    runMaxTests();
    drawSummary();
    printSummary();
    delay(SUMMARY_HOLD_MS);
    if (maxUsable()) startLive();
    return;
  }

  pollFifo();
  if (i2cFails >= I2C_FAILS_BEFORE_LOST) {
    loseLive();
    return;
  }
  liveStats();

  if (millis() - lastDraw >= LIVE_REFRESH_MS) {
    lastDraw = millis();
    drawLive();
  }
  delay(5);
}
