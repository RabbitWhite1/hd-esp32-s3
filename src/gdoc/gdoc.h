// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Zhanghan Wang

#pragma once
#include <Arduino.h>
#include <time.h>

// Google Doc backend: fetches a link-shared document's plain-text export over
// HTTPS and caches its lines for display on the LCD. No URL is baked into the
// firmware -- it is set at runtime via the web UI and persisted to SD, so the
// module simply no-ops until then; the doc must stay shared as "anyone with the
// link".
// Split so the network half runs on the background fetch task while the cached
// lines are only ever written by the loop task (see claude_usage.h). This module
// is why the split exists: gdocLine() returns a String's interior pointer, so a
// fetch rewriting lines[] under the renderer would be a use-after-free.
void gdocFetch();             // fetch + parse into a staging slot (call when Wi-Fi is up)
bool gdocCommit();            // loop task: promote the staged revision; true if it did
void gdocUpdate();            // Fetch + Commit, for callers already on the loop task
bool gdocOk();                // true if the most recent fetch succeeded
int gdocLineCount();          // number of cached non-empty lines
const char *gdocLine(int i);  // i-th cached line ("" if out of range)
const char *gdocTitle();      // document title (from the export filename); "" if unknown
time_t gdocAsOf();            // wall-clock time of the last successful fetch (0 = never)

// Pending diff: the added/edited lines of the newest revision, each as
// "<line number> <text>", accumulated across fetches until dismissed (deletions
// are not reported). Non-zero count is what makes the sketch show the doc-update
// popup. The first successful fetch after boot only seeds the snapshot, so it
// never reports a diff.
int gdocDiffCount();              // number of pending changed lines (0 = nothing to show)
const char *gdocDiffLine(int i);  // i-th changed line ("" if out of range)
bool gdocDiffTruncated();         // true if more changes arrived than the buffer holds
void gdocDiffClear();             // dismiss the pending diff (user closed the popup)

// Doc URL configuration (overridable + persisted in config, esp32.json key gdoc_url).
void gdocSetUrl(const String &url);  // set the URL (a normal Docs link is reduced to the base doc URL; the txt-export suffix is added at fetch time)
const String &gdocUrl();             // current doc URL
void gdocLoadUrl();                  // load the URL from config (call after configBegin, before gdocUpdate)
bool gdocSaveUrl();                  // persist the current URL to config (esp32.json); false on write failure

// Auto-refresh interval, in minutes (backed by the shared esp32.json store).
int gdocIntervalMin();               // configured interval (>= 1; default 60)
bool gdocSetIntervalMin(int minutes);  // clamp to >= 1, then persist to config; false on write failure
