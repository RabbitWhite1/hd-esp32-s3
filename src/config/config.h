// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Zhanghan Wang

#pragma once
#include <Arduino.h>

// Tiny persistent key/value store backed by a single text file
// (/sdcard/esp32.conf, one "key=value" line per entry). It holds the handful of
// small runtime settings that don't each warrant their own file (e.g. refresh
// intervals). Load once at boot with configBegin(); get/set operate on an
// in-RAM table and only touch the card on configSave().
void configBegin();  // load the store from SD (call after sdBegin)

String configGet(const char *key, const String &def = "");  // value, or def if the key is absent
long configGetInt(const char *key, long def);               // value parsed as an int, or def if absent
void configSet(const char *key, const String &value);       // set in RAM (call configSave to persist)
void configSetInt(const char *key, long value);
bool configSave();   // write the whole store back to SD; true on success
