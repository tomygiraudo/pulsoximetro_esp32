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
  static constexpr uint8_t ERR_BAD_PTRS = 0xF4;      // FIFO pointers out of range: a corrupted read

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

  /*******************************************************************************
   * @brief       Devuelve las muestras perdidas por desborde del FIFO desde el
   *              último configure().
   *
   * @details     Devuelve overflow_ tal cual.
   *
   * @param       Entrada: overflow_ - muestras perdidas acumuladas.
   *              Salida:  retorno (uint32_t) - ese acumulado.
   *
   * @peripherals Ninguno.
   *******************************************************************************/
  uint32_t overflowCount() const { return overflow_; }

  /*******************************************************************************
   * @brief       Devuelve las muestras corruptas que readFifo() reemplazó desde
   *              begin().
   *
   * @details     Un glitch del bus lee como seis bytes 0xFF, en los dos canales a
   *              la vez. Sin reemplazarlo sería una muestra a fondo de escala
   *              (262143) en una señal de ~90000: satura los filtros e invalida
   *              el SpO2 los 10 s que dura en la ventana.
   *
   * @param       Entrada: corrupt_ - muestras corruptas reemplazadas.
   *              Salida:  retorno (uint32_t) - ese acumulado.
   *
   * @peripherals Ninguno.
   *******************************************************************************/
  uint32_t corruptCount() const { return corrupt_; }

#ifdef PULSOX_TEST_HOOKS
  /*******************************************************************************
   * @brief       Pide que la próxima muestra leída salga corrupta (solo builds de
   *              prueba).
   *
   * @details     Activa testCorrupt_: readFifo() pone la próxima muestra en 0xFF,
   *              como tras un glitch del bus. Existe solo con PULSOX_TEST_HOOKS.
   *
   * @param       Salida:  testCorrupt_ - pasa a true.
   *
   * @peripherals Ninguno.
   *******************************************************************************/
  void testCorruptNextSample() { testCorrupt_ = true; }
#endif

  /*******************************************************************************
   * @brief       Devuelve el error del último intento I2C fallido (0 si todavía
   *              no hubo).
   *
   * @details     Devuelve lastErr_ tal cual; errorText() lo traduce a texto.
   *
   * @param       Entrada: lastErr_ - código del último error.
   *              Salida:  retorno (uint8_t) - ese código.
   *
   * @peripherals Ninguno.
   *******************************************************************************/
  uint8_t lastError() const { return lastErr_; }
  static const char *errorText(uint8_t err);

 private:
  bool writeReg(uint8_t reg, uint8_t val);
  bool readBytes(uint8_t reg, uint8_t *buf, size_t n);

  /*******************************************************************************
   * @brief       Lee un solo registro del MAX30102.
   *
   * @details     Atajo de readBytes() con n = 1.
   *
   * @param       Entrada: reg - dirección del registro.
   *              Salida:  val - valor leído.
   *              Salida:  retorno (bool) - true si la lectura salió bien.
   *
   * @peripherals Internos: I2C (Wire). Externos: MAX30102.
   *******************************************************************************/
  bool readReg(uint8_t reg, uint8_t &val) { return readBytes(reg, &val, 1); }
  void clearInterrupts();
  bool softReset();

  TwoWire *wire_ = nullptr;
  uint8_t lastErr_ = 0;
  uint32_t overflow_ = 0;
  uint32_t corrupt_ = 0;
  uint8_t corruptRun_ = 0;   // all-ones samples in a row
  PpgSample last_ = {0, 0};  // the latest good sample: what a corrupt one is replaced with
  bool haveLast_ = false;
#ifdef PULSOX_TEST_HOOKS
  bool testCorrupt_ = false;
#endif
};
