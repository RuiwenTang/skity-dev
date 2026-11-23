// Copyright 2021 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

#ifndef SRC_RENDER_SW_ACCEL_SW_ACCEL_HPP
#define SRC_RENDER_SW_ACCEL_SW_ACCEL_HPP

#include <stddef.h>
#include <stdint.h>

#include <skity/graphic/blend_mode.hpp>

#include "src/render/sw/sw_subpixel.hpp"

// Only the runtime switch is exported from the shared library (dev tooling
// such as the examples calls it); every other entry point is used inside
// the library or by targets that link skity_sw_accel statically, and stays
// hidden together with the Highway namespace. Unified C-ABI-only builds
// export nothing here either.
#if !defined(SKITY_HIDE_CXX_API) && defined(__GNUC__)
#define SW_ACCEL_API __attribute__((visibility("default")))
#else
#define SW_ACCEL_API
#endif

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

}  // namespace sw_accel
}  // namespace skity

#endif  // SRC_RENDER_SW_ACCEL_SW_ACCEL_HPP
