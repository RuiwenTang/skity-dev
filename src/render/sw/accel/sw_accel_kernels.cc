// Copyright 2021 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

// Kernel body for the SW accelerator. Re-included once per SIMD target by
// sw_accel.cpp through hwy/foreach_target.h; must not be compiled directly.
//
// Width-generic: ScalableTag (the target's native width) everywhere, so the
// same source instantiates at 128-bit on NEON/SSE and 256/512-bit on
// AVX2/AVX-512. The LowerHalf/UpperHalf pairs and the Combine in each loop
// are expressed against the full-width tags and adapt automatically; shuffle
// control is built arithmetically from Iota instead of 16-byte tables.

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

// Expands the constant src color into one group (Lanes(d8) / 4 identical
// kRGBA pixels) for the vector loops.
static inline void FillSrcBytes(uint8_t* src_bytes, uint32_t src_pm,
                                size_t pixels) {
  for (size_t px = 0; px < pixels; px++) {
    src_bytes[px * 4 + 0] = static_cast<uint8_t>(src_pm >> 16);  // R
    src_bytes[px * 4 + 1] = static_cast<uint8_t>(src_pm >> 8);   // G
    src_bytes[px * 4 + 2] = static_cast<uint8_t>(src_pm);        // B
    src_bytes[px * 4 + 3] = static_cast<uint8_t>(src_pm >> 24);  // A
  }
}

// Replicates the byte at offset `lane_byte` of every 4-byte pixel into the
// pixel's four byte lanes, so a per-pixel scale can be built after
// PromoteTo. The shuffle control is arithmetic ((i & ~3) + lane_byte), which
// stays correct at any vector width because pixels never cross 128-bit
// lanes. Used for alpha (byte 3) and for the gradient LUT fraction (byte 0).
template <class D8, class V8>
static inline V8 BroadcastLaneByte(D8 d8, V8 px, uint8_t lane_byte) {
  const auto idx =
      hn::Add(hn::And(hn::Iota(d8, 0), hn::Set(d8, uint8_t{0xFC})),
              hn::Set(d8, lane_byte));
  return hn::TableLookupBytes(px, idx);
}

void SolidRunClearKernel(uint8_t* HWY_RESTRICT dst, size_t count,
                         uint32_t src_pm) {
  (void)src_pm;
  memset(dst, 0, count * 4);
}

