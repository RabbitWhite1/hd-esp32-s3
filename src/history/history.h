#pragma once
#include <Arduino.h>
#include <time.h>

// Temperature/humidity logging + range queries for the web UI chart.
//
// Samples are appended (one per minute) to per-year CSV files under
// /sdcard/sensor_data/sensors_YYYY.csv (header "timestamp,temperature,humidity";
// the timestamp is the Unix epoch in seconds, UTC, so it renders in any zone).
// The year in the filename is the local-time year of the sample.
//
// The web UI does NOT load whole files: historyQuery() reads only the requested
// time window (locating the start with a binary search over the file) and
// averages it into a bounded set of buckets, so memory + transfer stay small
// regardless of how big the logs grow.
void historyBegin();  // create /sdcard/sensor_data (call after sdBegin)
void historyAdd(time_t when, float tempC, float rh);  // append one sample to the yearly CSV

// Average [from,to] into buckets of ~bucketSec each; returns JSON
// {"bucket":<sec>,"t":[epoch...],"temp":[...],"hum":[...]} with one entry per
// non-empty bucket. bucketSec is automatically coarsened if the range would
// otherwise produce more than the internal output cap.
String historyQuery(time_t from, time_t to, long bucketSec);
