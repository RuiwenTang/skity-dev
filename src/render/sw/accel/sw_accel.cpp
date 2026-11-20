// Copyright 2021 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

// Multi-target entry for the SW accelerator. foreach_target.h re-includes
// the kernel body once per enabled SIMD target; the direct include after it
// compiles the static (baseline) target and finalizes the dispatch table.

#undef HWY_TARGET_INCLUDE
#define HWY_TARGET_INCLUDE "src/render/sw/accel/sw_accel_kernels.cc"
#include <hwy/foreach_target.h>

#include "src/render/sw/accel/sw_accel_kernels.cc"

namespace skity {
namespace sw_accel {

HWY_EXPORT(SaturatedAddRow);

void SaturatedAddRow(const uint8_t* in, uint8_t* out, size_t count,
                     uint8_t alpha) {
  HWY_DYNAMIC_DISPATCH(SaturatedAddRow)(in, out, count, alpha);
}

bool Validate() {
  uint8_t in[67];
  uint8_t out[67];
  uint8_t expect[67];
  for (size_t i = 0; i < 67; i++) {
    in[i] = static_cast<uint8_t>(i * 7 + (i % 3) * 85);
    const uint32_t v = static_cast<uint32_t>(in[i]) + 200;
    expect[i] = v > 255 ? 255 : static_cast<uint8_t>(v);
  }
  SaturatedAddRow(in, out, 67, 200);
  for (size_t i = 0; i < 67; i++) {
    if (out[i] != expect[i]) {
      return false;
    }
  }
  return true;
}

}  // namespace sw_accel
}  // namespace skity
