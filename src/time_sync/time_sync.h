// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Zhanghan Wang

#pragma once
#include <Arduino.h>

// Time backend: NTP sync and dual-timezone formatting. The two zones (primary +
// secondary) are user-selectable from a built-in table and persisted to the SD
// card; defaults are Los Angeles (primary) / New York (secondary).
void timeBegin();                                 // start SNTP and wait briefly for sync
bool timeFormatDateTime(char *buf, size_t len);   // false until the clock is set

// Time-zone selection (for the web UI dropdowns + persistence).
int timeZoneCount();              // number of selectable zones in the table
const char *timeZoneLabel(int i); // "PST - Los Angeles" ("" if out of range)
int timePrimaryZone();            // index of the primary (main) zone
int timeSecondaryZone();          // index of the secondary (parenthesized) zone
void timeSetZones(int primary, int secondary);  // apply (out-of-range values are ignored)
void timeLoadZones();             // restore from config (call after configBegin, before timeBegin)
void timeSaveZones();             // persist the current selection to config (esp32.json)
