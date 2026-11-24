// Copyright 2021 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

#include <gtest/gtest.h>

#ifdef SKITY_SW_ACCEL

#include <stdint.h>

#include <cmath>
#include <skity/effect/shader.hpp>
#include <skity/graphic/bitmap.hpp>
#include <skity/graphic/paint.hpp>
#include <skity/render/canvas.hpp>
#include <vector>

#include "src/graphic/blend_mode_priv.hpp"
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
// then the real PorterDuffBlend (the exact function the legacy brush calls).
// Takes and returns memory words (0xAABBGGRR, ColorType::kRGBA layout) like
// the kernels do.
uint32_t ReferenceBlend(BlendMode mode, uint32_t src_pm, uint32_t dst_mem,
                        uint8_t cover) {
  const uint32_t src = cover == 255 ? src_pm : skity::AlphaMulQ(src_pm, cover);
  return skity::PMColorSwapRB(
      skity::PorterDuffBlend(src, skity::PMColorSwapRB(dst_mem), mode));
}

constexpr int32_t kWidth = 64;
constexpr int32_t kHeight = 5;

void FillReference(std::vector<uint32_t>& bitmap,
                   const std::vector<Span>& spans, BlendMode mode,
                   uint32_t src_pm) {
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
    int32_t cover = span.cover < 0 ? 0 : span.cover > 255 ? 255 : span.cover;
    for (int32_t i = 0; i < len; i++) {
      size_t idx = static_cast<size_t>(y) * kWidth + x + i;
      bitmap[idx] = ReferenceBlend(mode, src_pm, bitmap[idx],
                                   static_cast<uint8_t>(cover));
    }
  }
}

std::vector<Span> RandomSpans(uint32_t& state) {
  std::vector<Span> spans;
  for (int y = 0; y < kHeight; y++) {
    int32_t x = static_cast<int32_t>(NextRandom(state) % 20) - 4;
    while (x < kWidth) {
      int32_t len = static_cast<int32_t>(NextRandom(state) % 30) + 1;
      spans.push_back(
          Span{x, y, len, static_cast<int32_t>(NextRandom(state) % 256)});
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
  // Every blend mode, kClear through kLuminosity: coeff modes in integer
  // SIMD, advanced modes in f32 SIMD against the WGSL-aligned scalar
  // reference. kSoftLight carries a +/-1 tolerance (Highway's NEON Sqrt is
  // an rsqrte approximation); everything else is bit-exact.
  std::vector<BlendMode> modes;
  for (int m = 0; m <= static_cast<int>(BlendMode::kLastMode); m++) {
    modes.push_back(static_cast<BlendMode>(m));
  }
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
      uint32_t src_pm =
          round % 3 == 0 ? edge_colors[round % 3] : RandomColor(state);
      ASSERT_TRUE(FillSolidSpans(
          mode, src_pm, reinterpret_cast<uint8_t*>(actual.data()), kWidth * 4,
          kWidth, kHeight, spans.data(), spans.size()));
      FillReference(expect, spans, mode, src_pm);
      if (mode == BlendMode::kSoftLight) {
        for (size_t i = 0; i < expect.size(); i++) {
          for (int shift = 0; shift < 32; shift += 8) {
            const int32_t d =
                static_cast<int32_t>((expect[i] >> shift) & 0xff) -
                static_cast<int32_t>((actual[i] >> shift) & 0xff);
            ASSERT_LE(d < 0 ? -d : d, 1)
                << "round " << round << " pixel " << i << " shift " << shift;
          }
        }
      } else {
        ASSERT_EQ(expect, actual) << "round " << round;
      }
    }
  }
}

TEST(SWAccel, SolidSpansClampOutOfBounds) {
  std::vector<uint32_t> actual(kWidth * kHeight, 0xff00ff00u);
  std::vector<uint32_t> expect = actual;
  // Out of bounds on every side; only the in-bounds slice may change.
  std::vector<Span> spans = {
      Span{-10, 2, 30, 255},
      Span{60, 2, 100, 255},
      Span{0, -3, 10, 255},
      Span{0, kHeight + 2, 10, 255},
  };
  ASSERT_TRUE(FillSolidSpans(BlendMode::kSrcOver, 0xffffffffu,
                             reinterpret_cast<uint8_t*>(actual.data()),
                             kWidth * 4, kWidth, kHeight, spans.data(),
                             spans.size()));
  FillReference(expect, spans, BlendMode::kSrcOver, 0xffffffffu);
  EXPECT_EQ(expect, actual);
}

