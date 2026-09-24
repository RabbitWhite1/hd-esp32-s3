// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Zhanghan Wang

#include "claude_usage.h"
#include "../wifi_net/wifi_net.h"
#include "../config/config.h"  // refresh interval persisted in esp32.json
#include "../logging/logging.h"
#include "../netsync/sync_flag.h"
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
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
static const int MAX_ACCOUNTS = 8;
static const int MAX_ALIAS_LEN = 24;

struct Account {
  String alias;
  String orgId;
  String sessionKey;
  Usage live;
};

static Account accounts[MAX_ACCOUNTS];
static int accountCount = 0;
static Usage staged[MAX_ACCOUNTS];
static String stagedAlias[MAX_ACCOUNTS];
static int stagedCount = 0;
static SyncFlag pending;

static Usage emptyUsage() {
  Usage u = {false, NAN, NAN, 0};
  return u;
}

// Fetch one account. A failed request returns the previous figures marked stale,
// so a temporary outage does not erase the last useful reading.
static Usage fetchAccount(const Account &account) {
  Usage u = account.live;
  u.ok = false;
  if (account.sessionKey.length() == 0 || account.orgId.length() == 0) return u;

  WiFiClientSecure client;
  client.setCACert(ISRG_ROOT_X1);  // validate the chain: this request carries the sessionKey

  String url = "https://claude.ai/api/organizations/" + account.orgId + "/usage";

  HTTPClient http;
  if (!http.begin(client, url)) {
    logError("Claude usage [%s]: TLS begin failed (cert/clock/CA?)", account.alias.c_str());
    return u;
  }
  http.addHeader("Cookie", String("sessionKey=") + account.sessionKey);
  http.addHeader("Accept", "application/json");
  // Browser-like UA reduces the chance of being bounced by anti-bot filtering.
  http.setUserAgent("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36");

  int code = http.GET();
  if (code != 200) {
    logError("Claude usage [%s] HTTP %d", account.alias.c_str(), code);
    http.end();
    return u;
  }
  String payload = http.getString();
  http.end();

  JsonDocument doc;
  if (deserializeJson(doc, payload)) {
    logError("Claude usage [%s]: JSON parse failed", account.alias.c_str());
    return u;
  }
  u.fiveHour = doc["five_hour"]["utilization"] | NAN;
  u.sevenDay = doc["seven_day"]["utilization"] | NAN;
  u.ok = !isnan(u.fiveHour) || !isnan(u.sevenDay);
  u.asOf = u.ok ? time(nullptr) : account.live.asOf;
  if (u.ok)
    logInfo("Claude usage [%s]: 5h %.0f%%  7d %.0f%%", account.alias.c_str(),
            u.fiveHour, u.sevenDay);
  return u;
}

void claudeUsageFetch() {
  // Do not overwrite the staged batch until the loop task has consumed it.
  if (pending.isSet() || !wifiConnected() || accountCount == 0) return;
  stagedCount = accountCount;
  for (int i = 0; i < stagedCount; i++) {
    stagedAlias[i] = accounts[i].alias;
    staged[i] = fetchAccount(accounts[i]);
  }
  pending.set();
}

// Promote a staged result if one is waiting. Runs on the loop task, which is the
// only reader of `live`, so nothing here races the fetch.
bool claudeUsageCommit() {
  if (!pending.isSet()) return false;
  bool changed = false;
  for (int i = 0; i < stagedCount; i++) {
    for (int j = 0; j < accountCount; j++) {
      if (accounts[j].alias != stagedAlias[i]) continue;
      accounts[j].live = staged[i];
      changed = true;
      break;
    }
  }
  pending.clear();
  return changed;
}

// Convenience for the synchronous boot path and the web handlers, which run on
// the loop task and want the result straight away.
void claudeUsageUpdate() {
  claudeUsageFetch();
  claudeUsageCommit();
}

