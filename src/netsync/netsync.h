// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Zhanghan Wang

#pragma once
#include <Arduino.h>

// Concurrency plumbing for the background fetch task (the task itself lives in
// the sketch, next to the other scheduling). Two locks, with different jobs:
//
//  * The NETWORK lock serialises everything that opens a TLS connection. Two
//    WiFiClientSecure contexts alive at once starve the heap on this board --
//    that is the failure fixed in commit 5da1af8, where a request and its
//    redirect target overlapped -- and now that the feeds run on their own task
//    they can collide with the web UI's GitHub calls, which still happen on the
//    loop task (`ghRefreshIfStale` fires on every page load).
//
//    The two sides take it differently on purpose. Background polling uses
//    netTryLock() with a short wait and simply retries on its next tick if the
//    radio is busy; anything the user triggered uses netLock() and waits. So
//    interactive work never queues behind a background refresh, only the other
//    way round.
//
// There is no data lock: feed results are handed over by staging rather than
// shared, so the loop task is the only writer of anything the renderer reads.
// See the staging note in claude_usage.cpp.
void netsyncBegin();  // create the locks (call early in setup, before any use)

bool netTryLock(uint32_t waitMs);  // background use: false = busy, retry later
void netLock();                    // interactive use: wait for the radio
void netUnlock();

// RAII wrappers. The fetch paths are full of early returns on HTTP errors, so
// scope-based release is the difference between a missed unlock and a device
// that quietly stops fetching forever.
struct NetGuard {
  bool ok;
  // waitMs == 0 waits indefinitely (interactive); otherwise it is a try-lock and
  // `ok` says whether the radio was actually acquired.
  explicit NetGuard(uint32_t waitMs);
  ~NetGuard();
};
