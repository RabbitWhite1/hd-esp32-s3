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

// Restore the last listing from /sdcard/releases.tsv (call after sdBegin). The
// picker then renders on a reboot, or with no network at all, instead of showing
// an empty dropdown -- and a page load does not have to spend one of the 60/hour
// unauthenticated GitHub API calls just to redraw what we already knew. A
// successful ghRefresh() rewrites the file. The cache records which repo it came
// from and is ignored if "gh_repo" has since changed.
void ghLoadCache();

// Re-list only if the cache is empty or older than maxAgeSec. Page loads call
// this so the picker tracks GitHub without a manual refresh, while the TTL keeps
// the panel snappy -- every form submit re-renders the page, and unauthenticated
// GitHub API calls are capped at 60/hour, so a fetch per render would both stall
// the UI and burn through the limit. Returns true if the cache is usable.
bool ghRefreshIfStale(unsigned long maxAgeSec);
int ghCount();                     // cached releases, newest first
const char *ghTag(int i);          // i-th release tag ("" out of range)
uint32_t ghSize(int i);            // i-th release image size in bytes (0 if unknown)
time_t ghListedAt();               // when the cache was filled (0 = never)
const char *ghError();             // reason the last refresh/install failed ("" if none)
const char *ghRepo();              // "owner/repo" being polled

// Download + flash the named release, then reboot into it. Blocks for the whole
// transfer (progress goes to the LCD via otaReport) and only returns on failure.
bool ghInstall(const String &tag);
