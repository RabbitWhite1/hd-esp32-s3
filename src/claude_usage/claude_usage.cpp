// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Zhanghan Wang

#include "claude_usage.h"
#include "../wifi_net/wifi_net.h"
#include "../config/config.h"  // refresh interval persisted in esp32.json
#include "../logging/logging.h"
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <atomic>
#include <math.h>

// Trust anchor for the HTTPS fetch. This request mails the sessionKey cookie, so
// (unlike the public weather/asset reads) the TLS connection MUST be validated --
// otherwise a man-in-the-middle could accept any cert and read the credential.
// claude.ai's chain is: leaf -> Let's Encrypt E8 -> ISRG Root X1. Pinning the
// long-lived root (valid to 2035) survives leaf/intermediate renewals; it only
// needs updating if Anthropic ever switches certificate authority. Verify with:
//   openssl s_client -connect claude.ai:443 -servername claude.ai -showcerts
static const char ISRG_ROOT_X1[] PROGMEM = R"EOF(
-----BEGIN CERTIFICATE-----
MIIFazCCA1OgAwIBAgIRAIIQz7DSQONZRGPgu2OCiwAwDQYJKoZIhvcNAQELBQAw
TzELMAkGA1UEBhMCVVMxKTAnBgNVBAoTIEludGVybmV0IFNlY3VyaXR5IFJlc2Vh
cmNoIEdyb3VwMRUwEwYDVQQDEwxJU1JHIFJvb3QgWDEwHhcNMTUwNjA0MTEwNDM4
WhcNMzUwNjA0MTEwNDM4WjBPMQswCQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJu
ZXQgU2VjdXJpdHkgUmVzZWFyY2ggR3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBY
MTCCAiIwDQYJKoZIhvcNAQEBBQADggIPADCCAgoCggIBAK3oJHP0FDfzm54rVygc
h77ct984kIxuPOZXoHj3dcKi/vVqbvYATyjb3miGbESTtrFj/RQSa78f0uoxmyF+
0TM8ukj13Xnfs7j/EvEhmkvBioZxaUpmZmyPfjxwv60pIgbz5MDmgK7iS4+3mX6U
A5/TR5d8mUgjU+g4rk8Kb4Mu0UlXjIB0ttov0DiNewNwIRt18jA8+o+u3dpjq+sW
T8KOEUt+zwvo/7V3LvSye0rgTBIlDHCNAymg4VMk7BPZ7hm/ELNKjD+Jo2FR3qyH
B5T0Y3HsLuJvW5iB4YlcNHlsdu87kGJ55tukmi8mxdAQ4Q7e2RCOFvu396j3x+UC
B5iPNgiV5+I3lg02dZ77DnKxHZu8A/lJBdiB3QW0KtZB6awBdpUKD9jf1b0SHzUv
KBds0pjBqAlkd25HN7rOrFleaJ1/ctaJxQZBKT5ZPt0m9STJEadao0xAH0ahmbWn
OlFuhjuefXKnEgV4We0+UXgVCwOPjdAvBbI+e0ocS3MFEvzG6uBQE3xDk3SzynTn
jh8BCNAw1FtxNrQHusEwMFxIt4I7mKZ9YIqioymCzLq9gwQbooMDQaHWBfEbwrbw
qHyGO0aoSCqI3Haadr8faqU9GY/rOPNk3sgrDQoo//fb4hVC1CLQJ13hef4Y53CI
rU7m2Ys6xt0nUW7/vGT1M0NPAgMBAAGjQjBAMA4GA1UdDwEB/wQEAwIBBjAPBgNV
HRMBAf8EBTADAQH/MB0GA1UdDgQWBBR5tFnme7bl5AFzgAiIyBpY9umbbjANBgkq
hkiG9w0BAQsFAAOCAgEAVR9YqbyyqFDQDLHYGmkgJykIrGF1XIpu+ILlaS/V9lZL
ubhzEFnTIZd+50xx+7LSYK05qAvqFyFWhfFQDlnrzuBZ6brJFe+GnY+EgPbk6ZGQ
3BebYhtF8GaV0nxvwuo77x/Py9auJ/GpsMiu/X1+mvoiBOv/2X/qkSsisRcOj/KK
NFtY2PwByVS5uCbMiogziUwthDyC3+6WVwW6LLv3xLfHTjuCvjHIInNzktHCgKQ5
ORAzI4JMPJ+GslWYHb4phowim57iaztXOoJwTdwJx4nLCgdNbOhdjsnvzqvHu7Ur
TkXWStAmzOVyyghqpZXjFaH3pO3JLF+l+/+sKAIuvtd7u+Nxe5AW0wdeRlN8NwdC
jNPElpzVmbUq4JUagEiuTDkHzsxHpFKVK7q4+63SM1N95R1NbdWhscdCb+ZAJzVc
oyi3B43njTOQ5yOf+1CceWxG1bQVs5ZufpsMljq4Ui0/1lvh+wjChP4kqKOJ2qxq
4RgqsahDYVvTH9w7jXbyLeiNdd8XM2w9U/t7y0Ff/9yi0GE44Za4rF2LN9d11TPA
mRGunUHBcnWEvgJBQl9nJEiU0Zsnvgc/ubhPgXRR4Xq37Z0j4r7g1SgEEzwxA57d
emyPxgcYxn/eR44/KJ4EBs+lVDR3veyJm+kXQ99b21/+jh5Xos1AnX5iItreGCc=
-----END CERTIFICATE-----
)EOF";

