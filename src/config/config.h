// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Zhanghan Wang

#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

// Shared persistent settings store backed by a single JSON file
// (/sdcard/esp32.json), managed with ArduinoJson. It holds runtime settings that
// don't each warrant their own file (refresh intervals, time-zone selection,
// Claude credentials, the weather-city list, ...). Load once at boot with
// configBegin(); get/set operate on an in-RAM JsonDocument and only touch the
// card on configSave(). Feature modules own their own key names + defaults.
void configBegin();  // load the store from SD (call after sdBegin)

String configGet(const char *key, const String &def = "");  // value, or def if the key is absent
long configGetInt(const char *key, long def);               // value parsed as an int, or def if absent
void configSet(const char *key, const String &value);       // set in RAM (call configSave to persist)
void configSetInt(const char *key, long value);
bool configSave();   // write the whole store back to SD; true on success

// Direct access to the in-RAM document, for modules that store structured values
// (arrays/objects) under their own key rather than a flat string/int. Mutate it,
// then call configSave(). Keys used here must be string literals (linked, not
// copied, by ArduinoJson).
JsonDocument &configDoc();
