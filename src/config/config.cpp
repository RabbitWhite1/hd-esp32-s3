// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Zhanghan Wang

#include "config.h"
#include "../sdcard/sdcard.h"  // the store lives in /sdcard/esp32.conf
#include "../logging/logging.h"

static const char *CONF_FILE = "esp32.conf";
static const int MAX_ENTRIES = 16;

struct Entry {
  String key;
  String value;
};
static Entry entries[MAX_ENTRIES];
static int entryCount = 0;

static int findKey(const char *key) {
  for (int i = 0; i < entryCount; i++)
    if (entries[i].key == key) return i;
  return -1;
}

void configBegin() {
  entryCount = 0;
  String data = sdReadText(CONF_FILE);
  int start = 0;
  while (start < (int)data.length() && entryCount < MAX_ENTRIES) {
    int nl = data.indexOf('\n', start);
    String line = (nl < 0) ? data.substring(start) : data.substring(start, nl);
    int eq = line.indexOf('=');
    if (eq > 0) {
      String k = line.substring(0, eq);
      String v = line.substring(eq + 1);
      k.trim();
      v.trim();
      if (k.length()) {
        entries[entryCount].key = k;
        entries[entryCount].value = v;
        entryCount++;
      }
    }
    if (nl < 0) break;
    start = nl + 1;
  }
  logInfo("Config loaded from SD (%d entries)", entryCount);
}

String configGet(const char *key, const String &def) {
  int i = findKey(key);
  return i < 0 ? def : entries[i].value;
}

long configGetInt(const char *key, long def) {
  int i = findKey(key);
  return i < 0 ? def : entries[i].value.toInt();
}

void configSet(const char *key, const String &value) {
  int i = findKey(key);
  if (i >= 0) {
    entries[i].value = value;
    return;
  }
  if (entryCount >= MAX_ENTRIES) {
    logWarn("Config full (%d), dropping key %s", MAX_ENTRIES, key);
    return;
  }
  entries[entryCount].key = key;
  entries[entryCount].value = value;
  entryCount++;
}

void configSetInt(const char *key, long value) {
  configSet(key, String(value));
}

bool configSave() {
  String out;
  for (int i = 0; i < entryCount; i++) {
    out += entries[i].key;
    out += '=';
    out += entries[i].value;
    out += '\n';
  }
  bool ok = sdWriteText(CONF_FILE, out);
  if (ok) logInfo("Config saved to SD (%d entries)", entryCount);
  return ok;
}
