// Copyright 2021 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

// Kernel body for the SW accelerator. Re-included once per SIMD target by
// sw_accel.cpp through hwy/foreach_target.h; must not be compiled directly.

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <hwy/highway.h>

#include "src/graphic/blend_mode_priv.hpp"
#include "src/graphic/color_priv.hpp"

HWY_BEFORE_NAMESPACE();

namespace skity {
namespace sw_accel {
namespace HWY_NAMESPACE {

namespace hn = hwy::HWY_NAMESPACE;

// Blends a run of premul RGBA8888 pixels in place against a constant premul
// src color. Bit-exact with the scalar path (PorterDuffBlend): coverage has
// already been folded into src_pm by the caller.
//
// Memory layout is skity's ColorType::kRGBA: bytes [R, G, B, A], i.e. a
// little-endian u32 word of 0xAABBGGRR. src_pm is a PMColor word
// (0xAARRGGBB) and is converted at this boundary; the formulas themselves
// are per-channel symmetric with alpha in the top byte.
//
// Both operands are premultiplied (the accelerator only runs on premul
// surfaces), so every per-channel result fits a byte: the u16 lanes never
// wrap and match the scalar 32-bit arithmetic exactly.

// AlphaMulQ as a per-lane u16 op: truncating (x * scale) >> 8. Exact because
// x <= 255 and scale <= 256, so x * scale <= 65280 fits u16.
template <class V>
static inline V ScaleMul(V x, V scale) {
  return hn::ShiftRight<8>(hn::Mul(x, scale));
}

// MulDiv255Round as a per-lane u16 op: ((p + 128) + ((p + 128) >> 8)) >> 8.
// p <= 255 * 255 = 65025, and p + 128 + ((p + 128) >> 8) <= 65407 fits u16.
template <class V>
static inline V RoundMul(V a, V b) {
  const hn::DFromV<V> d16;
  const V t = hn::Add(hn::Mul(a, b), hn::Set(d16, uint16_t{128}));
  return hn::ShiftRight<8>(hn::Add(t, hn::ShiftRight<8>(t)));
}

// Expands the constant src color into 16 bytes (four identical kRGBA
// pixels) for the vector loops.
static inline void FillSrcBytes(uint8_t* src_bytes, uint32_t src_pm) {
  for (int px = 0; px < 4; px++) {
    src_bytes[px * 4 + 0] = static_cast<uint8_t>(src_pm >> 16);  // R
    src_bytes[px * 4 + 1] = static_cast<uint8_t>(src_pm >> 8);   // G
    src_bytes[px * 4 + 2] = static_cast<uint8_t>(src_pm);        // B
    src_bytes[px * 4 + 3] = static_cast<uint8_t>(src_pm >> 24);  // A
  }
}

// TableLookupBytes mask replicating each pixel's alpha byte (kRGBA byte 3 of
// every pixel) into the pixel's four byte lanes, so a per-pixel dst-alpha
// scale can be built after PromoteTo.
template <class D8, class V8>
static inline V8 BroadcastAlpha(D8 d8, V8 px) {
  alignas(16) static constexpr uint8_t kAlphaLanes[16] = {
      3, 3, 3, 3, 7, 7, 7, 7, 11, 11, 11, 11, 15, 15, 15, 15};
  return hn::TableLookupBytes(px, hn::LoadU(d8, kAlphaLanes));
}

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

// kDst: r = d - the destination is already the result, nothing to write.
void SolidRunDstKernel(uint8_t* HWY_RESTRICT dst, size_t count,
                       uint32_t src_pm) {
  (void)dst;
  (void)count;
  (void)src_pm;
}

// How one blend term (src or dst) is scaled before the two terms are added.
// Every Porter-Duff coeff mode is "src term + dst term" per channel with an
// AlphaMulQ-style truncating scale on each term; constant scales derive from
// the constant src alpha, per-pixel scales from that pixel's dst alpha. The
// scalar shortcuts in PorterDuffBlend (sa or da at 0/255) are exact special
// cases of the uniform formula: (c * 256) >> 8 == c and (c * 1) >> 8 == 0.
enum class ScaleKind : uint8_t {
  kOne,      // 256, the term passes through unchanged
  kInvSrcA,  // 256 - sa, constant
  kSrcA,     // sa + 1, constant
  kInvDstA,  // 256 - da, per pixel
  kDstA,     // da + 1, per pixel
  kZero,     // the term is always zero
};

template <ScaleKind K, class D16, class V16>
static inline V16 TermScale(D16 d16, V16 da16, uint32_t sa) {
  if constexpr (K == ScaleKind::kOne) {
    return hn::Set(d16, uint16_t{256});
  } else if constexpr (K == ScaleKind::kInvSrcA) {
    return hn::Set(d16, static_cast<uint16_t>(256 - sa));
  } else if constexpr (K == ScaleKind::kSrcA) {
    return hn::Set(d16, static_cast<uint16_t>(sa + 1));
  } else if constexpr (K == ScaleKind::kInvDstA) {
    return hn::Sub(hn::Set(d16, uint16_t{256}), da16);
  } else if constexpr (K == ScaleKind::kDstA) {
    return hn::Add(da16, hn::Set(d16, uint16_t{1}));
  } else {
    return hn::Zero(d16);
  }
}

// Shared engine for the Porter-Duff coeff modes expressible as
// (s * src_scale) >> 8 + (d * dst_scale) >> 8; MODE names the scalar formula
// for the tail loop.
template <BlendMode MODE, ScaleKind SRC, ScaleKind DST>
static void SolidRunCoeffKernel(uint8_t* HWY_RESTRICT dst, size_t count,
                                uint32_t src_pm) {
  constexpr bool kNeedsDstA = SRC == ScaleKind::kInvDstA ||
                              SRC == ScaleKind::kDstA ||
                              DST == ScaleKind::kInvDstA ||
                              DST == ScaleKind::kDstA;

  uint8_t src_bytes[16];
  FillSrcBytes(src_bytes, src_pm);

  const hn::CappedTag<uint8_t, 16> d8;
  const hn::Half<decltype(d8)> d8h;
  const hn::CappedTag<uint16_t, 8> d16;
  const size_t vl = hn::Lanes(d8);  // 16 bytes = 4 pixels

  const auto src8 = hn::LoadU(d8, src_bytes);
  const auto s_lo = hn::PromoteTo(d16, hn::LowerHalf(d8h, src8));
  const auto s_hi = hn::PromoteTo(d16, hn::UpperHalf(d8h, src8));
  const uint32_t sa = ColorGetA(src_pm);

  uint32_t* dst32 = reinterpret_cast<uint32_t*>(dst);
  size_t px = 0;
  for (; px + vl / 4 <= count; px += vl / 4) {
    const auto d8v = hn::LoadU(d8, dst + px * 4);
    const auto d_lo = hn::PromoteTo(d16, hn::LowerHalf(d8h, d8v));
    const auto d_hi = hn::PromoteTo(d16, hn::UpperHalf(d8h, d8v));
    const auto term_add = [&](const auto& src16, const auto& dst16,
                              const auto& da16) {
      return hn::Add(ScaleMul(src16, TermScale<SRC>(d16, da16, sa)),
                     ScaleMul(dst16, TermScale<DST>(d16, da16, sa)));
    };
    if constexpr (kNeedsDstA) {
      const auto a8 = BroadcastAlpha(d8, d8v);
      const auto a_lo = hn::PromoteTo(d16, hn::LowerHalf(d8h, a8));
      const auto a_hi = hn::PromoteTo(d16, hn::UpperHalf(d8h, a8));
      const auto r_lo = term_add(s_lo, d_lo, a_lo);
      const auto r_hi = term_add(s_hi, d_hi, a_hi);
      hn::StoreU(hn::Combine(d8, hn::TruncateTo(d8h, r_hi),
                             hn::TruncateTo(d8h, r_lo)),
                 d8, dst + px * 4);
    } else {
      const auto r_lo = term_add(s_lo, d_lo, hn::Zero(d16));
      const auto r_hi = term_add(s_hi, d_hi, hn::Zero(d16));
      hn::StoreU(hn::Combine(d8, hn::TruncateTo(d8h, r_hi),
                             hn::TruncateTo(d8h, r_lo)),
                 d8, dst + px * 4);
    }
  }
  for (; px < count; px++) {
    // Memory words are 0xAABBGGRR; blend in the PMColor convention.
    dst32[px] = PMColorSwapRB(
        PorterDuffBlend(src_pm, PMColorSwapRB(dst32[px]), MODE));
  }
}

// kSrcOver: r = s + (1 - sa) * d
void SolidRunSrcOverKernel(uint8_t* HWY_RESTRICT dst, size_t count,
                           uint32_t src_pm) {
  SolidRunCoeffKernel<BlendMode::kSrcOver, ScaleKind::kOne, ScaleKind::kInvSrcA>(
      dst, count, src_pm);
}

// kDstOver: r = d + (1 - da) * s
void SolidRunDstOverKernel(uint8_t* HWY_RESTRICT dst, size_t count,
                           uint32_t src_pm) {
  SolidRunCoeffKernel<BlendMode::kDstOver, ScaleKind::kInvDstA,
                      ScaleKind::kOne>(dst, count, src_pm);
}

// kSrcIn: r = s * da
void SolidRunSrcInKernel(uint8_t* HWY_RESTRICT dst, size_t count,
                         uint32_t src_pm) {
  SolidRunCoeffKernel<BlendMode::kSrcIn, ScaleKind::kDstA, ScaleKind::kZero>(
      dst, count, src_pm);
}

// kDstIn: r = d * sa
void SolidRunDstInKernel(uint8_t* HWY_RESTRICT dst, size_t count,
                         uint32_t src_pm) {
  SolidRunCoeffKernel<BlendMode::kDstIn, ScaleKind::kZero, ScaleKind::kSrcA>(
      dst, count, src_pm);
}

// kSrcOut: r = s * (1 - da)
void SolidRunSrcOutKernel(uint8_t* HWY_RESTRICT dst, size_t count,
                          uint32_t src_pm) {
  SolidRunCoeffKernel<BlendMode::kSrcOut, ScaleKind::kInvDstA,
                      ScaleKind::kZero>(dst, count, src_pm);
}

// kDstOut: r = d * (1 - sa)
void SolidRunDstOutKernel(uint8_t* HWY_RESTRICT dst, size_t count,
                          uint32_t src_pm) {
  SolidRunCoeffKernel<BlendMode::kDstOut, ScaleKind::kZero,
                      ScaleKind::kInvSrcA>(dst, count, src_pm);
}

// kSrcATop: r = s * da + d * (1 - sa)
void SolidRunSrcATopKernel(uint8_t* HWY_RESTRICT dst, size_t count,
                           uint32_t src_pm) {
  SolidRunCoeffKernel<BlendMode::kSrcATop, ScaleKind::kDstA,
                      ScaleKind::kInvSrcA>(dst, count, src_pm);
}

// kDstATop: r = d * sa + s * (1 - da)
void SolidRunDstATopKernel(uint8_t* HWY_RESTRICT dst, size_t count,
                           uint32_t src_pm) {
  SolidRunCoeffKernel<BlendMode::kDstATop, ScaleKind::kInvDstA,
                      ScaleKind::kSrcA>(dst, count, src_pm);
}

// kXor: r = s * (1 - da) + d * (1 - sa)
void SolidRunXorKernel(uint8_t* HWY_RESTRICT dst, size_t count,
                       uint32_t src_pm) {
  SolidRunCoeffKernel<BlendMode::kXor, ScaleKind::kInvDstA,
                      ScaleKind::kInvSrcA>(dst, count, src_pm);
}

// kPlus: r = min(s + d, 255) per channel (saturating add).
void SolidRunPlusKernel(uint8_t* HWY_RESTRICT dst, size_t count,
                        uint32_t src_pm) {
  uint8_t src_bytes[16];
  FillSrcBytes(src_bytes, src_pm);

  const hn::CappedTag<uint8_t, 16> d8;
  const hn::Half<decltype(d8)> d8h;
  const hn::CappedTag<uint16_t, 8> d16;
  const size_t vl = hn::Lanes(d8);

  const auto src8 = hn::LoadU(d8, src_bytes);
  const auto s_lo = hn::PromoteTo(d16, hn::LowerHalf(d8h, src8));
  const auto s_hi = hn::PromoteTo(d16, hn::UpperHalf(d8h, src8));
  const auto k255 = hn::Set(d16, uint16_t{255});

  uint32_t* dst32 = reinterpret_cast<uint32_t*>(dst);
  size_t px = 0;
  for (; px + vl / 4 <= count; px += vl / 4) {
    const auto d8v = hn::LoadU(d8, dst + px * 4);
    const auto r_lo =
        hn::Min(hn::Add(s_lo, hn::PromoteTo(d16, hn::LowerHalf(d8h, d8v))),
                k255);
    const auto r_hi =
        hn::Min(hn::Add(s_hi, hn::PromoteTo(d16, hn::UpperHalf(d8h, d8v))),
                k255);
    hn::StoreU(hn::Combine(d8, hn::TruncateTo(d8h, r_hi),
                           hn::TruncateTo(d8h, r_lo)),
               d8, dst + px * 4);
  }
  for (; px < count; px++) {
    dst32[px] = PMColorSwapRB(
        PorterDuffBlend(src_pm, PMColorSwapRB(dst32[px]), BlendMode::kPlus));
  }
}

// kModulate: r = s * d per channel (MulDiv255Round rounding product).
void SolidRunModulateKernel(uint8_t* HWY_RESTRICT dst, size_t count,
                            uint32_t src_pm) {
  uint8_t src_bytes[16];
  FillSrcBytes(src_bytes, src_pm);

  const hn::CappedTag<uint8_t, 16> d8;
  const hn::Half<decltype(d8)> d8h;
  const hn::CappedTag<uint16_t, 8> d16;
  const size_t vl = hn::Lanes(d8);

  const auto src8 = hn::LoadU(d8, src_bytes);
  const auto s_lo = hn::PromoteTo(d16, hn::LowerHalf(d8h, src8));
  const auto s_hi = hn::PromoteTo(d16, hn::UpperHalf(d8h, src8));

  uint32_t* dst32 = reinterpret_cast<uint32_t*>(dst);
  size_t px = 0;
  for (; px + vl / 4 <= count; px += vl / 4) {
    const auto d8v = hn::LoadU(d8, dst + px * 4);
    const auto r_lo =
        RoundMul(s_lo, hn::PromoteTo(d16, hn::LowerHalf(d8h, d8v)));
    const auto r_hi =
        RoundMul(s_hi, hn::PromoteTo(d16, hn::UpperHalf(d8h, d8v)));
    hn::StoreU(hn::Combine(d8, hn::TruncateTo(d8h, r_hi),
                           hn::TruncateTo(d8h, r_lo)),
               d8, dst + px * 4);
  }
  for (; px < count; px++) {
    dst32[px] = PMColorSwapRB(PorterDuffBlend(
        src_pm, PMColorSwapRB(dst32[px]), BlendMode::kModulate));
  }
}

// kScreen: r = s + d - s * d per channel (rounding product, like PMColorMul).
void SolidRunScreenKernel(uint8_t* HWY_RESTRICT dst, size_t count,
                          uint32_t src_pm) {
  uint8_t src_bytes[16];
  FillSrcBytes(src_bytes, src_pm);

  const hn::CappedTag<uint8_t, 16> d8;
  const hn::Half<decltype(d8)> d8h;
  const hn::CappedTag<uint16_t, 8> d16;
  const size_t vl = hn::Lanes(d8);

  const auto src8 = hn::LoadU(d8, src_bytes);
  const auto s_lo = hn::PromoteTo(d16, hn::LowerHalf(d8h, src8));
  const auto s_hi = hn::PromoteTo(d16, hn::UpperHalf(d8h, src8));

  uint32_t* dst32 = reinterpret_cast<uint32_t*>(dst);
  size_t px = 0;
  for (; px + vl / 4 <= count; px += vl / 4) {
    const auto d8v = hn::LoadU(d8, dst + px * 4);
    const auto d_lo = hn::PromoteTo(d16, hn::LowerHalf(d8h, d8v));
    const auto d_hi = hn::PromoteTo(d16, hn::UpperHalf(d8h, d8v));
    const auto r_lo = hn::Sub(hn::Add(s_lo, d_lo), RoundMul(s_lo, d_lo));
    const auto r_hi = hn::Sub(hn::Add(s_hi, d_hi), RoundMul(s_hi, d_hi));
    hn::StoreU(hn::Combine(d8, hn::TruncateTo(d8h, r_hi),
                           hn::TruncateTo(d8h, r_lo)),
               d8, dst + px * 4);
  }
  for (; px < count; px++) {
    dst32[px] = PMColorSwapRB(
        PorterDuffBlend(src_pm, PMColorSwapRB(dst32[px]), BlendMode::kScreen));
  }
}

}  // namespace HWY_NAMESPACE
}  // namespace sw_accel
}  // namespace skity

HWY_AFTER_NAMESPACE();
