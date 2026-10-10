// Simulated SpO2 / BPM / PPG, port of Vitals + ppg_pulse from tools/cloud/fake_device.py.
// Replaced by the real sensor pipeline when this is integrated into firmware/pulsox.
#pragma once
#include <Arduino.h>

class FakeVitals {
 public:
  // mixed = false: values hover around a healthy base. true: random episodes of low SpO2 /
  // abnormal heart rate, to see the app change state. The finger "settles" after fingerDelay ticks.
  FakeVitals(bool mixed, int fs, int fingerDelayTicks);

  void tick();  // advance one second

  bool finger() const { return finger_; }
  float spo2() const { return spo2_; }
  float bpm() const { return bpm_; }
  float quality() const { return quality_; }
  int fs() const { return fs_; }
  bool valid() const { return finger_ && quality_ > 0.45f; }

  // 1 s of PPG (fs integers, systolic peak up). Only meaningful with a finger on.
  // out must hold fs() entries.
  void ppgBatch(int16_t* out);

 private:
  struct Episode {
    bool active = false;
    bool onSpo2 = true;  // else on bpm
    float target = 0;
    int ticksLeft = 0;
  };
  void advanceEpisode();

  bool mixed_;
  int fs_;
  int fingerTicksLeft_;
  bool finger_ = false;
  float spo2_ = 97.5f, bpm_ = 74.0f, quality_ = 0.0f;
  float phase_ = 0.0f, beatAmp_ = 1.0f;
  Episode episode_;
};