// claude.ai organization id and the "sessionKey" cookie used to authenticate.
// Both are set at runtime (via the web UI), so no secret lives in the firmware/git.
// Both start blank, so until they are set fetches fail with HTTP 401. The key looks like "sk-ant-sid0X-..."
// and expires periodically (re-enter it on the web page when it does).
static String orgId = "";
static String sessionKey = "";

// Fetched values are handed from the fetch task to the loop task by staging,
// not by sharing: claudeUsageFetch() writes only `staged`, and the loop task
// promotes it into `live` in claudeUsageCommit(). Getters read `live`, so every
// value the renderer sees is written by the thread that reads it. `pending` is
// the whole protocol -- producer sets it, consumer clears it, and the producer
// never restages while it is set, so neither side needs a lock.
struct Usage {
  bool ok;
  float fiveHour, sevenDay;
  time_t asOf;  // wall-clock time of the last successful fetch
};
static Usage live = {false, NAN, NAN, 0};
static Usage staged = {false, NAN, NAN, 0};
static std::atomic<uint32_t> pending{0};

// A failed fetch stages "not ok" while keeping the last good numbers, so the UI
// shows a stale reading rather than blanking out.
static void stageFailure();

void claudeUsageFetch() {
  // Do not overwrite the single staged slot until the loop task has consumed it.
  if (pending.load(std::memory_order_acquire)) return;

  if (!wifiConnected()) return;
  if (sessionKey.length() == 0) return;  // not configured yet -> nothing to fetch
  WiFiClientSecure client;
  client.setCACert(ISRG_ROOT_X1);  // validate the chain: this request carries the sessionKey

  String url = "https://claude.ai/api/organizations/" + orgId + "/usage";

  HTTPClient http;
  if (!http.begin(client, url)) {
    logError("Claude usage: TLS begin failed (cert/clock/CA?)");
    return;
  }
  http.addHeader("Cookie", String("sessionKey=") + sessionKey);
  http.addHeader("Accept", "application/json");
  // Browser-like UA reduces the chance of being bounced by anti-bot filtering.
  http.setUserAgent("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36");

  int code = http.GET();
  if (code != 200) {
    logError("Claude usage HTTP %d", code);
    http.end();
    stageFailure();
    return;
  }
  String payload = http.getString();
  http.end();

  JsonDocument doc;
  if (deserializeJson(doc, payload)) {
    logError("Claude usage JSON parse failed");
    stageFailure();
    return;
  }
  Usage u;
  u.fiveHour = doc["five_hour"]["utilization"] | NAN;
  u.sevenDay = doc["seven_day"]["utilization"] | NAN;
  u.ok = !isnan(u.fiveHour) || !isnan(u.sevenDay);
  u.asOf = u.ok ? time(nullptr) : live.asOf;  // keep the old timestamp on a bad payload
  if (u.ok) logInfo("Claude usage: 5h %.0f%%  7d %.0f%%", u.fiveHour, u.sevenDay);
  staged = u;
  pending.store(1, std::memory_order_release);
}

