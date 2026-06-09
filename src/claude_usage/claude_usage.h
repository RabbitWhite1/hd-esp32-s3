// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Zhanghan Wang

#pragma once
#include <Arduino.h>

// Claude usage backend: fetches the organization usage summary from claude.ai
// over HTTPS and caches the two headline utilization figures. The org id and
// session cookie are set at runtime (e.g. from the web UI), not hardcoded.
void claudeUsageUpdate();  // fetch + parse; updates the cached values (call when Wi-Fi is up)
bool claudeUsageOk();      // true if the most recent fetch succeeded
float claudeFiveHour();    // 5-hour-window utilization, percent (NAN if unknown)
float claudeSevenDay();    // 7-day-window utilization, percent (NAN if unknown)

// Credentials (held in RAM; cleared on reboot).
void claudeUsageSetOrgId(const String &orgId);
void claudeUsageSetSessionKey(const String &key);  // pass "" to keep the current key
const String &claudeUsageOrgId();
bool claudeUsageHasKey();  // true once a non-empty session key has been set
