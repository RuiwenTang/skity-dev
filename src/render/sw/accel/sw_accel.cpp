// Copyright 2021 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

// Multi-target entry for the SW accelerator. foreach_target.h re-includes
// the kernel body once per enabled SIMD target; the direct include after it
// compiles the static (baseline) target and finalizes the dispatch table.

#undef HWY_TARGET_INCLUDE
#define HWY_TARGET_INCLUDE "src/render/sw/accel/sw_accel_kernels.cc"
#include "src/render/sw/accel/sw_accel.hpp"

#include <hwy/foreach_target.h>

#include <cmath>
#include <cstring>
#include <mutex>

#include "src/graphic/blend_mode_priv.hpp"
#include "src/graphic/color_priv.hpp"
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

HWY_EXPORT(VariableRunDstOverKernel);
HWY_EXPORT(VariableRunSrcInKernel);
HWY_EXPORT(VariableRunDstInKernel);
HWY_EXPORT(VariableRunSrcOutKernel);
HWY_EXPORT(VariableRunDstOutKernel);
HWY_EXPORT(VariableRunSrcATopKernel);
HWY_EXPORT(VariableRunDstATopKernel);
HWY_EXPORT(VariableRunXorKernel);
HWY_EXPORT(VariableRunPlusKernel);
HWY_EXPORT(VariableRunModulateKernel);
HWY_EXPORT(VariableRunScreenKernel);
HWY_EXPORT(VariableRunOverlayKernel);
HWY_EXPORT(VariableRunDarkenKernel);
HWY_EXPORT(VariableRunLightenKernel);
HWY_EXPORT(VariableRunColorDodgeKernel);
HWY_EXPORT(VariableRunColorBurnKernel);
HWY_EXPORT(VariableRunHardLightKernel);
HWY_EXPORT(VariableRunSoftLightKernel);
HWY_EXPORT(VariableRunDifferenceKernel);
HWY_EXPORT(VariableRunExclusionKernel);
HWY_EXPORT(VariableRunMultiplyKernel);

HWY_EXPORT(ImageRunNearestSrcKernel);
HWY_EXPORT(ImageRunLinearSrcKernel);
HWY_EXPORT(ImageRunNearestSrcOverKernel);
HWY_EXPORT(ImageRunLinearSrcOverKernel);

HWY_EXPORT(MaskRunSrcKernel);
HWY_EXPORT(MaskRunSrcOverKernel);

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

// The staged path blends rows produced by the kSrc fetch kernels with the
// per-pixel-src engines. Mirrors the solid mode table one-for-one, with the
// constant src alpha replaced by each pixel's own.
using VariableKernelFn = void (*)(uint8_t*, const uint8_t*, size_t);

VariableKernelFn SelectVariableKernel(BlendMode mode) {
  switch (mode) {
    case BlendMode::kDstOver:
      return HWY_DYNAMIC_DISPATCH(VariableRunDstOverKernel);
    case BlendMode::kSrcIn:
      return HWY_DYNAMIC_DISPATCH(VariableRunSrcInKernel);
    case BlendMode::kDstIn:
      return HWY_DYNAMIC_DISPATCH(VariableRunDstInKernel);
    case BlendMode::kSrcOut:
      return HWY_DYNAMIC_DISPATCH(VariableRunSrcOutKernel);
    case BlendMode::kDstOut:
      return HWY_DYNAMIC_DISPATCH(VariableRunDstOutKernel);
    case BlendMode::kSrcATop:
      return HWY_DYNAMIC_DISPATCH(VariableRunSrcATopKernel);
    case BlendMode::kDstATop:
      return HWY_DYNAMIC_DISPATCH(VariableRunDstATopKernel);
    case BlendMode::kXor:
      return HWY_DYNAMIC_DISPATCH(VariableRunXorKernel);
    case BlendMode::kPlus:
      return HWY_DYNAMIC_DISPATCH(VariableRunPlusKernel);
    case BlendMode::kModulate:
      return HWY_DYNAMIC_DISPATCH(VariableRunModulateKernel);
    case BlendMode::kScreen:
      return HWY_DYNAMIC_DISPATCH(VariableRunScreenKernel);
    case BlendMode::kOverlay:
      return HWY_DYNAMIC_DISPATCH(VariableRunOverlayKernel);
    case BlendMode::kDarken:
      return HWY_DYNAMIC_DISPATCH(VariableRunDarkenKernel);
    case BlendMode::kLighten:
      return HWY_DYNAMIC_DISPATCH(VariableRunLightenKernel);
    case BlendMode::kColorDodge:
      return HWY_DYNAMIC_DISPATCH(VariableRunColorDodgeKernel);
    case BlendMode::kColorBurn:
      return HWY_DYNAMIC_DISPATCH(VariableRunColorBurnKernel);
    case BlendMode::kHardLight:
      return HWY_DYNAMIC_DISPATCH(VariableRunHardLightKernel);
    case BlendMode::kSoftLight:
      return HWY_DYNAMIC_DISPATCH(VariableRunSoftLightKernel);
    case BlendMode::kDifference:
      return HWY_DYNAMIC_DISPATCH(VariableRunDifferenceKernel);
    case BlendMode::kExclusion:
      return HWY_DYNAMIC_DISPATCH(VariableRunExclusionKernel);
    case BlendMode::kMultiply:
      return HWY_DYNAMIC_DISPATCH(VariableRunMultiplyKernel);
    default:
      return nullptr;
  }
}

