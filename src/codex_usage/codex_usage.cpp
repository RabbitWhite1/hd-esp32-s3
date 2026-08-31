// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Zhanghan Wang

#include "codex_usage.h"
#include "../wifi_net/wifi_net.h"
#include "../config/config.h"  // token + refresh interval persisted in esp32.json
#include "../logging/logging.h"
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <atomic>
#include <math.h>

// Trust anchor for the HTTPS fetch. This request mails the OAuth access token, so
// (like the Claude fetch, and unlike the public weather/asset reads) the TLS chain
// MUST be validated. chatgpt.com's chain is:
//   leaf -> Google Trust Services WE1 -> GTS Root R4
// Pinning the self-signed root (valid to 2036-06-22) survives leaf/intermediate
// renewals; it only needs updating if OpenAI switches certificate authority.
// Verify with:
//   openssl s_client -connect chatgpt.com:443 -servername chatgpt.com -showcerts
// and re-download the root from https://pki.goog/repo/certs/gtsr4.pem if it moves.
static const char GTS_ROOT_R4[] PROGMEM = R"EOF(
-----BEGIN CERTIFICATE-----
MIICCTCCAY6gAwIBAgINAgPlwGjvYxqccpBQUjAKBggqhkjOPQQDAzBHMQswCQYD
VQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2VzIExMQzEUMBIG
A1UEAxMLR1RTIFJvb3QgUjQwHhcNMTYwNjIyMDAwMDAwWhcNMzYwNjIyMDAwMDAw
WjBHMQswCQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2Vz
IExMQzEUMBIGA1UEAxMLR1RTIFJvb3QgUjQwdjAQBgcqhkjOPQIBBgUrgQQAIgNi
AATzdHOnaItgrkO4NcWBMHtLSZ37wWHO5t5GvWvVYRg1rkDdc/eJkTBa6zzuhXyi
QHY7qca4R9gq55KRanPpsXI5nymfopjTX15YhmUPoYRlBtHci8nHc8iMai/lxKvR
HYqjQjBAMA4GA1UdDwEB/wQEAwIBhjAPBgNVHRMBAf8EBTADAQH/MB0GA1UdDgQW
BBSATNbrdP9JNqPV2Py1PsVq8JQdjDAKBggqhkjOPQQDAwNpADBmAjEA6ED/g94D
9J+uHXqnLrmvT/aDHQ4thQEd0dlq7A/Cr8deVl5c1RxYIigL9zC2L7F8AjEA8GE8
p/SgguMh1YQdc4acLa/KNJvxn7kjNuK8YAOdgLOaVsjh4rsUecrNIdSUtUlD
-----END CERTIFICATE-----
)EOF";

// The rate-limit snapshot the Codex CLI itself reads. The newer /api/codex/usage
// path 403s for at least some accounts, so the original wham path is used.
static const char *USAGE_URL = "https://chatgpt.com/backend-api/wham/usage";

// OAuth access token, relayed from the machine running the Codex CLI. Held in RAM
// and mirrored into esp32.json; empty until the first relay, in which case
// codexUsageUpdate() is a no-op rather than a guaranteed 401.
static String accessToken = "";
static time_t tokenExp = 0;  // "exp" claim of the token above (0 = unknown)

// Staged handoff between the fetch task and the loop task -- see the note in
// claude_usage.cpp. planType is a String, so it matters that only the loop task
// ever writes the copy the renderer reads: promoting by value in Commit() means
// codexPlanType()'s c_str() can never point at a buffer the fetch just freed.
struct Usage {
  bool ok;
  float primaryPct, secondaryPct;
  int primaryWinMin, secondaryWinMin;
  String planType;
  time_t asOf;  // wall-clock time of the last successful fetch
};
static Usage live = {false, NAN, NAN, 0, 0, "", 0};
static Usage staged = {false, NAN, NAN, 0, 0, "", 0};
static std::atomic<uint32_t> pending{0};

// A failed fetch stages "not ok" while keeping the last good numbers, so the UI
// shows a stale reading rather than blanking out.
static void stageFailure();
static const char *windowLabel(int minutes, int slot);  // defined below, used when logging a fetch

