#include "wifi_net.h"
#include "logging.h"
#include <WiFi.h>

// ---------- Wi-Fi credentials ----------
static const char *ssid = "2493-26APR03";
static const char *password = "greenG2493";

void wifiBegin() {
  logInfo("Connecting to %s", ssid);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
  }
  logInfo("WiFi connected, IP: %s", WiFi.localIP().toString().c_str());
}

void wifiEnsureConnected() {
  if (WiFi.status() != WL_CONNECTED) wifiBegin();
}

bool wifiConnected() {
  return WiFi.status() == WL_CONNECTED;
}

const char *wifiSSID() {
  return ssid;
}

String wifiIP() {
  if (WiFi.status() != WL_CONNECTED) return String("");
  return WiFi.localIP().toString();
}
