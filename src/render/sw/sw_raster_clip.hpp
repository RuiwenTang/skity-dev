// Copyright 2021 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

#ifndef SRC_RENDER_SW_SW_RASTER_CLIP_HPP
#define SRC_RENDER_SW_SW_RASTER_CLIP_HPP

#include <cstdint>
#include <skity/geometry/rect.hpp>
#include <vector>

#include "src/render/sw/sw_subpixel.hpp"

namespace skity {

// A resolved raster clip: the per-pixel antialiased coverage of the whole
// clip stack at one save state, in device pixels.
//
// Two storage tiers:
// - opaque (runs empty): every pixel inside `bounds` is fully covered; the
//   bounds alone describe the clip. Integer-aligned rect intersections land
//   here - no per-pixel work at all.
// - run-encoded: per-row sorted coverage segments (Span{x, y, len, cover},
//   cover in [1, 255]). Pixels inside bounds without a segment are fully
//   outside the clip. Memory grows with the clip's edge complexity, not its
//   area, so deep save/restore stacks of large path clips stay cheap.
//
// Combine operations happen at clip time (IntersectClip / DifferenceClip);
// fills only ever see one resolved clip. Antialiased coverage composes with
// MulDiv255Round (multiply), matching how every other coverage mask in the
// pipeline combines; difference subtracts and clamps at zero.
struct RasterClip {
  Rect bounds{};
  std::vector<Span> runs{};
  bool opaque = true;

  bool IsEmpty() const { return bounds.IsEmpty(); }

  bool HasRuns() const { return !opaque && !runs.empty(); }
};

// Opaque clip: fully covered inside device_bounds.
RasterClip MakeBoundsClip(const Rect& device_bounds);

// Run-encoded clip from rasterized coverage spans (already device space,
// row-sorted). bounds must contain the spans.
RasterClip MakeRunsClip(const Rect& device_bounds, std::vector<Span> runs);

// cur = cur ∩ add, composing antialiased coverage with MulDiv255Round.
RasterClip IntersectClip(const RasterClip& cur, const RasterClip& add);

// cur = cur - add, subtracting coverage and clamping at zero.
RasterClip DifferenceClip(const RasterClip& cur, const RasterClip& sub);

// Intersects fill spans with the resolved clip, producing the final blended
// spans (coverage multiplied at clip edges, spans split where the clip
// coverage changes). Both inputs are row-sorted. When the clip is opaque the
// spans pass through unchanged (bounds were already applied by the
// rasterizer).
std::vector<Span> ClipSpans(const RasterClip& clip,
                            std::vector<Span> const& spans);

// Stages one row of the clip into a dense coverage buffer covering
// [x0, x1): staged[i] is the clip coverage of device pixel x0 + i. The
// buffer must hold x1 - x0 bytes. Used by blitters that consume dense mask
// rows (the glyph mask blit) so the storage tier stays opaque to them.
void StageClipRow(const RasterClip& clip, int32_t y, int32_t x0, int32_t x1,
                  uint8_t* staged);

}  // namespace skity

#endif  // SRC_RENDER_SW_SW_RASTER_CLIP_HPP
