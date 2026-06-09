// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Zhanghan Wang

#include "claude_usage.h"
#include "wifi_net.h"
#include "logging.h"
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <math.h>

// claude.ai organization id and the "sessionKey" cookie used to authenticate.
// Both are set at runtime (via the web UI), so no secret lives in the firmware/git.
// The org id defaults to a known value for convenience; the key starts blank, so
// until it is set fetches fail with HTTP 401. The key looks like "sk-ant-sid0X-..."
// and expires periodically (re-enter it on the web page when it does).
static String orgId = "";
static String sessionKey = "";

static bool ok = false;
static float fiveHour = NAN;
static float sevenDay = NAN;

void claudeUsageUpdate() {
  if (!wifiConnected()) return;
  if (sessionKey.length() == 0) return;  // not configured yet -> nothing to fetch
  WiFiClientSecure client;
  client.setInsecure();  // skip cert validation (same approach as the weather fetch)

  String url = "https://claude.ai/api/organizations/" + orgId + "/usage";

  HTTPClient http;
  if (!http.begin(client, url)) return;
  http.addHeader("Cookie", String("sessionKey=") + sessionKey);
  http.addHeader("Accept", "application/json");
  // Browser-like UA reduces the chance of being bounced by anti-bot filtering.
  http.setUserAgent("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36");

  int code = http.GET();
  if (code != 200) {
    logError("Claude usage HTTP %d", code);
    http.end();
    ok = false;
    return;
  }
  String payload = http.getString();
  http.end();

  JsonDocument doc;
  if (deserializeJson(doc, payload)) {
    logError("Claude usage JSON parse failed");
    ok = false;
    return;
  }
  fiveHour = doc["five_hour"]["utilization"] | NAN;
  sevenDay = doc["seven_day"]["utilization"] | NAN;
  ok = !isnan(fiveHour) || !isnan(sevenDay);
  if (ok) logInfo("Claude usage: 5h %.0f%%  7d %.0f%%", fiveHour, sevenDay);
}

bool claudeUsageOk() {
  return ok;
}
float claudeFiveHour() {
  return fiveHour;
}
float claudeSevenDay() {
  return sevenDay;
}

void claudeUsageSetOrgId(const String &id) {
  if (id.length() > 0) orgId = id;
}
void claudeUsageSetSessionKey(const String &key) {
  if (key.length() > 0) sessionKey = key;  // "" means "keep current key"
}
const String &claudeUsageOrgId() {
  return orgId;
}
bool claudeUsageHasKey() {
  return sessionKey.length() > 0;
}
