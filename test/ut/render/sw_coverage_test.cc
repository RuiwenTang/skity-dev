// Copyright 2021 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "src/render/sw/sw_coverage_row_buffer.hpp"
#include "src/render/sw/sw_raster.hpp"

namespace {

using skity::CoverageRowBuffer;
using skity::Span;
using skity::SpanBuilder;

constexpr int kLeft = 5;
constexpr int kWidth = 40;
constexpr int kTop = 3;

// CoverageRowBuffer is the drop-in replacement for the accumulation side of
// the legacy builder pair; these tests pin the equivalence against
// SpanBuilder (the legacy dense accumulation + Flush path) and cover the
// semantics the legacy path could not express (x clipping instead of OOB
// writes, dirty-extent reuse).

uint32_t NextRandom(uint32_t& state) {
  state ^= state << 13;
  state ^= state >> 17;
  state ^= state << 5;
  return state;
}

struct Op {
  enum Kind { kRun, kAlphas } kind;
  int x;
  int y;
  int len;
  uint8_t alpha;
  std::vector<uint8_t> alphas;
};

std::vector<Span> ApplyToLegacy(const std::vector<Op>& ops) {
  SpanBuilder legacy(kLeft, kWidth, skity::Rect::MakeXYWH(kLeft, kTop, kWidth, 64),
                     nullptr);
  for (const Op& op : ops) {
    if (op.kind == Op::kRun) {
      legacy.BuildSpan(op.x, op.y, op.len, op.alpha);
    } else {
      legacy.BuildSpans(op.x, op.y, op.alphas.data(), op.len);
    }
  }
  // SWRaster::RastePath flushes explicitly before taking the spans.
  legacy.Flush();
  return legacy.TakeSpans();
}

std::vector<Span> ApplyToBuffer(const std::vector<Op>& ops) {
  CoverageRowBuffer buffer(kLeft, kWidth, kTop);
  for (const Op& op : ops) {
    if (op.kind == Op::kRun) {
      buffer.AddRun(op.x, op.y, op.len, op.alpha);
    } else {
      buffer.AddAlphas(op.x, op.y, op.alphas.data(), op.len);
    }
  }
  return buffer.TakeSpans();
}

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

TEST(SWCoverageRowBuffer, MatchesLegacySpanBuilderOnRandomSequences) {
  uint32_t state = 0x12345678u;
  // Random op streams over a few device rows. Rows are generated in
  // non-decreasing y order (the rasterizer walker is monotonic). x stays
  // inside the buffer because the legacy builder has no bounds clipping
  // (out-of-range x is UB there); the clipping behavior of the new buffer is
  // covered by SWCoverageRowBuffer.ClipsOutOfRangePixels below.
  for (int round = 0; round < 8; round++) {
    std::vector<Op> ops;
    int y = kTop - 2;  // exercises the dropped-rows path first
    for (int i = 0; i < 400; i++) {
      if (NextRandom(state) % 37 == 0) {
        y++;
      }
      Op op{};
      op.y = y;
      op.len = static_cast<int>(NextRandom(state) % 20) + 1;
      op.x = kLeft + static_cast<int>(NextRandom(state) % (kWidth - 20));
      if (NextRandom(state) % 3 == 0) {
        op.kind = Op::kAlphas;
        op.alphas.resize(op.len);
        for (int j = 0; j < op.len; j++) {
          op.alphas[j] = static_cast<uint8_t>(NextRandom(state) % 256);
        }
      } else {
        op.kind = Op::kRun;
        op.alpha = static_cast<uint8_t>(NextRandom(state) % 256);
      }
      ops.push_back(op);
    }
    ExpectSpanListsEqual(ApplyToLegacy(ops), ApplyToBuffer(ops));
  }
}

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