TEST(SWAccel, SupportsSolidModeCoversEveryMode) {
  // The kernel table is complete: every BlendMode value is supported.
  for (int m = 0; m <= static_cast<int>(BlendMode::kLastMode); m++) {
    EXPECT_TRUE(skity::sw_accel::SupportsSolidMode(static_cast<BlendMode>(m)));
  }
}

TEST(SWAccel, RuntimeSwitchTogglesFallback) {
  std::vector<uint32_t> bitmap(kWidth, 0xffffffffu);
  Span span{0, 0, 4, 255};

  skity::sw_accel::SetEnabled(true);
  EXPECT_TRUE(skity::sw_accel::IsEnabled());
  EXPECT_TRUE(FillSolidSpans(BlendMode::kSrc, 0xff00ff00u,
                             reinterpret_cast<uint8_t*>(bitmap.data()),
                             kWidth * 4, kWidth, 1, &span, 1));

  // Disabled at runtime: the brush layer checks IsEnabled() and falls back
  // to the legacy path; the raw kernel entry points still run the dispatched
  // SIMD target (they only honor the Validate fuse). The renderer-level
  // effect of the toggle is covered end to end by
  // GradientCanvasAccelMatchesLegacy, whose arms must differ.
  skity::sw_accel::SetEnabled(false);
  EXPECT_FALSE(skity::sw_accel::IsEnabled());
  EXPECT_TRUE(FillSolidSpans(BlendMode::kSrc, 0xff00ff00u,
                             reinterpret_cast<uint8_t*>(bitmap.data()),
                             kWidth * 4, kWidth, 1, &span, 1));

  // Restore for the other tests.
  skity::sw_accel::SetEnabled(true);
  EXPECT_TRUE(FillSolidSpans(BlendMode::kSrc, 0xff00ff00u,
                             reinterpret_cast<uint8_t*>(bitmap.data()),
                             kWidth * 4, kWidth, 1, &span, 1));
}

// ---------------------------------------------------------------------------
// Gradient kernels
// ---------------------------------------------------------------------------