void SolidRunSrcKernel(uint8_t* HWY_RESTRICT dst, size_t count,
                       uint32_t src_pm) {
  const uint32_t src_mem = PMColorSwapRB(src_pm);
  const hn::ScalableTag<uint32_t> d32;
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

  alignas(64) uint8_t src_bytes[64];

  const hn::ScalableTag<uint8_t> d8;
  const hn::Half<decltype(d8)> d8h;
  // Same total bytes as the u8 halves: Rebind keeps lane counts, so the u16
  // tag shares its lane count with d8h.
  const hn::Rebind<uint16_t, decltype(d8h)> d16;
  const size_t vl = hn::Lanes(d8);  // bytes; one group is vl / 4 pixels

  FillSrcBytes(src_bytes, src_pm, vl / 4);
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
      const auto a8 = BroadcastLaneByte(d8, d8v, 3);
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
  alignas(64) uint8_t src_bytes[64];

  const hn::ScalableTag<uint8_t> d8;
  const hn::Half<decltype(d8)> d8h;
  const hn::Rebind<uint16_t, decltype(d8h)> d16;
  const size_t vl = hn::Lanes(d8);

  FillSrcBytes(src_bytes, src_pm, vl / 4);

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
  alignas(64) uint8_t src_bytes[64];

  const hn::ScalableTag<uint8_t> d8;
  const hn::Half<decltype(d8)> d8h;
  const hn::Rebind<uint16_t, decltype(d8h)> d16;
  const size_t vl = hn::Lanes(d8);

  FillSrcBytes(src_bytes, src_pm, vl / 4);

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
  alignas(64) uint8_t src_bytes[64];

  const hn::ScalableTag<uint8_t> d8;
  const hn::Half<decltype(d8)> d8h;
  const hn::Rebind<uint16_t, decltype(d8h)> d16;
  const size_t vl = hn::Lanes(d8);

  FillSrcBytes(src_bytes, src_pm, vl / 4);

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
// Advanced blend kernels
// ---------------------------------------------------------------------------

// Computes one advanced (non-coeff) blend component per byte lane in the
// interleaved layout: the lane holds a premultiplied channel, and the alpha
// operands arrive pre-broadcast to every lane of their pixel. Formulas
// mirror the scalar AdvancedBlend reference (and behind it the WGSL
// functions) expression for expression; divisions are IEEE-exact vector
// divisions, guards are lane-wise selects whose unused branches may produce
// Inf/NaN that the select discards, exactly like the scalar select.
template <BlendMode MODE, class F>
static inline F AdvancedComponent(F sf, F saf, F df, F daf) {
  const hn::DFromV<F> f32;
  const F zero = hn::Zero(f32);
  const F one = hn::Set(f32, 1.0f);
  const F two = hn::Set(f32, 2.0f);
  const F kMinNormalHalf = hn::Set(f32, 0.0001f);
  if constexpr (MODE == BlendMode::kOverlay || MODE == BlendMode::kHardLight) {
    // HardLight is Overlay with the operands swapped.
    const F sc = MODE == BlendMode::kHardLight ? df : sf;
    const F sa = MODE == BlendMode::kHardLight ? daf : saf;
    const F dc = MODE == BlendMode::kHardLight ? sf : df;
    const F da = MODE == BlendMode::kHardLight ? saf : daf;
    const F mul2 = hn::Mul(two, dc);
    const F base = hn::IfThenElse(hn::Le(mul2, da), hn::Mul(two, hn::Mul(sc, dc)),
                                  hn::Sub(hn::Mul(sa, da),
                                          hn::Mul(two, hn::Mul(hn::Sub(da, dc),
                                                               hn::Sub(sa, sc)))));
    // extra = dst * (1 - sa) + src * (1 - da), alpha zeroed.
    return hn::Add(base, hn::Add(hn::Mul(df, hn::Sub(one, saf)),
                                 hn::Mul(sf, hn::Sub(one, daf))));
  } else if constexpr (MODE == BlendMode::kDarken ||
                       MODE == BlendMode::kLighten) {
    const F over = hn::Add(sf, hn::Mul(hn::Sub(one, saf), df));
    const F other = hn::Add(hn::Mul(hn::Sub(one, daf), sf), df);
    if constexpr (MODE == BlendMode::kDarken) {
      return hn::Min(over, other);
    } else {
      return hn::Max(over, other);
    }
  } else if constexpr (MODE == BlendMode::kColorDodge) {
    const F dx_scale = hn::IfThenElse(hn::Eq(df, zero), zero, one);
    const F guarded = hn::IfThenElse(
        hn::Ge(hn::Abs(hn::Sub(saf, sf)), kMinNormalHalf),
        hn::Div(hn::Mul(df, saf), hn::Sub(saf, sf)), daf);
    const F delta = hn::Mul(dx_scale, hn::Min(daf, guarded));
    return hn::Add(hn::Add(hn::Mul(delta, saf),
                           hn::Mul(sf, hn::Sub(one, daf))),
                   hn::Mul(df, hn::Sub(one, saf)));
  } else if constexpr (MODE == BlendMode::kColorBurn) {
    const F dy_term = hn::IfThenElse(hn::Eq(daf, df), daf, zero);
    const F delta = hn::IfThenElse(
        hn::Ge(hn::Abs(sf), kMinNormalHalf),
        hn::Sub(daf, hn::Min(daf, hn::Div(hn::Mul(hn::Sub(daf, df), saf), sf))),
        dy_term);
    return hn::Add(hn::Add(hn::Mul(delta, saf),
                           hn::Mul(sf, hn::Sub(one, daf))),
                   hn::Mul(df, hn::Sub(one, saf)));
  } else if constexpr (MODE == BlendMode::kSoftLight) {
    // sa - 2*sc (reused) and -sa + 2*sc + 1, mirrored left-to-right from the
    // scalar helper (IEEE addition commutes bit-exactly, so Sub orderings
    // that differ by sign only are still exact).
    const F sm2 = hn::Sub(saf, hn::Mul(two, sf));
    const F m2s_p1 = hn::Add(hn::Sub(hn::Mul(two, sf), saf), one);
    const F branch_a =
        hn::Add(hn::Add(hn::Div(hn::Mul(hn::Mul(df, df), sm2), daf),
                        hn::Mul(hn::Sub(one, daf), sf)),
                hn::Mul(df, m2s_p1));
    // (da^2 * (sc - dc*(3*sa - 6*sc - 1)) + 12*da*dc^2*(sa-2*sc)
    //  - 16*dc^3*(sa-2*sc) - da^3*sc) / da^2, same association as scalar.
    const F d_sq = hn::Mul(df, df);
    const F d_cu = hn::Mul(d_sq, df);
    const F da_sq = hn::Mul(daf, daf);
    const F da_cu = hn::Mul(da_sq, daf);
    const F inner3 =
        hn::Sub(hn::Sub(hn::Mul(hn::Set(f32, 3.f), saf),
                        hn::Mul(hn::Set(f32, 6.f), sf)),
                one);
    const F t_a = hn::Mul(da_sq, hn::Sub(sf, hn::Mul(df, inner3)));
    const F t_b = hn::Mul(hn::Mul(hn::Set(f32, 12.f), daf),
                          hn::Mul(d_sq, sm2));
    const F t_c = hn::Mul(hn::Mul(hn::Set(f32, 16.f), d_cu), sm2);
    const F t_d = hn::Mul(da_cu, sf);
    const F branch_b = hn::Div(hn::Sub(hn::Sub(hn::Add(t_a, t_b), t_c), t_d),
                               da_sq);
    const F branch_c =
        hn::Sub(hn::Sub(hn::Add(hn::Mul(df, hn::Add(sm2, one)), sf),
                        hn::Mul(hn::Sqrt(hn::Mul(daf, df)), sm2)),
                hn::Mul(daf, sf));
    F res = hn::IfThenElse(hn::Le(hn::Mul(two, sf), saf), branch_a,
                           hn::IfThenElse(
                               hn::Le(hn::Mul(hn::Set(f32, 4.f), df), daf),
                               branch_b, branch_c));
    // dst.a == 0 -> the whole result is src.
    res = hn::IfThenElse(hn::Eq(daf, zero), sf, res);
    return res;
  } else if constexpr (MODE == BlendMode::kDifference) {
    return hn::Sub(hn::Add(sf, df),
                   hn::Mul(two, hn::Min(hn::Mul(sf, daf), hn::Mul(df, saf))));
  } else if constexpr (MODE == BlendMode::kExclusion) {
    return hn::Sub(hn::Add(df, sf), hn::Mul(two, hn::Mul(df, sf)));
  } else if constexpr (MODE == BlendMode::kMultiply) {
    return hn::Add(hn::Add(hn::Mul(hn::Sub(one, saf), df),
                           hn::Mul(hn::Sub(one, daf), sf)),
                   hn::Mul(sf, df));
  }
}

// Engine for the separable advanced modes. Each group is processed as
// quarter-width u8 sub-chunks converted to f32 (one lane per byte), the
// component formula runs lane-wise with pre-broadcast alphas, and the result
// is spliced with the composite alpha (sa + (1 - sa) * da, or src on
// SoftLight's transparent-dst path) back into the interleaved bytes.
// Quantization mirrors the scalar reference exactly: clamp [0, 1], * 255,
// + 0.5, truncating convert.
template <BlendMode MODE>
static void AdvancedRunKernelT(uint8_t* HWY_RESTRICT dst, size_t count,
                               uint32_t src_pm) {
  const hn::ScalableTag<uint8_t> d8;
  const hn::Half<hn::Half<decltype(d8)>> df8;
  const hn::Rebind<uint32_t, decltype(df8)> du32;
  const hn::Rebind<float, decltype(df8)> df32;
  const size_t vl = hn::Lanes(d8);
  const size_t sub = hn::Lanes(df8);  // f32 lanes per sub-chunk

  alignas(64) uint8_t src_bytes[64];
  FillSrcBytes(src_bytes, src_pm, vl / 4);

  const auto k255f = hn::Set(df32, 255.0f);
  const auto kHalf = hn::Set(df32, 0.5f);
  const auto kOne = hn::Set(df32, 1.0f);
  const auto kInv255 = hn::Set(df32, 255.0f);
  const auto zero32 = hn::Zero(du32);

  // Lane mask selecting each pixel's alpha byte.
  const auto alpha_lanes = hn::Eq(hn::And(hn::Iota(df8, 0), hn::Set(df8, uint8_t{3})),
                                  hn::Set(df8, uint8_t{3}));

  uint32_t* dst32 = reinterpret_cast<uint32_t*>(dst);
  size_t px = 0;
  for (; px + vl / 4 <= count; px += vl / 4) {
    for (size_t off = 0; off < vl; off += sub) {
      const auto s8 = hn::LoadU(df8, src_bytes + off);
      const auto d8c = hn::LoadU(df8, dst + px * 4 + off);
      const auto sa8 = BroadcastLaneByte(df8, s8, 3);
      const auto da8 = BroadcastLaneByte(df8, d8c, 3);
      const auto sf = hn::Div(
          hn::ConvertTo(df32, hn::PromoteTo(du32, s8)), kInv255);
      const auto df = hn::Div(
          hn::ConvertTo(df32, hn::PromoteTo(du32, d8c)), kInv255);
      const auto saf = hn::Div(
          hn::ConvertTo(df32, hn::PromoteTo(du32, sa8)), kInv255);
      const auto daf = hn::Div(
          hn::ConvertTo(df32, hn::PromoteTo(du32, da8)), kInv255);

      const auto rf = AdvancedComponent<MODE>(sf, saf, df, daf);
      auto af = hn::Add(saf, hn::Mul(hn::Sub(kOne, saf), daf));
      if constexpr (MODE == BlendMode::kSoftLight) {
        af = hn::IfThenElse(hn::Eq(daf, hn::Zero(df32)), saf, af);
      }

      const auto quantize = [&](const hn::Vec<decltype(df32)> v) {
        const auto clamped = hn::Min(kOne, hn::Max(hn::Zero(df32), v));
        return hn::ConvertTo(du32, hn::Add(hn::Mul(clamped, k255f), kHalf));
      };
      const auto qc = quantize(rf);
      const auto qa = quantize(af);
      const auto out8 =
          hn::IfThenElse(alpha_lanes, hn::TruncateTo(df8, qa),
                         hn::TruncateTo(df8, qc));
      hn::StoreU(out8, df8, dst + px * 4 + off);
    }
  }
  for (; px < count; px++) {
    dst32[px] = PMColorSwapRB(
        PorterDuffBlend(src_pm, PMColorSwapRB(dst32[px]), MODE));
  }
}

// The four HSL modes mix channels (luminance / saturation across rgb), which
// the interleaved lane layout cannot express cheaply; they run the scalar
// reference per pixel behind the same kernel-table interface.
template <BlendMode MODE>
static void HSLRunKernelT(uint8_t* HWY_RESTRICT dst, size_t count,
                          uint32_t src_pm) {
  uint32_t* dst32 = reinterpret_cast<uint32_t*>(dst);
  for (size_t i = 0; i < count; i++) {
    dst32[i] =
        PMColorSwapRB(PorterDuffBlend(src_pm, PMColorSwapRB(dst32[i]), MODE));
  }
}

void SolidRunOverlayKernel(uint8_t* HWY_RESTRICT dst, size_t count,
                           uint32_t src_pm) {
  AdvancedRunKernelT<BlendMode::kOverlay>(dst, count, src_pm);
}

void SolidRunDarkenKernel(uint8_t* HWY_RESTRICT dst, size_t count,
                          uint32_t src_pm) {
  AdvancedRunKernelT<BlendMode::kDarken>(dst, count, src_pm);
}

void SolidRunLightenKernel(uint8_t* HWY_RESTRICT dst, size_t count,
                           uint32_t src_pm) {
  AdvancedRunKernelT<BlendMode::kLighten>(dst, count, src_pm);
}

void SolidRunColorDodgeKernel(uint8_t* HWY_RESTRICT dst, size_t count,
                              uint32_t src_pm) {
  AdvancedRunKernelT<BlendMode::kColorDodge>(dst, count, src_pm);
}

void SolidRunColorBurnKernel(uint8_t* HWY_RESTRICT dst, size_t count,
                             uint32_t src_pm) {
  AdvancedRunKernelT<BlendMode::kColorBurn>(dst, count, src_pm);
}

void SolidRunHardLightKernel(uint8_t* HWY_RESTRICT dst, size_t count,
                             uint32_t src_pm) {
  AdvancedRunKernelT<BlendMode::kHardLight>(dst, count, src_pm);
}

void SolidRunSoftLightKernel(uint8_t* HWY_RESTRICT dst, size_t count,
                             uint32_t src_pm) {
  AdvancedRunKernelT<BlendMode::kSoftLight>(dst, count, src_pm);
}

void SolidRunDifferenceKernel(uint8_t* HWY_RESTRICT dst, size_t count,
                              uint32_t src_pm) {
  AdvancedRunKernelT<BlendMode::kDifference>(dst, count, src_pm);
}

void SolidRunExclusionKernel(uint8_t* HWY_RESTRICT dst, size_t count,
                             uint32_t src_pm) {
  AdvancedRunKernelT<BlendMode::kExclusion>(dst, count, src_pm);
}

void SolidRunMultiplyKernel(uint8_t* HWY_RESTRICT dst, size_t count,
                           uint32_t src_pm) {
  AdvancedRunKernelT<BlendMode::kMultiply>(dst, count, src_pm);
}

void SolidRunHueKernel(uint8_t* HWY_RESTRICT dst, size_t count,
                       uint32_t src_pm) {
  HSLRunKernelT<BlendMode::kHue>(dst, count, src_pm);
}

void SolidRunSaturationKernel(uint8_t* HWY_RESTRICT dst, size_t count,
                              uint32_t src_pm) {
  HSLRunKernelT<BlendMode::kSaturation>(dst, count, src_pm);
}

void SolidRunColorKernel(uint8_t* HWY_RESTRICT dst, size_t count,
                         uint32_t src_pm) {
  HSLRunKernelT<BlendMode::kColor>(dst, count, src_pm);
}

void SolidRunLuminosityKernel(uint8_t* HWY_RESTRICT dst, size_t count,
                              uint32_t src_pm) {
  HSLRunKernelT<BlendMode::kLuminosity>(dst, count, src_pm);
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

  // Width-generic tags: d8 spans the native width (one group is Lanes(d8) / 4
  // pixels); f32/i32/u32 carry one lane per pixel (same bytes as d8, a
  // quarter of the u8 lanes - Rebind keeps lane counts, hence two Halves).
  const hn::ScalableTag<uint8_t> d8;
  const hn::Half<decltype(d8)> d8h;
  const hn::Rebind<uint16_t, decltype(d8h)> d16;
  const hn::Rebind<int16_t, decltype(d16)> di16;
  const hn::Half<hn::Half<hn::Rebind<float, decltype(d8)>>> f32;
  const hn::Half<hn::Half<hn::Rebind<int32_t, decltype(d8)>>> i32;
  const hn::Half<hn::Half<hn::Rebind<uint32_t, decltype(d8)>>> u32;
  const size_t vl = hn::Lanes(d8);  // bytes; one group is vl / 4 pixels

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

  const size_t group = vl / 4;  // pixels per iteration
  size_t px = 0;
  for (; px < count; px += group) {
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
    alignas(64) uint32_t i0a[16];
    hn::Store(i0, u32, i0a);

    alignas(64) uint32_t c0a[16];
    alignas(64) uint32_t c1a[16];
    for (size_t j = 0; j < vl / 4; j++) {
      const uint32_t k = i0a[j];
      c0a[j] = p.lut[k];
      c1a[j] = p.lut[k + 1 == static_cast<uint32_t>(p.lut_entries) ? k : k + 1];
    }
    const auto c0_8 = hn::BitCast(d8, hn::Load(u32, c0a));
    const auto c1_8 = hn::BitCast(d8, hn::Load(u32, c1a));
    const auto frac8b = BroadcastLaneByte(d8, hn::BitCast(d8, frac8), 0);
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
      if (rem >= group) {
        hn::StoreU(word, u32, reinterpret_cast<uint32_t*>(dst + px * 4));
      } else {
        hn::BlendedStore(word, hn::FirstN(u32, rem), u32,
                         reinterpret_cast<uint32_t*>(dst + px * 4));
      }
    } else {
      // ---- variable-src SrcOver: d + (d * (256 - sa)) >> 8 -------------
      alignas(64) uint32_t words[16];
      hn::Store(word, u32, words);
      const auto s8 = hn::LoadU(d8, reinterpret_cast<const uint8_t*>(words));
      const auto sa8 = BroadcastLaneByte(d8, s8, 3);
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
      if (rem >= group) {
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
