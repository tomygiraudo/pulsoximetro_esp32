// I2C bring-up diagnostics, to tell a missing sensor from a wiring or pull-up
// problem when the MAX30102 does not answer.
#pragma once

// Run once BEFORE Wire.begin(): tells "no pull-up" apart from "line held low".
// Both lines should read 1 with the sensor powered (the module has its own pull-ups).
void i2cPreCheck();

// Run after the sensor failed to start: probes 0x57 with a STOP condition (the
// raw error says whether anything ACKs the address), scans the whole bus and
// prints the SDA / SCL / INT levels.
void i2cDiagnose();