namespace {

constexpr float kTestNearlyZero = 1.0f / (1 << 12);

// Independent scalar mirror of the legacy gradient chain, written from the
// GradientColorBrush source (LerpColor + Color4fToColor + ColorToPMColor +
// the BrushH coverage fold). Produces the same memory word (0xAABBGGRR)
// FillGradientSpans stores for kSrc.
struct TestGradient {
  int type = 0;  // 0 linear, 1 radial
  float m[6] = {};
  skity::TileMode tile = skity::TileMode::kClamp;
  std::vector<float> offsets;  // empty = uniform stops
  std::vector<skity::Color4f> colors;
};

float RemapTileRef(float t, skity::TileMode mode) {
  if (mode == skity::TileMode::kClamp) {
    t = std::min(1.f, std::max(0.f, t));
  } else if (mode == skity::TileMode::kRepeat) {
    t = t - std::floor(t);
  } else if (mode == skity::TileMode::kMirror) {
    float t1 = t - 1;
    float t2 = t1 - 2 * std::floor(t1 * 0.5f) - 1;
    t = std::abs(t2);
  }
  return t;
}

skity::Color4f LerpColorRef(const TestGradient& g, float t) {
  if (std::abs(t) <= kTestNearlyZero) {
    t = 0.0f;
  } else if (std::abs(t - 1.0f) <= kTestNearlyZero) {
    t = 1.0f;
  }
  if (g.tile == skity::TileMode::kDecal && (t < 0.0f || t >= 1.0f)) {
    return skity::Color4f{};
  }
  t = RemapTileRef(t, g.tile);
  const int32_t count = static_cast<int32_t>(g.colors.size());
  const bool has_off = !g.offsets.empty();
  if (has_off && t <= g.offsets[0]) {
    return g.colors[0];
  }
  const float step = 1.f / (count - 1);
  int32_t i = 0;
  float start = 0.f;
  float end = 0.f;
  for (i = 0; i < count - 1; i++) {
    start = has_off ? g.offsets[i] : step * i;
    end = has_off ? g.offsets[i + 1] : step * (i + 1);
    if (t >= start && t <= end) {
      break;
    }
  }
  if (i == count - 1) {
    return g.colors[count - 1];
  }
  float total = end - start;
  float value = t - start;
  float mix = 0.5f;
  if (total > 0) {
    mix = value / total;
  }
  return g.colors[i] * (1 - mix) + g.colors[i + 1] * mix;
}

uint32_t GradientWordRef(const TestGradient& g, int32_t x, int32_t y,
                         uint8_t cover) {
  const float fx = x + 0.5f;
  const float fy = y + 0.5f;
  const float u = fx * g.m[0] + fy * g.m[1] + g.m[2];
  float t = u;
  if (g.type == 1) {
    const float v = fx * g.m[3] + fy * g.m[4] + g.m[5];
    t = std::sqrt(u * u + v * v);
  }
  const skity::Color4f c = LerpColorRef(g, t);
  const float rgba[4] = {c.r, c.g, c.b, c.a};
  uint32_t ch[4];
  for (int i = 0; i < 4; i++) {
    ch[i] =
        static_cast<uint32_t>(std::min(255.f, std::max(0.f, rgba[i] * 255.f)));
  }
  if (ch[3] != 255) {
    for (int i = 0; i < 3; i++) {
      ch[i] = skity::MulDiv255Round(ch[i], ch[3]);
    }
  }
  if (cover != 255) {
    for (int i = 0; i < 4; i++) {
      ch[i] = (ch[i] * cover) >> 8;
    }
  }
  return ch[0] | (ch[1] << 8) | (ch[2] << 16) | (ch[3] << 24);
}

skity::sw_accel::GradientParams ToParams(const TestGradient& g,
                                         std::vector<float>* seg_start,
                                         std::vector<float>* seg_end,
                                         std::vector<float>* colors,
                                         std::vector<uint32_t>* lut) {
  const int32_t count = static_cast<int32_t>(g.colors.size());
  const bool has_off = !g.offsets.empty();
  const float step = 1.f / (count - 1);
  seg_start->resize(static_cast<size_t>(count) - 1);
  seg_end->resize(static_cast<size_t>(count) - 1);
  for (int32_t i = 0; i < count - 1; i++) {
    (*seg_start)[i] = has_off ? g.offsets[i] : step * i;
    (*seg_end)[i] = has_off ? g.offsets[i + 1] : step * (i + 1);
  }
  colors->clear();
  for (const skity::Color4f& c : g.colors) {
    colors->push_back(c.r);
    colors->push_back(c.g);
    colors->push_back(c.b);
    colors->push_back(c.a);
  }
  skity::sw_accel::GradientParams p{};
  p.type = g.type;
  p.m00 = g.m[0];
  p.m01 = g.m[1];
  p.m02 = g.m[2];
  p.m10 = g.m[3];
  p.m11 = g.m[4];
  p.m12 = g.m[5];
  p.tile_mode = static_cast<int32_t>(g.tile);
  p.use_offsets = has_off;
  p.first_offset = has_off ? g.offsets.front() : 0.f;
  p.color_count = count;
  p.seg_start = seg_start->data();
  p.seg_end = seg_end->data();
  p.colors = colors->data();
  lut->resize(512);
  if (!skity::sw_accel::BuildGradientLUT(p, lut->data())) {
    return p;
  }
  p.lut = lut->data();
  p.lut_entries = static_cast<int32_t>(lut->size());
  return p;
}

TestGradient RandomGradient(uint32_t& state) {
  TestGradient g;
  g.type = NextRandom(state) % 2;
  for (float& v : g.m) {
    v = (static_cast<int32_t>(NextRandom(state) % 4001) - 2000) / 20000.f;
  }
  // Keep one entry big enough that t sweeps well past [0, 1] over a row.
  g.m[NextRandom(state) % (g.type == 1 ? 3 : 2)] +=
      (static_cast<int32_t>(NextRandom(state) % 4001) + 1000) / 20000.f;
  const skity::TileMode tiles[] = {
      skity::TileMode::kClamp, skity::TileMode::kRepeat,
      skity::TileMode::kMirror, skity::TileMode::kDecal};
  g.tile = tiles[NextRandom(state) % 4];
  const int32_t count = 2 + static_cast<int32_t>(NextRandom(state) % 5);
  const bool has_off = NextRandom(state) % 2 == 0;
  if (has_off) {
    float prev = static_cast<int32_t>(NextRandom(state) % 20) / 100.f;
    for (int32_t i = 0; i < count; i++) {
      // Strictly increasing (duplicate offsets / hard stops are baked into
      // the LUT like any other stop; their edge spreads over at most one
      // 1/255 grid cell, which the tolerance below absorbs).
      const float next = prev + 0.08f + (NextRandom(state) % 10) / 100.f;
      g.offsets.push_back(std::min(prev, 1.f));
      prev = std::min(next, 1.f);
    }
  }
  for (int32_t i = 0; i < count; i++) {
    const float a = 0.2f + (NextRandom(state) % 81) / 100.f;
    const float r = (NextRandom(state) % 256) / 255.f * a;
    const float gg = (NextRandom(state) % 256) / 255.f * a;
    const float b = (NextRandom(state) % 256) / 255.f * a;
    g.colors.push_back(skity::Color4f{r, gg, b, a});
  }
  return g;
}

}  // namespace

