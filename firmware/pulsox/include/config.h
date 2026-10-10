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

// ---- TFT orientation (lib/tft_st7735) -----------------------------------------
// The panel is mounted upside down, so the controller scans it rotated 180 degrees
// (rotation 2) and every screen is drawn as designed. The image origin in the
// controller RAM depends on the rotation: this module shows its window at column 0,
// row 32 in rotation 0 (offset -2, +29 from the library's 2, 3); in rotation 2 the
// library assumes column 2, row 1, so the same window needs -2, -1. If a noise strip
// shows up on one edge, nudge these (see lib/tft_st7735/tft_st7735.h, section 2.8).
#define TFT_ROTATION 2
#define TFT_X_ADJUST -2
#define TFT_Y_ADJUST -1
// The backlight stays off while the panel is initialised and comes on with the first frame
// (display.cpp), so waking up does not flash the controller's random RAM.
#define TFT_BL_ON_AT_BEGIN 0

// ---- Status bar ---------------------------------------------------------------
// There is no battery measurement yet, and WiFi is not used: the bar shows this
// fixed battery level (a placeholder, NOT a reading) and the WiFi icon in grey.
#define BATTERY_PLACEHOLDER_PCT 100

// ---- Button and sleep ------------------------------------------------------------
// SW1 lives on a separate board (it is not in the KiCad): 10K pull-up to 3V3 and a 10K/1 uF
// RC debounce to GPIO0, active low. GPIO0 is one of the pins (0-5) that can wake the
// ESP32-C3 from deep sleep, by level.
#define PIN_BUTTON 0
#define BUTTON_DEBOUNCE_MS 30        // software debounce on top of the RC
#define BUTTON_RELEASE_WAIT_MS 5000  // before deep sleep: longest wait for a held button to be released
#ifndef BUTTON_LOG_RAW
#define BUTTON_LOG_RAW 0             // 1 (env `dev`): log every edge of the raw GPIO0 level, bounces included
#endif
#define BUTTON_STUCK_RETRY_S 60      // button still down after that wait: sleep on a timer instead
                                     // (a GPIO wakeup would fire at once and loop)
#ifndef POWER_DEEP_SLEEP
#define POWER_DEEP_SLEEP 1           // 0 (env `dev`): the "sleep" is simulated, CPU and USB stay up
#endif
#define BOOT_WINDOW_MS 5000          // after a cold boot: time to flash or open the monitor before sleeping
#define SERIAL_WAIT_ON_WAKE_MS 0     // waking from deep sleep: do not wait for the monitor (USB comes
                                     // back re-enumerated, waiting would only cost battery)

// ---- Measurement flow ---------------------------------------------------------------
#define CONNECT_MAX_MS 40000         // longest the cloud may take to connect (WiFi + login + first writes)
#define FINGER_WAIT_S 30             // connected (or failed) and nobody puts a finger on: back to sleep
#define MEAS_MAX_S 60                // PROVISIONAL (stage 2): fixed length; stage 4 makes it 45 s min / 60 s max
#define MEAS_FINGER_LOST_S 1         // finger away this long during the measurement: it is discarded

// ---- Cloud (Firebase Realtime Database, PROTOCOL.md "Transporte en la nube") -------------
// WiFi and Firebase credentials live in include/secrets.h (gitignored, see secrets.h.example).
#define DB_HOST "pulsoximetro-esp-default-rtdb.firebaseio.com"  // databaseURL without scheme
#define DEVICE_ID "pulsox-4ba0e4ea46d7"
#define FW_VERSION "pulsox-0.2.0"
#define WIFI_CONNECT_TIMEOUT_MS 10000  // the whole WiFi association; login and the first writes come after
#define CLOUD_TASK_STACK 12288         // bytes: the TLS handshake needs well over the 8 KB of the loop task
#define CLOUD_PPG_FS 50                // samples/s in `live.ppg`: the 100 sps view trace, averaged in pairs
#define READING_EVERY_S 5              // history reading while SpO2 and BPM are valid
#define CLOUD_CLOSE_TIMEOUT_MS 10000   // before sleeping: longest wait for the cloud to finish what it is doing

// ---- Runtime ------------------------------------------------------------------
#define SERIAL_WAIT_MS 4000          // max wait for the monitor at a cold boot (USB-CDC)
#define SERIAL_TX_TIMEOUT_MS 5       // longest a Serial write may wait for the host; never 0 (see serial_stream.cpp)
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
// The same Butterworth filters as tools/procesamiento.py, run in a single pass
// (the script uses filtfilt, which needs the whole recording). The device does not
// compute their coefficients: tools/gen_ppg_coefs.py reads these four values and
// the sample rate above, and writes lib/ppg/ppg_coefs.h. Run it again after
// changing any of them.
#define FILTER_ORDER 4             // order of each filter (the band-pass has twice the poles)
#define BAND_LOW_HZ 0.5f           // band-pass for the AC component
#define BAND_HIGH_HZ 4.0f
#define DC_CUTOFF_HZ 0.2f          // low-pass for the DC component

// A second, gentler band-pass only to DRAW the pulse on the TFT. The 0.5-4 Hz filter above
// is meant for measuring (RMS amplitude) and is too narrow to show the waveform: the
// dicrotic notch lives between 4 and 10 Hz. SpO2 and heart rate do not use this one.
#define VIEW_FILTER_ORDER 2
#define VIEW_BAND_LOW_HZ 0.5f
#define VIEW_BAND_HIGH_HZ 10.0f

// ---- Beat detection --------------------------------------------------------------
#define BEAT_ENV_TAU_S 2.0f        // decay of the peak envelope
#define BEAT_THRESHOLD_K 0.5f      // threshold = K * envelope
#define BEAT_MIN_AMP_PCT 0.01f     // ... but never below this % of the IR DC (rejects noise)
#define BEAT_REFRACTORY_S 0.3f

// ---- Heart rate -------------------------------------------------------------------
#define RR_MIN_S 0.3f              // 200 bpm
#define RR_MAX_S 2.0f              // 30 bpm
#define RR_MAX_DEVIATION 0.30f     // reject an RR this far from the current median
#define RR_MAX_REJECTS 3           // this many rejected RRs in a row: forget the history and start over
#define BPM_MEDIAN_N 5             // RRs kept; BPM = 60 / their median
#define BPM_VALID_MIN_RR 5
#define BPM_VALID_MAX_CV 0.15f

// ---- SpO2 ----------------------------------------------------------------------------
// R = (AC_red / DC_red) / (AC_ir / DC_ir), with AC the RMS of the band-passed signal
// and DC the mean of the low-passed one, both over the last SPO2_WINDOW_S seconds.
// SpO2 = spo2_table[round(R * 100)] (lib/ppg/spo2_table.h, Maxim reference table).
// UNCALIBRATED for this sensor/housing: values are indicative only.
#define SPO2_WINDOW_S 10           // whole seconds; the result is refreshed once per second.
                                   // Longer = steadier R but slower to follow a real change
                                   // (about this many seconds) and 1.6 KB of RAM per second.
#define SPO2_MIN_PI_PCT 0.02f      // AC RMS / DC (%) below this: no pulse to measure, SpO2 invalid
#define SPO2_MIN_VALID 80          // % : a reading below this is reported as not valid

// ---- Signal quality ---------------------------------------------------------------------
#define QUALITY_PI_GOOD_PCT 1.0f   // perfusion index (AC/DC, %) that counts as a good signal
#define QUALITY_CV_BAD 0.30f       // RR coefficient of variation that drives the score to 0
