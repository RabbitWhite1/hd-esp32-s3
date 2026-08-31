// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Zhanghan Wang

#pragma once
#include <Arduino.h>
#include <time.h>

// Codex usage backend: fetches the ChatGPT/Codex rate-limit snapshot over HTTPS
// and caches the headline utilization figures. Unlike claude_usage (a long-lived
// cookie pasted once), Codex authenticates with a short-lived OAuth access token
// that only the `codex` CLI can mint, so the token is *relayed* to the device by
// the machine running Codex (POST /codextoken from a cron job -- see README) and
// persisted in esp32.json. Tokens are good for ~10 days, so the panel keeps
// working while that machine is off.
//
// The API reports two rolling windows whose lengths depend on the plan (e.g. a
// 7-day primary with no secondary on Plus), so windows are exposed as generic
// primary/secondary pairs with their length, not as fixed "5h"/"7d" fields.
// Split so the network half runs on the background fetch task while the values
// the renderer reads are only ever written by the loop task (see claude_usage.h).
void codexUsageFetch();    // fetch + parse into a staging slot (call when Wi-Fi is up)
bool codexUsageCommit();   // loop task: promote a staged result; true if it did
void codexUsageUpdate();   // Fetch + Commit, for callers already on the loop task
bool codexUsageOk();       // true if the most recent fetch succeeded
float codexPrimaryPercent();     // primary window utilization, percent (NAN if unknown)
float codexSecondaryPercent();   // secondary window utilization, percent (NAN if unknown)
int codexPrimaryWindowMin();     // primary window length in minutes (0 = none reported)
int codexSecondaryWindowMin();   // secondary window length in minutes (0 = none reported)
const char *codexPrimaryLabel();    // short window label for the LCD, e.g. "5h" / "7d"
const char *codexSecondaryLabel();
const char *codexPlanType();     // plan reported by the API ("plus", "pro", ...; "" if unknown)
time_t codexUsageAsOf();         // wall-clock time of the last successful fetch (0 = never)

// Access token (relayed from the machine running the Codex CLI).
void codexUsageSetToken(const String &token);
bool codexUsageHasToken();
time_t codexTokenExpiry();   // token's JWT "exp" (0 = unknown/unparsable)
bool codexTokenExpired();    // true once exp has passed (false while the clock is unset)

// Persistence of the token in the shared config (esp32.json).
void codexUsageLoad();  // restore the token from config (call after configBegin)
bool codexUsageSave();  // write the current token to config + persist; false on write failure

// Auto-refresh interval, in minutes (backed by the shared esp32.json store).
int codexUsageIntervalMin();
bool codexUsageSetIntervalMin(int minutes);

// True if `token` is byte-identical to the one already stored. Lets the relay
// endpoint skip a redundant SD write when the CLI hasn't re-minted the token.
bool codexTokenMatches(const String &token);
