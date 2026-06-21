// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Zhanghan Wang

#pragma once
#include <Arduino.h>
#include <time.h>

// Google Doc backend: fetches a link-shared document's plain-text export over
// HTTPS and caches its lines for display on the LCD. The doc URL has a default in
// gdoc.cpp but is overridable at runtime via the web UI (persisted to SD); the doc
// must stay shared as "anyone with the link".
void gdocUpdate();            // fetch + parse; updates the cached lines (call when Wi-Fi is up)
bool gdocOk();                // true if the most recent fetch succeeded
int gdocLineCount();          // number of cached non-empty lines
const char *gdocLine(int i);  // i-th cached line ("" if out of range)
const char *gdocTitle();      // document title (from the export filename); "" if unknown
time_t gdocAsOf();            // wall-clock time of the last successful fetch (0 = never)

// Doc URL configuration (overridable + persisted in config, esp32.json key gdoc_url).
void gdocSetUrl(const String &url);  // set the URL (a normal Docs link is reduced to the base doc URL; the txt-export suffix is added at fetch time)
const String &gdocUrl();             // current doc URL
void gdocLoadUrl();                  // load the URL from config (call after configBegin, before gdocUpdate)
void gdocSaveUrl();                  // persist the current URL to config (esp32.json)

// Auto-refresh interval, in minutes (backed by the shared esp32.json store).
int gdocIntervalMin();               // configured interval (>= 1; default 240)
void gdocSetIntervalMin(int minutes);  // clamp to >= 1, then persist to config
