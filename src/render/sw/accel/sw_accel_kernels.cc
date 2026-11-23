// Copyright 2021 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

// Kernel body for the SW accelerator. Re-included once per SIMD target by
// sw_accel.cpp through hwy/foreach_target.h; must not be compiled directly.

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <hwy/highway.h>

#include "src/graphic/color_priv.hpp"

HWY_BEFORE_NAMESPACE();

namespace skity {
namespace sw_accel {
namespace HWY_NAMESPACE {

namespace hn = hwy::HWY_NAMESPACE;

// Blends a run of premul RGBA8888 pixels in place against a constant premul
// src color. Bit-exact with the scalar path (PorterDuffBlend / FastBlend):
// coverage has already been folded into src_pm by the caller.
//
// Memory layout is skity's ColorType::kRGBA: bytes [R, G, B, A], i.e. a
// little-endian u32 word of 0xAABBGGRR. src_pm is a PMColor word
// (0xAARRGGBB) and is converted at this boundary; the formulas themselves
// are per-channel symmetric with alpha in the top byte.

void SolidRunClearKernel(uint8_t* HWY_RESTRICT dst, size_t count,
                         uint32_t src_pm) {
  (void)src_pm;
  memset(dst, 0, count * 4);
}

void SolidRunSrcKernel(uint8_t* HWY_RESTRICT dst, size_t count,
                       uint32_t src_pm) {
  const uint32_t src_mem = PMColorSwapRB(src_pm);
  const hn::CappedTag<uint32_t, 4> d32;
  const size_t vl = hn::Lanes(d32);
  const auto v = hn::Set(d32, src_mem);
  uint32_t* dst32 = reinterpret_cast<uint32_t*>(dst);

  size_t i = 0;
  for (; i + vl <= count; i += vl) {
    hn::StoreU(v, d32, dst32 + i);
  }
  for (; i < count; i++) {
    dst32[i] = src_mem;
  }
}

void SolidRunSrcOverKernel(uint8_t* HWY_RESTRICT dst, size_t count,
                           uint32_t src_pm) {
  // dst' = src + ((dst * (256 - a)) >> 8), truncating like AlphaMulQ.
  // The low u16 multiply is exact: dst * scale <= 255 * 256 < 65536.
  const uint32_t scale = Alpha255To256(255 - ColorGetA(src_pm));

  uint8_t src_bytes[16];
  for (int px = 0; px < 4; px++) {
    src_bytes[px * 4 + 0] = static_cast<uint8_t>(src_pm >> 16);  // R
    src_bytes[px * 4 + 1] = static_cast<uint8_t>(src_pm >> 8);   // G
    src_bytes[px * 4 + 2] = static_cast<uint8_t>(src_pm);        // B
    src_bytes[px * 4 + 3] = static_cast<uint8_t>(src_pm >> 24);  // A
  }

  const hn::CappedTag<uint8_t, 16> d8;
  const hn::Half<decltype(d8)> d8h;
  const hn::CappedTag<uint16_t, 8> d16;
  const size_t vl = hn::Lanes(d8);  // 16 bytes = 4 pixels

  const auto src8 = hn::LoadU(d8, src_bytes);
  const auto s_lo = hn::PromoteTo(d16, hn::LowerHalf(d8h, src8));
  const auto s_hi = hn::PromoteTo(d16, hn::UpperHalf(d8h, src8));
  const auto v_scale = hn::Set(d16, static_cast<uint16_t>(scale));

  uint32_t* dst32 = reinterpret_cast<uint32_t*>(dst);
  size_t px = 0;
  for (; px + vl / 4 <= count; px += vl / 4) {
    const auto d8v = hn::LoadU(d8, dst + px * 4);
    const auto d_lo = hn::PromoteTo(d16, hn::LowerHalf(d8h, d8v));
    const auto d_hi = hn::PromoteTo(d16, hn::UpperHalf(d8h, d8v));
    const auto r_lo = hn::Add(s_lo, hn::ShiftRight<8>(hn::Mul(d_lo, v_scale)));
    const auto r_hi = hn::Add(s_hi, hn::ShiftRight<8>(hn::Mul(d_hi, v_scale)));
    hn::StoreU(hn::Combine(d8, hn::TruncateTo(d8h, r_hi),
                           hn::TruncateTo(d8h, r_lo)),
               d8, dst + px * 4);
  }
  for (; px < count; px++) {
    // Memory words are 0xAABBGGRR; blend in the PMColor convention.
    dst32[px] =
        PMColorSwapRB(PMSrcOver(src_pm, PMColorSwapRB(dst32[px])));
  }
}

}  // namespace HWY_NAMESPACE
}  // namespace sw_accel
}  // namespace skity

HWY_AFTER_NAMESPACE();
