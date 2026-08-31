// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Zhanghan Wang

#include "battery.h"
#include <Arduino.h>
#include <math.h>

// VBAT is divided 3:1 into GPIO4 (see board ADC notes). 12 dB attenuation lets
// the ~1.4 V seen at the pin (4.2 V / 3) stay inside the ADC's full-scale range.
static const int BAT_ADC_PIN = 4;
static const float BAT_DIVIDER = 3.0f;
static const int BAT_SAMPLES = 16;  // average to suppress ADC noise

// Charging is inferred from the terminal voltage (no status pin exists). On
// battery the loaded terminal tops out near 4.05 V; a charger drives it up to
// ~4.25-4.35 V, so 4.15 V cleanly separates the two. Hysteresis keeps the state
// from flickering around the threshold.
static const float CHG_ENTER = 4.15f;  // above this -> a charger is pushing the cell
static const float CHG_EXIT = 4.08f;   // back below this -> running on the battery again

// The curve is steep across the discharge plateaus, so a raw 2.9 mV ADC wobble
// swings the percentage by a point or two. The percentage is therefore read off
// an exponentially-smoothed voltage rather than the instantaneous one. At one
// sample per SAMPLE_INTERVAL (10 s) this alpha is roughly a 10-sample average --
// a ~100 s time constant, far quicker than the battery actually moves. The
// reported/logged voltage stays raw so the log keeps its full resolution for a
// future refit.
static const float V_FILT_ALPHA = 0.18f;

static float voltage = NAN;  // live measured terminal voltage (raw)
static float vFilt = NAN;    // smoothed voltage the percentage is derived from
static int percent = -1;     // runtime remaining; held while charging
static bool charging = false;

// Voltage -> charge curve, FITTED from a measured full discharge of this
// device's own cells (2026-08-27, 4.00 V down to cutoff at 2.89 V over 17.6 h,
// 1058 one-minute samples logged by history.*).
//
// The percentage is deliberately *not* a textbook state-of-charge: each entry
// is the fraction of RUNTIME REMAINING at that voltage, so 50% means about half
// the hours left rather than half the coulombs. That is the question someone
// glancing at the indicator is actually asking, and it is what the log can
// measure directly -- it also absorbs any error in the 3:1 divider and the ADC
// calibration, since the same reading path produced the fit.
//
// Entries are a 5% grid (21 points), which lands mean error at 0.5% -- about
// 5 minutes over a 17.6 h run, comfortably inside the +-2.9 mV ADC noise that
// alone moves the result by ~1%. Note how little voltage separates the middle
// rows: the cell spends a quarter of its life between 3.90 and 3.95 V and
// another quarter between 3.75 and 3.80 V, so voltage carries very little
// information there. That flatness is physics, not a bad fit; it is why the
// reading is filtered before it reaches this table.
//
// Refit from a fresh run if the cells are replaced or age noticeably: pull
// battery_YYYY.csv, drop charging rows, label each sample with its true
// fraction of remaining runtime, and regenerate this grid. Entries run from
// full to empty.
struct VPoint {
  float v;
  int pct;
};
static const VPoint CURVE[] = {
 { 3.998f, 100 }, { 3.955f,  95 }, { 3.924f,  90 },
 { 3.918f,  85 }, { 3.912f,  80 }, { 3.902f,  75 },
 { 3.892f,  70 }, { 3.872f,  65 }, { 3.824f,  60 },
 { 3.781f,  55 }, { 3.767f,  50 }, { 3.759f,  45 },
 { 3.751f,  40 }, { 3.737f,  35 }, { 3.728f,  30 },
 { 3.703f,  25 }, { 3.677f,  20 }, { 3.639f,  15 },
 { 3.565f,  10 }, { 3.411f,   5 }, { 2.939f,   0 },
};
static const int CURVE_LEN = sizeof(CURVE) / sizeof(CURVE[0]);

static int voltageToPercent(float v) {
  if (v >= CURVE[0].v) return 100;
  if (v <= CURVE[CURVE_LEN - 1].v) return 0;
  for (int i = 1; i < CURVE_LEN; i++) {
    if (v >= CURVE[i].v) {
      const VPoint &hi = CURVE[i - 1];
      const VPoint &lo = CURVE[i];
      float f = (v - lo.v) / (hi.v - lo.v);  // 0..1 between the two points
      return lo.pct + (int)lroundf(f * (hi.pct - lo.pct));
    }
  }
  return 0;
}

void batteryUpdate() {
  // analogReadMilliVolts() applies the per-chip ADC calibration, so we get
  // millivolts at the pin directly rather than raw counts.
  uint32_t accum = 0;
  for (int i = 0; i < BAT_SAMPLES; i++) accum += analogReadMilliVolts(BAT_ADC_PIN);
  float pinV = (accum / (float)BAT_SAMPLES) / 1000.0f;
  voltage = pinV * BAT_DIVIDER;
  vFilt = isnan(vFilt) ? voltage : vFilt + V_FILT_ALPHA * (voltage - vFilt);

  if (!charging && voltage >= CHG_ENTER) charging = true;
  else if (charging && voltage <= CHG_EXIT) charging = false;

  // The percentage must reflect the battery, not the charger-inflated voltage,
  // so refresh it only while running on the battery. It is also seeded once on
  // the first sample so a device booted already-charging still shows something.
  if (!charging || percent < 0) percent = voltageToPercent(vFilt);
}

void batteryBegin() {
  analogSetPinAttenuation(BAT_ADC_PIN, ADC_11db);  // ~12 dB: full input range
  batteryUpdate();
}

float batteryVoltage() {
  return voltage;
}

int batteryPercent() {
  return percent;
}

bool batteryCharging() {
  return charging;
}
