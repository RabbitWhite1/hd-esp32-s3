// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Zhanghan Wang

#pragma once
#include <Arduino.h>
#include <time.h>

// Temperature/humidity + battery logging and range queries for the web UI charts.
//
// Samples are appended (one per minute) to per-year CSV files under
// /sdcard/sensor_data/. The year in the filename is the local-time year of the
// sample; every timestamp is the Unix epoch in seconds (UTC), so it renders in
// any zone. Two families of files are kept, each rectangular in its own right:
//
//   sensors_YYYY.csv  "timestamp,temperature,humidity"
//   battery_YYYY.csv  "timestamp,voltage,battery,charging"
//
// Battery lives in its own file rather than as extra columns on the sensor rows
// for two reasons: the sensor files predate it (widening them would leave old
// rows short of the header, which trips a plain CSV reader), and a battery
// sample is still worth recording on a boot where the SHTC3 never answers.
// Join them on the timestamp column when analysing offline.
//
// `voltage` is the raw measured terminal voltage and `battery` the percentage
// the firmware's fixed discharge curve currently derives from it -- both are
// logged so the curve can be refitted later against the real cells. `charging`
// is 1 while a charger is attached; the percentage is *held* at its last
// off-charger value then, so those rows must be dropped from any voltage->SoC
// fit.
void historyBegin();  // create /sdcard/sensor_data (call after sdBegin)
void historyAdd(time_t when, float tempC, float rh);                    // append one temp/humidity sample
void historyAddBattery(time_t when, float volts, int pct, bool charging);  // append one battery sample

// Average [from,to] into buckets of ~bucketSec each and emit JSON
//   {"bucket":<sec>,"t":[epoch...],"temp":[...],"hum":[...],"volt":[...],"batt":[...]}
// with one entry per bucket that held at least one sample of *either* family;
// a series with nothing in that bucket gets null, so the arrays stay aligned
// with "t". bucketSec is automatically coarsened if the range would otherwise
// produce more than the internal output cap.
//
// The document is handed to `emit` in ~1 KB pieces rather than returned whole.
// A day of minutely data is ~45 KB across the five arrays, and building that as
// one String -- let alone joining the arrays with a `+` chain, which keeps the
// previous buffer alive while allocating the next -- asks the heap for a block
// it cannot reliably supply once Wi-Fi and TLS have taken their share. The
// allocation failure is silent: String degrades to empty, the client gets a
// truncated body, and the chart draws its axes with no data. Streaming keeps
// peak RAM flat no matter how long the requested range is.
void historyQuery(time_t from, time_t to, long bucketSec,
                  void (*emit)(const String &chunk, void *ctx), void *ctx);
