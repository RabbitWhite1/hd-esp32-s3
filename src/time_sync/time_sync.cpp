// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Zhanghan Wang

#include "time_sync.h"
#include "../sdcard/sdcard.h"  // persist the zone selection to /sdcard/tz.txt
#include "../logging/logging.h"
#include <time.h>

// Selectable time zones: a POSIX TZ string (drives offset + DST + the %Z
// abbreviation shown on the LCD) paired with a human label for the web UI
// dropdowns (abbreviation + a famous city in that zone).
struct Zone {
  const char *tz;     // POSIX TZ string
  const char *label;  // "ABBR - City"
};

// Sorted by UTC offset (the dropdown order); the label leads with the standard
// (non-DST) UTC offset. Persistence keys on the POSIX TZ string, not the index,
// so this list can be reordered freely.
static const Zone ZONES[] = {
  {"HST10",                          "UTC-10 HST - Honolulu"},
  {"AKST9AKDT,M3.2.0,M11.1.0",       "UTC-9 AKST/AKDT - Anchorage"},
  {"PST8PDT,M3.2.0,M11.1.0",         "UTC-8 PST/PDT - Los Angeles"},
  {"MST7MDT,M3.2.0,M11.1.0",         "UTC-7 MST/MDT - Denver"},
  {"MST7",                           "UTC-7 MST - Phoenix"},
  {"CST6CDT,M3.2.0,M11.1.0",         "UTC-6 CST/CDT - Chicago"},
  {"EST5EDT,M3.2.0,M11.1.0",         "UTC-5 EST/EDT - New York"},
  {"<-03>3",                         "UTC-3 BRT - Sao Paulo"},
  {"GMT0BST,M3.5.0/1,M10.5.0",       "UTC+0 GMT - London"},
  {"CET-1CEST,M3.5.0,M10.5.0/3",     "UTC+1 CET - Paris"},
  {"EET-2EEST,M3.5.0/3,M10.5.0/4",   "UTC+2 EET - Athens"},
  {"MSK-3",                          "UTC+3 MSK - Moscow"},
  {"<+04>-4",                        "UTC+4 GST - Dubai"},
  {"IST-5:30",                       "UTC+5:30 IST - Kolkata"},
  {"<+07>-7",                        "UTC+7 ICT - Bangkok"},
  {"CST-8",                          "UTC+8 CST - Shanghai"},
  {"JST-9",                          "UTC+9 JST - Tokyo"},
  {"AEST-10AEDT,M10.1.0,M4.1.0/3",   "UTC+10 AEST - Sydney"},
  {"NZST-12NZDT,M9.5.0,M4.1.0/3",    "UTC+12 NZST - Auckland"},
};
static const int ZONE_COUNT = sizeof(ZONES) / sizeof(ZONES[0]);

// Defaults mirror the original hardcoded PST primary / EST secondary.
static const int DEFAULT_PRIMARY = 2;    // PST - Los Angeles
static const int DEFAULT_SECONDARY = 6;  // EST - New York

static int g_primary = DEFAULT_PRIMARY;
static int g_secondary = DEFAULT_SECONDARY;

// Make the primary zone the process-wide active TZ (used by anything that calls
// localtime_r() without first selecting a zone).
static void applyPrimaryEnv() {
  setenv("TZ", ZONES[g_primary].tz, 1);
  tzset();
}

static int findZoneByTz(const String &tz) {
  for (int i = 0; i < ZONE_COUNT; i++)
    if (tz == ZONES[i].tz) return i;
  return -1;
}

void timeBegin() {
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  applyPrimaryEnv();
  struct tm ti;
  for (int i = 0; i < 20; i++) {
    if (getLocalTime(&ti, 500) && ti.tm_year > (2020 - 1900)) {
      logInfo("Time synced.");
      return;
    }
  }
  logWarn("Time sync timed out.");
}

bool timeFormatDateTime(char *buf, size_t len) {
  time_t nowt = time(nullptr);
  if (nowt < 1700000000) return false;
  struct tm pt, et;
  char d[20], pT[8], pZ[8], eT[8], eZ[8];
  setenv("TZ", ZONES[g_primary].tz, 1);
  tzset();
  localtime_r(&nowt, &pt);
  strftime(d, sizeof(d), "%a %b %e", &pt);
  strftime(pT, sizeof(pT), "%H:%M", &pt);
  strftime(pZ, sizeof(pZ), "%Z", &pt);
  setenv("TZ", ZONES[g_secondary].tz, 1);
  tzset();
  localtime_r(&nowt, &et);
  strftime(eT, sizeof(eT), "%H:%M", &et);
  strftime(eZ, sizeof(eZ), "%Z", &et);
  applyPrimaryEnv();  // leave the primary zone active
  snprintf(buf, len, "%s %s %s  (%s %s)", d, pT, pZ, eT, eZ);
  return true;
}

int timeZoneCount() {
  return ZONE_COUNT;
}

const char *timeZoneLabel(int i) {
  if (i < 0 || i >= ZONE_COUNT) return "";
  return ZONES[i].label;
}

int timePrimaryZone() {
  return g_primary;
}

int timeSecondaryZone() {
  return g_secondary;
}

void timeSetZones(int primary, int secondary) {
  if (primary >= 0 && primary < ZONE_COUNT) g_primary = primary;
  if (secondary >= 0 && secondary < ZONE_COUNT) g_secondary = secondary;
  applyPrimaryEnv();  // a changed primary takes effect immediately
}

// Persisted as two lines (primary TZ, then secondary TZ). Storing the POSIX
// strings rather than indices keeps the file valid if the table is reordered.
void timeSaveZones() {
  String s = String(ZONES[g_primary].tz) + "\n" + ZONES[g_secondary].tz;
  if (sdWriteText("tz.txt", s)) logInfo("Time zones saved to SD");
}

void timeLoadZones() {
  String data = sdReadText("tz.txt");
  if (data.length() == 0) return;
  int nl = data.indexOf('\n');
  String p = (nl < 0) ? data : data.substring(0, nl);
  String e = (nl < 0) ? String("") : data.substring(nl + 1);
  p.trim();
  e.trim();
  int pi = findZoneByTz(p);
  int ei = findZoneByTz(e);
  if (pi >= 0) g_primary = pi;
  if (ei >= 0) g_secondary = ei;
  applyPrimaryEnv();
  logInfo("Time zones loaded from SD (%s / %s)", ZONES[g_primary].label, ZONES[g_secondary].label);
}
