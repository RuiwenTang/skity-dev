// Copyright 2006 The Android Open Source Project.
// Copyright 2021 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

#ifndef SRC_RENDER_SW_SW_RASTER_HPP
#define SRC_RENDER_SW_SW_RASTER_HPP

#include <array>
#include <glm/glm.hpp>
#include <limits>
#include <skity/graphic/path.hpp>
#include <vector>

#include "src/render/sw/sw_coverage_row_buffer.hpp"
#include "src/render/sw/sw_edge.hpp"
#include "src/render/sw/sw_subpixel.hpp"

namespace skity {

void WalkEdges(SWEdge* prev_head, SWEdge* next_tail,
               Path::PathFillType fill_type, CoverageRowBuffer* buffer,
               int start_y, int stop_y, SWFixed left_clip, SWFixed right_clip);

class SWRaster {
 public:
  constexpr static Rect kCullRect = Rect::MakeLTRB(-1E9F, -1E9F, 1E9F, 1E9F);
  void SetEvenOdd(bool even_odd) { even_odd_ = even_odd; }
  void RastePath(Path const& path, Matrix const& transform,
                 const Rect& clip_bounds = kCullRect);

  std::vector<Span> const& CurrentSpans() const { return spans_; }

  Rect GetBounds() const { return bounds_; }

 private:
  bool even_odd_;
  std::vector<Span> spans_;
  Rect bounds_;
};
}  // namespace skity

#endif  // SRC_RENDER_SW_SW_RASTER_HPP
