// Copyright 2021 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <skity/effect/shader.hpp>
#include <skity/graphic/bitmap.hpp>
#include <skity/graphic/color.hpp>
#include <skity/graphic/paint.hpp>
#include <skity/render/canvas.hpp>
#include <skity/render/sw_context.hpp>

#ifdef SKITY_SW_ACCEL
#include "src/render/sw/accel/sw_accel.hpp"
#endif

namespace {

// Saves the accel switch on construction and restores it on teardown, so a
// failing assertion cannot leak the state into other tests.
class AccelSwitchScope {
 public:
  explicit AccelSwitchScope(bool enabled)
      : saved_(skity::SWContext::Get()->AccelEnabled()) {
    skity::SWContext::Get()->SetAccelEnabled(enabled);
  }

  ~AccelSwitchScope() { skity::SWContext::Get()->SetAccelEnabled(saved_); }

  AccelSwitchScope(const AccelSwitchScope&) = delete;
  AccelSwitchScope& operator=(const AccelSwitchScope&) = delete;

 private:
  bool saved_;
};

int32_t MaxChannelDiff(uint32_t a, uint32_t b) {
  int32_t max_diff = 0;
  for (int shift = 0; shift < 32; shift += 8) {
    const int32_t diff = static_cast<int32_t>((a >> shift) & 0xff) -
                         static_cast<int32_t>((b >> shift) & 0xff);
    max_diff = std::max(max_diff, diff < 0 ? -diff : diff);
  }
  return max_diff;
}

// Linear gradient plus a SaveLayer'ed radial circle. The gradient interiors
// round differently through the LUT fetch and the scalar chain, which is
// what proves the switch reached the renderer; the layer exercises the
// sub-canvas (its brushes read the same context).
void DrawContextScene(skity::Canvas* canvas) {
  const skity::Vec4 lin_colors[] = {
      skity::Vec4{0.f, 1.f, 1.f, 0.f},
      skity::Vec4{1.f, 0.f, 0.f, 1.f},
  };
  skity::Point pts[] = {{4.f, 4.f, 0.f, 1.f}, {88.f, 64.f, 0.f, 1.f}};

  skity::Paint paint;
  paint.SetAntiAlias(true);
  paint.SetShader(skity::Shader::MakeLinear(pts, lin_colors, nullptr, 2,
                                            skity::TileMode::kClamp, 0));
  canvas->DrawRect(skity::Rect::MakeXYWH(2.f, 2.f, 92.f, 68.f), paint);

  canvas->SaveLayer(skity::Rect::MakeXYWH(8.f, 8.f, 80.f, 56.f),
                    skity::Paint{});
  const skity::Vec4 rad_colors[] = {
      skity::Vec4{1.f, 1.f, 1.f, 1.f},
      skity::Vec4{0.f, 0.f, 0.f, 0.5f},
  };
  skity::Point center{48.f, 36.f, 0.f, 1.f};
  paint.SetShader(
      skity::Shader::MakeRadial(center, 26.f, rad_colors, nullptr, 2));
  canvas->DrawCircle(48.f, 36.f, 26.f, paint);
  canvas->Restore();
}

}  // namespace

TEST(SWContext, SingletonIdentityAndDefaults) {
  EXPECT_EQ(skity::SWContext::Get(), skity::SWContext::Get());

  auto* context = skity::SWContext::Get();

  // The parallel switch round-trips and defaults to off semantics being
  // preserved through a save/toggle/restore cycle.
  const bool saved_parallel = context->ParallelEnabled();
  context->SetParallelEnabled(!saved_parallel);
  EXPECT_EQ(!saved_parallel, context->ParallelEnabled());
  context->SetParallelEnabled(saved_parallel);
  EXPECT_EQ(saved_parallel, context->ParallelEnabled());

  // An explicit budget reads back verbatim; auto (0) resolves to at least
  // one worker thread. Restore the factory default (auto) afterwards.
  context->SetThreadCount(4);
  EXPECT_EQ(4u, context->ThreadCount());
  context->SetThreadCount(0);
  EXPECT_GE(context->ThreadCount(), 1u);

  // The accel switch round-trips through the context.
  const bool saved_accel = context->AccelEnabled();
  context->SetAccelEnabled(!saved_accel);
  EXPECT_EQ(!saved_accel, context->AccelEnabled());
  context->SetAccelEnabled(saved_accel);
  EXPECT_EQ(saved_accel, context->AccelEnabled());
}

