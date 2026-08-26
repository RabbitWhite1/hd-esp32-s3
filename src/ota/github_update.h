// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Zhanghan Wang

#pragma once
#include <Arduino.h>

// Pull-side firmware updates: lists the releases CI publishes (see
// .github/workflows/firmware.yml) and installs one on request from the web UI.
// The image is streamed straight into the idle OTA slot and checked against the
// release's published SHA-256 before anything is committed, so a truncated or
// tampered download is rejected rather than booted.
//
// Repo and (optional) token live in the shared config: "gh_repo" defaults to the
// upstream repo, and "gh_token" is only needed while that repo is private --
// public releases list and download anonymously.

bool ghRefresh();                  // re-list releases over HTTPS (blocking); false on failure
int ghCount();                     // cached releases, newest first
const char *ghTag(int i);          // i-th release tag ("" out of range)
uint32_t ghSize(int i);            // i-th release image size in bytes (0 if unknown)
time_t ghListedAt();               // when the cache was filled (0 = never)
const char *ghError();             // reason the last refresh/install failed ("" if none)
const char *ghRepo();              // "owner/repo" being polled

// Download + flash the named release, then reboot into it. Blocks for the whole
// transfer (progress goes to the LCD via otaReport) and only returns on failure.
bool ghInstall(const String &tag);
