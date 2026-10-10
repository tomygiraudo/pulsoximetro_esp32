// MAX30102 register map (only what the firmware uses).
#pragma once
#include <stdint.h>

namespace max30102_reg {

constexpr uint8_t ADDR = 0x57;
constexpr uint8_t PART_ID_EXPECTED = 0x15;

constexpr uint8_t INT_STATUS_1 = 0x00;
constexpr uint8_t INT_STATUS_2 = 0x01;
constexpr uint8_t INT_ENABLE_2 = 0x03;
constexpr uint8_t FIFO_WR_PTR = 0x04;  // WR_PTR, OVF_COUNTER and RD_PTR are consecutive
constexpr uint8_t FIFO_OVF = 0x05;
constexpr uint8_t FIFO_RD_PTR = 0x06;
constexpr uint8_t FIFO_DATA = 0x07;
constexpr uint8_t FIFO_CONFIG = 0x08;
constexpr uint8_t MODE_CONFIG = 0x09;
constexpr uint8_t SPO2_CONFIG = 0x0A;
constexpr uint8_t LED1_PA = 0x0C;  // RED
constexpr uint8_t LED2_PA = 0x0D;  // IR
constexpr uint8_t REV_ID = 0xFE;
constexpr uint8_t PART_ID = 0xFF;

constexpr uint8_t MODE_SHDN = 0x80;   // shutdown: registers kept, LEDs/ADC off
constexpr uint8_t MODE_RESET = 0x40;  // soft reset, the bit clears itself when done
constexpr uint8_t MODE_SPO2 = 0x03;   // RED + IR pulsing

}  // namespace max30102_reg
