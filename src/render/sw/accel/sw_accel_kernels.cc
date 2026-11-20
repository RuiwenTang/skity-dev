// Copyright 2021 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

// Kernel body for the SW accelerator. Re-included once per SIMD target by
// sw_accel.cpp through hwy/foreach_target.h; must not be compiled directly.

#include <stddef.h>
#include <stdint.h>

#include <hwy/highway.h>

HWY_BEFORE_NAMESPACE();

namespace skity {
namespace sw_accel {
namespace HWY_NAMESPACE {

namespace hn = hwy::HWY_NAMESPACE;

// Smoke kernel for the M1b-1 toolchain wiring; real composite kernels
// arrive with M1b-3. out[i] = min(255, in[i] + alpha).
void SaturatedAddRow(const uint8_t* HWY_RESTRICT in, uint8_t* HWY_RESTRICT out,
                     size_t count, uint8_t alpha) {
  const hn::CappedTag<uint8_t, 16> d;
  const size_t vl = hn::Lanes(d);
  const auto va = hn::Set(d, alpha);

  size_t i = 0;
  for (; i + vl <= count; i += vl) {
    hn::StoreU(hn::SaturatedAdd(hn::LoadU(d, in + i), va), d, out + i);
  }
  for (; i < count; i++) {
    const uint32_t v = static_cast<uint32_t>(in[i]) + alpha;
    out[i] = v > 255 ? 255 : static_cast<uint8_t>(v);
  }
}

}  // namespace HWY_NAMESPACE
}  // namespace sw_accel
}  // namespace skity

HWY_AFTER_NAMESPACE();
