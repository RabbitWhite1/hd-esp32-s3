#include "claude_usage.h"
#include "../wifi_net/wifi_net.h"
#include "../config/config.h"  // refresh interval persisted in esp32.conf
#include "../logging/logging.h"
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
static time_t asOf = 0;  // wall-clock time of the last successful fetch

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
  if (ok) {
    asOf = time(nullptr);
    logInfo("Claude usage: 5h %.0f%%  7d %.0f%%", fiveHour, sevenDay);
  }
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
time_t claudeUsageAsOf() {
  return asOf;
}
const String &claudeUsageOrgId() {
  return orgId;
}
const String &claudeUsageSessionKey() {
  return sessionKey;
}
bool claudeUsageHasKey() {
  return sessionKey.length() > 0;
}

static const char *CLAUDE_INTERVAL_KEY = "claude_refresh_min";
static const int CLAUDE_INTERVAL_DEFAULT = 30;

int claudeUsageIntervalMin() {
  long m = configGetInt(CLAUDE_INTERVAL_KEY, CLAUDE_INTERVAL_DEFAULT);
  return m < 1 ? 1 : (int)m;
}
void claudeUsageSetIntervalMin(int minutes) {
  if (minutes < 1) minutes = 1;
  configSetInt(CLAUDE_INTERVAL_KEY, minutes);
  configSave();
}
