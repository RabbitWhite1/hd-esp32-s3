// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Zhanghan Wang

#pragma once
#include <Arduino.h>
#include <time.h>

// Claude usage backend: fetches organization usage summaries from claude.ai
// over HTTPS and caches the two headline utilization figures for each configured
// account. Every account has a user-facing alias; session cookies are runtime
// settings (e.g. from the web UI), never hardcoded.
// Split so the network half can run on the background fetch task while the
// values the renderer reads are only ever written by the loop task:
// claudeUsageFetch() stages a result, claudeUsageCommit() promotes it. Neither
// needs a lock -- see the staging note in claude_usage.cpp.
void claudeUsageFetch();    // fetch + parse into a staging slot (call when Wi-Fi is up)
bool claudeUsageCommit();   // loop task: promote a staged result; true if it did
void claudeUsageUpdate();   // Fetch + Commit, for callers already on the loop task

// Per-account results. The LCD displays the first (pinned) account; the web UI
// lists every account. No API exposes a key.
int claudeUsageAccountCount();
int claudeUsageMaxAccounts();
int claudeUsageDisplayIndex();  // pinned index 0, or -1 when nothing is configured
const String &claudeUsageAlias(int idx);
bool claudeUsageOk(int idx);
float claudeFiveHour(int idx);
float claudeSevenDay(int idx);
time_t claudeUsageAsOf(int idx);

// Add or update an account. Alias is required and unique (case-insensitive).
// Blank org/key fields preserve those values on an existing alias, but a new
// account requires both. The cookie form extracts sessionKey and lastActiveOrg.
bool claudeUsageSetAccount(const String &alias, const String &orgId,
                           const String &key, String &errorOut);
bool claudeUsageSetFromCookie(const String &alias, const String &cookie,
                              String &errorOut);
bool claudeUsageRename(int idx, const String &alias, String &errorOut);
bool claudeUsageMoveToTop(int idx, String &errorOut);  // pin this account for LCD display

// Persistence in esp32.json. Legacy claude_org/claude_key values are migrated
// to a claude_accounts entry named "default" and removed on the next save.
void claudeUsageLoad();
bool claudeUsageSave();

// Auto-refresh interval, in minutes (backed by the shared esp32.json store).
int claudeUsageIntervalMin();             // configured interval (>= 1; default 30)
bool claudeUsageSetIntervalMin(int minutes);  // clamp to >= 1, then persist to config; false on write failure