// Decode one base64url segment (RFC 4648 §5, unpadded) as used by JWTs. Returns
// "" if the input holds a character outside the alphabet.
static String b64UrlDecode(const String &in) {
  String out;
  out.reserve(in.length() * 3 / 4 + 4);
  uint32_t acc = 0;
  int bits = 0;
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    int v;
    if (c >= 'A' && c <= 'Z') v = c - 'A';
    else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
    else if (c >= '0' && c <= '9') v = c - '0' + 52;
    else if (c == '-') v = 62;
    else if (c == '_') v = 63;
    else if (c == '=') break;  // tolerate padding even though JWTs omit it
    else return "";
    acc = (acc << 6) | (uint32_t)v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out += (char)((acc >> bits) & 0xFF);
    }
  }
  return out;
}

// Pull the "exp" claim out of a JWT's payload segment. Knowing when the token
// dies lets the UI say "relay a fresh one" instead of just showing HTTP 401.
static time_t parseJwtExp(const String &jwt) {
  int dot1 = jwt.indexOf('.');
  if (dot1 < 0) return 0;
  int dot2 = jwt.indexOf('.', dot1 + 1);
  if (dot2 < 0) return 0;
  String payload = b64UrlDecode(jwt.substring(dot1 + 1, dot2));
  if (payload.length() == 0) return 0;
  JsonDocument filter;  // the payload carries a lot of claims; keep only "exp"
  filter["exp"] = true;
  JsonDocument doc;
  if (deserializeJson(doc, payload, DeserializationOption::Filter(filter))) return 0;
  return (time_t)(doc["exp"] | 0);
}

// Read one {used_percent, limit_window_seconds} window object. A null/absent
// window (Plus reports no secondary) leaves the outputs at NAN/0.
static void readWindow(JsonVariantConst win, float *pct, int *winMin) {
  *pct = NAN;
  *winMin = 0;
  if (win.isNull()) return;
  *pct = win["used_percent"] | NAN;
  long secs = win["limit_window_seconds"] | 0L;
  if (secs > 0) *winMin = (int)(secs / 60);
}

void codexUsageFetch() {
  // Do not overwrite the single staged slot until the loop task has consumed it.
  if (pending.load(std::memory_order_acquire)) return;

  if (!wifiConnected()) return;
  if (accessToken.length() == 0) return;  // no token relayed yet -> nothing to fetch
  if (codexTokenExpired()) {
    logWarn("Codex usage: access token expired -- re-run the relay "
            "(README section \"Codex usage relay\")");
    stageFailure();
    return;
  }
  WiFiClientSecure client;
  client.setCACert(GTS_ROOT_R4);  // validate the chain: this request carries the token

  HTTPClient http;
  if (!http.begin(client, USAGE_URL)) {
    logError("Codex usage: TLS begin failed (cert/clock/CA?)");
    return;
  }
  http.addHeader("Authorization", String("Bearer ") + accessToken);
  http.addHeader("Accept", "application/json");
  // Browser-like UA reduces the chance of being bounced by anti-bot filtering.
  http.setUserAgent("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36");

  int code = http.GET();
  if (code != 200) {
    logError("Codex usage HTTP %d", code);
    http.end();
    stageFailure();
    return;
  }
  String payload = http.getString();
  http.end();

  JsonDocument doc;
  if (deserializeJson(doc, payload)) {
    logError("Codex usage JSON parse failed");
    stageFailure();
    return;
  }
  JsonVariantConst rl = doc["rate_limit"];
  Usage u;
  readWindow(rl["primary_window"], &u.primaryPct, &u.primaryWinMin);
  readWindow(rl["secondary_window"], &u.secondaryPct, &u.secondaryWinMin);
  u.planType = doc["plan_type"] | "";
  u.ok = !isnan(u.primaryPct) || !isnan(u.secondaryPct);
  u.asOf = u.ok ? time(nullptr) : live.asOf;  // keep the old timestamp on a bad payload
  if (u.ok)
    logInfo("Codex usage (%s): %s %.0f%%  %s %.0f%%", u.planType.c_str(),
            windowLabel(u.primaryWinMin, 0), u.primaryPct, windowLabel(u.secondaryWinMin, 1),
            u.secondaryPct);
  else
    logWarn("Codex usage: no rate-limit windows in the response");
  staged = u;
  pending.store(1, std::memory_order_release);
}

