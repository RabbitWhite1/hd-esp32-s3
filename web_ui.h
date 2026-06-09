#pragma once
#include <Arduino.h>

// LAN-only web UI backend. Serves a tiny page where anyone on the same Wi-Fi
// can submit a short text message; the frontend (.ino) renders it on the LCD.
// State lives in RAM only, so the message is cleared on reboot.
void webBegin();             // register routes and start the HTTP server (call after Wi-Fi is up)
void webHandle();            // service pending HTTP clients (call often from loop())
const String &webMessage();  // current user-submitted message ("" when none)
