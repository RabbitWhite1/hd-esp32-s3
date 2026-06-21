#include "wifi_net.h"
#include "../sdcard/sdcard.h"
#include "../logging/logging.h"
#include <WiFi.h>
#include <ESPmDNS.h>
#include <DNSServer.h>

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

static String statusMsg = "";           // transient footer line during connection attempts
static void (*redrawHook)() = nullptr;  // frontend redraw, called when statusMsg changes

void wifiSetRedrawHook(void (*fn)()) {
  redrawHook = fn;
}
const char *wifiStatus() {
  return statusMsg.c_str();
}
static void setStatus(const String &s) {
  statusMsg = s;
  if (redrawHook) redrawHook();  // push the new status to the LCD footer now
}

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

// ---------- first-time setup hotspot (SoftAP) ----------
// Open AP brought up only when no saved network can be joined, so a phone can
// connect and configure Wi-Fi. WIFI_AP_STA keeps the station side alive while the
// AP is up, so we can still scan and test-connect candidate networks.
static const char *AP_SSID = "h4d-esp32-setup";
static bool apMode = false;       // true while the setup AP is up
static uint32_t apStopAt = 0;     // when !=0, tear the AP down at this millis() (deferred so a reply can flush)
// Captive-portal DNS: while the AP is up this resolves EVERY hostname to our own
// IP, so the phone's connectivity probe lands on us and the OS auto-opens the
// "Sign in to network" sheet on the setup page.
static DNSServer dnsServer;
static IPAddress apIp;

bool wifiInSetupMode() {
  return apMode;
}
const char *wifiSetupApSsid() {
  return AP_SSID;
}
String wifiSetupApIp() {
  return apMode ? apIp.toString() : String("");
}

// The setup-AP footer line, kept in one place so the idempotent re-entry below
// can restore it after wifiBegin() clears statusMsg on each reconnect retry.
static void setSetupStatus() {
  setStatus(String("Setup: join '") + AP_SSID + "' -> " + apIp.toString());
}

void wifiStartSetupAP() {
  if (apMode) {
    setSetupStatus();  // already up; just refresh the footer (wifiBegin cleared it)
    return;
  }
  WiFi.mode(WIFI_AP_STA);
  bool ok = WiFi.softAP(AP_SSID);  // open network (no password) for easy first join
  apMode = true;
  apStopAt = 0;
  apIp = WiFi.softAPIP();
  dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
  dnsServer.start(53, "*", apIp);  // catch-all: every lookup -> us (captive portal)
  if (ok)
    logInfo("WiFi setup AP up: join '%s', then open http://%s/", AP_SSID, apIp.toString().c_str());
  else
    logError("WiFi setup AP failed to start");
  setSetupStatus();  // surface it on the LCD footer (rendered via wifiStatus())
}

static void stopSetupAP() {
  if (!apMode) return;
  dnsServer.stop();
  WiFi.softAPdisconnect(true);  // stop broadcasting + free the AP
  WiFi.mode(WIFI_STA);
  apMode = false;
  apStopAt = 0;
  logInfo("WiFi setup AP stopped (joined a network)");
}

void wifiRequestStopAP(uint32_t delayMs) {
  if (apMode) apStopAt = millis() + delayMs;
}

void wifiLoop() {
  if (apMode) dnsServer.processNextRequest();  // answer captive-portal lookups
  // Deferred AP teardown: once we've joined a real network we keep the AP up for
  // a short grace period so the setup page's "Connected" reply can reach the phone.
  if (apStopAt && (int32_t)(millis() - apStopAt) >= 0) {
    stopSetupAP();
    startMdns();  // re-advertise on the freshly joined network
    if (redrawHook) redrawHook();
  }
}

int wifiScan() {
  int n = WiFi.scanNetworks();  // synchronous; negative on failure
  return n < 0 ? 0 : n;
}
String wifiScanSSID(int i) {
  return WiFi.SSID(i);
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
  WiFi.mode(apMode ? WIFI_AP_STA : WIFI_STA);  // keep the setup AP alive while retrying
  bool connected = false;
  for (int i = 0; i < netCount && !connected; i++) {
    setStatus(String("Trying ") + nets[i].ssid);  // shown on the LCD footer
    connected = tryConnect(nets[i].ssid.c_str(), nets[i].pass.c_str(), 8000);
  }
  statusMsg = "";  // attempt phase over; footer reverts to joined/disconnected
  if (connected) {
    logInfo("WiFi connected to %s, IP: %s", currentSsid.c_str(), WiFi.localIP().toString().c_str());
    startMdns();
    if (apMode) wifiRequestStopAP(3000);  // got onto a real network; drop the setup AP shortly
  } else {
    // Only NOW — after every saved SSID/password has failed — do we expose the AP.
    logError("WiFi: no known network joined");
    wifiStartSetupAP();
  }
  if (redrawHook) redrawHook();  // reflect the final state immediately
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
// True once the priority order has been changed in RAM but not yet persisted to
// SD; cleared whenever the list is saved or (re)loaded.
static bool orderDirty = false;
bool wifiOrderDirty() {
  return orderDirty;
}

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
  orderDirty = false;
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
  orderDirty = false;
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
  if (apMode) wifiRequestStopAP(3000);  // configured via the setup AP; tear it down once the reply flushes
  logInfo("WiFi network saved + connected: %s", s.c_str());
  return true;
}

bool wifiRemoveNetwork(const String &s) {
  for (int i = 0; i < netCount; i++) {
    if (nets[i].ssid == s) {
      for (int j = i; j < netCount - 1; j++) nets[j] = nets[j + 1];
      netCount--;
      wifiSaveNetworks();
      logInfo("WiFi network removed: %s", s.c_str());
      return true;
    }
  }
  return false;
}

bool wifiMoveNetwork(int idx, int dir) {
  int j = idx + dir;
  if (idx < 0 || idx >= netCount || j < 0 || j >= netCount) return false;
  WifiNet tmp = nets[idx];
  nets[idx] = nets[j];
  nets[j] = tmp;
  orderDirty = true;
  return true;  // RAM only; caller persists via wifiSaveNetworks()
}

bool wifiApplyOrder(const int *order, int count) {
  // order[] must be a permutation of 0..netCount-1 (the new position -> old index).
  if (count != netCount) return false;
  bool seen[MAX_NETS] = {false};
  for (int i = 0; i < count; i++) {
    int o = order[i];
    if (o < 0 || o >= netCount || seen[o]) return false;  // out of range or duplicate
    seen[o] = true;
  }
  WifiNet reordered[MAX_NETS];
  for (int i = 0; i < count; i++) reordered[i] = nets[order[i]];
  for (int i = 0; i < count; i++) nets[i] = reordered[i];
  orderDirty = true;
  return true;  // RAM only; caller persists via wifiSaveNetworks()
}

int wifiNetCount() {
  return netCount;
}

const char *wifiNetSSID(int i) {
  if (i < 0 || i >= netCount) return "";
  return nets[i].ssid.c_str();
}
