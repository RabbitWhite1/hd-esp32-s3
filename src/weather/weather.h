// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Zhanghan Wang

#pragma once
#include <Arduino.h>

// Weather backend: per-city current temperature + daily hi/lo from Open-Meteo.
// The forecast API takes coordinates, so a city is added by name (resolved to
// lat/lon once via Open-Meteo's geocoding API) and the resolved coordinates are
// persisted in the shared config (esp32.json) as a "cities" array.
struct City {
  char name[32];   // city name only (drawn on the LCD, where width is tight)
  char label[48];  // "City, Region, Country" (shown in the web UI list)
  float lat, lon, cur, hi, lo;
  bool ok;
  int code;    // WMO weather-interpretation code for the current condition (-1 = unknown)
  float wind;  // current wind speed, km/h
};

extern City cities[];          // valid for indices [0, weatherCityCount())
int weatherCityCount();        // number of configured cities
int weatherMaxCities();        // capacity (how many cities may be configured)
int weatherShownMax();         // how many top cities the LCD shows + we fetch (2)
const char *weatherCityName(int i);  // full "City, Region, Country" label of city i ("" if out of range)

// Split so the network half runs on the background fetch task while cities[] is
// only ever written by the loop task (see claude_usage.h). Staged forecasts
// record the city name they were fetched for, so a list edit in between drops
// the result instead of mislabelling it.
void weatherFetch();      // fetch the top weatherShownMax() cities into staging
bool weatherCommit();     // loop task: promote staged forecasts; true if it did
void weatherUpdateAll();  // Fetch + Commit, for callers already on the loop task

// City configuration (geocode a name -> coordinates, persisted in config).
void weatherLoadCities();   // restore from config, or seed defaults (call after configBegin)
bool weatherAddCity(const String &query, String &resolvedOut);  // geocode + append + persist; resolvedOut gets a human-readable result/error
bool weatherRemoveCity(int idx);  // remove city idx + persist
bool weatherSaveCities();   // re-write the city list to config; false on write failure

// Priority ordering (top weatherShownMax() are shown/fetched), mirroring Wi-Fi:
// move/apply change RAM only + mark dirty; persist with weatherSaveCities().
bool weatherMoveCity(int idx, int dir);          // swap city idx with its neighbor (dir -1/+1)
bool weatherApplyOrder(const int *order, int count);  // reorder to a permutation (new pos -> old index)
bool weatherOrderDirty();                        // true while the RAM order isn't persisted yet