using ImageKernelFn = void (*)(uint8_t*, const ImageParams*, int32_t, int32_t,
                               size_t, uint32_t);

ImageKernelFn SelectImageKernel(const ImageParams& params, BlendMode mode) {
  if (mode == BlendMode::kSrc) {
    return params.bilinear ? HWY_DYNAMIC_DISPATCH(ImageRunLinearSrcKernel)
                           : HWY_DYNAMIC_DISPATCH(ImageRunNearestSrcKernel);
  }
  if (mode == BlendMode::kSrcOver) {
    return params.bilinear ? HWY_DYNAMIC_DISPATCH(ImageRunLinearSrcOverKernel)
                           : HWY_DYNAMIC_DISPATCH(ImageRunNearestSrcOverKernel);
  }
  return nullptr;
}

using MaskKernelFn = void (*)(uint8_t*, const uint8_t*, size_t, uint32_t,
                              uint32_t);

MaskKernelFn SelectMaskKernel(BlendMode mode) {
  if (mode == BlendMode::kSrc) {
    return HWY_DYNAMIC_DISPATCH(MaskRunSrcKernel);
  }
  if (mode == BlendMode::kSrcOver) {
    return HWY_DYNAMIC_DISPATCH(MaskRunSrcOverKernel);
  }
  return nullptr;
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
    return static_cast<uint32_t>(std::min(255.f, std::max(0.f, v * 255.f)));
  };
  uint32_t ch[4] = {to_u8(rgba[0]), to_u8(rgba[1]), to_u8(rgba[2]),
                    to_u8(rgba[3])};
  if (p.premul_stops) {
    // Premultiplied stop domain: the shader clamps RGB into [0, A] at the
    // output, folding out-of-gamut resolved stops back into range.
    for (int c = 0; c < 3; c++) {
      ch[c] = std::min(ch[c], ch[3]);
    }
  } else if (ch[3] != 255) {
    // ColorToPMColor: rounding premul against the alpha channel.
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

// Scalar mirrors of the legacy image chain (PixmapBrush::CalculateColor via
// BitmapSampler::GetColor, then the premul / coverage folds and the blend),
// producing the premul memory word (0xAABBGGRR) for one destination pixel.
// ValidateImages() checks the dispatched kernels against them.
float ReferenceImageRemap(float t, int32_t tile_mode) {
  if (tile_mode == static_cast<int32_t>(TileMode::kClamp)) {
    return std::min(1.f, std::max(0.f, t));
  } else if (tile_mode == static_cast<int32_t>(TileMode::kRepeat)) {
    return t - std::floor(t);
  } else if (tile_mode == static_cast<int32_t>(TileMode::kMirror)) {
    float t1 = t - 1;
    float t2 = t1 - 2 * std::floor(t1 * 0.5f) - 1;
    return std::abs(t2);
  }
  return t;  // kDecal passes through; the decal early-out is a separate check.
}

uint32_t ReferenceImageColor(const ImageParams& p, int32_t x, int32_t y) {
  const float fx = x + 0.5f;
  const float fy = y + 0.5f;
  // MapPoint: (fx * sx + fy * skx) + tx, left-associative.
  const float u = (fx * p.m00 + fy * p.m01) + p.m02;
  const float v = (fx * p.m10 + fy * p.m11) + p.m12;
  // Decal early-out on the raw uv, before remapping (GetColor order).
  if ((p.x_tile_mode == static_cast<int32_t>(TileMode::kDecal) &&
       (u < 0.f || u >= 1.f)) ||
      (p.y_tile_mode == static_cast<int32_t>(TileMode::kDecal) &&
       (v < 0.f || v >= 1.f))) {
    return 0;
  }
  const float uu = ReferenceImageRemap(u, p.x_tile_mode);
  const float vv = ReferenceImageRemap(v, p.y_tile_mode);

  const auto texel = [&p](int32_t ix, int32_t iy) {
    uint32_t word;
    memcpy(&word,
           p.texels + static_cast<size_t>(iy) * p.tex_row_bytes +
               static_cast<size_t>(ix) * 4,
           4);
    return word;
  };

  float ch[4] = {0.f, 0.f, 0.f, 0.f};
  if (!p.bilinear) {
    // Nearest: truncating uint32 convert (the value is non-negative after
    // the remap), clamped to the last texel.
    const uint32_t ix =
        std::min<uint32_t>(static_cast<uint32_t>(uu * p.tex_width),
                           static_cast<uint32_t>(p.tex_width - 1));
    const uint32_t iy =
        std::min<uint32_t>(static_cast<uint32_t>(vv * p.tex_height),
                           static_cast<uint32_t>(p.tex_height - 1));
    const uint32_t word =
        texel(static_cast<int32_t>(ix), static_cast<int32_t>(iy));
    for (int c = 0; c < 4; c++) {
      ch[c] = static_cast<float>((word >> (c * 8)) & 0xff) / 255.f;
    }
  } else {
    const float tx = uu * p.tex_width;
    const float ty = vv * p.tex_height;
    const float txm = tx - 0.5f;
    const float tym = ty - 0.5f;
    const float a = txm - std::floor(txm);
    const float b = tym - std::floor(tym);
    int32_t i0 = static_cast<int32_t>(std::floor(txm));
    int32_t j0 = static_cast<int32_t>(std::floor(tym));
    int32_t i1 = i0 + 1;
    int32_t j1 = j0 + 1;
    const auto wrap = [](int32_t& t0, int32_t& t1, int32_t dim) {
      if (t0 < 0) {
        t0 += dim;
      }
      if (t1 >= dim) {
        t1 -= dim;
      }
    };
    if (p.x_tile_mode == static_cast<int32_t>(TileMode::kRepeat)) {
      wrap(i0, i1, p.tex_width);
    }
    if (p.y_tile_mode == static_cast<int32_t>(TileMode::kRepeat)) {
      wrap(j0, j1, p.tex_height);
    }
    i0 = std::min(std::max(i0, 0), p.tex_width - 1);
    i1 = std::min(std::max(i1, 0), p.tex_width - 1);
    j0 = std::min(std::max(j0, 0), p.tex_height - 1);
    j1 = std::min(std::max(j1, 0), p.tex_height - 1);
    const uint32_t c00 = texel(i0, j0);
    const uint32_t c10 = texel(i1, j0);
    const uint32_t c01 = texel(i0, j1);
    const uint32_t c11 = texel(i1, j1);
    const float w00 = (1.f - a) * (1.f - b);
    const float w10 = a * (1.f - b);
    const float w01 = (1.f - a) * b;
    const float w11 = a * b;
    for (int c = 0; c < 4; c++) {
      const float v00 = static_cast<float>((c00 >> (c * 8)) & 0xff) / 255.f;
      const float v10 = static_cast<float>((c10 >> (c * 8)) & 0xff) / 255.f;
      const float v01 = static_cast<float>((c01 >> (c * 8)) & 0xff) / 255.f;
      const float v11 = static_cast<float>((c11 >> (c * 8)) & 0xff) / 255.f;
      // SampleUnitLinear's combination in its exact expression order. The
      // products stay in separate statements like the kernel's combine (see
      // sw_accel_kernels.cc): contracting them into fma changes the
      // last-bit rounding against the glm-shaped scalar sampler.
      const float t0 = w00 * v00;
      const float t1 = w10 * v10;
      const float t2 = w01 * v01;
      const float t3 = w11 * v11;
      const float mixed = t0 + t1 + t2 + t3;
      // Color4fToColor: clamp(v * 255, 0, 255), truncating cast.
      ch[c] = static_cast<float>(
          static_cast<uint32_t>(std::min(255.f, std::max(0.f, mixed * 255.f))));
    }
  }
  uint32_t out[4];
  for (int c = 0; c < 4; c++) {
    out[c] = static_cast<uint32_t>(ch[c]);
  }
  // ColorToPMColor for unpremul textures: rounding premul of RGB by A.
  if (!p.src_premul) {
    for (int c = 0; c < 3; c++) {
      out[c] = MulDiv255Round(out[c], out[3]);
    }
  }
  return out[0] | (out[1] << 8) | (out[2] << 16) | (out[3] << 24);
}

// Coverage fold + real PorterDuffBlend over memory words, mirroring the
// legacy brush -> BlendPixelH boundary.
uint32_t ReferenceImageBlend(BlendMode mode, uint32_t src_mem, uint32_t dst_mem,
                             uint8_t alpha) {
  const uint32_t src_pm = PMColorSwapRB(src_mem);
  const uint32_t src = alpha == 255 ? src_pm : AlphaMulQ(src_pm, alpha);
  return PMColorSwapRB(PorterDuffBlend(src, PMColorSwapRB(dst_mem), mode));
}

void ValidateImages() {
  // Image kernels against the scalar legacy chain: premul and unpremul
  // textures, both filters, every tile-mode pair, both supported blend modes
  // and two coverage levels. The probe rows sweep across and past the
  // texture on both axes (negative uv, wrap seams, right/bottom edges) so
  // every tap branch runs; the bit-exact fuse must not trip on rounding.
  constexpr int32_t kTexW = 7;
  constexpr int32_t kTexH = 5;
  constexpr size_t kRowBytes = static_cast<size_t>(kTexW) * 4;
  uint8_t texels[kTexH * kRowBytes];
  const auto texel_at = [&](int32_t x, int32_t y) -> uint32_t& {
    return *reinterpret_cast<uint32_t*>(texels +
                                        static_cast<size_t>(y) * kRowBytes +
                                        static_cast<size_t>(x) * 4);
  };
  for (int32_t y = 0; y < kTexH; y++) {
    for (int32_t x = 0; x < kTexW; x++) {
      // Premul-legal pseudo-random words (alpha in byte 3, RGB <= alpha).
      const uint32_t hash = static_cast<uint32_t>(y * kTexW + x) * 2654435761u;
      const uint32_t alpha = (hash >> 24) % 256;
      const uint32_t r = hash % (alpha + 1);
      const uint32_t g = (hash >> 8) % (alpha + 1);
      const uint32_t b = (hash >> 16) % (alpha + 1);
      texel_at(x, y) = r | (g << 8) | (b << 16) | (alpha << 24);
    }
  }

  const int32_t tiles[] = {static_cast<int32_t>(TileMode::kClamp),
                           static_cast<int32_t>(TileMode::kRepeat),
                           static_cast<int32_t>(TileMode::kMirror),
                           static_cast<int32_t>(TileMode::kDecal)};
  for (int alpha_type = 0; alpha_type < 2; alpha_type++) {
    for (int filter = 0; filter < 2; filter++) {
      for (int t = 0; t < 4; t++) {
        ImageParams p{};
        p.texels = texels;
        p.tex_row_bytes = kRowBytes;
        p.tex_width = kTexW;
        p.tex_height = kTexH;
        p.m00 = 0.13f;
        p.m01 = 0.007f;
        p.m02 = -0.2f;
        p.m10 = 0.21f;
        p.m11 = 0.011f;
        p.m12 = -0.35f;
        p.x_tile_mode = tiles[t];
        p.y_tile_mode = tiles[(t + 1) % 4];
        p.bilinear = filter == 1;
        p.src_premul = alpha_type == 1;
        for (BlendMode mode : {BlendMode::kSrc, BlendMode::kSrcOver}) {
          for (uint32_t alpha : {255u, 128u}) {
            for (int32_t y : {0, 3, 17}) {
              constexpr int32_t kCount = 37;
              uint32_t dst[kCount];
              uint32_t expect[kCount];
              for (int32_t i = 0; i < kCount; i++) {
                dst[i] = ColorToPMColor(
                    static_cast<uint32_t>(i * 2654435761u + 0x40404040u));
                expect[i] =
                    ReferenceImageBlend(mode, ReferenceImageColor(p, i, y),
                                        dst[i], static_cast<uint8_t>(alpha));
              }
              ImageKernelFn kernel = SelectImageKernel(p, mode);
              kernel(reinterpret_cast<uint8_t*>(dst), &p, 0, y, kCount, alpha);
              for (int32_t i = 0; i < kCount; i++) {
                if (dst[i] != expect[i]) {
                  return;
                }
              }
            }
          }
        }
      }
    }
  }
}

// Scalar mirror of the legacy glyph-mask blend chain (Brush coverage fold:
// MulDiv255Round(mask, alpha) then AlphaMulQ into the color, skipping the
// fold at full coverage, then the real PorterDuffBlend), over memory words.
uint32_t ReferenceMaskBlend(BlendMode mode, uint32_t src_pm, uint8_t coverage,
                            uint8_t alpha, uint32_t dst_mem) {
  uint32_t m = coverage;
  if (alpha != 255) {
    m = MulDiv255Round(m, alpha);
  }
  const uint32_t src = m == 255 ? src_pm : AlphaMulQ(src_pm, m);
  return PMColorSwapRB(PorterDuffBlend(src, PMColorSwapRB(dst_mem), mode));
}

void ValidateMasks() {
  // Mask kernels against the scalar legacy chain: both supported modes, two
  // global alphas and mask rows with every coverage edge (0, 1, 64, 128,
  // 254, 255), row lengths past the vector and across the tail. Bit-exact.
  uint8_t mask[71];
  for (size_t i = 0; i < sizeof(mask); i++) {
    static const uint8_t kEdgeValues[] = {0, 1, 64, 128, 254, 255};
    mask[i] = kEdgeValues[(i * 7) % 6];
  }
  for (BlendMode mode : {BlendMode::kSrc, BlendMode::kSrcOver}) {
    MaskKernelFn kernel = SelectMaskKernel(mode);
    for (uint32_t alpha : {255u, 128u}) {
      uint32_t dst[71];
      uint32_t expect[71];
      for (size_t i = 0; i < 71; i++) {
        dst[i] = ColorToPMColor(static_cast<uint32_t>(i * 2654435761u));
        expect[i] = ReferenceMaskBlend(mode, 0xff204060u, mask[i],
                                       static_cast<uint8_t>(alpha), dst[i]);
      }
      kernel(reinterpret_cast<uint8_t*>(dst), mask, 71, 0xff204060u, alpha);
      for (size_t i = 0; i < 71; i++) {
        if (dst[i] != expect[i]) {
          return;
        }
      }
    }
  }
}

std::once_flag g_validate_once;
bool g_available = false;

void ValidateGradients();
void ValidateGradientModes();

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
  ValidateImages();
  ValidateMasks();
  ValidateGradientModes();
  g_available = true;
}

