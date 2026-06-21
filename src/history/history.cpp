// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Zhanghan Wang

#include "history.h"
#include "../sdcard/sdcard.h"
#include "../logging/logging.h"
#include <sys/stat.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static const char *DIR = "sensor_data";
static const int MAX_POINTS = 1500;  // cap on output buckets (>=1440 so a full day fits minutely)

static String yearFile(int year) {
  char n[40];
  snprintf(n, sizeof(n), "%s/sensors_%04d.csv", DIR, year);
  return String(n);
}

static int yearOf(time_t t) {
  struct tm tmv;
  localtime_r(&t, &tmv);
  return tmv.tm_year + 1900;
}

void historyBegin() {
  String d = sdPath(DIR);
  if (d.length()) mkdir(d.c_str(), 0777);  // harmless if it already exists
}

// Write the CSV header the first time a fresh/empty yearly file is opened.
static void ensureHeader(const String &rel) {
  String p = sdPath(rel.c_str());
  if (p.length() == 0) return;
  struct stat st;
  if (stat(p.c_str(), &st) != 0 || st.st_size == 0)
    sdAppendText(rel.c_str(), "timestamp,temperature,humidity\n");
}

void historyAdd(time_t when, float tempC, float rh) {
  String rel = yearFile(yearOf(when));
  ensureHeader(rel);
  sdAppendText(rel.c_str(), String((long)when) + "," + String(tempC, 1) + "," + String(rh, 1) + "\n");
}

// Parse "<epoch>,<temp>,<hum>"; false for the header row or any malformed line.
static bool parseRow(const char *line, time_t *ts, float *t, float *h) {
  char *end;
  long e = strtol(line, &end, 10);
  if (end == line || *end != ',') return false;
  float tv = strtof(end + 1, &end);
  if (*end != ',') return false;
  float hv = strtof(end + 1, &end);
  *ts = (time_t)e;
  *t = tv;
  *h = hv;
  return true;
}

// Byte offset of the first row with ts >= from (binary search over the file, so
// we don't scan a multi-MB file from the start to reach a recent window).
static long findStartOffset(FILE *f, long size, time_t from) {
  long lo = 0, hi = size;
  char line[64];
  while (lo < hi) {
    long mid = (lo + hi) / 2;
    fseek(f, mid, SEEK_SET);
    if (mid > 0 && !fgets(line, sizeof(line), f)) {  // discard partial line
      hi = mid;
      continue;
    }
    if (!fgets(line, sizeof(line), f)) {  // hit EOF in this half
      hi = mid;
      continue;
    }
    time_t ts;
    float a, b;
    if (!parseRow(line, &ts, &a, &b)) {  // header/garbage -> treat as before 'from'
      lo = ftell(f);
      continue;
    }
    if (ts < from) lo = ftell(f);
    else hi = mid;
  }
  return lo;
}

String historyQuery(time_t from, time_t to, long bucketSec) {
  if (to < from) {
    time_t tmp = from;
    from = to;
    to = tmp;
  }
  if (bucketSec < 1) bucketSec = 60;
  // Coarsen the bucket so the output never exceeds MAX_POINTS.
  long span = (long)(to - from);
  long nb = span / bucketSec + 1;
  if (nb > MAX_POINTS) {
    bucketSec = span / MAX_POINTS + 1;
    nb = span / bucketSec + 1;
  }
  if (nb < 1) nb = 1;
  if (nb > MAX_POINTS) nb = MAX_POINTS;

  static float sumT[MAX_POINTS], sumH[MAX_POINTS];
  static uint16_t cnt[MAX_POINTS];
  for (int i = 0; i < nb; i++) {
    sumT[i] = 0;
    sumH[i] = 0;
    cnt[i] = 0;
  }

  int y0 = yearOf(from), y1 = yearOf(to);
  for (int y = y0; y <= y1; y++) {
    String p = sdPath(yearFile(y).c_str());
    if (p.length() == 0) continue;
    FILE *f = fopen(p.c_str(), "rb");
    if (!f) continue;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    long start = (y == y0) ? findStartOffset(f, size, from) : 0;
    if (start > 256) start -= 256;  // small margin against line-alignment slop
    else start = 0;
    fseek(f, start, SEEK_SET);
    char line[64];
    if (start > 0) fgets(line, sizeof(line), f);  // skip the partial first line
    bool done = false;
    while (fgets(line, sizeof(line), f)) {
      time_t ts;
      float t, h;
      if (!parseRow(line, &ts, &t, &h)) continue;
      if (ts < from) continue;
      if (ts > to) {
        done = true;
        break;
      }
      long idx = (long)((ts - from) / bucketSec);
      if (idx < 0 || idx >= nb) continue;
      sumT[idx] += t;
      sumH[idx] += h;
      cnt[idx]++;
    }
    fclose(f);
    if (done) break;
  }

  String t = "[", tp = "[", hm = "[";
  bool first = true;
  for (int i = 0; i < nb; i++) {
    if (!cnt[i]) continue;
    if (!first) {
      t += ',';
      tp += ',';
      hm += ',';
    }
    first = false;
    t += String((long)(from + (time_t)i * bucketSec));
    tp += String(sumT[i] / cnt[i], 1);
    hm += String(sumH[i] / cnt[i], 1);
  }
  t += "]";
  tp += "]";
  hm += "]";
  return "{\"bucket\":" + String(bucketSec) + ",\"t\":" + t + ",\"temp\":" + tp + ",\"hum\":" + hm + "}";
}
