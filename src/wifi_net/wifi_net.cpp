// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Zhanghan Wang

#include "wifi_net.h"
#include "../sdcard/sdcard.h"
#include "../logging/logging.h"
#include <WiFi.h>
#include <ESPmDNS.h>

// No Wi-Fi network is baked into the firmware; the device connects only to its
// saved networks (loaded from the SD card / added via the web UI).
static const char *hostname = "esp32";  // advertised over mDNS as "esp32.local"

// ---------- saved networks (loaded from / saved to SD) ----------
struct WifiNet {
  String ssid;
  String pass;
};
static const int MAX_NETS = 16;
static WifiNet nets[MAX_NETS];
static int netCount = 0;
static String currentSsid = "";  // name of the network we are actually joined to

// Re-advertise the web UI over mDNS. end() first so a reconnect restarts cleanly.
static void startMdns() {
  MDNS.end();
  if (MDNS.begin(hostname)) {
    MDNS.addService("http", "tcp", 80);
    logInfo("mDNS started: %s.local", hostname);
  } else {
    logWarn("mDNS start failed");
  }
}

// Attempt to join one network, giving up after timeoutMs instead of blocking forever.
static bool tryConnect(const char *s, const char *p, uint32_t timeoutMs) {
  logInfo("WiFi try %s", s);
  WiFi.disconnect();
  WiFi.begin(s, p);
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) delay(200);
  if (WiFi.status() == WL_CONNECTED) {
    currentSsid = s;
    return true;
  }
  return false;
}

void wifiBegin() {
  WiFi.mode(WIFI_STA);
  bool connected = false;
  for (int i = 0; i < netCount && !connected; i++)
    connected = tryConnect(nets[i].ssid.c_str(), nets[i].pass.c_str(), 8000);
  if (!connected) {
    logError("WiFi: no known network joined");
    return;
  }
  logInfo("WiFi connected to %s, IP: %s", currentSsid.c_str(), WiFi.localIP().toString().c_str());
  startMdns();
}

void wifiEnsureConnected() {
  if (WiFi.status() == WL_CONNECTED) return;
  // Retry at most once every 30 s (this also rate-limits the "no known network
  // joined" log) instead of hammering wifiBegin() on every loop iteration.
  static uint32_t lastAttempt = 0;
  uint32_t now = millis();
  if (lastAttempt != 0 && now - lastAttempt < 30000) return;
  lastAttempt = now;
  wifiBegin();
}

bool wifiConnected() {
  return WiFi.status() == WL_CONNECTED;
}

const char *wifiSSID() {
  return currentSsid.length() ? currentSsid.c_str() : "";  // "" until a network is joined
}

const char *wifiHostname() {
  return hostname;
}

String wifiIP() {
  if (WiFi.status() != WL_CONNECTED) return String("");
  return WiFi.localIP().toString();
}

// ---------- saved-network persistence ----------
// File format: one "ssid\tpassword" line per network.
void wifiLoadNetworks() {
  netCount = 0;
  String data = sdReadText("wifi.txt");
  int start = 0;
  while (start < (int)data.length() && netCount < MAX_NETS) {
    int nl = data.indexOf('\n', start);
    String line = (nl < 0) ? data.substring(start) : data.substring(start, nl);
    line.replace("\r", "");
    int tab = line.indexOf('\t');
    if (tab > 0) {
      nets[netCount].ssid = line.substring(0, tab);
      nets[netCount].pass = line.substring(tab + 1);
      netCount++;
    }
    if (nl < 0) break;
    start = nl + 1;
  }
  logInfo("WiFi: loaded %d saved network(s)", netCount);
}

void wifiSaveNetworks() {
  String out;
  for (int i = 0; i < netCount; i++) {
    out += nets[i].ssid;
    out += '\t';
    out += nets[i].pass;
    out += '\n';
  }
  if (sdWriteText("wifi.txt", out)) logInfo("WiFi: saved %d network(s)", netCount);
}

// Insert or update one network by SSID and persist the list. Dedups: an existing
// SSID's password is overwritten in place; the oldest entry is dropped when full.
static void upsertNetwork(const String &s, const String &p) {
  int idx = -1;
  for (int i = 0; i < netCount; i++)
    if (nets[i].ssid == s) { idx = i; break; }
  if (idx < 0) idx = (netCount < MAX_NETS) ? netCount++ : MAX_NETS - 1;
  nets[idx].ssid = s;
  nets[idx].pass = p;
  wifiSaveNetworks();
}

bool wifiStoreNetwork(const String &s, const String &p) {
  if (s.length() == 0) return false;
  upsertNetwork(s, p);  // dedup by SSID; no connectivity test
  logInfo("WiFi network stored (unverified): %s", s.c_str());
  // Dump the whole file back so it's visible on the serial monitor.
  String all = sdReadText("wifi.txt");
  logInfo("wifi.txt now (%d bytes):\n%s", all.length(), all.c_str());
  return true;
}

bool wifiAddNetwork(const String &s, const String &p) {
  if (s.length() == 0) return false;
  // Verify it actually connects before trusting it. This leaves the current AP,
  // so the requesting browser may briefly lose the device until it rejoins.
  if (!tryConnect(s.c_str(), p.c_str(), 12000)) {
    logWarn("WiFi add rejected (can't connect): %s", s.c_str());
    wifiBegin();  // restore a known-good network now (bypassing the 30 s throttle)
    return false;
  }
  upsertNetwork(s, p);
  startMdns();  // re-advertise on the new connection
  logInfo("WiFi network saved + connected: %s", s.c_str());
  return true;
}

int wifiNetCount() {
  return netCount;
}

const char *wifiNetSSID(int i) {
  if (i < 0 || i >= netCount) return "";
  return nets[i].ssid.c_str();
}
