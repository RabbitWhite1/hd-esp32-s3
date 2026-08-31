// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Zhanghan Wang

#include "weather.h"
#include "../wifi_net/wifi_net.h"
#include "../config/config.h"  // persist the configured cities in esp32.json
#include "../logging/logging.h"
#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <math.h>

static const int MAX_CITIES = 16;   // how many cities may be configured
static const int SHOWN_CITIES = 2;  // how many top rows the LCD shows + we fetch

City cities[MAX_CITIES];
static int cityCount = 0;
static bool orderDirty = false;  // RAM order differs from the persisted one

int weatherCityCount() {
  return cityCount;
}
int weatherMaxCities() {
  return MAX_CITIES;
}
int weatherShownMax() {
  return SHOWN_CITIES;
}
bool weatherOrderDirty() {
  return orderDirty;
}
const char *weatherCityName(int i) {
  if (i < 0 || i >= cityCount) return "";
  return cities[i].label;
}

// Initialize a City to a name + coordinates with "no reading yet" fields. The
// LCD uses the short city name; the full "City, Region, Country" label (empty
// -> falls back to the name) is shown in the web UI list.
static void initCity(City &c, const char *name, const char *label, float lat, float lon) {
  strncpy(c.name, name, sizeof(c.name) - 1);
  c.name[sizeof(c.name) - 1] = '\0';
  const char *lbl = (label && label[0]) ? label : name;
  strncpy(c.label, lbl, sizeof(c.label) - 1);
  c.label[sizeof(c.label) - 1] = '\0';
  c.lat = lat;
  c.lon = lon;
  c.cur = c.hi = c.lo = NAN;
  c.ok = false;
  c.code = -1;
  c.wind = 0.0f;
}

// weatherSaveCities() is declared in weather.h (public, used by the web reorder).

// Staged handoff between the fetch task and the loop task -- see the note in
// claude_usage.cpp. Only the numeric forecast fields are fetched; a city's name
// and coordinates are written by initCity() on the loop task alone. Each slot
// records the name it was fetched for, so if the list is edited between the
// fetch and the commit the stale result is dropped rather than landing on
// whichever city inherited that index.
struct Forecast {
  char forName[32];
  float cur, hi, lo, wind;
  int code;
  bool ok;
};
static Forecast staged[SHOWN_CITIES];
static int stagedCount = 0;
static volatile bool pending = false;

static bool fetchWeather(const City &c, Forecast &f) {
  if (!wifiConnected()) return false;
  WiFiClientSecure client;
  client.setInsecure();
  char url[200];
  snprintf(url, sizeof(url),
           "https://api.open-meteo.com/v1/forecast?latitude=%.2f&longitude=%.2f"
           "&current=temperature_2m,weather_code,wind_speed_10m"
           "&daily=temperature_2m_max,temperature_2m_min"
           "&timezone=auto&forecast_days=1",
           c.lat, c.lon);
  HTTPClient http;
  if (!http.begin(client, url)) return false;
  int code = http.GET();
  if (code != 200) {
    logError("HTTP %d for %s", code, c.name);
    http.end();
    return false;
  }
  String payload = http.getString();
  http.end();
  JsonDocument doc;
  if (deserializeJson(doc, payload)) return false;
  f.cur = doc["current"]["temperature_2m"] | NAN;
  f.code = doc["current"]["weather_code"] | -1;
  f.wind = doc["current"]["wind_speed_10m"] | 0.0f;
  f.hi = doc["daily"]["temperature_2m_max"][0] | NAN;
  f.lo = doc["daily"]["temperature_2m_min"][0] | NAN;
  f.ok = !isnan(f.cur);
  return f.ok;
}

void weatherFetch() {
  // Only the top SHOWN_CITIES are displayed on the LCD, so only those are fetched.
  int n = cityCount < SHOWN_CITIES ? cityCount : SHOWN_CITIES;
  for (int i = 0; i < n; i++) {
    staged[i].ok = false;
    strncpy(staged[i].forName, cities[i].name, sizeof(staged[i].forName) - 1);
    staged[i].forName[sizeof(staged[i].forName) - 1] = '\0';
    fetchWeather(cities[i], staged[i]);
  }
  stagedCount = n;
  pending = true;
}

