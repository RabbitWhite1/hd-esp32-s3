// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Zhanghan Wang

#include "netsync.h"
#include "../logging/logging.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

static SemaphoreHandle_t netMutex = nullptr;

void netsyncBegin() {
  if (!netMutex) netMutex = xSemaphoreCreateMutex();
  // Allocation only fails if the heap is already exhausted at boot. Degrade to
  // unsynchronised behaviour rather than refusing to run: every call below
  // treats a null handle as "acquired".
  if (!netMutex) logError("netsync: mutex alloc failed -- running unsynchronised");
}

bool netTryLock(uint32_t waitMs) {
  if (!netMutex) return true;
  return xSemaphoreTake(netMutex, pdMS_TO_TICKS(waitMs)) == pdTRUE;
}

void netLock() {
  if (netMutex) xSemaphoreTake(netMutex, portMAX_DELAY);
}

void netUnlock() {
  if (netMutex) xSemaphoreGive(netMutex);
}

NetGuard::NetGuard(uint32_t waitMs) {
  if (waitMs == 0) {
    netLock();
    ok = true;
  } else {
    ok = netTryLock(waitMs);
  }
}

NetGuard::~NetGuard() {
  if (ok) netUnlock();
}
