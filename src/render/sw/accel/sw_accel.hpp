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
// path, on by default. Defined in the skity target itself (one mirror per
// process), not in the skity_sw_accel archive: test and benchmark
// executables link the archive statically, and a switch defined there would
// shadow the shared library's copy, making A/B arms toggle a state the
// renderer never read. When off, the brush layer falls back to the legacy
// scalar path within the same binary - for A/B comparisons of rendering
// results and performance. Compile-time exclusion (SKITY_SW_ACCEL=OFF)
// remains the packaging knob.
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
// kScreen, including the kDst no-op). The caller must check IsEnabled()
// first (the kernels do not read the runtime switch; the state lives in the
// skity target). Returns false (and does nothing) when the mode is
// unsupported or the accelerator is unavailable (Validate fuse), in which
// case the caller must use the legacy brush path.
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
  // True when the stop table is already premultiplied (the non-zero
  // interpolation flag): interpolation stays in that domain and the
  // trailing rounding premul is skipped.
  bool premul_stops;
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

// Returns a pointer to the 512-entry LUT for the stop table in params,
// building it on a cache miss. The LUT depends only on the stop table and
// tile mode - not on the transform - so animated gradients (same stops,
// moving matrix) reuse one entry across draws. The cache is per-thread and
// holds a few entries; the pointer stays valid only until the acquiring
// thread has acquired a few other gradients, so it MUST be consumed on the
// same thread, within the current draw - never handed to another thread
// (the slot can be evicted and overwritten under it, which corrupts output
// silently). Band-parallel callers should acquire per worker, or copy the
// 2KB table into the job. Returns nullptr on unsupported input (the caller
// falls back).
const uint32_t* AcquireGradientLUT(const GradientParams& params);

// Fills a span list with gradient colors fetched per pixel and blended with
// the given mode. Destination contract is the same as FillSolidSpans
// (premul ColorType::kRGBA; spans clamped to the surface). Coverage is
// folded into each fetched color with AlphaMulQ, like the legacy brush.
//
// kSrc/kSrcOver fuse the fetch and the blend in one pass. Every other mode
// fetches each span row into a dense color buffer (the same fetch kernels)
// and blends it through the per-pixel-src engines - the row-buffer handoff
// keeps one blend call per chunk instead of per-pixel virtual work. The
// cross-channel HSL modes keep a scalar blend. Like FillSolidSpans, the
// caller must check IsEnabled() first. Returns false (without touching
// pixels) when the gradient type or blend mode is unsupported or the
// accelerator is unavailable (Validate fuse), in which case the caller must
// use the legacy brush path.
bool FillGradientSpans(const GradientParams& params, BlendMode mode,
                       uint8_t* dst_base, size_t row_bytes, int32_t width,
                       int32_t height, const Span* spans, size_t span_count);

// Returns true when FillGradientSpans supports the blend mode (every mode:
// fused kSrc/kSrcOver, variable-src engines for the rest, scalar HSL).
bool SupportsGradientMode(BlendMode mode);

// Precomputed, POD description of one image draw, built by the software
// brush from the shader's pixmap, transform and sampling options. The
// kernel reproduces the legacy PixmapBrush chain bit-exactly (the sampled
// color, its premul fold, the coverage fold and the blend), so accelerated
// and legacy output are identical - unlike the gradient LUT, there is no
// rounding headroom.
struct ImageParams {
  // Texture base pointer, ColorType::kRGBA memory (bytes [R, G, B, A],
  // little-endian u32 0xAABBGGRR) with tex_row_bytes stride.
  const uint8_t* texels;
  uint32_t tex_row_bytes;
  int32_t tex_width;
  int32_t tex_height;
  // Entries of points_to_unit_ (device pixel center -> unit uv), evaluated
  // exactly like the scalar MapPoint:
  // u = ((x + 0.5f) * m00 + (y + 0.5f) * m01) + m02, likewise v with m1x.
  float m00, m01, m02;
  float m10, m11, m12;
  // skity::TileMode values for the u (x) and v (y) axes.
  int32_t x_tile_mode;
  int32_t y_tile_mode;
  // FilterMode::kLinear (false = kNearest). Cubic sampling is not
  // accelerated; the scalar sampler handles it.
  bool bilinear;
  // Texture alpha type == kPremul (kUnpremul textures premultiply the
  // sampled color after filtering, like the scalar brush).
  bool src_premul;
  // Paint alpha (255 * paint.GetAlphaF(), the brush's global_alpha_). Each
  // span folds its coverage with MulDiv255Round(cover, global_alpha).
  uint8_t global_alpha;
};

// Returns true when FillImageSpans supports the blend mode (kSrc/kSrcOver).
bool SupportsImageMode(BlendMode mode);

// Fills a span list with image colors fetched per pixel (nearest or bilinear
// filtering) and blended with the given mode. Destination contract is the
// same as FillSolidSpans (premul ColorType::kRGBA; spans clamped to the
// surface). Bit-exact with the legacy scalar brush for every supported
// configuration; kSrc / kSrcOver only.
//
// Like FillSolidSpans, the caller must check IsEnabled() first. Returns
// false (without touching pixels) when the configuration is unsupported or
// the accelerator is unavailable (Validate fuse), in which case the caller
// must use the legacy brush path.
bool FillImageSpans(const ImageParams& params, BlendMode mode,
                    uint8_t* dst_base, size_t row_bytes, int32_t width,
                    int32_t height, const Span* spans, size_t span_count);

// Blends a dense A8 glyph mask with a constant premul color: each mask byte
// is the pixel coverage, folded with global_alpha via MulDiv255Round and
// into the color via AlphaMulQ (both truncating, exactly like the scalar
// brush - the 255-coverage lanes keep the raw color), then blended with the
// given mode. The rect is clipped to the destination surface bounds; the
// mask pointer must cover [0, width) x [0, height) of the rect AFTER the
// clip (the caller shifts it, or lets this function do it - pass the
// unclipped rect and the mask covering it). kSrc / kSrcOver only; other
// modes go through the scalar mask blend. Like FillSolidSpans, the caller
// must check IsEnabled() first.
bool BlendMaskRect(BlendMode mode, uint32_t src_pm, uint8_t global_alpha,
                   uint8_t* dst_base, size_t row_bytes, int32_t dst_width,
                   int32_t dst_height, int32_t x, int32_t y,
                   const uint8_t* mask, size_t mask_stride, int32_t width,
                   int32_t height);

}  // namespace sw_accel
}  // namespace skity

#endif  // SRC_RENDER_SW_ACCEL_SW_ACCEL_HPP
