#pragma once
#include <Arduino.h>

// Wi-Fi backend: station connect, reconnect, and status helpers.
void wifiBegin();            // connect to the configured AP (blocks until joined)
void wifiEnsureConnected();  // reconnect if the link has dropped
bool wifiConnected();
const char *wifiSSID();      // configured network name
String wifiIP();             // dotted-quad string, or "" when disconnected
