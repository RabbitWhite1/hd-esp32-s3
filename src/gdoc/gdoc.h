// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Zhanghan Wang

#pragma once
#include <Arduino.h>
#include <time.h>

// Google Doc backend: fetches a link-shared document's plain-text export over
// HTTPS and caches its non-empty lines for display on the LCD. The doc URL is a
// hardcoded literal in gdoc.cpp (the doc must stay shared as "anyone with link").
void gdocUpdate();            // fetch + parse; updates the cached lines (call when Wi-Fi is up)
bool gdocOk();                // true if the most recent fetch succeeded
int gdocLineCount();          // number of cached non-empty lines
const char *gdocLine(int i);  // i-th cached line ("" if out of range)
const char *gdocTitle();      // document title (from the export filename); "" if unknown
time_t gdocAsOf();            // wall-clock time of the last successful fetch (0 = never)