// Fetch-level accuracy: kSrc isolates the fetched (premul, coverage folded)
// words, compared against the independent scalar mirror above. The LUT
// interpolation keeps the difference within about 2 per channel.
TEST(SWAccel, GradientFetchMatchesScalarReference) {
  uint32_t state = 0x85ebca6bu;
  constexpr int32_t kW = 40;
  constexpr int32_t kH = 9;
  std::vector<uint32_t> bitmap(kW * kH, 0xdeadbeefu);
  int64_t exact = 0;
  int64_t total = 0;

  for (int round = 0; round < 60; round++) {
    const TestGradient g = RandomGradient(state);
    std::vector<float> seg_start, seg_end, colors;
    std::vector<uint32_t> lut;
    const skity::sw_accel::GradientParams p =
        ToParams(g, &seg_start, &seg_end, &colors, &lut);
    ASSERT_NE(nullptr, p.lut);
    for (auto& px : bitmap) {
      px = 0xdeadbeefu;
    }
    std::vector<Span> spans;
    for (int32_t y = 0; y < kH; y++) {
      spans.push_back(Span{0, y, kW, 255});
      spans.push_back(Span{static_cast<int32_t>(NextRandom(state) % 10), y,
                           static_cast<int32_t>(NextRandom(state) % 30) + 1,
                           static_cast<int32_t>(NextRandom(state) % 256)});
    }
    ASSERT_TRUE(skity::sw_accel::FillGradientSpans(
        p, BlendMode::kSrc, reinterpret_cast<uint8_t*>(bitmap.data()), kW * 4,
        kW, kH, spans.data(), spans.size()));
    // Apply in span order like the fill does.
    std::vector<uint32_t> expect(kW * kH, 0xdeadbeefu);
    for (const Span& span : spans) {
      if (span.y < 0 || span.y >= kH || span.x >= kW || span.len <= 0) {
        continue;
      }
      for (int32_t i = 0; i < span.len && span.x + i < kW; i++) {
        expect[static_cast<size_t>(span.y) * kW + span.x + i] = GradientWordRef(
            g, span.x + i, span.y, static_cast<uint8_t>(span.cover & 0xff));
      }
    }
    for (size_t i = 0; i < expect.size(); i++) {
      // Pixels whose t lands in the nearly-zero snap band around 1 under
      // kRepeat follow the left-limit convention (see BuildGradientLUT) and
      // legitimately differ from the legacy wrap-around.
      if (g.tile == skity::TileMode::kRepeat) {
        const int32_t dx = static_cast<int32_t>(i % kW);
        const int32_t dy = static_cast<int32_t>(i / kW);
        const float fx = dx + 0.5f;
        const float fy = dy + 0.5f;
        const float u = fx * g.m[0] + fy * g.m[1] + g.m[2];
        float tv = u;
        if (g.type == 1) {
          const float v2 = fx * g.m[3] + fy * g.m[4] + g.m[5];
          tv = std::sqrt(u * u + v2 * v2);
        }
        if (std::abs(tv - 1.f) <= 4.f / (1 << 12)) {
          continue;
        }
      }
      total++;
      if (expect[i] == bitmap[i]) {
        exact++;
        continue;
      }
      for (int shift = 0; shift < 32; shift += 8) {
        const int32_t d = static_cast<int32_t>((expect[i] >> shift) & 0xff) -
                          static_cast<int32_t>((bitmap[i] >> shift) & 0xff);
        ASSERT_LE(d < 0 ? -d : d, 4) << "round " << round << " pixel " << i;
      }
    }
  }
  // Sanity: the overwhelming majority is exact.
  EXPECT_GT(exact * 100 / total, 80);
}

