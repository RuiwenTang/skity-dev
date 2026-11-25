// Copyright 2021 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

#include "src/render/sw/sw_raster_clip.hpp"

#include <gtest/gtest.h>
#include <stdint.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "src/graphic/color_priv.hpp"

namespace {

using skity::MulDiv255Round;
using skity::RasterClip;
using skity::Span;

constexpr int32_t kW = 40;
constexpr int32_t kH = 17;

// Dense reference model of the clip stack: one coverage byte per pixel.
struct DenseClip {
  std::vector<uint8_t> cover;

  DenseClip() : cover(static_cast<size_t>(kW) * kH, 255u) {}

  void Intersect(const RasterClip& clip) { Apply(clip, false); }
  void Difference(const RasterClip& clip) { Apply(clip, true); }

  void Apply(const RasterClip& clip, bool difference) {
    for (int32_t y = 0; y < kH; y++) {
      for (int32_t x = 0; x < kW; x++) {
        const size_t i = static_cast<size_t>(y) * kW + x;
        uint8_t add = ClipPixel(clip, x, y);
        if (difference) {
          cover[i] = static_cast<uint8_t>(std::max(0, cover[i] - add));
        } else {
          cover[i] = static_cast<uint8_t>(MulDiv255Round(cover[i], add));
        }
      }
    }
  }

  // The reference per-pixel semantics of the two tiers.
  static uint8_t ClipPixel(const RasterClip& clip, int32_t x, int32_t y) {
    if (x < clip.bounds.Left() || x >= clip.bounds.Right() ||
        y < clip.bounds.Top() || y >= clip.bounds.Bottom()) {
      return 0;
    }
    if (clip.opaque) {
      return 255;
    }
    auto it = std::lower_bound(
        clip.runs.begin(), clip.runs.end(), y,
        [](const Span& s, int32_t value) { return s.y < value; });
    while (it != clip.runs.end() && it->y == y) {
      if (x >= it->x && x < it->x + it->len) {
        return static_cast<uint8_t>(it->cover);
      }
      ++it;
    }
    return 0;
  }
};

// Expands a resolved clip into the dense grid: outside its bounds is zero.
void ExpectClipMatchesDense(const RasterClip& clip, const DenseClip& dense) {
  for (int32_t y = 0; y < kH; y++) {
    for (int32_t x = 0; x < kW; x++) {
      const size_t i = static_cast<size_t>(y) * kW + x;
      const int32_t diff =
          static_cast<int32_t>(DenseClip::ClipPixel(clip, x, y)) -
          static_cast<int32_t>(dense.cover[i]);
      ASSERT_EQ(0, diff) << "pixel (" << x << ", " << y << ")";
    }
  }
}

uint32_t NextRandom(uint32_t& state) {
  state ^= state << 13;
  state ^= state >> 17;
  state ^= state << 5;
  return state;
}

// Random per-row coverage segments covering an arbitrary sub-rectangle with
// random coverages (the shape the run algebra must handle generically).
RasterClip RandomRunsClip(uint32_t& state) {
  const int32_t left = static_cast<int32_t>(NextRandom(state) % kW);
  const int32_t right =
      left + 1 + static_cast<int32_t>(NextRandom(state) % (kW - left));
  const int32_t top = static_cast<int32_t>(NextRandom(state) % kH);
  const int32_t bottom =
      top + 1 + static_cast<int32_t>(NextRandom(state) % (kH - top));
  std::vector<Span> runs;
  for (int32_t y = top; y < bottom; y++) {
    int32_t x = left;
    while (x < right) {
      const int32_t len =
          1 + static_cast<int32_t>(NextRandom(state) %
                                   static_cast<uint32_t>(right - x));
      const uint32_t cover = NextRandom(state) % 256;
      if (cover > 0) {
        runs.push_back(Span{x, y, len, static_cast<int32_t>(cover)});
      }
      x += len;
    }
  }
  std::sort(runs.begin(), runs.end(), [](const Span& a, const Span& b) {
    return a.y != b.y ? a.y < b.y : a.x < b.x;
  });
  return skity::MakeRunsClip(
      skity::Rect::MakeLTRB(static_cast<float>(left), static_cast<float>(top),
                            static_cast<float>(right),
                            static_cast<float>(bottom)),
      std::move(runs));
}

RasterClip RandomBoundsClip(uint32_t& state) {
  const int32_t left = static_cast<int32_t>(NextRandom(state) % kW);
  const int32_t right =
      left + 1 + static_cast<int32_t>(NextRandom(state) % (kW - left));
  const int32_t top = static_cast<int32_t>(NextRandom(state) % kH);
  const int32_t bottom =
      top + 1 + static_cast<int32_t>(NextRandom(state) % (kH - top));
  return skity::MakeBoundsClip(skity::Rect::MakeLTRB(
      static_cast<float>(left), static_cast<float>(top),
      static_cast<float>(right), static_cast<float>(bottom)));
}

}  // namespace

