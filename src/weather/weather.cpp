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
  return cities[i].name;
}

// Initialize a City to a name + coordinates with "no reading yet" fields.
static void initCity(City &c, const char *name, float lat, float lon) {
  strncpy(c.name, name, sizeof(c.name) - 1);
  c.name[sizeof(c.name) - 1] = '\0';
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
// Open-Meteo's geocoding API (the first/most-relevant match). This is how the
// human-readable city name becomes the lat/lon the forecast API needs.
static bool geocode(const String &query, String &nameOut, String &countryOut, float &latOut, float &lonOut) {
  if (!wifiConnected()) return false;
  String q = query;
  q.trim();
  if (q.length() == 0) return false;
  // Minimal percent-encoding (spaces are the common case in city names).
  String enc;
  for (size_t i = 0; i < q.length(); i++) {
    char ch = q[i];
    if (ch == ' ') enc += "%20";
    else enc += ch;
  }
  WiFiClientSecure client;
  client.setInsecure();
  String url = "https://geocoding-api.open-meteo.com/v1/search?name=" + enc +
               "&count=1&language=en&format=json";
  HTTPClient http;
  if (!http.begin(client, url)) return false;
  int code = http.GET();
  if (code != 200) {
    logError("Geocode HTTP %d for %s", code, q.c_str());
    http.end();
    return false;
  }
  String payload = http.getString();
  http.end();
  JsonDocument doc;
  if (deserializeJson(doc, payload)) return false;
  JsonArray results = doc["results"].as<JsonArray>();
  if (results.isNull() || results.size() == 0) return false;
  JsonObject r = results[0];
  const char *nm = r["name"] | "";
  latOut = r["latitude"] | NAN;
  lonOut = r["longitude"] | NAN;
  if (strlen(nm) == 0 || isnan(latOut) || isnan(lonOut)) return false;
  nameOut = String(nm);
  countryOut = String(r["country"] | "");
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
  String nm, country;
  float lat, lon;
  if (!geocode(query, nm, country, lat, lon)) {
    resolvedOut = "no match - look up valid names at the link below";
    return false;
  }
  initCity(cities[cityCount], nm.c_str(), lat, lon);
  cityCount++;
  weatherSaveCities();
  fetchWeather(cities[cityCount - 1]);  // populate the reading right away
  resolvedOut = nm + (country.length() ? (", " + country) : String("")) +
                " (" + String(lat, 2) + ", " + String(lon, 2) + ")";
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

// Persisted as one "name<TAB>lat<TAB>lon" line per city.
static void weatherSaveCities() {
  String out;
  for (int i = 0; i < cityCount; i++) {
    out += cities[i].name;
    out += '\t';
    out += String(cities[i].lat, 4);
    out += '\t';
    out += String(cities[i].lon, 4);
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
    if (t1 > 0 && t2 > t1) {
      String nm = line.substring(0, t1);
      float lat = line.substring(t1 + 1, t2).toFloat();
      float lon = line.substring(t2 + 1).toFloat();
      nm.trim();
      if (nm.length()) initCity(cities[cityCount++], nm.c_str(), lat, lon);
    }
    if (nl < 0) break;
    start = nl + 1;
  }
  if (cityCount == 0) {
    // Seed the original defaults so a fresh device still shows weather.
    initCity(cities[cityCount++], "Sunnyvale", 37.37f, -122.04f);
    if (cityCount < MAX_CITIES) initCity(cities[cityCount++], "New York", 40.71f, -74.01f);
    logInfo("Cities: seeded defaults (%d)", cityCount);
  } else {
    logInfo("Cities loaded from SD (%d)", cityCount);
  }
}