int claudeUsageAccountCount() {
  return accountCount;
}
int claudeUsageMaxAccounts() {
  return MAX_ACCOUNTS;
}
int claudeUsageDisplayIndex() {
  return accountCount > 0 ? 0 : -1;
}
const String &claudeUsageAlias(int idx) {
  static const String empty;
  return idx >= 0 && idx < accountCount ? accounts[idx].alias : empty;
}
bool claudeUsageOk(int idx) {
  return idx >= 0 && idx < accountCount && accounts[idx].live.ok;
}
float claudeFiveHour(int idx) {
  return idx >= 0 && idx < accountCount ? accounts[idx].live.fiveHour : NAN;
}
float claudeSevenDay(int idx) {
  return idx >= 0 && idx < accountCount ? accounts[idx].live.sevenDay : NAN;
}
time_t claudeUsageAsOf(int idx) {
  return idx >= 0 && idx < accountCount ? accounts[idx].live.asOf : 0;
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

static int findAlias(const String &alias, int except = -1) {
  for (int i = 0; i < accountCount; i++) {
    if (i != except && accounts[i].alias.equalsIgnoreCase(alias)) return i;
  }
  return -1;
}

static bool validAlias(String &alias, String &errorOut) {
  alias.trim();
  if (alias.length() == 0) {
    errorOut = "Alias is required";
    return false;
  }
  if ((int)alias.length() > MAX_ALIAS_LEN) {
    errorOut = "Alias must be 24 characters or fewer";
    return false;
  }
  return true;
}

bool claudeUsageSetAccount(const String &aliasIn, const String &orgIn,
                           const String &keyIn, String &errorOut) {
  String alias = aliasIn, org = orgIn, key = keyIn;
  if (!validAlias(alias, errorOut)) return false;
  org.trim();
  key.trim();
  int idx = findAlias(alias);
  if (idx < 0) {
    if (accountCount >= MAX_ACCOUNTS) {
      errorOut = "Claude account list is full";
      return false;
    }
    if (org.length() == 0 || key.length() == 0) {
      errorOut = "A new alias requires an org ID and session key";
      return false;
    }
    idx = accountCount++;
    accounts[idx].alias = alias;
    accounts[idx].orgId = org;
    accounts[idx].sessionKey = key;
    accounts[idx].live = emptyUsage();
  } else {
    if (org.length() > 0) accounts[idx].orgId = org;
    if (key.length() > 0) accounts[idx].sessionKey = key;
  }
  errorOut = "";
  return true;
}

bool claudeUsageSetFromCookie(const String &alias, const String &cookie,
                              String &errorOut) {
  String key = cookieValue(cookie, "sessionKey");
  if (key.length() == 0) {
    errorOut = "No sessionKey found in the pasted cookie";
    return false;
  }
  return claudeUsageSetAccount(alias, cookieValue(cookie, "lastActiveOrg"), key, errorOut);
}

bool claudeUsageRename(int idx, const String &aliasIn, String &errorOut) {
  String alias = aliasIn;
  if (idx < 0 || idx >= accountCount) {
    errorOut = "Unknown Claude account";
    return false;
  }
  if (!validAlias(alias, errorOut)) return false;
  if (findAlias(alias, idx) >= 0) {
    errorOut = "That Claude alias already exists";
    return false;
  }
  accounts[idx].alias = alias;
  errorOut = "";
  return true;
}

bool claudeUsageMoveToTop(int idx, String &errorOut) {
  if (idx < 0 || idx >= accountCount) {
    errorOut = "Unknown Claude account";
    return false;
  }
  Account pinned = accounts[idx];
  for (int i = idx; i > 0; i--) accounts[i] = accounts[i - 1];
  accounts[0] = pinned;
  errorOut = "";
  return true;
}

// Accounts are persisted in the shared config store. Keys live on the SD card
// in plaintext, accepted here because the device is physically trusted.
static const char *CLAUDE_ACCOUNTS_KEY = "claude_accounts";
static const char *CLAUDE_ORG_KEY = "claude_org";
static const char *CLAUDE_SESSION_KEY = "claude_key";

void claudeUsageLoad() {
  accountCount = 0;
  JsonArrayConst arr = configDoc()[CLAUDE_ACCOUNTS_KEY].as<JsonArrayConst>();
  for (JsonObjectConst o : arr) {
    if (accountCount >= MAX_ACCOUNTS) break;
    String alias = o["alias"] | "";
    String org = o["org"] | "";
    String key = o["key"] | "";
    String error;
    if (!claudeUsageSetAccount(alias, org, key, error))
      logWarn("Skipping invalid Claude account in config: %s", error.c_str());
  }

  // One-time migration from the old unnamed flat credential pair.
  String legacyKey = configGet(CLAUDE_SESSION_KEY);
  if (accountCount == 0 && legacyKey.length() > 0) {
    accounts[0].alias = "default";
    accounts[0].orgId = configGet(CLAUDE_ORG_KEY);
    accounts[0].sessionKey = legacyKey;
    accounts[0].live = emptyUsage();
    accountCount = 1;
    logInfo("Migrating legacy Claude credentials to alias 'default'");
    claudeUsageSave();
  }
  logInfo("Claude accounts loaded from config (%d)", accountCount);
}

bool claudeUsageSave() {
  JsonArray arr = configDoc()[CLAUDE_ACCOUNTS_KEY].to<JsonArray>();
  for (int i = 0; i < accountCount; i++) {
    JsonObject o = arr.add<JsonObject>();
    o["alias"] = accounts[i].alias;
    o["org"] = accounts[i].orgId;
    o["key"] = accounts[i].sessionKey;
  }
  JsonObject root = configDoc().as<JsonObject>();
  root.remove(CLAUDE_ORG_KEY);
  root.remove(CLAUDE_SESSION_KEY);
  bool ok = configSave();
  if (ok) logInfo("Claude accounts saved to config (%d)", accountCount);
  else logError("Claude accounts save failed (SD card?)");
  return ok;
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
