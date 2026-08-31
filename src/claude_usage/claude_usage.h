// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Zhanghan Wang

#pragma once
#include <Arduino.h>
#include <time.h>

// Claude usage backend: fetches the organization usage summary from claude.ai
// over HTTPS and caches the two headline utilization figures. The org id and
// session cookie are set at runtime (e.g. from the web UI), not hardcoded.
// Split so the network half can run on the background fetch task while the
// values the renderer reads are only ever written by the loop task:
// claudeUsageFetch() stages a result, claudeUsageCommit() promotes it. Neither
// needs a lock -- see the staging note in claude_usage.cpp.
void claudeUsageFetch();    // fetch + parse into a staging slot (call when Wi-Fi is up)
bool claudeUsageCommit();   // loop task: promote a staged result; true if it did
void claudeUsageUpdate();   // Fetch + Commit, for callers already on the loop task
bool claudeUsageOk();       // true if the most recent fetch succeeded
float claudeFiveHour();     // 5-hour-window utilization, percent (NAN if unknown)
float claudeSevenDay();     // 7-day-window utilization, percent (NAN if unknown)
time_t claudeUsageAsOf();   // wall-clock time of the last successful fetch (0 = never)

// Credentials (held in RAM; cleared on reboot).
void claudeUsageSetOrgId(const String &orgId);
void claudeUsageSetSessionKey(const String &key);  // pass "" to keep the current key
// Parse a whole browser "Cookie:" header string: pulls out the sessionKey value
// (and the org id from lastActiveOrg, if present). Returns true if a session key
// was found. Convenience for pasting the full cookie instead of the two fields.
bool claudeUsageSetFromCookie(const String &cookie);

// Persistence of the org id + session key in the shared config (esp32.json).
void claudeUsageLoad();  // restore org id + key from config (call after configBegin)
bool claudeUsageSave();  // write the current org id + key to config + persist; false on write failure
const String &claudeUsageOrgId();
const String &claudeUsageSessionKey();  // current session key ("" if unset)
bool claudeUsageHasKey();  // true once a non-empty session key has been set

// Auto-refresh interval, in minutes (backed by the shared esp32.json store).
int claudeUsageIntervalMin();             // configured interval (>= 1; default 30)
bool claudeUsageSetIntervalMin(int minutes);  // clamp to >= 1, then persist to config; false on write failure