void ValidateGradients() {
  // Gradient kernels against the scalar legacy chain through the LUT: every
  // tile mode, an offsets fixture and a uniform fixture; the row sweeps t
  // across and past [0, 1] so all remap branches run. The SrcOver variant
  // runs on a zeroed destination, where its output is exactly the fetched
  // word. Tolerance covers the LUT interpolation (about 2 per channel);
  // the fuse must not trip on rounding.
  const float colors4[16] = {0.05f, 0.62f, 0.91f, 1.f,   0.80f, 0.15f,
                             0.30f, 1.f,   0.33f, 0.90f, 0.12f, 0.60f,
                             0.95f, 0.05f, 0.55f, 1.f};
  const float seg4s[3] = {0.20f, 0.55f, 0.75f};
  const float seg4e[3] = {0.55f, 0.75f, 0.85f};
  const float colors3[12] = {0.9f, 0.1f,  0.1f, 0.4f, 0.1f, 0.9f,
                             0.1f, 0.85f, 0.1f, 0.1f, 0.9f, 1.f};
  const float seg3s[2] = {0.f, 0.5f};
  const float seg3e[2] = {0.5f, 1.f};
  const int32_t tiles[] = {static_cast<int32_t>(TileMode::kClamp),
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
              const int32_t d = static_cast<int32_t>((out[i] >> shift) & 0xff) -
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

void ValidateGradientModes() {
  // The variable-src engines behind the staged gradient path. Each engine is
  // driven with per-pixel src words cycling the premul patterns (sa varies
  // within one run, covering the 0/255 shortcut boundaries) and is compared
  // against the scalar blend of the very same words. The integer coeff
  // engines are bit-exact; the f32 advanced engines carry a +/-1 tolerance
  // because the scalar reference contracts its expressions into fma (even at
  // -O0, the M2-1 finding) while the intrinsics stay uncontracted - the
  // difference is final rounding only. kSoftLight adds Highway's NEON Sqrt
  // rsqrte+NR approximation on top (+/-2).
  const uint32_t srcs[] = {0x00000000u, 0x80808080u, 0xffffffffu, 0xff204060u,
                           0x40302010u};
  uint32_t src[71];
  uint32_t dst[71];
  for (size_t i = 0; i < 71; i++) {
    src[i] = srcs[i % 5];
    dst[i] = ColorToPMColor(static_cast<uint32_t>(i * 2654435761u));
  }
  const BlendMode modes[] = {
      BlendMode::kDstOver,    BlendMode::kSrcIn,     BlendMode::kDstIn,
      BlendMode::kSrcOut,     BlendMode::kDstOut,    BlendMode::kSrcATop,
      BlendMode::kDstATop,    BlendMode::kXor,       BlendMode::kPlus,
      BlendMode::kModulate,   BlendMode::kScreen,    BlendMode::kOverlay,
      BlendMode::kDarken,     BlendMode::kLighten,   BlendMode::kColorDodge,
      BlendMode::kColorBurn,  BlendMode::kHardLight, BlendMode::kSoftLight,
      BlendMode::kDifference, BlendMode::kExclusion, BlendMode::kMultiply,
  };
  for (BlendMode mode : modes) {
    VariableKernelFn kernel = SelectVariableKernel(mode);
    if (kernel == nullptr) {
      return;  // dispatch table incomplete - fail the fuse
    }
    uint32_t out[71];
    memcpy(out, dst, sizeof(out));
    kernel(reinterpret_cast<uint8_t*>(out),
           reinterpret_cast<const uint8_t*>(src), 71);
    const int32_t tolerance = mode == BlendMode::kSoftLight ? 2
                              : mode >= BlendMode::kOverlay ? 1
                                                            : 0;
    for (size_t i = 0; i < 71; i++) {
      const uint32_t expect =
          ReferenceBlend(mode, PMColorSwapRB(src[i]), dst[i], 255);
      for (int shift = 0; shift < 32; shift += 8) {
        const int32_t d = static_cast<int32_t>((out[i] >> shift) & 0xff) -
                          static_cast<int32_t>((expect >> shift) & 0xff);
        if ((d < 0 ? -d : d) > tolerance) {
          return;
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

bool FillSolidSpans(BlendMode mode, uint32_t src_pm, uint8_t* dst_base,
                    size_t row_bytes, int32_t width, int32_t height,
                    const Span* spans, size_t span_count) {
  // Callers gate on SWContext::AccelEnabled() (the context lives in the
  // skity target, not this archive); only the Validate fuse is checked here.
  if (!Available()) {
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
    const float t =
        i == 511 ? 1.0f - 2.f / (1 << 12) : static_cast<float>(i) / 511.f;
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
  bool premul_stops = false;
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
  mix(&p.premul_stops, sizeof(p.premul_stops));
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
         slot.premul_stops == p.premul_stops &&
         slot.first_offset == p.first_offset &&
         slot.color_count == p.color_count && slot.seg_start.size() == segs &&
         memcmp(slot.seg_start.data(), p.seg_start, segs * sizeof(float)) ==
             0 &&
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
    if (slot.lut.empty() || slot.hash != hash ||
        !StopTableEqual(slot, params)) {
      continue;
    }
    return slot.lut.data();
  }
  GradientLUTCacheSlot& slot = slots[next];
  next = (next + 1) % kGradientLUTCacheSlots;
  slot.hash = hash;
  slot.tile_mode = params.tile_mode;
  slot.use_offsets = params.use_offsets;
  slot.premul_stops = params.premul_stops;
  slot.first_offset = params.first_offset;
  slot.color_count = params.color_count;
  const size_t segs = static_cast<size_t>(params.color_count - 1);
  slot.seg_start.assign(params.seg_start, params.seg_start + segs);
  slot.seg_end.assign(params.seg_end, params.seg_end + segs);
  slot.colors.assign(
      params.colors,
      params.colors + static_cast<size_t>(params.color_count) * 4);
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
  // Same contract as FillSolidSpans: the caller checked the SWContext accel
  // gate.
  if (!Available()) {
    return false;
  }
  // r = d: nothing to write.
  if (mode == BlendMode::kDst) {
    return true;
  }
  const bool fused = mode == BlendMode::kSrc || mode == BlendMode::kSrcOver;
  GradientKernelFn kernel = nullptr;
  if (fused) {
    kernel = SelectGradientKernel(params, mode);
    if (kernel == nullptr) {
      return false;
    }
  }
  // The remaining modes fetch each span row into a dense color buffer with
  // the kSrc kernels and blend it through the variable-src engines; the
  // cross-channel HSL modes keep a scalar blend (no interleaved-lane engine).
  const bool hsl = !fused && mode >= BlendMode::kHue;
  VariableKernelFn variable = nullptr;
  if (!fused && !hsl && mode != BlendMode::kClear) {
    variable = SelectVariableKernel(mode);
    if (variable == nullptr) {
      return false;
    }
  }
  const bool fetch = !fused && mode != BlendMode::kClear;
  GradientKernelFn fetch_kernel = nullptr;
  if (fetch) {
    fetch_kernel = params.type == 1
                       ? HWY_DYNAMIC_DISPATCH(GradientRunRadialSrcKernel)
                       : HWY_DYNAMIC_DISPATCH(GradientRunLinearSrcKernel);
  }
  // Stack staging for the dense color rows; long spans fetch and blend in
  // chunks so the buffer stays fixed-size.
  constexpr int32_t kStagedChunk = 512;
  alignas(64) uint32_t staged[kStagedChunk];

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
    // The brush folds the span cover with MulDiv255Round against the brush's
    // global alpha; the gradient brushes always pass global alpha 1.0, so
    // the folded value is the raw cover.
    const uint32_t cover = static_cast<uint32_t>(spans[i].cover) & 0xffu;
    uint8_t* dst = dst_base + static_cast<size_t>(y) * row_bytes +
                   static_cast<size_t>(x) * 4;
    if (mode == BlendMode::kClear) {
      memset(dst, 0, static_cast<size_t>(len) * 4);
      continue;
    }
    if (fused) {
      kernel(dst, &params, x, y, static_cast<size_t>(len), cover);
      continue;
    }
    for (int32_t off = 0; off < len; off += kStagedChunk) {
      const int32_t n = std::min(kStagedChunk, len - off);
      fetch_kernel(reinterpret_cast<uint8_t*>(staged), &params, x + off, y,
                   static_cast<size_t>(n), cover);
      uint32_t* dst32 = reinterpret_cast<uint32_t*>(dst) + off;
      if (hsl) {
        for (int32_t k = 0; k < n; k++) {
          dst32[k] = PMColorSwapRB(PorterDuffBlend(
              PMColorSwapRB(staged[k]), PMColorSwapRB(dst32[k]), mode));
        }
      } else {
        variable(reinterpret_cast<uint8_t*>(dst32),
                 reinterpret_cast<const uint8_t*>(staged),
                 static_cast<size_t>(n));
      }
    }
  }
  return true;
}

bool SupportsGradientMode(BlendMode mode) {
  // Every mode is served: kSrc/kSrcOver through the fused kernels, the other
  // coeff modes through the variable-src engines, the separable advanced
  // modes in f32, and the cross-channel HSL modes as scalar loops.
  return mode <= BlendMode::kLastMode;
}

bool SupportsImageMode(BlendMode mode) {
  return mode == BlendMode::kSrc || mode == BlendMode::kSrcOver;
}

bool FillImageSpans(const ImageParams& params, BlendMode mode,
                    uint8_t* dst_base, size_t row_bytes, int32_t width,
                    int32_t height, const Span* spans, size_t span_count) {
  // Same contract as FillSolidSpans: the caller checked the SWContext accel
  // gate.
  if (!Available()) {
    return false;
  }
  ImageKernelFn kernel = SelectImageKernel(params, mode);
  if (kernel == nullptr) {
    return false;
  }
  // The caller (PixmapBrush) validates its configuration before building the
  // params; re-check the invariants the kernel itself relies on (the u32
  // offset math needs the texture to fit, and non-finite matrix entries
  // would poison the uv lanes).
  if (params.texels == nullptr || params.tex_width < 1 ||
      params.tex_height < 1 ||
      params.tex_row_bytes < static_cast<size_t>(params.tex_width) * 4 ||
      params.tex_row_bytes * static_cast<size_t>(params.tex_height) >
          0xffffffffull) {
    return false;
  }
  const float matrix[6] = {params.m00, params.m01, params.m02,
                           params.m10, params.m11, params.m12};
  for (float entry : matrix) {
    if (!std::isfinite(entry)) {
      return false;
    }
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
    // Brush() folds the span cover with the brush's global alpha
    // (MulDiv255Round), matching the fixed scalar path.
    const uint32_t cover = static_cast<uint32_t>(spans[i].cover) & 0xffu;
    const uint32_t alpha = MulDiv255Round(cover, params.global_alpha);
    kernel(dst_base + static_cast<size_t>(y) * row_bytes +
               static_cast<size_t>(x) * 4,
           &params, x, y, static_cast<size_t>(len), alpha);
  }
  return true;
}

bool BlendMaskRect(BlendMode mode, uint32_t src_pm, uint8_t global_alpha,
                   uint8_t* dst_base, size_t row_bytes, int32_t dst_width,
                   int32_t dst_height, int32_t x, int32_t y,
                   const uint8_t* mask, size_t mask_stride, int32_t width,
                   int32_t height) {
  // Same contract as FillSolidSpans: the caller checked the SWContext accel
  // gate.
  if (!Available()) {
    return false;
  }
  MaskKernelFn kernel = SelectMaskKernel(mode);
  if (kernel == nullptr || mask == nullptr || width < 0 || height < 0) {
    return false;
  }
  // Clip the rect to the destination surface; the mask pointer follows the
  // clip so the caller can pass an unclipped rect with a full mask.
  int32_t x0 = std::max(x, 0);
  int32_t y0 = std::max(y, 0);
  const int32_t x1 = std::min(x + width, dst_width);
  const int32_t y1 = std::min(y + height, dst_height);
  if (x0 >= x1 || y0 >= y1) {
    return true;  // Fully outside; nothing to blend.
  }
  mask +=
      static_cast<size_t>(y0 - y) * mask_stride + static_cast<size_t>(x0 - x);
  const size_t count = static_cast<size_t>(x1 - x0);
  for (int32_t row = y0; row < y1; row++) {
    kernel(dst_base + static_cast<size_t>(row) * row_bytes +
               static_cast<size_t>(x0) * 4,
           mask, count, src_pm, global_alpha);
    mask += mask_stride;
  }
  return true;
}

}  // namespace sw_accel
}  // namespace skity
