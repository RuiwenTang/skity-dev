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
#include "src/render/sw/accel/sw_accel.hpp"

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

// MulDiv255Round as a per-lane integer op: ((p + 128) + ((p + 128) >> 8)) >> 8.
// p <= 255 * 255 = 65025, and p + 128 + ((p + 128) >> 8) <= 65407 fits u16
// (and trivially u32, which the gradient path uses).
template <class V>
static inline V RoundMul(V a, V b) {
  const hn::DFromV<V> d;
  const V t = hn::Add(hn::Mul(a, b), hn::Set(d, hn::TFromV<V>{128}));
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

// ---------------------------------------------------------------------------
// Gradient fetch + blend kernels
// ---------------------------------------------------------------------------

// Fetches the gradient color per pixel and writes/blends it. The parameter t
// keeps the exact legacy float chain (MapPoint of {x + 0.5f, y + 0.5f},
// nearly-zero snaps, tile remap); the color comes from the per-draw LUT
// (built by BuildGradientLUT from the legacy stop chain) with a linear
// interpolation between adjacent entries, like Skia/blend2D CPU gradients:
//   word = lerp(lut[i], lut[i + 1], frac(t * 255)); word' = AlphaMulQ(cover)
// which stays within about 2 per channel of the scalar brush. The final
// partial group is stored masked; lanes past the span end and decal lanes
// compute harmless values (the index is clamped into the LUT) that the
// masked store / decal select discards.
template <bool kRadial, bool kBlendSrcOver>
static void GradientRunKernelT(uint8_t* HWY_RESTRICT dst,
                               const GradientParams* gp, int32_t x, int32_t y,
                               size_t count, uint32_t cover) {
  const GradientParams& p = *gp;

  const hn::CappedTag<float, 4> f32;
  const hn::CappedTag<int32_t, 4> i32;
  const hn::CappedTag<uint32_t, 4> u32;
  const hn::CappedTag<uint8_t, 16> d8;
  const hn::Half<decltype(d8)> d8h;
  const hn::CappedTag<uint16_t, 8> d16;
  const hn::CappedTag<int16_t, 8> di16;

  // MapPoint terms, in the scalar expression order
  // src.x * m00 + src.y * m01 + m02 (left-associative).
  const auto k05 = hn::Set(f32, 0.5f);
  const auto fy = hn::Set(f32, y + 0.5f);
  const auto m00 = hn::Set(f32, p.m00);
  const auto m01 = hn::Set(f32, p.m01);
  const auto m02 = hn::Set(f32, p.m02);
  const auto m10 = hn::Set(f32, p.m10);
  const auto m11 = hn::Set(f32, p.m11);
  const auto m12 = hn::Set(f32, p.m12);

  const auto zero = hn::Zero(f32);
  const auto one = hn::Set(f32, 1.0f);
  const auto two = hn::Set(f32, 2.0f);
  const auto kHalf = hn::Set(f32, 0.5f);
  const auto kEps = hn::Set(f32, 1.0f / (1 << 12));  // skity kNearlyZero

  const bool decal = p.tile_mode == static_cast<int32_t>(TileMode::kDecal);
  const bool clamp_tile =
      p.tile_mode == static_cast<int32_t>(TileMode::kClamp);
  const bool repeat_tile =
      p.tile_mode == static_cast<int32_t>(TileMode::kRepeat);
  // kMirror and kDecal keep t through RemapFloatTile's fall-through.

  // LUT addressing.
  const float lut_last = static_cast<float>(p.lut_entries - 1);
  const auto lut_scale = hn::Set(f32, lut_last);
  const auto lut_max = hn::Set(u32, static_cast<uint32_t>(p.lut_entries - 1));
  const auto k256f = hn::Set(f32, 256.0f);
  const auto k255u = hn::Set(u32, 255u);
  const auto kRound128 = hn::Set(d16, uint16_t{128});
  const auto cover16 = hn::Set(d16, static_cast<uint16_t>(cover));
  // Broadcasts each pixel's frac byte (low byte of the u32 frac) to the
  // pixel's four byte lanes, like BroadcastAlpha but from byte 0.
  alignas(16) static constexpr uint8_t kFracLanes[16] = {
      0, 0, 0, 0, 4, 4, 4, 4, 8, 8, 8, 8, 12, 12, 12, 12};

  size_t px = 0;
  for (; px < count; px += 4) {
    // ---- t -----------------------------------------------------------
    const auto fx = hn::Add(
        hn::ConvertTo(f32,
                      hn::Add(hn::Set(i32, x + static_cast<int32_t>(px)),
                              hn::Iota(i32, 0))),
        k05);
    auto t = hn::Add(hn::Add(hn::Mul(fx, m00), hn::Mul(fy, m01)), m02);
    if constexpr (kRadial) {
      const auto v = hn::Add(hn::Add(hn::Mul(fx, m10), hn::Mul(fy, m11)), m12);
      t = hn::Sqrt(hn::Add(hn::Mul(t, t), hn::Mul(v, v)));
    }
    // FloatNearlyZero snaps (<=), else-if order.
    t = hn::IfThenElse(hn::Le(hn::Abs(t), kEps), zero, t);
    t = hn::IfThenElse(hn::Le(hn::Abs(hn::Sub(t, one)), kEps), one, t);
    // kDecal outside [0, 1) -> transparent, checked before remapping.
    const auto decal_mask =
        decal ? hn::Or(hn::Lt(t, zero), hn::Ge(t, one)) : hn::MaskFalse(f32);
    // RemapFloatTile.
    if (clamp_tile) {
      t = hn::Max(zero, hn::Min(one, t));
    } else if (repeat_tile) {
      t = hn::Sub(t, hn::Floor(t));
    } else if (!decal) {  // kMirror
      const auto t1 = hn::Sub(t, one);
      const auto t2 = hn::Sub(
          hn::Sub(t1, hn::Mul(two, hn::Floor(hn::Mul(t1, kHalf)))), one);
      t = hn::Abs(t2);
    }

    // ---- LUT fetch + interpolation -------------------------------------
    const auto u = hn::Mul(t, lut_scale);
    const auto i0 = hn::Min(
        hn::Max(hn::ConvertTo(u32, u), hn::Zero(u32)), lut_max);
    const auto frac_f = hn::Sub(u, hn::ConvertTo(f32, i0));
    // Rounded weight: +0.5 before the truncating convert keeps the lerp
    // error below 1 instead of letting it accumulate with the grid error.
    const auto frac8 = hn::Min(
        hn::ConvertTo(u32, hn::Add(hn::Mul(frac_f, k256f), kHalf)),
        k255u);
    alignas(16) uint32_t i0a[4];
    hn::Store(i0, u32, i0a);

    alignas(16) uint32_t c0a[4];
    alignas(16) uint32_t c1a[4];
    for (int j = 0; j < 4; j++) {
      const uint32_t k = i0a[j];
      c0a[j] = p.lut[k];
      c1a[j] = p.lut[k + 1 == static_cast<uint32_t>(p.lut_entries) ? k : k + 1];
    }
    const auto c0_8 = hn::BitCast(d8, hn::Load(u32, c0a));
    const auto c1_8 = hn::BitCast(d8, hn::Load(u32, c1a));
    const auto frac8b = hn::TableLookupBytes(hn::BitCast(d8, frac8),
                                             hn::LoadU(d8, kFracLanes));
    const auto c16lo = hn::PromoteTo(d16, hn::LowerHalf(d8h, c0_8));
    const auto c16hi = hn::PromoteTo(d16, hn::UpperHalf(d8h, c0_8));
    // Signed per-channel deltas (two's complement in the u16 lanes): the
    // low bits of the wrapping multiply are exact for negative deltas too,
    // and the arithmetic shift below rounds like MulDiv255Round.
    const auto d16lo = hn::Sub(hn::PromoteTo(d16, hn::LowerHalf(d8h, c1_8)),
                               c16lo);
    const auto d16hi = hn::Sub(hn::PromoteTo(d16, hn::UpperHalf(d8h, c1_8)),
                               c16hi);
    const auto scaled_lo = hn::Add(
        hn::Mul(d16lo, hn::PromoteTo(d16, hn::LowerHalf(d8h, frac8b))),
        kRound128);
    const auto scaled_hi = hn::Add(
        hn::Mul(d16hi, hn::PromoteTo(d16, hn::UpperHalf(d8h, frac8b))),
        kRound128);
    auto w16lo = hn::Add(c16lo, hn::BitCast(
                                    d16, hn::ShiftRight<8>(
                                             hn::BitCast(di16, scaled_lo))));
    auto w16hi = hn::Add(c16hi, hn::BitCast(
                                    d16, hn::ShiftRight<8>(
                                             hn::BitCast(di16, scaled_hi))));
    if (cover != 255) {
      // BrushH coverage fold: AlphaMulQ, truncating per channel.
      w16lo = ScaleMul(w16lo, cover16);
      w16hi = ScaleMul(w16hi, cover16);
    }
    auto word = hn::BitCast(
        u32, hn::Combine(d8, hn::TruncateTo(d8h, w16hi),
                         hn::TruncateTo(d8h, w16lo)));
    word = hn::IfThenElse(hn::RebindMask(u32, decal_mask), hn::Zero(u32),
                          word);

    const size_t rem = count - px;
    if constexpr (!kBlendSrcOver) {
      // kSrc: store the fetched words as-is.
      if (rem >= 4) {
        hn::StoreU(word, u32, reinterpret_cast<uint32_t*>(dst + px * 4));
      } else {
        hn::BlendedStore(word, hn::FirstN(u32, rem), u32,
                         reinterpret_cast<uint32_t*>(dst + px * 4));
      }
    } else {
      // ---- variable-src SrcOver: d + (d * (256 - sa)) >> 8 -------------
      alignas(16) uint32_t words[4];
      hn::Store(word, u32, words);
      const auto s8 = hn::LoadU(d8, reinterpret_cast<const uint8_t*>(words));
      const auto sa8 = BroadcastAlpha(d8, s8);
      const auto d8v = hn::LoadU(d8, dst + px * 4);
      const auto k256 = hn::Set(d16, uint16_t{256});
      const auto s_lo = hn::PromoteTo(d16, hn::LowerHalf(d8h, s8));
      const auto s_hi = hn::PromoteTo(d16, hn::UpperHalf(d8h, s8));
      const auto scale_lo =
          hn::Sub(k256, hn::PromoteTo(d16, hn::LowerHalf(d8h, sa8)));
      const auto scale_hi =
          hn::Sub(k256, hn::PromoteTo(d16, hn::UpperHalf(d8h, sa8)));
      const auto d_lo = hn::PromoteTo(d16, hn::LowerHalf(d8h, d8v));
      const auto d_hi = hn::PromoteTo(d16, hn::UpperHalf(d8h, d8v));
      const auto r_lo =
          hn::Add(s_lo, hn::ShiftRight<8>(hn::Mul(d_lo, scale_lo)));
      const auto r_hi =
          hn::Add(s_hi, hn::ShiftRight<8>(hn::Mul(d_hi, scale_hi)));
      const auto out8 = hn::Combine(d8, hn::TruncateTo(d8h, r_hi),
                                    hn::TruncateTo(d8h, r_lo));
      if (rem >= 4) {
        hn::StoreU(out8, d8, dst + px * 4);
      } else {
        hn::BlendedStore(out8, hn::FirstN(d8, rem * 4), d8, dst + px * 4);
      }
    }
  }
}

// kSrc variants: store the fetched (coverage-folded, premul) colors.
void GradientRunLinearSrcKernel(uint8_t* HWY_RESTRICT dst,
                                const GradientParams* gp, int32_t x, int32_t y,
                                size_t count, uint32_t cover) {
  GradientRunKernelT<false, false>(dst, gp, x, y, count, cover);
}

void GradientRunRadialSrcKernel(uint8_t* HWY_RESTRICT dst,
                                const GradientParams* gp, int32_t x, int32_t y,
                                size_t count, uint32_t cover) {
  GradientRunKernelT<true, false>(dst, gp, x, y, count, cover);
}

// kSrcOver variants: fetch, then blend as a variable-src SrcOver.
void GradientRunLinearSrcOverKernel(uint8_t* HWY_RESTRICT dst,
                                    const GradientParams* gp, int32_t x,
                                    int32_t y, size_t count, uint32_t cover) {
  GradientRunKernelT<false, true>(dst, gp, x, y, count, cover);
}

void GradientRunRadialSrcOverKernel(uint8_t* HWY_RESTRICT dst,
                                    const GradientParams* gp, int32_t x,
                                    int32_t y, size_t count, uint32_t cover) {
  GradientRunKernelT<true, true>(dst, gp, x, y, count, cover);
}

}  // namespace HWY_NAMESPACE
}  // namespace sw_accel
}  // namespace skity

HWY_AFTER_NAMESPACE();
