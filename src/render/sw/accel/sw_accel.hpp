// Copyright 2021 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

#ifndef SRC_RENDER_SW_ACCEL_SW_ACCEL_HPP
#define SRC_RENDER_SW_ACCEL_SW_ACCEL_HPP

#include <stddef.h>
#include <stdint.h>

#include <skity/graphic/blend_mode.hpp>
#include <skity/graphic/tile_mode.hpp>
#include <skity/macros.hpp>

#include "src/render/sw/sw_subpixel.hpp"

// Only the runtime switch is exported from the shared library (dev tooling
// such as the examples calls it); every other entry point is used inside
// the library or by targets that link skity_sw_accel statically, and stays
// hidden together with the Highway namespace. Unified C-ABI-only builds
// export nothing here either (SKITY_HIDE_CXX_API is defined on the
// skity_sw_accel target in that shape; see CMakeLists.txt).
//
// This reuses SKITY_API: SKITY_DLL / SKITY_WIN are self-defined by
// macros.hpp in every TU, so the macro expands to __declspec(dllexport)
// while compiling sw_accel.cpp too. That is what lifts the two symbols out
// of the statically linked skity_sw_accel into the skity.dll export table -
// the dllexport directive travels with the archive member, whereas
// CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS only scans the target's own objects and
// never sees symbols coming from static archives. Consumers bind through
// the skity.lib import library exactly like every other skity symbol.
#define SW_ACCEL_API SKITY_API

namespace skity {
namespace sw_accel {

// Known-answer health check over the dispatched SIMD target. Called by the
// renderer before the kernels are used (a failed check must disable the
// accelerator and fall back to the scalar path), and by unit tests. Returns
// false on mismatch.
bool Validate();

// Returns true once Validate() has passed; the first call runs the check
// (std::call_once). All accelerator entry points are no-ops / return false
// when this returns false.
bool Available();

// Dev/testing facility: process-global runtime switch for the accelerated
// path, on by default. When off, FillSolidSpans fails and SWCanvas falls
// back to the legacy brush within the same binary - for A/B comparisons of
// rendering results and performance. Compile-time exclusion
// (SKITY_SW_ACCEL=OFF) remains the packaging knob.
SW_ACCEL_API void SetEnabled(bool enabled);
SW_ACCEL_API bool IsEnabled();

// Fills a span list into a premul destination with a constant premul color,
// applying each span's coverage exactly like the legacy SolidColorBrush
// path (coverage folded into src via AlphaMulQ, then the blend mode). The
// destination must be ColorType::kRGBA (memory bytes [R, G, B, A],
// little-endian u32 0xAABBGGRR); src_pm is a PMColor word (0xAARRGGBB).
// Spans are clamped to [0, width) x [0, height) like SWSpanBrush::Brush.
//
// Supported modes today: every Porter-Duff coeff mode (kClear through
// kScreen, including the kDst no-op). Returns false (and does nothing) when
// the mode is unsupported or the accelerator is unavailable, in which case
// the caller must use the legacy brush path.
bool FillSolidSpans(BlendMode mode, uint32_t src_pm, uint8_t* dst_base,
                    size_t row_bytes, int32_t width, int32_t height,
                    const Span* spans, size_t span_count);

// Returns true when FillSolidSpans supports the blend mode.
bool SupportsSolidMode(BlendMode mode);

// Precomputed, POD description of one gradient draw, built by the software
// brush after resolving the shader into stops and a device-space transform.
//
// The per-pixel t keeps the legacy evaluation order exactly (MapPoint of
// {x + 0.5f, y + 0.5f}, nearly-zero snapping, tile remap). The color for a
// given t comes from a 512-entry LUT built per draw by BuildGradientLUT()
// from the legacy chain (tile handling, stop scan, lerp, u8 conversion,
// rounding premul), interpolated between adjacent entries - the same design
// as Skia/blend2D CPU gradients. This trades bit-exactness with the scalar
// brush for speed: the difference is bounded by about 2 per channel, within
// the GPU cross-tolerance.
struct GradientParams {
  // 0 = linear (t = u), 1 = radial (t = sqrt(u*u + v*v)).
  int32_t type;
  // Entries of points_to_unit_ (the unit-space matrix in device coordinates).
  // u = ((x + 0.5f) * m00 + (y + 0.5f) * m01) + m02, likewise v with m1x.
  float m00, m01, m02;
  float m10, m11, m12;
  // skity::TileMode value (kClamp / kRepeat / kMirror / kDecal).
  int32_t tile_mode;

  // Stop table consumed by BuildGradientLUT (the kernels only read the LUT).
  bool use_offsets;  // explicit offsets present (legacy early-out applies)
  float first_offset;
  // color_count >= 2 stops; colors is color_count * 4 floats (r, g, b, a).
  int32_t color_count;
  const float* seg_start;  // color_count - 1 entries
  const float* seg_end;    // color_count - 1 entries
  const float* colors;

  // Premul memory words (0xAABBGGRR) built by BuildGradientLUT; entries is
  // 512 and lut[i] is the legacy result at t = i / 511 (the last entry
  // holds the left limit at t = 1).
  const uint32_t* lut;
  int32_t lut_entries;
};

// Builds the 512-entry gradient LUT for the stop table in params (which must
// describe a supported gradient: color_count >= 2, finite transform, and
// seg tables covering every stop). Returns false without touching lut on
// unsupported input.
bool BuildGradientLUT(const GradientParams& params, uint32_t* lut);

// Fills a span list with gradient colors fetched per pixel and blended with
// the given mode. Destination contract is the same as FillSolidSpans
// (premul ColorType::kRGBA; spans clamped to the surface). Coverage is
// folded into each fetched color with AlphaMulQ, like the legacy brush.
//
// Supported today: linear and radial gradients (kSrc and kSrcOver). Returns
// false (without touching pixels) when the gradient type or blend mode is
// unsupported or the accelerator is unavailable/disabled.
bool FillGradientSpans(const GradientParams& params, BlendMode mode,
                       uint8_t* dst_base, size_t row_bytes, int32_t width,
                       int32_t height, const Span* spans, size_t span_count);

}  // namespace sw_accel
}  // namespace skity

#endif  // SRC_RENDER_SW_ACCEL_SW_ACCEL_HPP
