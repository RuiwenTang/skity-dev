// Copyright 2021 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

#ifndef SRC_RENDER_SW_ACCEL_SW_ACCEL_HPP
#define SRC_RENDER_SW_ACCEL_SW_ACCEL_HPP

#include <stddef.h>
#include <stdint.h>

namespace skity {
namespace sw_accel {

// out[i] = min(255, in[i] + alpha). Dispatched through Highway's runtime
// target selection; smoke kernel proving the accelerator toolchain.
void SaturatedAddRow(const uint8_t* in, uint8_t* out, size_t count,
                     uint8_t alpha);

// Runs SaturatedAddRow over deterministic patterns and checks the result
// against the scalar reference. Returns false on mismatch.
bool SelfTest();

}  // namespace sw_accel
}  // namespace skity

#endif  // SRC_RENDER_SW_ACCEL_SW_ACCEL_HPP
