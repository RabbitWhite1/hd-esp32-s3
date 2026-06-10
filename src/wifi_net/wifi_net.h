// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Zhanghan Wang

#pragma once
#include <Arduino.h>

// Wi-Fi backend: station connect/reconnect/status, plus a small list of saved
// networks (persisted to SD). wifiBegin() tries each saved network in turn, then
// falls back to the hardcoded default. New networks are only added once proven
// connectable (see wifiAddNetwork), keeping the saved list known-good.
void wifiBegin();            // join a known network (saved list first, then the hardcoded default)
void wifiEnsureConnected();  // reconnect if the link has dropped
bool wifiConnected();
const char *wifiSSID();      // the currently-joined network name (falls back to the default)
String wifiIP();             // dotted-quad string, or "" when disconnected
const char *wifiHostname();  // mDNS hostname; the device is reachable at "<name>.local"

// Saved-network list (persisted to /sdcard/wifi.txt).
void wifiLoadNetworks();     // load the saved list from SD (call before wifiBegin)
void wifiSaveNetworks();     // re-write the saved list to SD
// Try to join (ssid, pass); only on success is it added to the saved list (one
// entry per SSID, newest password wins) and persisted. Returns whether it joined.
bool wifiAddNetwork(const String &ssid, const String &pass);
// Add (ssid, pass) to the saved list and persist it WITHOUT testing the
// connection — for pre-seeding known networks that may not be in range yet.
// Dedups by SSID. Requires the SD card mounted (call after sdBegin()). False
// only for an empty SSID.
bool wifiStoreNetwork(const String &ssid, const String &pass);
int wifiNetCount();              // number of saved networks
const char *wifiNetSSID(int i);  // i-th saved SSID ("" if out of range)
