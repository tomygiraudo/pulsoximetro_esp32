#include "max30102.h"
#include "max30102_regs.h"

using namespace max30102_reg;

static constexpr uint8_t I2C_TRIES = 5;
static constexpr uint8_t I2C_RETRY_MS = 5;
static constexpr uint32_t RESET_TIMEOUT_MS = 100;

static constexpr size_t SAMPLE_BYTES = 6;      // RED (3 bytes) + IR (3 bytes)
static constexpr size_t FIFO_DEPTH = 32;
static constexpr size_t FIFO_CHUNK = 16;       // samples per I2C read: 96 bytes, fits Wire's 128 B buffer
static constexpr uint32_t ADC_MASK = 0x3FFFF;  // 18 bit
static constexpr uint8_t MAX_CORRUPT_RUN = 3;  // consecutive all-ones samples replaced; more is a real saturation

// ---- I2C access -------------------------------------------------------------
bool Max30102::writeReg(uint8_t reg, uint8_t val) {
  for (uint8_t i = 0; i < I2C_TRIES; i++) {
    wire_->beginTransmission(ADDR);
    wire_->write(reg);
    wire_->write(val);
    lastErr_ = wire_->endTransmission();
    if (lastErr_ == 0) return true;
    delay(I2C_RETRY_MS);
  }
  return false;
}

bool Max30102::readBytes(uint8_t reg, uint8_t *buf, size_t n) {
  for (uint8_t i = 0; i < I2C_TRIES; i++) {
    wire_->beginTransmission(ADDR);
    wire_->write(reg);
    lastErr_ = wire_->endTransmission(false);
    if (lastErr_ == 0) {
      if (wire_->requestFrom((uint8_t)ADDR, n) == n) {
        for (size_t b = 0; b < n; b++) buf[b] = wire_->read();
        return true;
      }
      lastErr_ = ERR_SHORT_READ;
    }
    delay(I2C_RETRY_MS);
  }
  return false;
}

// Reading the status registers clears them (and releases INT).
void Max30102::clearInterrupts() {
  uint8_t dummy;
  readReg(INT_STATUS_1, dummy);
  readReg(INT_STATUS_2, dummy);
}

const char *Max30102::errorText(uint8_t err) {
  switch (err) {
    case 0: return "ok";
    case 1: return "datos demasiado largos";
    case 2: return "NACK en la direccion (nadie responde)";
    case 3: return "NACK en los datos";
    case 4: return "error del bus";
    case 5: return "timeout (SDA/SCL trabadas?)";
    case ERR_SHORT_READ: return "lectura incompleta";
    case ERR_BAD_PART_ID: return "PART_ID incorrecto (no es un MAX30102)";
    case ERR_RESET_TIMEOUT: return "el soft reset no termina";
    case ERR_READBACK: return "el registro no guardo el valor escrito";
    case ERR_BAD_PTRS: return "punteros del FIFO fuera de rango (lectura corrupta)";
    default: return "desconocido";
  }
}

// ---- Control ------------------------------------------------------------------
bool Max30102::softReset() {
  if (!writeReg(MODE_CONFIG, MODE_RESET)) return false;
  uint32_t start = millis();
  uint8_t mode;
  while (millis() - start < RESET_TIMEOUT_MS) {
    if (readReg(MODE_CONFIG, mode) && !(mode & MODE_RESET)) {
      clearInterrupts();  // PWR_RDY is set again after a reset and holds INT low
      return true;
    }
    delay(1);
  }
  lastErr_ = ERR_RESET_TIMEOUT;
  return false;
}

bool Max30102::begin(TwoWire &wire) {
  wire_ = &wire;
  overflow_ = 0;
  corrupt_ = 0;
  corruptRun_ = 0;
  haveLast_ = false;

  wire_->beginTransmission(ADDR);  // throwaway probe: absorbs the first (failing) transaction
  wire_->endTransmission();

  uint8_t part = 0;
  if (!readReg(PART_ID, part)) return false;
  if (part != PART_ID_EXPECTED) {
    lastErr_ = ERR_BAD_PART_ID;
    return false;
  }
  return softReset();
}

