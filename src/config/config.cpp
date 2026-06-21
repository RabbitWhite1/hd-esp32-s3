// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Zhanghan Wang

#include "config.h"
#include "../sdcard/sdcard.h"  // the store lives in /sdcard/esp32.json
#include "../logging/logging.h"

static const char *CONF_FILE = "esp32.json";

// The whole store is a single JSON object held in RAM; the document IS the dict.
// Keys passed to get/set must be string literals with program-long lifetime
// (ArduinoJson links, not copies, const char* keys) - all callers use statics.
static JsonDocument doc;

static int keyCount() {
  return doc.is<JsonObject>() ? (int)doc.as<JsonObject>().size() : 0;
}

void configBegin() {
  doc.clear();
  String data = sdReadText(CONF_FILE);
  if (data.length()) {
    DeserializationError err = deserializeJson(doc, data);
    if (err) {
      // A truncated/corrupt file (e.g. power loss mid-write) parses as a whole,
      // so we lose everything and fall back to defaults rather than one bad line.
      logWarn("Config parse failed (%s); starting empty", err.c_str());
      doc.clear();
    }
  }
  logInfo("Config loaded from SD (%d keys)", keyCount());
}

String configGet(const char *key, const String &def) {
  JsonVariantConst v = doc[key];
  return v.isNull() ? def : v.as<String>();
}

long configGetInt(const char *key, long def) {
  JsonVariantConst v = doc[key];
  return v.isNull() ? def : v.as<long>();
}

void configSet(const char *key, const String &value) {
  doc[key] = value;  // value content is copied into the document pool
}

void configSetInt(const char *key, long value) {
  doc[key] = value;
}

bool configSave() {
  String out;
  serializeJsonPretty(doc, out);
  bool ok = sdWriteText(CONF_FILE, out);
  if (ok) logInfo("Config saved to SD (%d keys)", keyCount());
  return ok;
}

JsonDocument &configDoc() {
  return doc;
}
