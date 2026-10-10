#include "fake_vitals.h"

#include <math.h>

static const float BASE_SPO2 = 97.5f;
static const float BASE_BPM = 74.0f;

// Uniform in (0, 1], from the hardware RNG (24 bits: enough, and never 0 for log()).
static float randUnit() { return (float)((esp_random() >> 8) + 1) / 16777216.0f; }

// Normal(0, sigma) by Box-Muller.
static float gauss(float sigma) {
  float u1 = randUnit(), u2 = randUnit();
  return sigma * sqrtf(-2.0f * logf(u1)) * cosf(2.0f * (float)M_PI * u2);
}

static int randRange(int lo, int hi) { return lo + (int)(esp_random() % (uint32_t)(hi - lo + 1)); }

static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// One normalized cardiac cycle (phase 0..1): systolic peak + dicrotic wave.
static float ppgPulse(float t) {
  float a = (t - 0.22f) / 0.07f;
  float b = (t - 0.52f) / 0.10f;
  return expf(-a * a) + 0.36f * expf(-b * b);
}

FakeVitals::FakeVitals(bool mixed, int fs, int fingerDelayTicks)
    : mixed_(mixed), fs_(fs), fingerTicksLeft_(fingerDelayTicks), spo2_(BASE_SPO2), bpm_(BASE_BPM) {}

void FakeVitals::advanceEpisode() {
  if (episode_.active) {
    if (episode_.ticksLeft > 1) episode_.ticksLeft--;
    else episode_.active = false;
  } else if (randUnit() < 0.05f) {
    episode_.active = true;
    episode_.ticksLeft = randRange(8, 15);
    if (randUnit() < 0.5f) {
      episode_.onSpo2 = true;
      episode_.target = (float)randRange(84, 94);
    } else {
      episode_.onSpo2 = false;
      episode_.target = (float)(randUnit() < 0.5f ? randRange(40, 58) : randRange(102, 140));
    }
  }
}

void FakeVitals::tick() {
  if (fingerTicksLeft_ > 0) fingerTicksLeft_--;
  else finger_ = true;
  if (!finger_) {
    quality_ = 0.0f;
    return;
  }
  if (mixed_) advanceEpisode();
  spo2_ = clampf(spo2_ + gauss(0.25f), 82.0f, 100.0f);
  bpm_ = clampf(bpm_ + gauss(1.1f), 38.0f, 165.0f);
  if (!mixed_) {  // go back towards the base value
    spo2_ += (BASE_SPO2 - spo2_) * 0.1f;
    bpm_ += (BASE_BPM - bpm_) * 0.1f;
  } else if (episode_.active) {
    float& v = episode_.onSpo2 ? spo2_ : bpm_;
    v += (episode_.target - v) * 0.18f;
  }
  quality_ = clampf(0.72f + gauss(0.08f), 0.5f, 0.98f);
}

void FakeVitals::ppgBatch(int16_t* out) {
  const float step = bpm_ / 60.0f / (float)fs_;
  const float noise = (1.0f - quality_) * 0.06f;
  const float gain = 0.55f + 0.45f * quality_;
  for (int i = 0; i < fs_; i++) {
    phase_ += step;
    if (phase_ >= 1.0f) {
      phase_ -= 1.0f;
      beatAmp_ = 1.0f + gauss(0.04f);
    }
    float v = (ppgPulse(phase_) - 0.28f) * beatAmp_ * gain;
    out[i] = (int16_t)lroundf((v + gauss(noise)) * 1000.0f);
  }
}