// Promote staged forecasts onto the cities they were actually fetched for. Runs
// on the loop task, which is also the only writer of the city list.
bool weatherCommit() {
  if (!pending) return false;
  bool changed = false;
  for (int i = 0; i < stagedCount && i < cityCount; i++) {
    if (!staged[i].ok) continue;
    if (strncmp(staged[i].forName, cities[i].name, sizeof(staged[i].forName)) != 0)
      continue;  // the list moved under us; drop rather than mislabel a reading
    cities[i].cur = staged[i].cur;
    cities[i].hi = staged[i].hi;
    cities[i].lo = staged[i].lo;
    cities[i].wind = staged[i].wind;
    cities[i].code = staged[i].code;
    cities[i].ok = true;
    changed = true;
  }
  pending = false;
  return changed;
}

// Convenience for the synchronous boot path and the web handlers, which run on
// the loop task and want the result straight away.
void weatherUpdateAll() {
  weatherFetch();
  weatherCommit();
}

// Resolve a free-text place name to a canonical name + coordinates via
// Open-Meteo's geocoding API. The API matches on the place name only, so a
// "City, Region, Country" query won't match directly: we split on commas, send
// the first part as the name, and use the remaining parts (region/state and/or
// country) to filter the candidate results. This is how the human-readable
// city name becomes the lat/lon the forecast API needs.
static bool geocode(const String &query, String &nameOut, String &adminOut, String &countryOut, float &latOut, float &lonOut) {
  if (!wifiConnected()) return false;
  String q = query;
  q.trim();
  if (q.length() == 0) return false;
  // Split "City, Region, Country" -> city (sent to the API) + filter terms.
  String cityPart;
  String filters[4];
  int filterCount = 0;
  for (int start = 0; start <= (int)q.length();) {
    int c = q.indexOf(',', start);
    String part = (c < 0) ? q.substring(start) : q.substring(start, c);
    part.trim();
    if (cityPart.length() == 0) cityPart = part;
    else if (part.length() && filterCount < 4) filters[filterCount++] = part;
    if (c < 0) break;
    start = c + 1;
  }
  if (cityPart.length() == 0) return false;
  // Minimal percent-encoding (spaces are the common case in city names).
  String enc;
  for (size_t i = 0; i < cityPart.length(); i++) {
    char ch = cityPart[i];
    if (ch == ' ') enc += "%20";
    else enc += ch;
  }
  WiFiClientSecure client;
  client.setInsecure();
  // Ask for several matches so the region/country filters can disambiguate.
  String url = "https://geocoding-api.open-meteo.com/v1/search?name=" + enc +
               "&count=10&language=en&format=json";
  HTTPClient http;
  if (!http.begin(client, url)) return false;
  int code = http.GET();
  if (code != 200) {
    logError("Geocode HTTP %d for %s", code, cityPart.c_str());
    http.end();
    return false;
  }
  String payload = http.getString();
  http.end();
  JsonDocument doc;
  if (deserializeJson(doc, payload)) return false;
  JsonArray results = doc["results"].as<JsonArray>();
  if (results.isNull() || results.size() == 0) return false;
  // Pick the first result where every filter term matches the country or an
  // administrative level (province/state/county). With no filters, that's just
  // the top (most-relevant) result.
  JsonObject best;
  for (JsonVariant v : results) {
    JsonObject r = v.as<JsonObject>();
    bool allMatch = true;
    for (int f = 0; f < filterCount; f++) {
      String needle = filters[f];
      needle.toLowerCase();
      String hay = String(r["country"] | "") + "\n" +
                   String(r["country_code"] | "") + "\n" +
                   String(r["admin1"] | "") + "\n" +
                   String(r["admin2"] | "") + "\n" +
                   String(r["admin3"] | "");
      hay.toLowerCase();
      if (hay.indexOf(needle) < 0) { allMatch = false; break; }
    }
    if (allMatch) { best = r; break; }
  }
  if (best.isNull()) return false;  // filters given but nothing matched
  const char *nm = best["name"] | "";
  latOut = best["latitude"] | NAN;
  lonOut = best["longitude"] | NAN;
  if (strlen(nm) == 0 || isnan(latOut) || isnan(lonOut)) return false;
  nameOut = String(nm);
  adminOut = String(best["admin1"] | "");
  countryOut = String(best["country"] | "");
  return true;
}