static void stageFailure() {
  staged = live;
  staged.ok = false;
  pending.store(1, std::memory_order_release);
}

// Promote a staged result if one is waiting. Runs on the loop task, which is the
// only reader of `live`, so nothing here races the fetch.
bool claudeUsageCommit() {
  if (!pending.load(std::memory_order_acquire)) return false;
  live = staged;
  pending.store(0, std::memory_order_release);
  return true;
}

// Convenience for the synchronous boot path and the web handlers, which run on
// the loop task and want the result straight away.
void claudeUsageUpdate() {
  claudeUsageFetch();
  claudeUsageCommit();
}

bool claudeUsageOk() {
  return live.ok;
}
float claudeFiveHour() {
  return live.fiveHour;
}
float claudeSevenDay() {
  return live.sevenDay;
}

void claudeUsageSetOrgId(const String &id) {
  if (id.length() > 0) orgId = id;
}
void claudeUsageSetSessionKey(const String &key) {
  if (key.length() > 0) sessionKey = key;  // "" means "keep current key"
}

// Extract the value of cookie `name` from a "a=b; c=d; ..." string. Matches only
// at a token boundary (start, or after "; "), so searching "sessionKey" never
// hits "sessionKeyLC"; the value runs to the next ';' or end of string.
static String cookieValue(const String &cookie, const char *name) {
  String key = String(name) + "=";
  int from = 0;
  while (from <= (int)cookie.length()) {
    int idx = cookie.indexOf(key, from);
    if (idx < 0) return "";
    bool boundary = (idx == 0) || cookie[idx - 1] == ';' || cookie[idx - 1] == ' ';
    if (boundary) {
      int valStart = idx + key.length();
      int end = cookie.indexOf(';', valStart);
      String v = (end < 0) ? cookie.substring(valStart) : cookie.substring(valStart, end);
      v.trim();
      return v;
    }
    from = idx + key.length();
  }
  return "";
}

bool claudeUsageSetFromCookie(const String &cookie) {
  String org = cookieValue(cookie, "lastActiveOrg");
  if (org.length() > 0) orgId = org;
  String key = cookieValue(cookie, "sessionKey");
  if (key.length() > 0) {
    sessionKey = key;
    return true;
  }
  return false;
}

// Org id + session key are persisted in the shared config store (esp32.json).
// The key lives on the SD card in plaintext - acceptable here since the device
// is physically trusted.
static const char *CLAUDE_ORG_KEY = "claude_org";
static const char *CLAUDE_SESSION_KEY = "claude_key";

void claudeUsageLoad() {
  String o = configGet(CLAUDE_ORG_KEY);
  String k = configGet(CLAUDE_SESSION_KEY);
  if (o.length() > 0) orgId = o;
  if (k.length() > 0) sessionKey = k;
  logInfo("Claude creds loaded from config (%s)", claudeUsageHasKey() ? "key set" : "no key");
}

bool claudeUsageSave() {
  configSet(CLAUDE_ORG_KEY, orgId);
  configSet(CLAUDE_SESSION_KEY, sessionKey);
  bool ok = configSave();
  if (ok) logInfo("Claude creds saved to config");
  else logError("Claude creds save failed (SD card?)");
  return ok;
}
time_t claudeUsageAsOf() {
  return live.asOf;
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
bool claudeUsageSetIntervalMin(int minutes) {
  if (minutes < 1) minutes = 1;
  configSetInt(CLAUDE_INTERVAL_KEY, minutes);
  return configSave();
}
