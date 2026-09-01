// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Zhanghan Wang

#pragma once

#include <atomic>
#include <stdint.h>

// One atomic word for cross-task yes/no state. Keeping the memory ordering here
// makes call sites state intent instead of repeating atomic mechanics.
class SyncFlag final {
 public:
  bool isSet() const {
    return value.load(std::memory_order_acquire) != 0;
  }

  void set() {
    value.store(1, std::memory_order_release);
  }

  void clear() {
    value.store(0, std::memory_order_release);
  }

  // Clear and return the previous value. A request arriving while work runs can
  // set the flag again without being cleared by the work already in progress.
  bool take() {
    return value.exchange(0, std::memory_order_acq_rel) != 0;
  }

 private:
  std::atomic<uint32_t> value{0};
};
