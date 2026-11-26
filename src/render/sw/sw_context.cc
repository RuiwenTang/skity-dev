// Copyright 2021 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

#include <skity/render/sw_context.hpp>

#include <thread>

namespace skity {

SWContext* SWContext::Get() {
  static SWContext instance;
  return &instance;
}

SWContext::SWContext() = default;

bool SWContext::AccelEnabled() const {
  return accel_enabled_.load(std::memory_order_relaxed);
}

void SWContext::SetAccelEnabled(bool enabled) {
  accel_enabled_.store(enabled, std::memory_order_relaxed);
}

bool SWContext::ParallelEnabled() const {
  return parallel_enabled_.load(std::memory_order_relaxed);
}

void SWContext::SetParallelEnabled(bool enabled) {
  parallel_enabled_.store(enabled, std::memory_order_relaxed);
}

uint32_t SWContext::ThreadCount() const {
  uint32_t count = thread_count_.load(std::memory_order_relaxed);
  if (count == 0) {
    // Auto: hardware concurrency, falling back to a single thread when the
    // value is not available or ambiguous.
    count = std::thread::hardware_concurrency();
    if (count == 0) {
      count = 1;
    }
  }
  return count;
}

void SWContext::SetThreadCount(uint32_t count) {
  thread_count_.store(count, std::memory_order_relaxed);
}

}  // namespace skity