bool Max30102::configure(const Max30102Config &cfg) {
  bool ok = writeReg(INT_ENABLE_2, 0x00) && writeReg(FIFO_CONFIG, cfg.fifoConfig) &&
            writeReg(SPO2_CONFIG, cfg.spo2Config) && writeReg(LED1_PA, cfg.ledRedPa) &&
            writeReg(LED2_PA, cfg.ledIrPa) && writeReg(FIFO_WR_PTR, 0) && writeReg(FIFO_OVF, 0) &&
            writeReg(FIFO_RD_PTR, 0) && writeReg(MODE_CONFIG, MODE_SPO2);
  if (ok) overflow_ = 0;
  return ok;
}

bool Max30102::shutdown() {
  if (!writeReg(LED1_PA, 0x00) || !writeReg(LED2_PA, 0x00) || !writeReg(MODE_CONFIG, MODE_SHDN)) {
    return false;
  }
  uint8_t mode = 0, led1 = 0xFF, led2 = 0xFF;
  if (!readReg(MODE_CONFIG, mode) || !readReg(LED1_PA, led1) || !readReg(LED2_PA, led2)) return false;
  if (!(mode & MODE_SHDN) || led1 != 0 || led2 != 0) {
    lastErr_ = ERR_READBACK;
    return false;
  }
  return true;
}

// ---- FIFO -----------------------------------------------------------------------
int Max30102::readFifo(PpgSample *buf, size_t max) {
  uint8_t ptrs[3];  // WR_PTR, OVF_COUNTER, RD_PTR
  if (!readBytes(FIFO_WR_PTR, ptrs, 3)) return -1;
  // All three are 5-bit registers. A bigger value is a corrupted read (0xFF when the sensor
  // does not drive the bus): counted as it is, it would invent 255 lost samples.
  if (ptrs[0] > 0x1F || ptrs[1] > 0x1F || ptrs[2] > 0x1F) {
    lastErr_ = ERR_BAD_PTRS;
    return -1;
  }

  size_t pending = (ptrs[0] - ptrs[2]) & 0x1F;
  if (ptrs[1] != 0) {
    overflow_ += ptrs[1];
    writeReg(FIFO_OVF, 0);  // count it once: left set, the same value would be added on every poll
    if (pending == 0) pending = FIFO_DEPTH;  // the FIFO overran and wrapped all the way around
  }
  if (pending > max) pending = max;  // the rest stays in the FIFO for the next call

  size_t got = 0;  // samples taken out of the FIFO
  size_t out = 0;  // samples returned: a corrupt one comes back as the previous good one (the time axis
                   // stays right), or is dropped if there is none yet
  while (got < pending) {
    size_t chunk = pending - got < FIFO_CHUNK ? pending - got : FIFO_CHUNK;
    uint8_t raw[FIFO_CHUNK * SAMPLE_BYTES];
    if (!readBytes(FIFO_DATA, raw, chunk * SAMPLE_BYTES)) return got > 0 ? (int)out : -1;
#ifdef PULSOX_TEST_HOOKS
    if (testCorrupt_) {
      memset(raw, 0xFF, SAMPLE_BYTES);
      testCorrupt_ = false;
    }
#endif
    for (size_t i = 0; i < chunk; i++) {
      const uint8_t *s = &raw[i * SAMPLE_BYTES];  // RED (LED1) first, then IR (LED2)
      // The 6 bits above the 18th are NOT zero in good samples: with a finger on the sensor they carry
      // junk (0x04, 0x09... in the first byte) while the 18 low bits are a clean ramp. They are masked
      // off, never checked. What a bus glitch gives is all six bytes at 0xFF, on both channels at once.
      const bool allOnes = s[0] == 0xFF && s[1] == 0xFF && s[2] == 0xFF && s[3] == 0xFF && s[4] == 0xFF &&
                           s[5] == 0xFF;
      if (allOnes && corruptRun_ < MAX_CORRUPT_RUN) {  // a longer run would be a real saturation: let it through
        corrupt_++;
        corruptRun_++;
        if (haveLast_) buf[out++] = last_;
        continue;
      }
      corruptRun_ = 0;
      last_.red = (((uint32_t)s[0] << 16) | ((uint32_t)s[1] << 8) | s[2]) & ADC_MASK;
      last_.ir = (((uint32_t)s[3] << 16) | ((uint32_t)s[4] << 8) | s[5]) & ADC_MASK;
      haveLast_ = true;
      buf[out++] = last_;
    }
    got += chunk;
  }
  return (int)out;
}
