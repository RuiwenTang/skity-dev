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

// Known-answer health check over the dispatched SIMD target. Called by the
// accelerator loading path before the kernels are used (a failed check must
// disable the accelerator and fall back to the scalar path), and by unit
// tests. Returns false on mismatch.
bool Validate();

}  // namespace sw_accel
}  // namespace skity

#endif  // SRC_RENDER_SW_ACCEL_SW_ACCEL_HPP