TEST(SWRasterClip, IntersectAndDifferenceMatchDense) {
  // Fuzz clip-op sequences across both tiers; the run-encoded result must
  // agree with the per-pixel dense reference everywhere.
  uint32_t state = 0x9e3779b9u;
  for (int round = 0; round < 200; round++) {
    DenseClip dense;
    RasterClip clip = skity::MakeBoundsClip(skity::Rect::MakeLTRB(
        0.f, 0.f, static_cast<float>(kW), static_cast<float>(kH)));
    const int ops = 1 + static_cast<int>(NextRandom(state) % 6);
    for (int i = 0; i < ops; i++) {
      const bool difference = (NextRandom(state) % 2) == 0;
      RasterClip op_clip = (NextRandom(state) % 3) == 0
                               ? RandomBoundsClip(state)
                               : RandomRunsClip(state);
      if (difference) {
        dense.Difference(op_clip);
        clip = skity::DifferenceClip(clip, op_clip);
      } else {
        dense.Intersect(op_clip);
        clip = skity::IntersectClip(clip, op_clip);
      }
    }
    ExpectClipMatchesDense(clip, dense);
  }
}

TEST(SWRasterClip, LeadingDifferenceFramesTheSurface) {
  // A leading kDifference subtracts from the implicit full-surface base.
  DenseClip dense;
  RasterClip clip = skity::MakeBoundsClip(skity::Rect::MakeLTRB(
      0.f, 0.f, static_cast<float>(kW), static_cast<float>(kH)));
  const auto hole =
      skity::MakeBoundsClip(skity::Rect::MakeLTRB(5.f, 3.f, 11.f, 7.f));
  dense.Difference(hole);
  clip = skity::DifferenceClip(clip, hole);
  ExpectClipMatchesDense(clip, dense);
}

TEST(SWRasterClip, EmptyAndDegenerateOps) {
  // Intersecting with an empty clip kills everything; differencing with one
  // is a no-op; difference of a full clip by itself empties it.
  DenseClip dense;
  RasterClip clip = skity::MakeBoundsClip(skity::Rect::MakeLTRB(
      0.f, 0.f, static_cast<float>(kW), static_cast<float>(kH)));
  const auto empty = skity::MakeBoundsClip(skity::Rect::MakeEmpty());
  dense.Intersect(empty);
  clip = skity::IntersectClip(clip, empty);
  ExpectClipMatchesDense(clip, dense);
  EXPECT_TRUE(clip.IsEmpty());

  DenseClip dense2;
  RasterClip clip2 = skity::MakeBoundsClip(skity::Rect::MakeLTRB(
      0.f, 0.f, static_cast<float>(kW), static_cast<float>(kH)));
  dense2.Difference(empty);
  clip2 = skity::DifferenceClip(clip2, empty);
  ExpectClipMatchesDense(clip2, dense2);

  DenseClip dense3;
  RasterClip clip3 = skity::MakeBoundsClip(skity::Rect::MakeLTRB(
      0.f, 0.f, static_cast<float>(kW), static_cast<float>(kH)));
  dense3.Difference(clip3);
  clip3 = skity::DifferenceClip(clip3, clip3);
  ExpectClipMatchesDense(clip3, dense3);
  EXPECT_TRUE(clip3.IsEmpty());
}