namespace {

// A scene exercising every accelerated gradient configuration plus a sweep
// (which must fall back to the legacy brush).
void DrawGradientScene(skity::Canvas* canvas) {
  const skity::Vec4 lin_colors[] = {
      skity::Vec4{0.f, 1.f, 1.f, 0.f},
      skity::Vec4{0.f, 0.f, 1.f, 0.6f},
      skity::Vec4{1.f, 0.f, 0.f, 1.f},
  };
  const float lin_offsets[] = {0.15f, 0.55f, 0.9f};
  skity::Point pts[] = {{8.f, 8.f, 0.f, 1.f}, {70.f, 50.f, 0.f, 1.f}};

  skity::Paint paint;
  paint.SetAntiAlias(true);
  auto lin = skity::Shader::MakeLinear(pts, lin_colors, lin_offsets, 3,
                                       skity::TileMode::kMirror, 0);
  paint.SetShader(lin);
  canvas->DrawRect(skity::Rect::MakeXYWH(4, 4, 80, 60), paint);

  canvas->Save();
  canvas->Rotate(20.f, 40.f, 30.f);
  paint.SetShader(skity::Shader::MakeLinear(pts, lin_colors, nullptr, 3,
                                            skity::TileMode::kRepeat, 0));
  canvas->DrawRect(skity::Rect::MakeXYWH(10, 10, 70, 40), paint);
  canvas->Restore();

  // Third-stop configuration and decal tile.
  const skity::Vec4 hard_colors[] = {
      skity::Vec4{0.1f, 0.9f, 0.3f, 1.f},
      skity::Vec4{0.9f, 0.9f, 0.1f, 0.8f},
      skity::Vec4{0.2f, 0.2f, 0.9f, 1.f},
  };
  const float hard_offsets[] = {0.35f, 0.6f, 0.9f};
  paint.SetShader(skity::Shader::MakeLinear(pts, hard_colors, hard_offsets, 3,
                                            skity::TileMode::kDecal, 0));
  canvas->DrawCircle(40, 30, 24, paint);

  // Radial (uniform stops, no offsets).
  const skity::Vec4 rad_colors[] = {
      skity::Vec4{1.f, 1.f, 1.f, 1.f},
      skity::Vec4{0.f, 0.f, 0.f, 0.5f},
  };
  skity::Point center{60.f, 55.f, 0.f, 1.f};
  paint.SetShader(
      skity::Shader::MakeRadial(center, 30.f, rad_colors, nullptr, 2));
  canvas->DrawRect(skity::Rect::MakeXYWH(30, 30, 60, 40), paint);

  // Sweep: not accelerated; both arms run the legacy brush.
  const skity::Vec4 sweep_colors[] = {
      skity::Vec4{1.f, 0.f, 0.f, 1.f},
      skity::Vec4{0.f, 1.f, 0.f, 1.f},
  };
  paint.SetShader(skity::Shader::MakeSweep(center.x, center.y, 0.f, 360.f,
                                           sweep_colors, nullptr, 2));
  canvas->DrawRect(skity::Rect::MakeXYWH(0, 50, 40, 22), paint);
}

}  // namespace

