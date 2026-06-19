#pragma once

// Battery backend: reads the 18650 cell voltage from the ADC on GPIO4.
// The board feeds VBAT through a 3:1 divider into GPIO4, so the measured
// voltage is one third of the real cell voltage. Charge percentage is mapped
// from the resting voltage with a Li-ion discharge curve.
//
// This board exposes no charge-status / VBUS pin and no fuel-gauge IC, so
// charging is inferred from the terminal voltage: a charger drives it above any
// resting cell voltage. While charging the elevated voltage no longer maps to a
// true state-of-charge, so the percentage is held at its last off-charger value.
void batteryBegin();      // configure the ADC pin; takes one sample to prime the cache
void batteryUpdate();     // resample (averaged) and refresh the cached voltage/percent
float batteryVoltage();   // last measured terminal voltage in volts (the charging voltage while charging; NAN before first sample)
int batteryPercent();     // estimated charge 0..100, held while charging (-1 if unknown)
bool batteryCharging();   // true when the terminal voltage indicates a charger is attached