#ifdef SKITY_SW_ACCEL

// The legacy sw_accel entry points must stay thin delegates over the
// context: the two access paths can never diverge.
TEST(SWContext, AccelSwitchDelegatesToContext) {
  AccelSwitchScope scope(true);

  skity::sw_accel::SetEnabled(false);
  EXPECT_FALSE(skity::SWContext::Get()->AccelEnabled());

  skity::SWContext::Get()->SetAccelEnabled(true);
  EXPECT_TRUE(skity::sw_accel::IsEnabled());

  skity::sw_accel::SetEnabled(true);
  EXPECT_TRUE(skity::SWContext::Get()->AccelEnabled());
}

// Canvases read the context per draw: a toggle AFTER creation must still
// reach the renderer (and the layer sub-canvas with it). Arms that are
// pixel-identical mean the toggle never reached the renderer - the
// two-mirror defect this guards against.
TEST(SWContext, AccelToggleReachesExistingCanvas) {
  AccelSwitchScope scope(true);
  constexpr int32_t kW = 96;
  constexpr int32_t kH = 72;
  const uint32_t background = skity::ColorSetA(skity::Color_GRAY, 255);

  // Both canvases are created BEFORE any toggle.
  skity::Bitmap legacy(kW, kH, skity::AlphaType::kPremul_AlphaType);
  auto legacy_canvas = skity::Canvas::MakeSoftwareCanvas(&legacy);
  ASSERT_TRUE(legacy_canvas);
  skity::Bitmap accel(kW, kH, skity::AlphaType::kPremul_AlphaType);
  auto accel_canvas = skity::Canvas::MakeSoftwareCanvas(&accel);
  ASSERT_TRUE(accel_canvas);

  skity::SWContext::Get()->SetAccelEnabled(false);
  legacy_canvas->DrawColor(background);
  DrawContextScene(legacy_canvas.get());

  skity::SWContext::Get()->SetAccelEnabled(true);
  accel_canvas->DrawColor(background);
  DrawContextScene(accel_canvas.get());

  const uint32_t* legacy_pixels =
      reinterpret_cast<const uint32_t*>(legacy.GetPixelAddr());
  const uint32_t* accel_pixels =
      reinterpret_cast<const uint32_t*>(accel.GetPixelAddr());
  int64_t exact = 0;
  int64_t differing = 0;
  int64_t painted = 0;
  for (size_t i = 0; i < static_cast<size_t>(kW) * kH; i++) {
    // The off arm must paint the gradients through the scalar fallback; a
    // broken fallback would leave the flat background.
    if (legacy_pixels[i] != background) {
      painted++;
    }
    if (legacy_pixels[i] == accel_pixels[i]) {
      exact++;
      continue;
    }
    differing++;
    EXPECT_LE(MaxChannelDiff(legacy_pixels[i], accel_pixels[i]), 4)
        << "pixel " << i << " legacy " << std::hex << legacy_pixels[i]
        << " accel " << accel_pixels[i];
  }
  // Non-no-op guard: LUT vs scalar rounding differs on gradient interiors.
  EXPECT_GT(differing, 0) << "the accel switch did not reach the renderer";
  // The scalar fallback really painted (no transparent-base regression).
  EXPECT_GT(painted, static_cast<int64_t>(kW) * kH / 4)
      << "the legacy gradient fallback did not paint";
}

#endif  // SKITY_SW_ACCEL