TEST(SWRasterClip, ClipSpansComposeFillCoverage) {
  // Fill spans clipped through the resolved clip produce exactly the
  // per-pixel product of fill and clip coverage.
  uint32_t state = 0x51ed270bu;
  for (int round = 0; round < 100; round++) {
    DenseClip dense;
    RasterClip clip = skity::MakeBoundsClip(skity::Rect::MakeLTRB(
        0.f, 0.f, static_cast<float>(kW), static_cast<float>(kH)));
    const int ops = 1 + static_cast<int>(NextRandom(state) % 4);
    std::string trace;
    for (int i = 0; i < ops; i++) {
      RasterClip op_clip = (NextRandom(state) % 3) == 0
                               ? RandomBoundsClip(state)
                               : RandomRunsClip(state);
      char buf[128];
      std::snprintf(buf, sizeof(buf),
                    "op %d: bounds LTRB(%.3f, %.3f, %.3f, %.3f) runs=%zu\n", i,
                    op_clip.bounds.Left(), op_clip.bounds.Top(),
                    op_clip.bounds.Right(), op_clip.bounds.Bottom(),
                    op_clip.runs.size());
      trace += buf;
      dense.Intersect(op_clip);
      clip = skity::IntersectClip(clip, op_clip);
    }
    char tail[256];
    std::snprintf(tail, sizeof(tail),
                  "resolved bounds LTRB(%.3f, %.3f, %.3f, %.3f) runs=%zu\n",
                  clip.bounds.Left(), clip.bounds.Top(), clip.bounds.Right(),
                  clip.bounds.Bottom(), clip.runs.size());
    trace += tail;
    for (const Span& s : clip.runs) {
      if (s.y < 4) {
        char line[128];
        std::snprintf(line, sizeof(line), "  run y=%d x=%d len=%d cover=%d\n",
                      s.y, s.x, s.len, s.cover);
        trace += line;
      }
    }
    setenv("CLIP_TRACE", trace.c_str(), 1);

    // Random fill spans.
    std::vector<Span> fills;
    for (int32_t y = 0; y < kH; y++) {
      int32_t x = static_cast<int32_t>(NextRandom(state) % 5);
      while (x < kW) {
        const int32_t len = 1 + static_cast<int32_t>(NextRandom(state) % 9);
        // Real raster spans always carry coverage >= 1.
        const uint32_t cover = 1 + NextRandom(state) % 255;
        fills.push_back(Span{x, y, len, static_cast<int32_t>(cover)});
        x += len + static_cast<int32_t>(NextRandom(state) % 4);
      }
    }

    const std::vector<Span> clipped = skity::ClipSpans(clip, fills);
    for (const Span& s : clipped) {
      if (s.y < 2 || s.cover == 230) {
        char line[128];
        std::snprintf(line, sizeof(line),
                      "  clipped span y=%d x=%d len=%d cover=%d\n", s.y, s.x,
                      s.len, s.cover);
        trace += line;
      }
    }
    setenv("CLIP_TRACE", trace.c_str(), 1);
    // Expand the clipped spans densely and compare against the reference
    // product (fill coverage x clip coverage).
    std::vector<int32_t> expect(static_cast<size_t>(kW) * kH, -1);
    for (const Span& fill : fills) {
      for (int32_t x = fill.x; x < fill.x + fill.len && x < kW; x++) {
        if (x < 0) {
          continue;
        }
        const size_t i = static_cast<size_t>(fill.y) * kW + x;
        // Pixels with zero composed coverage are dropped (not blitted) - the
        // legacy FindSpan semantics and the Skia blitter's. Small coverages
        // multiply to a rounded zero (e.g. 1 x 1).
        const int32_t composed = MulDiv255Round(fill.cover, dense.cover[i]);
        expect[i] = dense.cover[i] == 0 || composed == 0 ? -1 : composed;
      }
    }
    std::vector<int32_t> actual(static_cast<size_t>(kW) * kH, -1);
    for (const Span& s : clipped) {
      for (int32_t x = s.x; x < s.x + s.len; x++) {
        actual[static_cast<size_t>(s.y) * kW + x] = s.cover;
      }
    }
    for (int32_t y = 0; y < kH; y++) {
      for (int32_t x = 0; x < kW; x++) {
        const size_t i = static_cast<size_t>(y) * kW + x;
        if (expect[i] != actual[i]) {
          ADD_FAILURE() << "round " << round << " pixel (" << x << ", " << y
                        << ") expect " << expect[i] << " actual " << actual[i]
                        << "\n"
                        << getenv("CLIP_TRACE");
          return;
        }
      }
    }
  }
}

TEST(SWRasterClip, StageClipRowMatchesCoverage) {
  const auto clip = skity::MakeRunsClip(
      skity::Rect::MakeLTRB(0.f, 0.f, static_cast<float>(kW),
                            static_cast<float>(kH)),
      {{2, 1, 4, 128}, {10, 1, 5, 255}, {2, 3, 30, 64}});
  std::vector<uint8_t> staged(static_cast<size_t>(kW));
  for (int32_t y = 0; y < kH; y++) {
    skity::StageClipRow(clip, y, 0, kW, staged.data());
    for (int32_t x = 0; x < kW; x++) {
      ASSERT_EQ(DenseClip::ClipPixel(clip, x, y),
                staged[static_cast<size_t>(x)])
          << "pixel (" << x << ", " << y << ")";
    }
  }
}
