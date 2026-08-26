// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Zhanghan Wang

#pragma once
#include <Arduino.h>

// Over-the-air firmware updates (ArduinoOTA / espota), so a rebuilt sketch can be
// pushed over the LAN instead of over USB. The 16 MB partition scheme the build
// uses (app3M_fat9M_16MB) already carries two 3 MB app slots plus otadata, so no
// repartitioning is needed: the image lands in the idle slot and the next boot
// switches to it. See README for the one-line build+push command.
void otaBegin();   // start listening (no-op until Wi-Fi is up; safe to call twice)
void otaHandle();  // pump from loop(); also starts the listener once Wi-Fi comes up

// The receive loop lives inside otaHandle(), so loop() can't redraw during an
// update. Give the module a redraw hook (as wifi_net does) and it repaints the
// progress box itself.
void otaSetRedrawHook(void (*fn)());

// Publish progress from an update this module didn't run -- the GitHub
// pull-updater in this folder shares the LCD progress box with the push path.
void otaReport(bool active, int percent, const char *status);
bool otaActive();          // true while an image is being received
int otaPercent();          // 0-100 progress of the running update
const char *otaStatus();   // short status/error line for the LCD