// End-to-end: the same scene drawn with the accelerator off and on must
// match within the composed tolerance. The off arm renders the gradients
// through the scalar fallback brushes, so the only remaining difference
// source is the LUT interpolation, about 2 per channel; horizontal blending
// is scalar on every architecture (the legacy NEON run blender, which
// differed from the scalar formula by +/-1, was removed with M2-0).
// The test also asserts the switch actually reaches the renderer: if the
// toggle were again confined to a static mirror that the renderer never
// reads (the M1 defect), both arms would be pixel-identical and the
// differing-pixel assertion below would fail.
// Cache semantics of AcquireGradientLUT: the key covers the stop table and
// tile mode but not the transform (animated gradients hit), and round-robin
// eviction under more gradients than cache slots never returns a mismatched
// table.
TEST(SWAccel, GradientLUTCacheKeysOnStopTableOnly) {
  uint32_t state = 0x1234567u;
  constexpr int32_t kW = 24;
  constexpr int32_t kH = 4;
  std::vector<uint32_t> bitmap(kW * kH);
  std::vector<Span> spans;
  for (int32_t y = 0; y < kH; y++) {
    spans.push_back(Span{0, y, kW, 255});
  }

  // Same stop table under two transforms must return the same cached entry.
  TestGradient ga = RandomGradient(state);
  ga.type = 0;
  TestGradient gb = ga;
  gb.m[0] = ga.m[0] * 3.f;
  gb.m[1] = -ga.m[1] + 0.05f;
  gb.m[2] = ga.m[2] + 0.4f;

  std::vector<float> ss, se, co;
  std::vector<uint32_t> fresh_lut;
  auto params_of = [&](const TestGradient& g, bool cached) {
    // ToParams builds a fresh LUT into fresh_lut; for the cached variant the
    // pointer comes from AcquireGradientLUT instead.
    skity::sw_accel::GradientParams p = ToParams(g, &ss, &se, &co, &fresh_lut);
    if (cached) {
      p.lut = skity::sw_accel::AcquireGradientLUT(p);
    }
    return p;
  };

  const skity::sw_accel::GradientParams pa = params_of(ga, true);
  const skity::sw_accel::GradientParams pb = params_of(gb, true);
  ASSERT_NE(nullptr, pa.lut);
  ASSERT_NE(nullptr, pb.lut);
  // Same stop table -> one cache entry, and its content matches a fresh
  // build bit-exactly.
  EXPECT_EQ(pa.lut, pb.lut);
  EXPECT_EQ(0, memcmp(pa.lut, fresh_lut.data(), 512 * sizeof(uint32_t)));

  // Fill with the cached entry and compare against the scalar reference.
  for (const TestGradient* g : {&ga, &gb}) {
    const skity::sw_accel::GradientParams p = params_of(*g, true);
    ASSERT_NE(nullptr, p.lut);
    for (auto& px : bitmap) {
      px = 0xdeadbeefu;
    }
    ASSERT_TRUE(skity::sw_accel::FillGradientSpans(
        p, BlendMode::kSrc, reinterpret_cast<uint8_t*>(bitmap.data()), kW * 4,
        kW, kH, spans.data(), spans.size()));
    for (int32_t y = 0; y < kH; y++) {
      for (int32_t x = 0; x < kW; x++) {
        const uint32_t expect = GradientWordRef(*g, x, y, 255);
        const uint32_t got = bitmap[static_cast<size_t>(y) * kW + x];
        for (int shift = 0; shift < 32; shift += 8) {
          const int32_t d = static_cast<int32_t>((expect >> shift) & 0xff) -
                            static_cast<int32_t>((got >> shift) & 0xff);
          ASSERT_LE(d < 0 ? -d : d, 4) << "x " << x << " y " << y;
        }
      }
    }
  }

  // More distinct gradients than cache slots, drawn round-robin twice: the
  // second pass runs entirely on evicted-and-rebuilt entries; every fill
  // must still match its own reference.
  std::vector<TestGradient> many;
  for (int i = 0; i < 12; i++) {
    TestGradient g = RandomGradient(state);
    g.type = i % 2;
    many.push_back(g);
  }
  for (int round = 0; round < 2; round++) {
    for (const TestGradient& g : many) {
      const skity::sw_accel::GradientParams p = params_of(g, true);
      ASSERT_NE(nullptr, p.lut);
      for (auto& px : bitmap) {
        px = 0xdeadbeefu;
      }
      ASSERT_TRUE(skity::sw_accel::FillGradientSpans(
          p, BlendMode::kSrc, reinterpret_cast<uint8_t*>(bitmap.data()), kW * 4,
          kW, kH, spans.data(), spans.size()));
      for (int32_t y = 0; y < kH; y++) {
        for (int32_t x = 0; x < kW; x++) {
          const uint32_t expect = GradientWordRef(g, x, y, 255);
          const uint32_t got = bitmap[static_cast<size_t>(y) * kW + x];
          for (int shift = 0; shift < 32; shift += 8) {
            const int32_t d = static_cast<int32_t>((expect >> shift) & 0xff) -
                              static_cast<int32_t>((got >> shift) & 0xff);
            ASSERT_LE(d < 0 ? -d : d, 4)
                << "round " << round << " x " << x << " y " << y;
          }
        }
      }
    }
  }
}

