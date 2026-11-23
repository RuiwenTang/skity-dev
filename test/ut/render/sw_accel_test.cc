// Copyright 2021 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

#include <gtest/gtest.h>

#ifdef SKITY_SW_ACCEL

#include <stdint.h>

#include <vector>

#include "src/graphic/color_priv.hpp"
#include "src/render/sw/accel/sw_accel.hpp"

namespace {

using skity::BlendMode;
using skity::Span;
using skity::sw_accel::FillSolidSpans;

uint32_t NextRandom(uint32_t& state) {
  state ^= state << 13;
  state ^= state >> 17;
  state ^= state << 5;
  return state;
}

uint32_t RandomColor(uint32_t& state) {
  const uint32_t a = NextRandom(state) % 256;
  const uint32_t r = NextRandom(state) % (a + 1);
  const uint32_t g = NextRandom(state) % (a + 1);
  const uint32_t b = NextRandom(state) % (a + 1);
  return (a << 24) | (r << 16) | (g << 8) | b;
}

// Scalar mirror of the legacy SolidColorBrush path: fold coverage into src,
// then PorterDuffBlend. Takes and returns memory words (0xAABBGGRR,
// ColorType::kRGBA layout) like the kernels do.
uint32_t ReferenceBlend(BlendMode mode, uint32_t src_pm, uint32_t dst_mem,
                        uint8_t cover) {
  uint32_t dst = skity::PMColorSwapRB(dst_mem);
  uint32_t src = cover == 255 ? src_pm : skity::AlphaMulQ(src_pm, cover);
  uint32_t result;
  switch (mode) {
    case BlendMode::kClear:
      result = 0;
      break;
    case BlendMode::kSrc:
      result = src;
      break;
    case BlendMode::kSrcOver:
      if (ColorGetA(src) == 0) {
        result = dst;
      } else {
        result = skity::PMSrcOver(src, dst);
      }
      break;
    default:
      result = dst;
      break;
  }
  return skity::PMColorSwapRB(result);
}

constexpr int32_t kWidth = 64;
constexpr int32_t kHeight = 5;

void FillReference(std::vector<uint32_t>& bitmap, const std::vector<Span>& spans,
                   BlendMode mode, uint32_t src_pm) {
  for (const Span& span : spans) {
    int32_t x = span.x;
    int32_t y = span.y;
    int32_t len = span.len;
    if (y < 0 || y >= kHeight || x >= kWidth || len <= 0) {
      continue;
    }
    if (x < 0) {
      len += x;
      x = 0;
    }
    if (x + len > kWidth) {
      len = kWidth - x;
    }
    int32_t cover = span.cover < 0     ? 0
                    : span.cover > 255 ? 255
                                       : span.cover;
    for (int32_t i = 0; i < len; i++) {
      size_t idx = static_cast<size_t>(y) * kWidth + x + i;
      bitmap[idx] =
          ReferenceBlend(mode, src_pm, bitmap[idx], static_cast<uint8_t>(cover));
    }
  }
}

std::vector<Span> RandomSpans(uint32_t& state) {
  std::vector<Span> spans;
  for (int y = 0; y < kHeight; y++) {
    int32_t x = static_cast<int32_t>(NextRandom(state) % 20) - 4;
    while (x < kWidth) {
      int32_t len = static_cast<int32_t>(NextRandom(state) % 30) + 1;
      spans.push_back(Span{x, y, len, static_cast<int32_t>(NextRandom(state) % 256)});
      x += len + static_cast<int32_t>(NextRandom(state) % 4) - 1;
    }
  }
  return spans;
}

}  // namespace

TEST(SWAccel, ValidatePasses) {
  EXPECT_TRUE(skity::sw_accel::Validate());
  EXPECT_TRUE(skity::sw_accel::Available());
}

TEST(SWAccel, SolidSpansMatchScalarReference) {
  uint32_t state = 0x9e3779b9u;
  const BlendMode modes[] = {BlendMode::kClear, BlendMode::kSrc,
                             BlendMode::kSrcOver};
  const uint32_t edge_colors[] = {0x00000000u, 0xffffffffu, 0x80404040u};

  for (int round = 0; round < 20; round++) {
    for (BlendMode mode : modes) {
      std::vector<uint32_t> actual(kWidth * kHeight);
      std::vector<uint32_t> expect(kWidth * kHeight);
      for (auto& px : expect) {
        px = RandomColor(state);
      }
      actual = expect;

      std::vector<Span> spans = RandomSpans(state);
      uint32_t src_pm = round % 3 == 0
                            ? edge_colors[round % 3]
                            : RandomColor(state);
      ASSERT_TRUE(FillSolidSpans(
          mode, src_pm, reinterpret_cast<uint8_t*>(actual.data()),
          kWidth * 4, kWidth, kHeight, spans.data(), spans.size()));
      FillReference(expect, spans, mode, src_pm);
      ASSERT_EQ(expect, actual) << "round " << round;
    }
  }
}

TEST(SWAccel, SolidSpansClampOutOfBounds) {
  std::vector<uint32_t> actual(kWidth * kHeight, 0xff00ff00u);
  std::vector<uint32_t> expect = actual;
  // Out of bounds on every side; only the in-bounds slice may change.
  std::vector<Span> spans = {
      Span{-10, 2, 30, 255}, Span{60, 2, 100, 255}, Span{0, -3, 10, 255},
      Span{0, kHeight + 2, 10, 255},
  };
  ASSERT_TRUE(FillSolidSpans(BlendMode::kSrcOver, 0xffffffffu,
                             reinterpret_cast<uint8_t*>(actual.data()),
                             kWidth * 4, kWidth, kHeight, spans.data(),
                             spans.size()));
  FillReference(expect, spans, BlendMode::kSrcOver, 0xffffffffu);
  EXPECT_EQ(expect, actual);
}

TEST(SWAccel, UnsupportedModeFallsBack) {
  EXPECT_FALSE(skity::sw_accel::SupportsSolidMode(BlendMode::kDstOver));
  std::vector<uint32_t> bitmap(kWidth, 0xffffffffu);
  Span span{0, 0, 4, 255};
  EXPECT_FALSE(FillSolidSpans(BlendMode::kDstOver, 0xff000000u,
                              reinterpret_cast<uint8_t*>(bitmap.data()),
                              kWidth * 4, kWidth, 1, &span, 1));
  // Nothing was written.
  EXPECT_EQ(0xffffffffu, bitmap[0]);
}

#endif  // SKITY_SW_ACCEL
