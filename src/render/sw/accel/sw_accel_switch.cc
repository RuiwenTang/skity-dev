// Copyright 2021 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

// The accelerator runtime switch is compiled into the skity target itself,
// NOT into the skity_sw_accel archive. Test and benchmark executables link
// the archive statically; a switch defined there shadows the shared
// library's copy, leaving those binaries with two independent mirrors - the
// A/B arms toggled a state the renderer (inside libskity) never read, so
// every legacy arm silently measured the accelerated path. Keeping the
// state in skity leaves exactly one mirror per process.

#include <atomic>

#include "src/render/sw/accel/sw_accel.hpp"

namespace skity {
namespace sw_accel {

namespace {
std::atomic<bool> g_runtime_enabled{true};
}  // namespace

void SetEnabled(bool enabled) {
  g_runtime_enabled.store(enabled, std::memory_order_relaxed);
}

bool IsEnabled() { return g_runtime_enabled.load(std::memory_order_relaxed); }

}  // namespace sw_accel
}  // namespace skity
