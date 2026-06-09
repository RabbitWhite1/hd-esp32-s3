// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Zhanghan Wang

#pragma once

// Weather backend: per-city current temperature + daily hi/lo from Open-Meteo.
struct City {
  const char *name;
  float lat, lon, cur, hi, lo;
  bool ok;
  int code;    // WMO weather-interpretation code for the current condition (-1 = unknown)
  float wind;  // current wind speed, km/h
};

extern City cities[];
extern const int NUM_CITIES;

void weatherUpdateAll();  // refresh every city; a city keeps its old fields on failure
