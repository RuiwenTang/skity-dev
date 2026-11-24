// Copyright 2021 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

// Multi-target entry for the SW accelerator. foreach_target.h re-includes
// the kernel body once per enabled SIMD target; the direct include after it
// compiles the static (baseline) target and finalizes the dispatch table.

#undef HWY_TARGET_INCLUDE
#define HWY_TARGET_INCLUDE "src/render/sw/accel/sw_accel_kernels.cc"
#include <hwy/foreach_target.h>

#include <atomic>
#include <cmath>

#include <cstring>
#include <mutex>

#include "src/graphic/blend_mode_priv.hpp"
#include "src/graphic/color_priv.hpp"
#include "src/render/sw/accel/sw_accel.hpp"
#include "src/render/sw/accel/sw_accel_kernels.cc"

namespace skity {
namespace sw_accel {

HWY_EXPORT(SolidRunClearKernel);
HWY_EXPORT(SolidRunSrcKernel);
HWY_EXPORT(SolidRunDstKernel);
HWY_EXPORT(SolidRunSrcOverKernel);
HWY_EXPORT(SolidRunDstOverKernel);
HWY_EXPORT(SolidRunSrcInKernel);
HWY_EXPORT(SolidRunDstInKernel);
HWY_EXPORT(SolidRunSrcOutKernel);
HWY_EXPORT(SolidRunDstOutKernel);
HWY_EXPORT(SolidRunSrcATopKernel);
HWY_EXPORT(SolidRunDstATopKernel);
HWY_EXPORT(SolidRunXorKernel);
HWY_EXPORT(SolidRunPlusKernel);
HWY_EXPORT(SolidRunModulateKernel);
HWY_EXPORT(SolidRunScreenKernel);
HWY_EXPORT(SolidRunOverlayKernel);
HWY_EXPORT(SolidRunDarkenKernel);
HWY_EXPORT(SolidRunLightenKernel);
HWY_EXPORT(SolidRunColorDodgeKernel);
HWY_EXPORT(SolidRunColorBurnKernel);
HWY_EXPORT(SolidRunHardLightKernel);
HWY_EXPORT(SolidRunSoftLightKernel);
HWY_EXPORT(SolidRunDifferenceKernel);
HWY_EXPORT(SolidRunExclusionKernel);
HWY_EXPORT(SolidRunMultiplyKernel);
HWY_EXPORT(SolidRunHueKernel);
HWY_EXPORT(SolidRunSaturationKernel);
HWY_EXPORT(SolidRunColorKernel);
HWY_EXPORT(SolidRunLuminosityKernel);

HWY_EXPORT(GradientRunLinearSrcKernel);
HWY_EXPORT(GradientRunRadialSrcKernel);
HWY_EXPORT(GradientRunLinearSrcOverKernel);
HWY_EXPORT(GradientRunRadialSrcOverKernel);

namespace {

using RunKernelFn = void (*)(uint8_t*, size_t, uint32_t);

using GradientKernelFn = void (*)(uint8_t*, const GradientParams*, int32_t,
                                  int32_t, size_t, uint32_t);

GradientKernelFn SelectGradientKernel(const GradientParams& params,
                                      BlendMode mode) {
  const bool radial = params.type == 1;
  switch (mode) {
    case BlendMode::kSrc:
      return radial ? HWY_DYNAMIC_DISPATCH(GradientRunRadialSrcKernel)
                    : HWY_DYNAMIC_DISPATCH(GradientRunLinearSrcKernel);
    case BlendMode::kSrcOver:
      return radial ? HWY_DYNAMIC_DISPATCH(GradientRunRadialSrcOverKernel)
                    : HWY_DYNAMIC_DISPATCH(GradientRunLinearSrcOverKernel);
    default:
      return nullptr;
  }
}

RunKernelFn SelectRunKernel(BlendMode mode) {
  switch (mode) {
    case BlendMode::kClear:
      return HWY_DYNAMIC_DISPATCH(SolidRunClearKernel);
    case BlendMode::kSrc:
      return HWY_DYNAMIC_DISPATCH(SolidRunSrcKernel);
    case BlendMode::kDst:
      return HWY_DYNAMIC_DISPATCH(SolidRunDstKernel);
    case BlendMode::kSrcOver:
      return HWY_DYNAMIC_DISPATCH(SolidRunSrcOverKernel);
    case BlendMode::kDstOver:
      return HWY_DYNAMIC_DISPATCH(SolidRunDstOverKernel);
    case BlendMode::kSrcIn:
      return HWY_DYNAMIC_DISPATCH(SolidRunSrcInKernel);
    case BlendMode::kDstIn:
      return HWY_DYNAMIC_DISPATCH(SolidRunDstInKernel);
    case BlendMode::kSrcOut:
      return HWY_DYNAMIC_DISPATCH(SolidRunSrcOutKernel);
    case BlendMode::kDstOut:
      return HWY_DYNAMIC_DISPATCH(SolidRunDstOutKernel);
    case BlendMode::kSrcATop:
      return HWY_DYNAMIC_DISPATCH(SolidRunSrcATopKernel);
    case BlendMode::kDstATop:
      return HWY_DYNAMIC_DISPATCH(SolidRunDstATopKernel);
    case BlendMode::kXor:
      return HWY_DYNAMIC_DISPATCH(SolidRunXorKernel);
    case BlendMode::kPlus:
      return HWY_DYNAMIC_DISPATCH(SolidRunPlusKernel);
    case BlendMode::kModulate:
      return HWY_DYNAMIC_DISPATCH(SolidRunModulateKernel);
    case BlendMode::kScreen:
      return HWY_DYNAMIC_DISPATCH(SolidRunScreenKernel);
    case BlendMode::kOverlay:
      return HWY_DYNAMIC_DISPATCH(SolidRunOverlayKernel);
    case BlendMode::kDarken:
      return HWY_DYNAMIC_DISPATCH(SolidRunDarkenKernel);
    case BlendMode::kLighten:
      return HWY_DYNAMIC_DISPATCH(SolidRunLightenKernel);
    case BlendMode::kColorDodge:
      return HWY_DYNAMIC_DISPATCH(SolidRunColorDodgeKernel);
    case BlendMode::kColorBurn:
      return HWY_DYNAMIC_DISPATCH(SolidRunColorBurnKernel);
    case BlendMode::kHardLight:
      return HWY_DYNAMIC_DISPATCH(SolidRunHardLightKernel);
    case BlendMode::kSoftLight:
      return HWY_DYNAMIC_DISPATCH(SolidRunSoftLightKernel);
    case BlendMode::kDifference:
      return HWY_DYNAMIC_DISPATCH(SolidRunDifferenceKernel);
    case BlendMode::kExclusion:
      return HWY_DYNAMIC_DISPATCH(SolidRunExclusionKernel);
    case BlendMode::kMultiply:
      return HWY_DYNAMIC_DISPATCH(SolidRunMultiplyKernel);
    case BlendMode::kHue:
      return HWY_DYNAMIC_DISPATCH(SolidRunHueKernel);
    case BlendMode::kSaturation:
      return HWY_DYNAMIC_DISPATCH(SolidRunSaturationKernel);
    case BlendMode::kColor:
      return HWY_DYNAMIC_DISPATCH(SolidRunColorKernel);
    case BlendMode::kLuminosity:
      return HWY_DYNAMIC_DISPATCH(SolidRunLuminosityKernel);
    default:
      return nullptr;
  }
}

// Scalar reference of the legacy solid path: fold coverage into src, then
// the real PorterDuffBlend. Takes and returns memory words (0xAABBGGRR,
// ColorType::kRGBA); used by Validate() to check the dispatched kernels.
uint32_t ReferenceBlend(BlendMode mode, uint32_t src_pm, uint32_t dst_mem,
                        uint8_t cover) {
  const uint32_t src = cover == 255 ? src_pm : AlphaMulQ(src_pm, cover);
  return PMColorSwapRB(PorterDuffBlend(src, PMColorSwapRB(dst_mem), mode));
}

// Scalar mirror of the legacy gradient chain (GradientColorBrush::
// LerpColor through the rounding premul), evaluated at an explicit t and
// producing the premul memory word (0xAABBGGRR). BuildGradientLUT bakes it
// into the per-draw LUT; Validate() also uses it as the known answer.
uint32_t ReferenceGradientColor(const GradientParams& p, float t) {
  const float kEps = 1.0f / (1 << 12);  // skity kNearlyZero
  if (std::abs(t) <= kEps) {
    t = 0.0f;
  } else if (std::abs(t - 1.0f) <= kEps) {
    t = 1.0f;
  }

  float rgba[4] = {0.f, 0.f, 0.f, 0.f};
  const bool decal = p.tile_mode == static_cast<int32_t>(TileMode::kDecal);
  if (!(decal && (t < 0.0f || t >= 1.0f))) {
    if (p.tile_mode == static_cast<int32_t>(TileMode::kClamp)) {
      t = std::min(1.f, std::max(0.f, t));
    } else if (p.tile_mode == static_cast<int32_t>(TileMode::kRepeat)) {
      t = t - std::floor(t);
    } else if (p.tile_mode == static_cast<int32_t>(TileMode::kMirror)) {
      float t1 = t - 1;
      float t2 = t1 - 2 * std::floor(t1 * 0.5f) - 1;
      t = std::abs(t2);
    }
    // Stop scan: first matching segment, then the two early-outs.
    int32_t start_index = 0;
    float start = 0.f;
    float end = 0.f;
    int32_t i = 0;
    for (i = 0; i < p.color_count - 1; i++) {
      start = p.seg_start[i];
      end = p.seg_end[i];
      if (t >= start && t <= end) {
        start_index = i;
        break;
      }
    }
    const float* c0;
    const float* c1 = nullptr;
    if (p.use_offsets && t <= p.first_offset) {
      c0 = p.colors;
    } else if (i == p.color_count - 1) {
      c0 = p.colors + (p.color_count - 1) * 4;
    } else {
      c0 = p.colors + start_index * 4;
      c1 = p.colors + (start_index + 1) * 4;
    }
    if (c1 == nullptr) {
      for (int c = 0; c < 4; c++) rgba[c] = c0[c];
    } else {
      float total = end - start;
      float value = t - start;
      float mix = 0.5f;
      if (total > 0) {
        mix = value / total;
      }
      for (int c = 0; c < 4; c++) {
        rgba[c] = c0[c] * (1 - mix) + c1[c] * mix;
      }
    }
  }

  // Color4fToColor: truncating cast of clamp(v * 255, 0, 255).
  const auto to_u8 = [](float v) {
    return static_cast<uint32_t>(
        std::min(255.f, std::max(0.f, v * 255.f)));
  };
  uint32_t ch[4] = {to_u8(rgba[0]), to_u8(rgba[1]), to_u8(rgba[2]),
                    to_u8(rgba[3])};
  // ColorToPMColor: rounding premul against the alpha channel.
  if (ch[3] != 255) {
    for (int c = 0; c < 3; c++) {
      ch[c] = MulDiv255Round(ch[c], ch[3]);
    }
  }
  return ch[0] | (ch[1] << 8) | (ch[2] << 16) | (ch[3] << 24);
}

// Computes the gradient parameter t exactly like the legacy MapPoint path.
float ReferenceGradientT(const GradientParams& p, int32_t x, int32_t y) {
  const float fx = x + 0.5f;
  const float fy = y + 0.5f;
  const float u = fx * p.m00 + fy * p.m01 + p.m02;
  if (p.type == 1) {
    const float v = fx * p.m10 + fy * p.m11 + p.m12;
    return std::sqrt(u * u + v * v);
  }
  return u;
}

std::once_flag g_validate_once;
bool g_available = false;
std::atomic<bool> g_runtime_enabled{true};

void ValidateGradients();

void ValidateOnce() {
  // Same deterministic pattern shape as the unit tests: rows longer than a
  // vector plus tail, and alpha edges (0, 128, 255). Dispatches the kernels
  // directly - going through FillSolidSpans would re-enter this call_once.
  struct Entry {
    BlendMode mode;
    RunKernelFn kernel;
  };
  const Entry entries[] = {
      {BlendMode::kClear, HWY_DYNAMIC_DISPATCH(SolidRunClearKernel)},
      {BlendMode::kSrc, HWY_DYNAMIC_DISPATCH(SolidRunSrcKernel)},
      {BlendMode::kDst, HWY_DYNAMIC_DISPATCH(SolidRunDstKernel)},
      {BlendMode::kSrcOver, HWY_DYNAMIC_DISPATCH(SolidRunSrcOverKernel)},
      {BlendMode::kDstOver, HWY_DYNAMIC_DISPATCH(SolidRunDstOverKernel)},
      {BlendMode::kSrcIn, HWY_DYNAMIC_DISPATCH(SolidRunSrcInKernel)},
      {BlendMode::kDstIn, HWY_DYNAMIC_DISPATCH(SolidRunDstInKernel)},
      {BlendMode::kSrcOut, HWY_DYNAMIC_DISPATCH(SolidRunSrcOutKernel)},
      {BlendMode::kDstOut, HWY_DYNAMIC_DISPATCH(SolidRunDstOutKernel)},
      {BlendMode::kSrcATop, HWY_DYNAMIC_DISPATCH(SolidRunSrcATopKernel)},
      {BlendMode::kDstATop, HWY_DYNAMIC_DISPATCH(SolidRunDstATopKernel)},
      {BlendMode::kXor, HWY_DYNAMIC_DISPATCH(SolidRunXorKernel)},
      {BlendMode::kPlus, HWY_DYNAMIC_DISPATCH(SolidRunPlusKernel)},
      {BlendMode::kModulate, HWY_DYNAMIC_DISPATCH(SolidRunModulateKernel)},
      {BlendMode::kScreen, HWY_DYNAMIC_DISPATCH(SolidRunScreenKernel)},
      {BlendMode::kOverlay, HWY_DYNAMIC_DISPATCH(SolidRunOverlayKernel)},
      {BlendMode::kDarken, HWY_DYNAMIC_DISPATCH(SolidRunDarkenKernel)},
      {BlendMode::kLighten, HWY_DYNAMIC_DISPATCH(SolidRunLightenKernel)},
      {BlendMode::kColorDodge, HWY_DYNAMIC_DISPATCH(SolidRunColorDodgeKernel)},
      {BlendMode::kColorBurn, HWY_DYNAMIC_DISPATCH(SolidRunColorBurnKernel)},
      {BlendMode::kHardLight, HWY_DYNAMIC_DISPATCH(SolidRunHardLightKernel)},
      {BlendMode::kDifference, HWY_DYNAMIC_DISPATCH(SolidRunDifferenceKernel)},
      {BlendMode::kExclusion, HWY_DYNAMIC_DISPATCH(SolidRunExclusionKernel)},
      {BlendMode::kMultiply, HWY_DYNAMIC_DISPATCH(SolidRunMultiplyKernel)},
      // kSoftLight is deliberately absent: Highway's NEON Sqrt is an
      // rsqrte+NR approximation, so the kernel differs from std::sqrt by
      // ~1 ulp and a bit-exact fuse would trip; the unit tests cover it
      // with a +/-1 tolerance instead.
      {BlendMode::kHue, HWY_DYNAMIC_DISPATCH(SolidRunHueKernel)},
      {BlendMode::kSaturation, HWY_DYNAMIC_DISPATCH(SolidRunSaturationKernel)},
      {BlendMode::kColor, HWY_DYNAMIC_DISPATCH(SolidRunColorKernel)},
      {BlendMode::kLuminosity, HWY_DYNAMIC_DISPATCH(SolidRunLuminosityKernel)},
  };
  // Premul src colors (channel values bounded by alpha): the mixed
  // Porter-Duff sums are byte-safe only on premul operands, which is the
  // accelerator's guaranteed input domain.
  const uint32_t srcs[] = {0x00000000u, 0x80808080u, 0xffffffffu, 0xff204060u,
                           0x40302010u};
  // Arbitrary words folded to premul for the same reason.
  uint32_t dst[71];
  uint32_t out[71];
  for (size_t i = 0; i < 71; i++) {
    dst[i] = ColorToPMColor(static_cast<uint32_t>(i * 2654435761u));
    out[i] = dst[i];
  }
  for (const Entry& e : entries) {
    for (uint32_t src_pm : srcs) {
      for (int cover_int = 0; cover_int < 256; cover_int += 5) {
        const uint8_t cover = static_cast<uint8_t>(cover_int);
        const uint32_t scaled =
            cover == 255 ? src_pm : AlphaMulQ(src_pm, cover);
        e.kernel(reinterpret_cast<uint8_t*>(out), 71, scaled);
        for (size_t i = 0; i < 71; i++) {
          if (out[i] != ReferenceBlend(e.mode, src_pm, dst[i], cover)) {
            return;
          }
          out[i] = dst[i];
        }
      }
    }
  }
  ValidateGradients();
  g_available = true;
}

void ValidateGradients() {
  // Gradient kernels against the scalar legacy chain through the LUT: every
  // tile mode, an offsets fixture and a uniform fixture; the row sweeps t
  // across and past [0, 1] so all remap branches run. The SrcOver variant
  // runs on a zeroed destination, where its output is exactly the fetched
  // word. Tolerance covers the LUT interpolation (about 2 per channel);
  // the fuse must not trip on rounding.
  const float colors4[16] = {0.05f, 0.62f, 0.91f, 1.f,  0.80f, 0.15f,
                             0.30f, 1.f,  0.33f, 0.90f, 0.12f, 0.60f,
                             0.95f, 0.05f, 0.55f, 1.f};
  const float seg4s[3] = {0.20f, 0.55f, 0.75f};
  const float seg4e[3] = {0.55f, 0.75f, 0.85f};
  const float colors3[12] = {0.9f, 0.1f, 0.1f, 0.4f, 0.1f, 0.9f,
                             0.1f, 0.85f, 0.1f, 0.1f, 0.9f, 1.f};
  const float seg3s[2] = {0.f, 0.5f};
  const float seg3e[2] = {0.5f, 1.f};
  const int32_t tiles[] = {
      static_cast<int32_t>(TileMode::kClamp),
      static_cast<int32_t>(TileMode::kRepeat),
      static_cast<int32_t>(TileMode::kMirror),
      static_cast<int32_t>(TileMode::kDecal)};
  for (int32_t tile : tiles) {
    GradientParams lin{};
    lin.type = 0;
    lin.tile_mode = tile;
    lin.m00 = 0.09f;
    lin.m01 = 0.013f;
    lin.m02 = -0.35f;
    lin.use_offsets = true;
    lin.first_offset = 0.20f;
    lin.color_count = 4;
    lin.seg_start = seg4s;
    lin.seg_end = seg4e;
    lin.colors = colors4;
    uint32_t lin_lut[512];
    if (!BuildGradientLUT(lin, lin_lut)) {
      return;
    }
    lin.lut = lin_lut;
    lin.lut_entries = 512;

    GradientParams rad{};
    rad.type = 1;
    rad.tile_mode = tile;
    rad.m00 = 0.075f;
    rad.m01 = 0.f;
    rad.m02 = -0.6f;
    rad.m10 = 0.f;
    rad.m11 = 0.07f;
    rad.m12 = -0.45f;
    rad.use_offsets = false;
    rad.color_count = 3;
    rad.seg_start = seg3s;
    rad.seg_end = seg3e;
    rad.colors = colors3;
    uint32_t rad_lut[512];
    if (!BuildGradientLUT(rad, rad_lut)) {
      return;
    }
    rad.lut = rad_lut;
    rad.lut_entries = 512;

    struct GEntry {
      const GradientParams* p;
      GradientKernelFn k;
    };
    const GEntry gentries[] = {
        {&lin, HWY_DYNAMIC_DISPATCH(GradientRunLinearSrcKernel)},
        {&lin, HWY_DYNAMIC_DISPATCH(GradientRunLinearSrcOverKernel)},
        {&rad, HWY_DYNAMIC_DISPATCH(GradientRunRadialSrcKernel)},
        {&rad, HWY_DYNAMIC_DISPATCH(GradientRunRadialSrcOverKernel)},
    };
    uint32_t out[29];
    for (const GEntry& e : gentries) {
      for (int32_t y : {0, 3, 17}) {
        for (uint32_t cover : {255u, 128u}) {
          memset(out, 0, sizeof(out));
          e.k(reinterpret_cast<uint8_t*>(out), e.p, 0, y, 29, cover);
          for (int32_t i = 0; i < 29; i++) {
            const float t = ReferenceGradientT(*e.p, i, y);
            if (e.p->tile_mode == static_cast<int32_t>(TileMode::kRepeat) &&
                std::abs(t - 1.f) <= 4.f / (1 << 12)) {
              // Left-limit convention at the kRepeat wrap seam (see
              // BuildGradientLUT).
              continue;
            }
            uint32_t expect = ReferenceGradientColor(*e.p, t);
            if (cover != 255) {
              uint32_t ch[4] = {expect & 0xff, (expect >> 8) & 0xff,
                                (expect >> 16) & 0xff, (expect >> 24) & 0xff};
              for (int c = 0; c < 4; c++) {
                ch[c] = (ch[c] * cover) >> 8;
              }
              expect = ch[0] | (ch[1] << 8) | (ch[2] << 16) | (ch[3] << 24);
            }
            for (int shift = 0; shift < 32; shift += 8) {
              const int32_t d =
                  static_cast<int32_t>((out[i] >> shift) & 0xff) -
                  static_cast<int32_t>((expect >> shift) & 0xff);
              if ((d < 0 ? -d : d) > 4) {
                return;
              }
            }
          }
        }
      }
    }
  }
}

}  // namespace

bool Validate() {
  // Run the check once through Available(); Validate itself triggers it.
  Available();
  return g_available;
}

bool Available() {
  std::call_once(g_validate_once, ValidateOnce);
  return g_available;
}

bool SupportsSolidMode(BlendMode mode) {
  // Every mode has a kernel: coeff modes in integer SIMD, separable advanced
  // modes in f32 SIMD (WGSL formulas), and the cross-channel HSL modes as
  // scalar loops behind the same table.
  return mode <= BlendMode::kLastMode;
}

void SetEnabled(bool enabled) {
  g_runtime_enabled.store(enabled, std::memory_order_relaxed);
}

bool IsEnabled() { return g_runtime_enabled.load(std::memory_order_relaxed); }

bool FillSolidSpans(BlendMode mode, uint32_t src_pm, uint8_t* dst_base,
                    size_t row_bytes, int32_t width, int32_t height,
                    const Span* spans, size_t span_count) {
  if (!IsEnabled() || !Available()) {
    return false;
  }
  RunKernelFn kernel = SelectRunKernel(mode);
  if (kernel == nullptr) {
    return false;
  }
  for (size_t i = 0; i < span_count; i++) {
    int32_t x = spans[i].x;
    int32_t y = spans[i].y;
    int32_t len = spans[i].len;
    if (y < 0 || y >= height || x >= width || len <= 0) {
      continue;
    }
    if (x < 0) {
      len += x;
      x = 0;
    }
    if (x + len > width) {
      len = width - x;
    }
    if (len <= 0) {
      continue;
    }
    int32_t cover = spans[i].cover;
    if (cover < 0) {
      cover = 0;
    } else if (cover > 255) {
      cover = 255;
    }
    const uint32_t scaled =
        cover == 255 ? src_pm : AlphaMulQ(src_pm, static_cast<unsigned>(cover));
    kernel(dst_base + static_cast<size_t>(y) * row_bytes +
               static_cast<size_t>(x) * 4,
           static_cast<size_t>(len), scaled);
  }
  return true;
}

bool BuildGradientLUT(const GradientParams& params, uint32_t* lut) {
  if (params.color_count < 2 || params.seg_start == nullptr ||
      params.seg_end == nullptr || params.colors == nullptr) {
    return false;
  }
  for (int32_t i = 0; i < 512; i++) {
    // The last entry uses the left limit at t = 1: kRepeat is discontinuous
    // there (fract(1) wraps to colors[0]), and interpolating across the
    // jump would smear the wrap seam across the whole last cell. The limit
    // is taken at 1 - 2/4096, safely outside the chain's own nearly-zero
    // snap band (anything closer to 1 snaps back to exactly 1 and wraps).
    // Pixels whose t lands on 1 read this left limit instead of legacy's
    // wrapped colors[0].
    const float t = i == 511 ? 1.0f - 2.f / (1 << 12)
                             : static_cast<float>(i) / 511.f;
    lut[i] = ReferenceGradientColor(params, t);
  }
  return true;
}

namespace {

// Per-thread LUT cache. The entries that matter for an animated UI are very
// few (each gradient of the scene), so a flat array with round-robin
// eviction keeps it simple; the hash only fast-rejects, equality is
// verified against the full stop table.
struct GradientLUTCacheSlot {
  uint64_t hash = 0;
  // Identity of the stop table the LUT was built from.
  int32_t tile_mode = -1;
  bool use_offsets = false;
  float first_offset = 0.f;
  int32_t color_count = 0;
  std::vector<float> seg_start;
  std::vector<float> seg_end;
  std::vector<float> colors;
  std::vector<uint32_t> lut;
};

// Sized so a typical scene's gradients all stay resident (the example
// scene alone has five); 8 x 2KB of thread-local storage.
constexpr size_t kGradientLUTCacheSlots = 8;

uint64_t StopTableHash(const GradientParams& p) {
  uint64_t h = 1469598103934665603ull;  // FNV-1a
  const auto mix = [&h](const void* data, size_t bytes) {
    const auto* b = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < bytes; i++) {
      h ^= b[i];
      h *= 1099511628211ull;
    }
  };
  mix(&p.tile_mode, sizeof(p.tile_mode));
  mix(&p.use_offsets, sizeof(p.use_offsets));
  mix(&p.first_offset, sizeof(p.first_offset));
  mix(&p.color_count, sizeof(p.color_count));
  const size_t segs =
      static_cast<size_t>(p.color_count > 0 ? p.color_count - 1 : 0);
  mix(p.seg_start, segs * sizeof(float));
  mix(p.seg_end, segs * sizeof(float));
  mix(p.colors, static_cast<size_t>(p.color_count) * 4 * sizeof(float));
  return h;
}

bool StopTableEqual(const GradientLUTCacheSlot& slot, const GradientParams& p) {
  const size_t segs = static_cast<size_t>(p.color_count - 1);
  return slot.tile_mode == p.tile_mode && slot.use_offsets == p.use_offsets &&
         slot.first_offset == p.first_offset &&
         slot.color_count == p.color_count && slot.seg_start.size() == segs &&
         memcmp(slot.seg_start.data(), p.seg_start, segs * sizeof(float)) == 0 &&
         memcmp(slot.seg_end.data(), p.seg_end, segs * sizeof(float)) == 0 &&
         memcmp(slot.colors.data(), p.colors,
                static_cast<size_t>(p.color_count) * 4 * sizeof(float)) == 0;
}

}  // namespace

const uint32_t* AcquireGradientLUT(const GradientParams& params) {
  if (params.color_count < 2 || params.seg_start == nullptr ||
      params.seg_end == nullptr || params.colors == nullptr) {
    return nullptr;
  }
  thread_local GradientLUTCacheSlot slots[kGradientLUTCacheSlots];
  thread_local size_t next = 0;

  const uint64_t hash = StopTableHash(params);
  for (GradientLUTCacheSlot& slot : slots) {
    if (slot.lut.empty() || slot.hash != hash || !StopTableEqual(slot, params)) {
      continue;
    }
    return slot.lut.data();
  }
  GradientLUTCacheSlot& slot = slots[next];
  next = (next + 1) % kGradientLUTCacheSlots;
  slot.hash = hash;
  slot.tile_mode = params.tile_mode;
  slot.use_offsets = params.use_offsets;
  slot.first_offset = params.first_offset;
  slot.color_count = params.color_count;
  const size_t segs = static_cast<size_t>(params.color_count - 1);
  slot.seg_start.assign(params.seg_start, params.seg_start + segs);
  slot.seg_end.assign(params.seg_end, params.seg_end + segs);
  slot.colors.assign(params.colors,
                     params.colors +
                         static_cast<size_t>(params.color_count) * 4);
  slot.lut.resize(512);
  if (!BuildGradientLUT(params, slot.lut.data())) {
    slot.lut.clear();
    return nullptr;
  }
  return slot.lut.data();
}

bool FillGradientSpans(const GradientParams& params, BlendMode mode,
                       uint8_t* dst_base, size_t row_bytes, int32_t width,
                       int32_t height, const Span* spans, size_t span_count) {
  if (!IsEnabled() || !Available()) {
    return false;
  }
  GradientKernelFn kernel = SelectGradientKernel(params, mode);
  if (kernel == nullptr) {
    return false;
  }
  for (size_t i = 0; i < span_count; i++) {
    int32_t x = spans[i].x;
    int32_t y = spans[i].y;
    int32_t len = spans[i].len;
    if (y < 0 || y >= height || x >= width || len <= 0) {
      continue;
    }
    if (x < 0) {
      len += x;
      x = 0;
    }
    if (x + len > width) {
      len = width - x;
    }
    if (len <= 0) {
      continue;
    }
    // BrushH folds the span cover with AlphaMulQ (cover & global_alpha,
    // and the gradient brushes always pass global alpha 1.0).
    const uint32_t cover = static_cast<uint32_t>(spans[i].cover) & 0xffu;
    kernel(dst_base + static_cast<size_t>(y) * row_bytes +
               static_cast<size_t>(x) * 4,
           &params, x, y, static_cast<size_t>(len), cover);
  }
  return true;
}

}  // namespace sw_accel
}  // namespace skity
