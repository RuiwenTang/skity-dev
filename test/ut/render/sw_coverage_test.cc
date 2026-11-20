// Copyright 2021 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "src/render/sw/sw_coverage_row_buffer.hpp"

namespace {

using skity::CoverageRowBuffer;
using skity::Span;

constexpr int kLeft = 5;
constexpr int kWidth = 40;
constexpr int kTop = 3;

// CoverageRowBuffer replaced the legacy RealSpanBuilder/SpanBuilder pair as
// the rasterizer's single emission sink. Equivalence to the legacy
// accumulation path was pinned by randomized differential tests while both
// implementations existed; these tests cover the semantics directly.

void ExpectSpanListsEqual(const std::vector<Span>& expected,
                          const std::vector<Span>& actual) {
  ASSERT_EQ(expected.size(), actual.size());
  for (size_t i = 0; i < expected.size(); i++) {
    EXPECT_EQ(expected[i].x, actual[i].x) << "span " << i;
    EXPECT_EQ(expected[i].y, actual[i].y) << "span " << i;
    EXPECT_EQ(expected[i].len, actual[i].len) << "span " << i;
    EXPECT_EQ(expected[i].cover, actual[i].cover) << "span " << i;
  }
}

std::vector<Span> Spans(std::initializer_list<Span> spans) {
  return std::vector<Span>(spans);
}

}  // namespace

TEST(SWCoverageRowBuffer, ClampsCoverageAndMergesEqualRuns) {
  CoverageRowBuffer buffer(kLeft, kWidth, kTop);
  // Overlapping contributions sum up and clamp at 255.
  buffer.AddRun(6, 4, 3, 200);
  buffer.AddRun(7, 4, 1, 200);
  // Equal adjacent coverages coalesce into one run.
  buffer.AddAlphas(10, 4, std::vector<uint8_t>{100, 100, 100, 100}.data(), 4);

  std::vector<Span> spans = buffer.TakeSpans();
  ExpectSpanListsEqual(
      Spans({Span{6, 4, 1, 200}, Span{7, 4, 1, 255}, Span{8, 4, 1, 200},
             Span{10, 4, 4, 100}}),
      spans);
}

TEST(SWCoverageRowBuffer, FlushesRowsInOrderAndClearsDirtyExtent) {
  CoverageRowBuffer buffer(kLeft, kWidth, kTop);
  buffer.AddRun(6, 5, 4, 255);
  // Next row touches a subset of the previous dirty extent; the remainder
  // must not leak between rows.
  buffer.AddRun(7, 6, 1, 90);

  std::vector<Span> spans = buffer.TakeSpans();
  ExpectSpanListsEqual(Spans({Span{6, 5, 4, 255}, Span{7, 6, 1, 90}}),
                       spans);
}

TEST(SWCoverageRowBuffer, ClipsOutOfRangePixels) {
  CoverageRowBuffer buffer(kLeft, kWidth, kTop);
  // Partially outside on the left: [3, 13) keeps [left, 13).
  buffer.AddRun(3, 5, 10, 80);
  // Fully outside.
  buffer.AddRun(kLeft + kWidth, 5, 8, 80);
  // Outside on both sides: [-2, 98) keeps the whole [left, left + width).
  buffer.AddRun(-2, 6, 100, 80);
  // Rows above top are dropped.
  buffer.AddRun(6, kTop - 1, 4, 255);

  std::vector<Span> spans = buffer.TakeSpans();
  ExpectSpanListsEqual(
      Spans({Span{kLeft, 5, 8, 80}, Span{kLeft, 6, kWidth, 80}}), spans);
}

TEST(SWCoverageRowBuffer, MergesDirectRunsWithDenseCoverage) {
  CoverageRowBuffer buffer(kLeft, kWidth, kTop);
  // Partial sub-scanline edges accumulate densely; the opaque interior is a
  // direct run; the flush merges both in ascending x order.
  buffer.AddRun(6, 5, 2, 64);
  buffer.AddDirectRun(8, 5, 4, 255);
  buffer.AddRun(12, 5, 2, 128);
  // Adjacent equal direct runs merge, in AddDirectRun and in the flush.
  buffer.AddDirectRun(20, 5, 2, 255);
  buffer.AddDirectRun(22, 5, 3, 255);

  std::vector<Span> spans = buffer.TakeSpans();
  ExpectSpanListsEqual(
      Spans({Span{6, 5, 2, 64}, Span{8, 5, 4, 255}, Span{12, 5, 2, 128},
             Span{20, 5, 5, 255}}),
      spans);
}

TEST(SWCoverageRowBuffer, EmptyTakeSpans) {
  CoverageRowBuffer buffer(kLeft, kWidth, kTop);
  EXPECT_TRUE(buffer.TakeSpans().empty());
  EXPECT_FALSE(buffer.HasPendingRow());

  buffer.AddRun(6, 5, 2, 10);
  EXPECT_TRUE(buffer.HasPendingRow());
  EXPECT_FALSE(buffer.TakeSpans().empty());
  // A second take yields nothing and the buffer stays usable.
  EXPECT_TRUE(buffer.TakeSpans().empty());
  buffer.AddRun(6, 6, 2, 10);
  EXPECT_EQ(1u, buffer.TakeSpans().size());
}
