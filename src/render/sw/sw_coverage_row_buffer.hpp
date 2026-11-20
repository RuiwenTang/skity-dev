// Copyright 2021 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

#ifndef SRC_RENDER_SW_SW_COVERAGE_ROW_BUFFER_HPP
#define SRC_RENDER_SW_SW_COVERAGE_ROW_BUFFER_HPP

#include <cstdint>
#include <vector>

#include "src/render/sw/sw_subpixel.hpp"

namespace skity {

// Dense per-device-row coverage accumulator.
//
// Replaces the legacy RealSpanBuilder/SpanBuilder pair as the single sink for
// all rasterizer emission points (blit_single_alpha / blit_two_alphas /
// blit_full_alpha / blit_aaa_trapezoid_row). Coverage is accumulated per pixel
// into a u16 row buffer; when the rasterizer moves to the next device row the
// pending row is clamped once to 8-bit and coalesced into constant-coverage
// runs (Span), in ascending x order.
//
// Semantics match the legacy accumulation path (SpanBuilder + Flush):
// - multiple contributions to the same pixel sum up and saturate at 255
//   (the legacy code saturated at every step; equivalent because
//   contributions are non-negative);
// - rows above top are dropped, like the legacy builders did;
// - zero coverage never produces a span.
//
// Unlike the legacy builders this buffer only clears and scans the dirty
// extent of each row, never allocates per trapezoid, and clips out-of-range
// x instead of writing out of bounds.
class CoverageRowBuffer {
 public:
  // Coverage can be added to [left, left + width) horizontally; rows with
  // y < top are dropped.
  CoverageRowBuffer(int left, int width, int top);

  CoverageRowBuffer(const CoverageRowBuffer&) = delete;
  CoverageRowBuffer& operator=(const CoverageRowBuffer&) = delete;

  // Adds a constant-alpha run of len pixels starting at x to device row y.
  void AddRun(int x, int y, int len, uint8_t alpha);

  // Adds len per-pixel alphas starting at x to device row y.
  void AddAlphas(int x, int y, const uint8_t* alphas, int len);

  // Flushes the pending row (if any) and returns all accumulated spans.
  std::vector<Span> TakeSpans();

  int left() const { return left_; }
  int width() const { return width_; }
  bool HasPendingRow() const { return curr_y_ != SW_NaN32; }

 private:
  // Flushes the pending row (if any) and makes row_ ready for row y.
  void SwitchRow(int y);
  // Coalesces the dirty extent of the pending row into spans_.
  void FlushRow();

  std::vector<uint16_t> row_;
  std::vector<Span> spans_;
  int32_t curr_y_ = SW_NaN32;
  int32_t left_;
  int32_t width_;
  int32_t top_;
  // Dirty extent of row_ in buffer-relative coordinates; the extent is empty
  // when max_x_ <= min_x_.
  int32_t min_x_;
  int32_t max_x_;
};

}  // namespace skity

#endif  // SRC_RENDER_SW_SW_COVERAGE_ROW_BUFFER_HPP
