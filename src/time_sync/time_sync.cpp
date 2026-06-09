// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Zhanghan Wang

#include "time_sync.h"
#include "../logging/logging.h"
#include <time.h>

#define TZ_PACIFIC "PST8PDT,M3.2.0,M11.1.0"
#define TZ_EASTERN "EST5EDT,M3.2.0,M11.1.0"

void timeBegin() {
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  setenv("TZ", TZ_PACIFIC, 1);
  tzset();
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
  setenv("TZ", TZ_PACIFIC, 1);
  tzset();
  localtime_r(&nowt, &pt);
  strftime(d, sizeof(d), "%a %b %e", &pt);
  strftime(pT, sizeof(pT), "%H:%M", &pt);
  strftime(pZ, sizeof(pZ), "%Z", &pt);
  setenv("TZ", TZ_EASTERN, 1);
  tzset();
  localtime_r(&nowt, &et);
  strftime(eT, sizeof(eT), "%H:%M", &et);
  strftime(eZ, sizeof(eZ), "%Z", &et);
  setenv("TZ", TZ_PACIFIC, 1);
  tzset();
  snprintf(buf, len, "%s %s %s  (%s %s)", d, pT, pZ, eT, eZ);
  return true;
}
