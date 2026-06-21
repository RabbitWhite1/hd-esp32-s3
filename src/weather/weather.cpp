// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Zhanghan Wang

#include "weather.h"
#include "../wifi_net/wifi_net.h"
#include "../sdcard/sdcard.h"  // persist the configured cities to /sdcard/cities.txt
#include "../logging/logging.h"
#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <math.h>

static const int MAX_CITIES = 2;  // the weather band on the LCD fits two rows

City cities[MAX_CITIES];
static int cityCount = 0;

int weatherCityCount() {
  return cityCount;
}
int weatherMaxCities() {
  return MAX_CITIES;
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

static void weatherSaveCities();  // defined below

static bool fetchWeather(City &c) {
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
  c.cur = doc["current"]["temperature_2m"] | NAN;
  c.code = doc["current"]["weather_code"] | -1;
  c.wind = doc["current"]["wind_speed_10m"] | 0.0f;
  c.hi = doc["daily"]["temperature_2m_max"][0] | NAN;
  c.lo = doc["daily"]["temperature_2m_min"][0] | NAN;
  c.ok = !isnan(c.cur);
  return c.ok;
}

void weatherUpdateAll() {
  for (int i = 0; i < cityCount; i++) fetchWeather(cities[i]);
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
  weatherSaveCities();
  fetchWeather(cities[cityCount - 1]);  // populate the reading right away
  resolvedOut = label + " (" + String(lat, 2) + ", " + String(lon, 2) + ")";
  logInfo("City added: %s -> %.2f,%.2f", nm.c_str(), lat, lon);
  return true;
}

bool weatherRemoveCity(int idx) {
  if (idx < 0 || idx >= cityCount) return false;
  for (int j = idx; j < cityCount - 1; j++) cities[j] = cities[j + 1];
  cityCount--;
  weatherSaveCities();
  logInfo("City %d removed (%d left)", idx, cityCount);
  return true;
}

// Persisted as one "name<TAB>lat<TAB>lon<TAB>label" line per city. (Files
// written before the label existed have only three fields and still load.)
static void weatherSaveCities() {
  String out;
  for (int i = 0; i < cityCount; i++) {
    out += cities[i].name;
    out += '\t';
    out += String(cities[i].lat, 4);
    out += '\t';
    out += String(cities[i].lon, 4);
    out += '\t';
    out += cities[i].label;
    out += '\n';
  }
  if (sdWriteText("cities.txt", out)) logInfo("Cities saved to SD (%d)", cityCount);
}

void weatherLoadCities() {
  cityCount = 0;
  String data = sdReadText("cities.txt");
  int start = 0;
  while (start < (int)data.length() && cityCount < MAX_CITIES) {
    int nl = data.indexOf('\n', start);
    String line = (nl < 0) ? data.substring(start) : data.substring(start, nl);
    int t1 = line.indexOf('\t');
    int t2 = (t1 >= 0) ? line.indexOf('\t', t1 + 1) : -1;
    int t3 = (t2 >= 0) ? line.indexOf('\t', t2 + 1) : -1;
    if (t1 > 0 && t2 > t1) {
      String nm = line.substring(0, t1);
      float lat = line.substring(t1 + 1, t2).toFloat();
      float lon = ((t3 >= 0) ? line.substring(t2 + 1, t3) : line.substring(t2 + 1)).toFloat();
      String label = (t3 >= 0) ? line.substring(t3 + 1) : String("");
      nm.trim();
      label.trim();
      if (nm.length()) initCity(cities[cityCount++], nm.c_str(), label.c_str(), lat, lon);
    }
    if (nl < 0) break;
    start = nl + 1;
  }
  if (cityCount == 0) {
    // Seed the original defaults so a fresh device still shows weather.
    initCity(cities[cityCount++], "Sunnyvale", "Sunnyvale, California, United States", 37.37f, -122.04f);
    if (cityCount < MAX_CITIES)
      initCity(cities[cityCount++], "New York", "New York, New York, United States", 40.71f, -74.01f);
    logInfo("Cities: seeded defaults (%d)", cityCount);
  } else {
    logInfo("Cities loaded from SD (%d)", cityCount);
  }
}
