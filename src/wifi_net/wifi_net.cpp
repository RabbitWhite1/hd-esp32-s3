#include "wifi_net.h"
#include "../logging/logging.h"
#include <WiFi.h>
#include <ESPmDNS.h>

// ---------- Wi-Fi credentials ----------
static const char *ssid = "2493-26APR03";
static const char *password = "greenG2493";
static const char *hostname = "esp32";  // advertised over mDNS as "esp32.local"

void wifiBegin() {
  logInfo("Connecting to %s", ssid);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
  }
  logInfo("WiFi connected, IP: %s", WiFi.localIP().toString().c_str());

  // Advertise <hostname>.local via mDNS. end() first so a reconnect restarts cleanly.
  MDNS.end();
  if (MDNS.begin(hostname)) {
    MDNS.addService("http", "tcp", 80);  // discoverable web UI
    logInfo("mDNS started: %s.local", hostname);
  } else {
    logWarn("mDNS start failed");
  }
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

const char *wifiHostname() {
  return hostname;
}

String wifiIP() {
  if (WiFi.status() != WL_CONNECTED) return String("");
  return WiFi.localIP().toString();
}
