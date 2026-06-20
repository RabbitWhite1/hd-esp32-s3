#pragma once
#include <Arduino.h>
#include <time.h>

// Claude usage backend: fetches the organization usage summary from claude.ai
// over HTTPS and caches the two headline utilization figures. The org id and
// session cookie are set at runtime (e.g. from the web UI), not hardcoded.
void claudeUsageUpdate();   // fetch + parse; updates the cached values (call when Wi-Fi is up)
bool claudeUsageOk();       // true if the most recent fetch succeeded
float claudeFiveHour();     // 5-hour-window utilization, percent (NAN if unknown)
float claudeSevenDay();     // 7-day-window utilization, percent (NAN if unknown)
time_t claudeUsageAsOf();   // wall-clock time of the last successful fetch (0 = never)

// Credentials (held in RAM; cleared on reboot).
void claudeUsageSetOrgId(const String &orgId);
void claudeUsageSetSessionKey(const String &key);  // pass "" to keep the current key
const String &claudeUsageOrgId();
const String &claudeUsageSessionKey();  // current session key ("" if unset)
bool claudeUsageHasKey();  // true once a non-empty session key has been set

// Auto-refresh interval, in minutes (backed by the shared esp32.conf store).
int claudeUsageIntervalMin();             // configured interval (>= 1; default 30)
void claudeUsageSetIntervalMin(int minutes);  // clamp to >= 1, then persist to config
