// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Zhanghan Wang

#include "ota.h"
#include "../wifi_net/wifi_net.h"
#include "../config/config.h"
#include "../logging/logging.h"
#include <ArduinoOTA.h>
#include <ESPmDNS.h>

static const int OTA_PORT = 3232;  // espota's default
// Optional password, read from the shared config (esp32.json). Empty -- the
// default -- leaves the port open to anyone on the LAN, same exposure as the
// unauthenticated web UI; set the key to require one.
static const char *OTA_PASS_KEY = "ota_pass";

static bool started = false;
static bool active = false;
static int percent = 0;
static int lastDrawnPct = -1;
static String status = "";
static void (*redrawHook)() = nullptr;

void otaSetRedrawHook(void (*fn)()) {
  redrawHook = fn;
}
bool otaActive() {
  return active;
}
int otaPercent() {
  return percent;
}
const char *otaStatus() {
  return status.c_str();
}

void otaBegin() {
  if (started || !wifiConnected()) return;
  String pass = configGet(OTA_PASS_KEY);
  ArduinoOTA.setPort(OTA_PORT);
  ArduinoOTA.setHostname(wifiHostname());
  if (pass.length()) ArduinoOTA.setPassword(pass.c_str());
  // wifi_net owns the mDNS responder (and its _http._tcp record), so don't let
  // ArduinoOTA re-init it -- just add the _arduino._tcp service to what's running.
  ArduinoOTA.setMdnsEnabled(false);

  ArduinoOTA.onStart([]() {
    active = true;
    percent = 0;
    lastDrawnPct = -1;
    status = (ArduinoOTA.getCommand() == U_FLASH) ? "Updating firmware" : "Updating filesystem";
    logInfo("OTA start: %s", status.c_str());
    if (redrawHook) redrawHook();
  });
  ArduinoOTA.onProgress([](unsigned int done, unsigned int total) {
    percent = total ? (int)((uint64_t)done * 100 / total) : 0;
    // A redraw is a full-frame SPI push, so repaint every 5% rather than per packet.
    if (redrawHook && percent >= lastDrawnPct + 5) {
      lastDrawnPct = percent;
      redrawHook();
    }
  });
  ArduinoOTA.onEnd([]() {
    percent = 100;
    status = "Rebooting";
    logInfo("OTA complete -> rebooting into the new image");
    if (redrawHook) redrawHook();
  });
  ArduinoOTA.onError([](ota_error_t err) {
    active = false;
    switch (err) {
      case OTA_AUTH_ERROR: status = "OTA: auth failed"; break;
      case OTA_BEGIN_ERROR: status = "OTA: no room in the idle slot"; break;
      case OTA_CONNECT_ERROR: status = "OTA: connection failed"; break;
      case OTA_RECEIVE_ERROR: status = "OTA: receive failed"; break;
      case OTA_END_ERROR: status = "OTA: finalize failed"; break;
      default: status = "OTA: failed"; break;
    }
    logError("%s (code %d)", status.c_str(), (int)err);
    if (redrawHook) redrawHook();
  });

  ArduinoOTA.begin();
  MDNS.enableArduino(OTA_PORT, pass.length() > 0);  // advertise so `-p esp32.local` resolves
  started = true;
  logInfo("OTA ready at %s.local:%d (%s)", wifiHostname(), OTA_PORT,
          pass.length() ? "password set" : "no password");
}

void otaHandle() {
  if (!started) {
    otaBegin();  // Wi-Fi may have come up after setup() (e.g. first-time AP setup)
    return;
  }
  ArduinoOTA.handle();
}
