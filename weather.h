#pragma once

// Weather backend: per-city current temperature + daily hi/lo from Open-Meteo.
struct City {
  const char *name;
  float lat, lon, cur, hi, lo;
  bool ok;
};

extern City cities[];
extern const int NUM_CITIES;

void weatherUpdateAll();  // refresh every city; a city keeps its old fields on failure
