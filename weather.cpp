#include "weather.h"
#include "wifi_net.h"
#include "logging.h"
#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <math.h>

City cities[] = {
  { "Sunnyvale", 37.37f, -122.04f, NAN, NAN, NAN, false, -1, 0.0f },
  { "New York", 40.71f, -74.01f, NAN, NAN, NAN, false, -1, 0.0f },
};
const int NUM_CITIES = sizeof(cities) / sizeof(cities[0]);

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
  for (int i = 0; i < NUM_CITIES; i++) fetchWeather(cities[i]);
}
