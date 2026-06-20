#pragma once
#include <Arduino.h>

// Weather backend: per-city current temperature + daily hi/lo from Open-Meteo.
// The forecast API takes coordinates, so a city is added by name (resolved to
// lat/lon once via Open-Meteo's geocoding API) and the resolved coordinates are
// persisted to /sdcard/cities.txt.
struct City {
  char name[32];
  float lat, lon, cur, hi, lo;
  bool ok;
  int code;    // WMO weather-interpretation code for the current condition (-1 = unknown)
  float wind;  // current wind speed, km/h
};

extern City cities[];          // valid for indices [0, weatherCityCount())
int weatherCityCount();        // number of configured cities
int weatherMaxCities();        // capacity (the LCD weather band fits this many rows)
const char *weatherCityName(int i);  // name of city i ("" if out of range)

void weatherUpdateAll();  // refresh every city; a city keeps its old fields on failure

// City configuration (geocode a name -> coordinates, persisted to SD).
void weatherLoadCities();   // restore from /sdcard/cities.txt, or seed defaults (call after sdBegin)
bool weatherAddCity(const String &query, String &resolvedOut);  // geocode + append + persist; resolvedOut gets a human-readable result/error
bool weatherRemoveCity(int idx);  // remove city idx + persist