TEST(SWAccel, GradientCanvasAccelMatchesLegacy) {
  constexpr int32_t kW = 96;
  constexpr int32_t kH = 72;
  const uint32_t background = skity::ColorSetA(skity::Color_GRAY, 255);

  skity::sw_accel::SetEnabled(false);
  skity::Bitmap legacy(kW, kH, skity::AlphaType::kPremul_AlphaType);
  auto legacy_canvas = skity::Canvas::MakeSoftwareCanvas(&legacy);
  legacy_canvas->DrawColor(background);
  DrawGradientScene(legacy_canvas.get());
  skity::sw_accel::SetEnabled(true);

  skity::Bitmap accel(kW, kH, skity::AlphaType::kPremul_AlphaType);
  auto accel_canvas = skity::Canvas::MakeSoftwareCanvas(&accel);
  accel_canvas->DrawColor(background);
  DrawGradientScene(accel_canvas.get());

  const uint32_t* lp = reinterpret_cast<const uint32_t*>(legacy.GetPixelAddr());
  const uint32_t* ap = reinterpret_cast<const uint32_t*>(accel.GetPixelAddr());
  int64_t exact = 0;
  int64_t differing = 0;
  int64_t legacy_gradient_pixels = 0;
  for (size_t i = 0; i < static_cast<size_t>(kW) * kH; i++) {
    // The off arm must paint the gradients through the scalar fallback.
    // A broken fallback (e.g. falling into the transparent unsupported-type
    // base) would leave the flat gray background here.
    if (lp[i] != background) {
      legacy_gradient_pixels++;
    }
    if (lp[i] == ap[i]) {
      exact++;
      continue;
    }
    differing++;
    for (int shift = 0; shift < 32; shift += 8) {
      const int32_t d = static_cast<int32_t>((lp[i] >> shift) & 0xff) -
                        static_cast<int32_t>((ap[i] >> shift) & 0xff);
      ASSERT_LE(d < 0 ? -d : d, 4) << "pixel " << i << " shift " << shift;
    }
  }
  // The sweep block falls back identically on both arms; the rest should be
  // near-identical (differences are confined to LUT rounding).
  EXPECT_GT(exact * 100 / (static_cast<int64_t>(kW) * kH), 80);
  // Non-no-op guard for the runtime switch: the LUT fetch and the scalar
  // chain round differently on real gradient interiors, so the arms must
  // differ somewhere. Pixel-identical arms mean the toggle never reached
  // the renderer (the two-mirror defect this test exists to catch).
  EXPECT_GT(differing, 0) << "accel and legacy arms are pixel-identical; "
                             "the runtime switch did not reach the renderer";
  EXPECT_GT(legacy_gradient_pixels, static_cast<int64_t>(kW) * kH / 4)
      << "the scalar gradient fallback did not paint";
}

#endif  // SKITY_SW_ACCEL