static void stageFailure() {
  staged = live;
  staged.ok = false;
  pending.store(1, std::memory_order_release);
}

// Promote a staged result if one is waiting. Runs on the loop task, the only
// reader of `live`, so nothing here races the fetch.
bool codexUsageCommit() {
  if (!pending.load(std::memory_order_acquire)) return false;
  live = staged;
  pending.store(0, std::memory_order_release);
  return true;
}

// Convenience for the synchronous boot path and the web handlers, which run on
// the loop task and want the result straight away.
void codexUsageUpdate() {
  codexUsageFetch();
  codexUsageCommit();
}

bool codexUsageOk() {
  return live.ok;
}
float codexPrimaryPercent() {
  return live.primaryPct;
}
float codexSecondaryPercent() {
  return live.secondaryPct;
}
int codexPrimaryWindowMin() {
  return live.primaryWinMin;
}
int codexSecondaryWindowMin() {
  return live.secondaryWinMin;
}
time_t codexUsageAsOf() {
  return live.asOf;
}
const char *codexPlanType() {
  return live.planType.c_str();
}

// Compact window label for the LCD gauge: minutes below an hour, then hours, then
// days ("45m" / "5h" / "7d"). Two rotating buffers so both labels can be used in
// one printf/draw call.
static const char *windowLabel(int minutes, int slot) {
  static char buf[2][8];
  char *b = buf[slot & 1];
  if (minutes <= 0) snprintf(b, sizeof(buf[0]), "--");
  else if (minutes < 60) snprintf(b, sizeof(buf[0]), "%dm", minutes);
  else if (minutes < 24 * 60) snprintf(b, sizeof(buf[0]), "%dh", minutes / 60);
  else snprintf(b, sizeof(buf[0]), "%dd", minutes / (24 * 60));
  return b;
}

const char *codexPrimaryLabel() {
  return windowLabel(live.primaryWinMin, 0);
}
const char *codexSecondaryLabel() {
  return windowLabel(live.secondaryWinMin, 1);
}

// The token lives on the SD card in plaintext - acceptable here (as with the
// Claude session key) since the device is physically trusted.
static const char *CODEX_TOKEN_KEY = "codex_token";

void codexUsageSetToken(const String &token) {
  if (token.length() == 0) return;  // "" means "keep current token"
  accessToken = token;
  tokenExp = parseJwtExp(accessToken);
}

bool codexUsageHasToken() {
  return accessToken.length() > 0;
}

bool codexTokenMatches(const String &token) {
  return token.length() > 0 && token == accessToken;
}

time_t codexTokenExpiry() {
  return tokenExp;
}

bool codexTokenExpired() {
  time_t now = time(nullptr);
  // Before NTP sync the clock reads ~1970, which would call every token expired.
  if (tokenExp == 0 || now < 1600000000) return false;
  return now >= tokenExp;
}

void codexUsageLoad() {
  String t = configGet(CODEX_TOKEN_KEY);
  if (t.length() > 0) codexUsageSetToken(t);
  if (codexUsageHasToken())
    logInfo("Codex token loaded from config");
  else
    logInfo("Codex token not set -- set up the relay (README section \"Codex usage relay\") "
            "or paste a token in the web UI");
}

bool codexUsageSave() {
  configSet(CODEX_TOKEN_KEY, accessToken);
  bool saved = configSave();
  if (saved) logInfo("Codex token saved to config");
  else logError("Codex token save failed (SD card?)");
  return saved;
}

static const char *CODEX_INTERVAL_KEY = "codex_refresh_min";
static const int CODEX_INTERVAL_DEFAULT = 30;

int codexUsageIntervalMin() {
  long m = configGetInt(CODEX_INTERVAL_KEY, CODEX_INTERVAL_DEFAULT);
  return m < 1 ? 1 : (int)m;
}
bool codexUsageSetIntervalMin(int minutes) {
  if (minutes < 1) minutes = 1;
  configSetInt(CODEX_INTERVAL_KEY, minutes);
  return configSave();
}
