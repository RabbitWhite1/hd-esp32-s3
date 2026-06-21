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

static float voltage = NAN;  // live measured terminal voltage
static int percent = -1;     // SoC; held while charging
static bool charging = false;

// Voltage -> charge curve for a single 18650 Li-ion cell, from a widely-cited
// SoC reference table. The discharge curve is flat in the middle, so a plain
// linear map reads badly; linear interpolation between these points follows the
// real shape. The table is open-circuit voltage, but we feed the measured
// (under-load) voltage straight in, so the reading runs slightly low while the
// device is busy. Entries run from full to empty.
struct VPoint {
  float v;
  int pct;
};
static const VPoint CURVE[] = {
  { 4.20f, 100 }, { 4.06f, 90 }, { 3.98f, 80 }, { 3.92f, 70 }, { 3.87f, 60 }, { 3.82f, 50 },
  { 3.79f, 40 },  { 3.77f, 30 }, { 3.74f, 20 }, { 3.68f, 10 }, { 3.45f, 5 },  { 3.00f, 0 },
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

  if (!charging && voltage >= CHG_ENTER) charging = true;
  else if (charging && voltage <= CHG_EXIT) charging = false;

  // The percentage must reflect the battery, not the charger-inflated voltage,
  // so refresh it only while running on the battery. It is also seeded once on
  // the first sample so a device booted already-charging still shows something.
  if (!charging || percent < 0) percent = voltageToPercent(voltage);
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
