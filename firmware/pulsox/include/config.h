// Single place for every tunable of the PulsOx firmware: pins, sensor setup and
// the DSP parameters. The signal-processing values are starting points, meant
// to be adjusted against real captures (see tools/ppg_colab.ipynb).
#pragma once

// ---- Pins (PCB/Sensor_ctrl_pwr_pcb, "Esquematico V2") ----------------------
#define PIN_SDA_OX 3
#define PIN_SCL_OX 4
#define PIN_INT_OX 1   // MAX30102 INT, open-drain active low (not used yet: the FIFO is polled)

// ESP32-C3 Super Mini on-board LED, not wired to anything on the PCB (V2).
#define PIN_LED 8
#define LED_ON_LEVEL LOW   // active-low on the Super Mini; flip if it looks inverted
#define LED_PERIOD_IDLE_MS 125   // toggle period while the sensor is missing (fast blink)
#define LED_PERIOD_LIVE_MS 500   // toggle period while streaming (slow blink)

// ---- Runtime ------------------------------------------------------------------
#define SERIAL_WAIT_MS 4000          // max wait for the monitor at boot (USB-CDC)
#define SENSOR_RETRY_MS 2000         // how often to retry while the sensor is missing
#define I2C_FAILS_BEFORE_LOST 10     // consecutive failed polls before declaring the sensor lost
#define STATS_PERIOD_MS 1000         // period of the summary line

// ---- MAX30102 setup ---------------------------------------------------------
// 400 sps with 4-sample averaging -> 100 samples/s. The 25 Hz of peripherals-test
// is too coarse to measure beat-to-beat intervals.
#define SENSOR_FS_HZ 100.0f
#define SENSOR_FIFO_CONFIG 0x50   // SMP_AVE=4, rollover on
#define SENSOR_SPO2_CONFIG 0x2F   // ADC_RGE=4096 nA, SR=400 sps, LED_PW=411 us (18 bit)
#define SENSOR_LED_RED_PA 0x24    // 36 * 0.2 mA ~ 7 mA
#define SENSOR_LED_IR_PA 0x24
#define SENSOR_ADC_MAX 262143.0f  // 2^18 - 1

#define I2C_CLOCK_HZ 400000
#define POLL_INTERVAL_MS 10       // the 32-sample FIFO holds 320 ms of data at 100 sps

// ---- Finger detection and saturation -----------------------------------------
#define FINGER_ON_IR_DC 50000.0f   // DC of IR above this -> finger present
#define FINGER_OFF_IR_DC 40000.0f  // ... and below this -> finger gone (hysteresis)
#define FINGER_DEBOUNCE_S 0.2f
#define SATURATION_FRACTION 0.95f  // raw sample above this fraction of full scale = saturated
#define SETTLE_S 2.0f              // after the finger appears: let the filters settle, no beats

// ---- Filtering -----------------------------------------------------------------
#define DC_TAU_S 1.0f              // EMA time constant of the DC estimate
#define BAND_HP_HZ 0.5f            // 2nd order Butterworth high-pass
#define BAND_LP_HZ 5.0f            // 2nd order Butterworth low-pass

// ---- Beat detection --------------------------------------------------------------
#define BEAT_ENV_TAU_S 2.0f        // decay of the peak envelope
#define BEAT_THRESHOLD_K 0.5f      // threshold = K * envelope
#define BEAT_REFRACTORY_S 0.3f

// ---- Heart rate -------------------------------------------------------------------
#define RR_MIN_S 0.3f              // 200 bpm
#define RR_MAX_S 2.0f              // 30 bpm
#define RR_MAX_DEVIATION 0.30f     // reject an RR this far from the current median
#define BPM_MEDIAN_N 5
#define BPM_VALID_MIN_RR 5
#define BPM_VALID_MAX_CV 0.15f

// ---- SpO2 ----------------------------------------------------------------------------
// SpO2 = A*R^2 + B*R + C, coefficients from the Maxim reference algorithm.
// UNCALIBRATED for this sensor/housing: values are indicative only.
#define SPO2_COEF_A -45.060f
#define SPO2_COEF_B 30.354f
#define SPO2_COEF_C 94.845f
#define SPO2_R_MEDIAN_N 8
#define SPO2_VALID_MIN_R 5
#define SPO2_VALID_MAX_SPREAD 0.10f

// ---- Signal quality ---------------------------------------------------------------------
#define QUALITY_PI_GOOD_PCT 1.0f   // perfusion index (AC/DC, %) that counts as a good signal
#define QUALITY_CV_BAD 0.30f       // RR coefficient of variation that drives the score to 0
