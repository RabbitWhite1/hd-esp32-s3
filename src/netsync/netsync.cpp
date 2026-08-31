// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Zhanghan Wang

#include "netsync.h"
#include "../logging/logging.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

static SemaphoreHandle_t netMutex = nullptr;
static SemaphoreHandle_t dataMutex = nullptr;
static volatile uint32_t version = 0;

void netsyncBegin() {
  if (!netMutex) netMutex = xSemaphoreCreateMutex();
  if (!dataMutex) dataMutex = xSemaphoreCreateMutex();
  // Allocation only fails if the heap is already exhausted at boot. Degrade to
  // the old unsynchronised behaviour rather than refusing to run: every lock
  // call below treats a null handle as "acquired".
  if (!netMutex || !dataMutex) logError("netsync: mutex alloc failed -- running unsynchronised");
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

void dataLock() {
  if (dataMutex) xSemaphoreTake(dataMutex, portMAX_DELAY);
}

void dataUnlock() {
  if (dataMutex) xSemaphoreGive(dataMutex);
}

uint32_t dataVersion() {
  return version;
}

void dataBump() {
  version++;
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

DataGuard::DataGuard() {
  dataLock();
}

DataGuard::~DataGuard() {
  dataUnlock();
}
