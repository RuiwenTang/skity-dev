// Copyright 2021 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

#ifndef SRC_RENDER_SW_SW_COVERAGE_ROW_BUFFER_HPP
#define SRC_RENDER_SW_SW_COVERAGE_ROW_BUFFER_HPP

#include <cstdint>
#include <vector>

#include "src/render/sw/sw_subpixel.hpp"

namespace skity {

// Per-device-row coverage sink with two accumulation modes, mirroring (and
// replacing) the legacy RealSpanBuilder/SpanBuilder pair:
//
// - AddDirectRun: O(1) append of a constant-coverage run. Used for full-alpha
//   sub-scanline emission (opaque interiors, 1-2px edge pixels, AAA edge
//   rows). These runs never overlap other direct runs by construction of
//   well-formed fills, so they must not pay a per-pixel cost; a 900px
//   interior run costs the same as a 1px run. A run overlapping the dense
//   dirty extent is the one exception: it folds into the dense accumulator
//   (O(len), rare boundary rows only) so every pixel is emitted - and thus
//   blended - exactly once.
// - AddRun / AddAlphas: dense u16 accumulation into the row buffer. Used for
//   partial sub-scanlines (1/4, 1/2 pixel steps) where multiple
//   contributions to the same pixel must sum up and clamp once at 255. Dense
//   coverage arriving on top of pending direct runs folds them into the
//   accumulator for the same reason.
//
// On row change (or TakeSpans) the pending row is coalesced into spans in
// ascending x order by merging the direct runs with the dense dirty extent.
// Rows above top are dropped; zero coverage never produces a span.
//
// Unlike the legacy builders this buffer only clears the dirty extent of the
// dense row, never allocates per trapezoid, merges adjacent equal direct
// runs opportunistically, and clips out-of-range x instead of writing out
// of bounds.
class CoverageRowBuffer {
 public:
  // Coverage can be added to [left, left + width) horizontally; rows with
  // y < top are dropped.
  CoverageRowBuffer(int left, int width, int top);

  CoverageRowBuffer(const CoverageRowBuffer&) = delete;
  CoverageRowBuffer& operator=(const CoverageRowBuffer&) = delete;

  // Appends a constant-coverage run of len pixels starting at x to device
  // row y without touching the dense buffer. Callers guarantee the run does
  // not overlap other direct runs of the same row.
  void AddDirectRun(int x, int y, int len, uint8_t alpha);

  // Accumulates a constant-alpha run of len pixels starting at x into the
  // dense row buffer for device row y.
  void AddRun(int x, int y, int len, uint8_t alpha);

  // Accumulates len per-pixel alphas starting at x into the dense row
  // buffer for device row y.
  void AddAlphas(int x, int y, const uint8_t* alphas, int len);

  // Flushes the pending row (if any) and returns all accumulated spans.
  std::vector<Span> TakeSpans();

  int left() const { return left_; }
  int width() const { return width_; }
  bool HasPendingRow() const { return curr_y_ != SW_NaN32; }

 private:
  // Flushes the pending row (if any) and makes the buffers ready for row y.
  void SwitchRow(int y);
  // Coalesces the pending row into spans_.
  void FlushRow();
  // Emits a direct run, merging into the previous span when adjacent.
  void EmitSpan(int x, int y, int len, uint8_t cover);
  // Adds every pending direct run into the dense accumulator (summing with
  // the dense coverage, which the flush clamps once) and clears the run
  // list. Used when direct runs and dense coverage overlap, so each pixel
  // receives exactly one blended contribution.
  void FoldDirectRunsIntoDense();

  std::vector<uint16_t> row_;
  // Direct (non-accumulated) constant runs of the pending row, plus their
  // [min x, max end) extent used to detect overlaps at O(1) before falling
  // back to the dense accumulator.
  std::vector<Span> direct_runs_;
  int32_t direct_lo_ = 0;
  int32_t direct_hi_ = 0;
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