bool weatherAddCity(const String &query, String &resolvedOut) {
  if (cityCount >= MAX_CITIES) {
    resolvedOut = "city list is full";
    return false;
  }
  if (!wifiConnected()) {
    resolvedOut = "Wi-Fi not connected";
    return false;
  }
  String nm, admin, country;
  float lat, lon;
  if (!geocode(query, nm, admin, country, lat, lon)) {
    resolvedOut = "no match - look up valid names at the link below";
    return false;
  }
  String label = nm + (admin.length() ? (", " + admin) : String("")) +
                 (country.length() ? (", " + country) : String(""));
  initCity(cities[cityCount], nm.c_str(), label.c_str(), lat, lon);
  cityCount++;
  if (!weatherSaveCities()) {  // couldn't persist (e.g. no SD) -> roll back the add
    cityCount--;
    resolvedOut = "could not save (SD card?)";
    return false;
  }
  if (cityCount - 1 < SHOWN_CITIES)
    {
      Forecast f;  // direct populate: this runs on the loop task, no staging needed
      f.ok = false;
      if (fetchWeather(cities[cityCount - 1], f)) {
        City &c = cities[cityCount - 1];
        c.cur = f.cur;
        c.hi = f.hi;
        c.lo = f.lo;
        c.wind = f.wind;
        c.code = f.code;
        c.ok = true;
      }
    }
  resolvedOut = label + " (" + String(lat, 2) + ", " + String(lon, 2) + ")";
  logInfo("City added: %s -> %.2f,%.2f", nm.c_str(), lat, lon);
  return true;
}

bool weatherRemoveCity(int idx) {
  if (idx < 0 || idx >= cityCount) return false;
  for (int j = idx; j < cityCount - 1; j++) cities[j] = cities[j + 1];
  cityCount--;
  bool ok = weatherSaveCities();
  logInfo("City %d removed (%d left) -> %s", idx, cityCount, ok ? "saved" : "save failed");
  return ok;
}

// Reordering mirrors the Wi-Fi priority list: both move-to-top and drag-to-order
// change RAM only and mark the order dirty; the web "Save order" button persists.
bool weatherMoveCity(int idx, int dir) {
  int j = idx + dir;
  if (idx < 0 || idx >= cityCount || j < 0 || j >= cityCount) return false;
  City tmp = cities[idx];
  cities[idx] = cities[j];
  cities[j] = tmp;
  orderDirty = true;
  return true;  // RAM only; caller persists via weatherSaveCities()
}

bool weatherApplyOrder(const int *order, int count) {
  // order[] must be a permutation of 0..cityCount-1 (new position -> old index).
  if (count != cityCount) return false;
  bool seen[MAX_CITIES] = {false};
  for (int i = 0; i < count; i++) {
    int o = order[i];
    if (o < 0 || o >= cityCount || seen[o]) return false;  // out of range or duplicate
    seen[o] = true;
  }
  City reordered[MAX_CITIES];
  for (int i = 0; i < count; i++) reordered[i] = cities[order[i]];
  for (int i = 0; i < count; i++) cities[i] = reordered[i];
  orderDirty = true;
  return true;  // RAM only; caller persists via weatherSaveCities()
}

// Persisted as a "cities" JSON array in the shared config, one object per city
// ({name, lat, lon, label}).
static const char *CITIES_KEY = "cities";

bool weatherSaveCities() {
  JsonArray arr = configDoc()[CITIES_KEY].to<JsonArray>();  // replaces any existing array
  for (int i = 0; i < cityCount; i++) {
    JsonObject o = arr.add<JsonObject>();
    o["name"] = cities[i].name;
    o["lat"] = cities[i].lat;
    o["lon"] = cities[i].lon;
    o["label"] = cities[i].label;
  }
  bool ok = configSave();
  if (ok) {
    logInfo("Cities saved to config (%d)", cityCount);
    orderDirty = false;
  } else {
    logError("Cities save failed (SD card?)");
  }
  return ok;
}

void weatherLoadCities() {
  cityCount = 0;
  JsonArrayConst arr = configDoc()[CITIES_KEY].as<JsonArrayConst>();
  for (JsonObjectConst o : arr) {
    if (cityCount >= MAX_CITIES) break;
    const char *nm = o["name"] | "";
    float lat = o["lat"] | NAN;
    float lon = o["lon"] | NAN;
    const char *label = o["label"] | "";
    if (nm[0] && !isnan(lat) && !isnan(lon)) initCity(cities[cityCount++], nm, label, lat, lon);
  }
  if (cityCount == 0) {
    // No cities configured: seed the single default so a fresh device still shows
    // weather. On a freshly created config file, write it through to esp32.json.
    initCity(cities[cityCount++], "New York", "New York, New York, United States", 40.71f, -74.01f);
    if (configWasCreated()) weatherSaveCities();
    logInfo("Cities: seeded default (%d)", cityCount);
  } else {
    logInfo("Cities loaded from config (%d)", cityCount);
  }
}
