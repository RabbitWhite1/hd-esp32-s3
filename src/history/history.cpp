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

// The two families of yearly CSVs, each with its own header (see history.h).
static const char *SENSOR_PREFIX = "sensors";
static const char *SENSOR_HEADER = "timestamp,temperature,humidity\n";
static const char *BATTERY_PREFIX = "battery";
static const char *BATTERY_HEADER = "timestamp,voltage,battery,charging\n";

static const int MAX_VALS = 3;  // widest row we parse (battery: voltage, battery, charging)

static String yearFile(const char *prefix, int year) {
  char n[48];
  snprintf(n, sizeof(n), "%s/%s_%04d.csv", DIR, prefix, year);
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
static void ensureHeader(const String &rel, const char *header) {
  String p = sdPath(rel.c_str());
  if (p.length() == 0) return;
  struct stat st;
  if (stat(p.c_str(), &st) != 0 || st.st_size == 0) sdAppendText(rel.c_str(), header);
}

void historyAdd(time_t when, float tempC, float rh) {
  String rel = yearFile(SENSOR_PREFIX, yearOf(when));
  ensureHeader(rel, SENSOR_HEADER);
  sdAppendText(rel.c_str(), String((long)when) + "," + String(tempC, 1) + "," + String(rh, 1) + "\n");
}

void historyAddBattery(time_t when, float volts, int pct, bool charging) {
  if (isnan(volts)) return;  // no reading yet -- skip rather than log a hole
  String rel = yearFile(BATTERY_PREFIX, yearOf(when));
  ensureHeader(rel, BATTERY_HEADER);
  sdAppendText(rel.c_str(), String((long)when) + "," + String(volts, 3) + "," + String(pct) + "," +
                              String(charging ? 1 : 0) + "\n");
}

// Parse "<epoch>[,<float>]*" into ts + up to maxVals values; returns how many
// values were read, or -1 when the line isn't a data row (the header, garbage).
static int parseRow(const char *line, time_t *ts, float *vals, int maxVals) {
  char *end;
  long e = strtol(line, &end, 10);
  if (end == line || *end != ',') return -1;
  *ts = (time_t)e;
  int n = 0;
  while (*end == ',' && n < maxVals) {
    const char *p = end + 1;
    float v = strtof(p, &end);
    if (end == p) break;  // empty or malformed field -- stop, keep what we have
    vals[n++] = v;
  }
  return n;
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
    if (parseRow(line, &ts, nullptr, 0) < 0) {  // header/garbage -> treat as before 'from'
      lo = ftell(f);
      continue;
    }
    if (ts < from) lo = ftell(f);
    else hi = mid;
  }
  return lo;
}

// One output bucket: running sums plus a per-family count, so a bucket that
// holds only battery samples still reports null (not 0) for temp/humidity.
struct Bucket {
  float sT, sH, sV, sB;
  uint16_t nS, nB;
};
static Bucket buckets[MAX_POINTS];

// Query state shared with the row sinks below (one query runs at a time).
static time_t qFrom;
static long qBucketSec;
static int qNb;

static void sinkSensor(time_t ts, const float *v, int n, int idx) {
  if (n < 2) return;
  buckets[idx].sT += v[0];
  buckets[idx].sH += v[1];
  buckets[idx].nS++;
}

static void sinkBattery(time_t ts, const float *v, int n, int idx) {
  if (n < 2) return;
  buckets[idx].sV += v[0];
  buckets[idx].sB += v[1];
  buckets[idx].nB++;
}

// Walk one family's yearly files over [from,to], handing every in-range row to
// `sink` along with the bucket it lands in.
static void scanRange(const char *prefix, time_t from, time_t to,
                      void (*sink)(time_t, const float *, int, int)) {
  int y0 = yearOf(from), y1 = yearOf(to);
  for (int y = y0; y <= y1; y++) {
    String p = sdPath(yearFile(prefix, y).c_str());
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
      float vals[MAX_VALS];
      int n = parseRow(line, &ts, vals, MAX_VALS);
      if (n < 0) continue;
      if (ts < from) continue;
      if (ts > to) {
        done = true;
        break;
      }
      long idx = (long)((ts - qFrom) / qBucketSec);
      if (idx < 0 || idx >= qNb) continue;
      sink(ts, vals, n, (int)idx);
    }
    fclose(f);
    if (done) break;
  }
}

// Piece-wise output. Text accumulates in a small fixed buffer and goes to the
// sink whenever it fills, so the whole document never exists at once.
struct Out {
  void (*emit)(const String &, void *);
  void *ctx;
  String buf;
};
static const size_t OUT_CHUNK = 1024;

static void outFlush(Out &o) {
  if (o.buf.length()) {
    o.emit(o.buf, o.ctx);
    o.buf = "";
    o.buf.reserve(OUT_CHUNK + 64);
  }
}
static void outPut(Out &o, const char *s) {
  o.buf += s;
  if (o.buf.length() >= OUT_CHUNK) outFlush(o);
}
static void outPut(Out &o, const String &s) {
  o.buf += s;
  if (o.buf.length() >= OUT_CHUNK) outFlush(o);
}

// The five arrays, in the order they appear in the document.
enum Series { S_T, S_TEMP, S_HUM, S_VOLT, S_BATT, S_COUNT };
static const char *SERIES_KEY[S_COUNT] = {",\"t\":[", ",\"temp\":[", ",\"hum\":[", ",\"volt\":[", ",\"batt\":["};

// Emit one series over the filled buckets. Every series walks the same buckets
// in the same order, so the arrays come out index-aligned with "t"; a bucket
// with no sample of that family contributes null rather than being skipped.
static void emitSeries(Out &o, int series, int nb, time_t from, long bucketSec) {
  bool first = true;
  for (int i = 0; i < nb; i++) {
    const Bucket &b = buckets[i];
    if (!b.nS && !b.nB) continue;
    if (!first) outPut(o, ",");
    first = false;
    switch (series) {
      case S_T: outPut(o, String((long)(from + (time_t)i * bucketSec))); break;
      case S_TEMP: outPut(o, b.nS ? String(b.sT / b.nS, 1) : String("null")); break;
      case S_HUM: outPut(o, b.nS ? String(b.sH / b.nS, 1) : String("null")); break;
      case S_VOLT: outPut(o, b.nB ? String(b.sV / b.nB, 3) : String("null")); break;
      case S_BATT: outPut(o, b.nB ? String(b.sB / b.nB, 1) : String("null")); break;
    }
  }
}

void historyQuery(time_t from, time_t to, long bucketSec,
                  void (*emit)(const String &, void *), void *ctx) {
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

  for (int i = 0; i < nb; i++) buckets[i] = Bucket{0, 0, 0, 0, 0, 0};

  qFrom = from;
  qBucketSec = bucketSec;
  qNb = (int)nb;
  scanRange(SENSOR_PREFIX, from, to, sinkSensor);
  scanRange(BATTERY_PREFIX, from, to, sinkBattery);

  // One pass per series. The bucket table is already in RAM, so re-walking it
  // five times costs nothing next to holding five half-built arrays at once.
  Out o{emit, ctx, String()};
  o.buf.reserve(OUT_CHUNK + 64);
  outPut(o, "{\"bucket\":");
  outPut(o, String(bucketSec));
  for (int s = 0; s < S_COUNT; s++) {
    outPut(o, SERIES_KEY[s]);
    emitSeries(o, s, (int)nb, from, bucketSec);
    outPut(o, "]");
  }
  outPut(o, "}");
  outFlush(o);
}
