// MAX30102 driver: I2C register access, setup in SpO2 mode and FIFO readout.
//
// Observed on this board: the first I2C transaction after Wire.begin() fails and
// the following ones work (cause not identified). begin() absorbs it with a
// throwaway probe, and every access is retried on the SAME bus session without
// re-initialising Wire.
#pragma once
#include <Arduino.h>
#include <Wire.h>
#include "ppg_types.h"

struct Max30102Config {
  uint8_t fifoConfig;  // FIFO_CONFIG register (sample averaging, rollover)
  uint8_t spo2Config;  // SPO2_CONFIG register (ADC range, sample rate, pulse width)
  uint8_t ledRedPa;    // LED1_PA, 0.2 mA per step
  uint8_t ledIrPa;     // LED2_PA, 0.2 mA per step
};

class Max30102 {
 public:
  // Error codes beyond what Wire.endTransmission() returns.
  static constexpr uint8_t ERR_SHORT_READ = 0xF0;    // requestFrom() returned fewer bytes than asked
  static constexpr uint8_t ERR_BAD_PART_ID = 0xF1;   // answered, but it is not a MAX30102
  static constexpr uint8_t ERR_RESET_TIMEOUT = 0xF2; // soft reset did not complete
  static constexpr uint8_t ERR_READBACK = 0xF3;      // a register did not keep the value written

  // Wire must already be started (Wire.begin) by the caller. Probes the sensor,
  // checks PART_ID and soft-resets it, which also clears LED currents and mode
  // left over from before an ESP32 reset (the sensor is powered from the 3V3 rail).
  bool begin(TwoWire &wire);

  // Applies the setup and starts sampling in SpO2 mode (RED + IR).
  bool configure(const Max30102Config &cfg);

  // Pops up to `max` samples from the FIFO into `buf`. Returns how many were
  // read (0 if the FIFO is empty) or -1 on an I2C error (see lastError()).
  int readFifo(PpgSample *buf, size_t max);

  // LEDs to 0 mA and shutdown mode (~1 uA), verified by reading back.
  bool shutdown();

  // Samples lost because the FIFO overran, since the last configure().
  uint32_t overflowCount() const { return overflow_; }

  // Error of the last failed I2C attempt (0 if none yet).
  uint8_t lastError() const { return lastErr_; }
  static const char *errorText(uint8_t err);

 private:
  bool writeReg(uint8_t reg, uint8_t val);
  bool readBytes(uint8_t reg, uint8_t *buf, size_t n);
  bool readReg(uint8_t reg, uint8_t &val) { return readBytes(reg, &val, 1); }
  void clearInterrupts();
  bool softReset();

  TwoWire *wire_ = nullptr;
  uint8_t lastErr_ = 0;
  uint32_t overflow_ = 0;
};
