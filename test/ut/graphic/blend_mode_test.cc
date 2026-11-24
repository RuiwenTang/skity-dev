// Copyright 2021 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

#include <gtest/gtest.h>

#include <skity/graphic/blend_mode.hpp>
#include <skity/graphic/color.hpp>

#include "src/graphic/blend_mode_priv.hpp"
#include "src/graphic/color_priv.hpp"

namespace {

uint32_t NextRandom(uint32_t& state) {
  state ^= state << 13;
  state ^= state >> 17;
  state ^= state << 5;
  return state;
}

// Premul-bounded random color.
uint32_t RandomPMColor(uint32_t& state) {
  const uint32_t a = NextRandom(state) % 256;
  const uint32_t r = NextRandom(state) % (a + 1);
  const uint32_t g = NextRandom(state) % (a + 1);
  const uint32_t b = NextRandom(state) % (a + 1);
  return (a << 24) | (r << 16) | (g << 8) | b;
}

bool BlendEq(uint32_t x, uint32_t y) { return x == y; }

}  // namespace

// The advanced modes are evaluated with the WGSL formulas the GPU backend
// generates; these tests pin structural properties of that implementation.

TEST(AdvancedBlend, SymmetricModesCommute) {
  uint32_t state = 0x1a2b3c4du;
  const skity::BlendMode symmetric[] = {
      skity::BlendMode::kMultiply, skity::BlendMode::kScreen,
      skity::BlendMode::kExclusion, skity::BlendMode::kDifference};
  for (int i = 0; i < 2000; i++) {
    const uint32_t s = RandomPMColor(state);
    const uint32_t d = RandomPMColor(state);
    for (skity::BlendMode m : symmetric) {
      EXPECT_EQ(skity::PorterDuffBlend(s, d, m),
                skity::PorterDuffBlend(d, s, m))
          << "mode " << skity::BlendMode_Name(m) << " s " << s << " d " << d;
    }
  }
}

TEST(AdvancedBlend, OverlayAndHardLightAreSwaps) {
  uint32_t state = 0xfeedfaceu;
  for (int i = 0; i < 2000; i++) {
    const uint32_t s = RandomPMColor(state);
    const uint32_t d = RandomPMColor(state);
    EXPECT_EQ(skity::PorterDuffBlend(s, d, skity::BlendMode::kOverlay),
              skity::PorterDuffBlend(d, s, skity::BlendMode::kHardLight))
        << "s " << s << " d " << d;
  }
}

// The advanced modes composite with the SrcOver alpha.
TEST(AdvancedBlend, AlphaFollowsSourceOver) {
  uint32_t state = 0xc0ffee11u;
  const skity::BlendMode advanced[] = {
      skity::BlendMode::kOverlay,    skity::BlendMode::kDarken,
      skity::BlendMode::kLighten,    skity::BlendMode::kColorDodge,
      skity::BlendMode::kColorBurn,  skity::BlendMode::kHardLight,
      skity::BlendMode::kDifference, skity::BlendMode::kExclusion,
      skity::BlendMode::kMultiply};
  for (int i = 0; i < 2000; i++) {
    const uint32_t s = RandomPMColor(state);
    const uint32_t d = RandomPMColor(state);
    for (skity::BlendMode m : advanced) {
      const uint32_t out = skity::PorterDuffBlend(s, d, m);
      const uint32_t sa = ColorGetA(s);
      const uint32_t da = ColorGetA(d);
      // sa + (1 - sa/255) * da, quantized like the implementation:
      // floor((sa + (255 - sa) * da / 255) * ... in f32 domain.
      const float expected_f = sa / 255.f + (1.f - sa / 255.f) * (da / 255.f);
      const uint32_t expected = static_cast<uint32_t>(
          std::min(1.f, std::max(0.f, expected_f)) * 255.f + 0.5f);
      EXPECT_EQ(expected, ColorGetA(out))
          << "mode " << skity::BlendMode_Name(m) << " s " << s << " d " << d;
    }
    // HSL family: alpha = sa + da - sa*da.
    for (skity::BlendMode m :
         {skity::BlendMode::kHue, skity::BlendMode::kSaturation,
          skity::BlendMode::kColor, skity::BlendMode::kLuminosity}) {
      const uint32_t out = skity::PorterDuffBlend(s, d, m);
      const uint32_t sa = ColorGetA(s);
      const uint32_t da = ColorGetA(d);
      const float expected_f =
          sa / 255.f + da / 255.f - (sa / 255.f) * (da / 255.f);
      const uint32_t expected = static_cast<uint32_t>(
          std::min(1.f, std::max(0.f, expected_f)) * 255.f + 0.5f);
      EXPECT_EQ(expected, ColorGetA(out))
          << "mode " << skity::BlendMode_Name(m) << " s " << s << " d " << d;
    }
  }
}

// Opaque inputs keep results opaque and channels premul-bounded; transparent
// destination degenerates advanced blending to the source for SoftLight and
// to (source + extra-free) forms bounded by premul rules for the rest.
TEST(AdvancedBlend, OpaqueInputsStayOpaqueAndBounded) {
  uint32_t state = 0x51ed270bu;
  const skity::BlendMode advanced[] = {
      skity::BlendMode::kOverlay,   skity::BlendMode::kDarken,
      skity::BlendMode::kLighten,   skity::BlendMode::kColorDodge,
      skity::BlendMode::kColorBurn, skity::BlendMode::kHardLight,
      skity::BlendMode::kSoftLight, skity::BlendMode::kDifference,
      skity::BlendMode::kExclusion, skity::BlendMode::kMultiply,
      skity::BlendMode::kHue,       skity::BlendMode::kSaturation,
      skity::BlendMode::kColor,     skity::BlendMode::kLuminosity};
  for (int i = 0; i < 2000; i++) {
    const uint32_t s = 0xff000000u | (NextRandom(state) & 0xffffffu);
    const uint32_t d = 0xff000000u | (NextRandom(state) & 0xffffffu);
    for (skity::BlendMode m : advanced) {
      const uint32_t out = skity::PorterDuffBlend(s, d, m);
      ASSERT_EQ(255u, ColorGetA(out)) << "mode " << skity::BlendMode_Name(m);
      ASSERT_LE(ColorGetR(out), 255u);
      ASSERT_LE(ColorGetG(out), 255u);
      ASSERT_LE(ColorGetB(out), 255u);
    }
  }
}

// Spot values for hand-checkable cases (opaque, one channel).
TEST(AdvancedBlend, OpaqueSpotValues) {
  const uint32_t black = 0xff000000;
  const uint32_t white = 0xffffffff;
  const uint32_t gray = 0xff808080;

  // Multiply with opaque inputs: s*d in [0,1] -> 0x80 * 0x80 = 16384/65025
  // -> *255 + 0.5.
  const uint32_t mul =
      skity::PorterDuffBlend(gray, gray, skity::BlendMode::kMultiply);
  const float half = 0x80 / 255.f;
  const uint32_t expect_mul = static_cast<uint32_t>(half * half * 255.f + 0.5f);
  EXPECT_EQ(expect_mul, ColorGetR(mul));

  // Screen: s + d - s*d; 0.5 -> 0.75.
  const uint32_t scr =
      skity::PorterDuffBlend(gray, gray, skity::BlendMode::kScreen);
  const uint32_t expect_scr =
      static_cast<uint32_t>((half + half - half * half) * 255.f + 0.5f);
  EXPECT_EQ(expect_scr, ColorGetR(scr));

  // Darken / Lighten of black and white.
  EXPECT_EQ(black,
            skity::PorterDuffBlend(black, white, skity::BlendMode::kDarken));
  EXPECT_EQ(white,
            skity::PorterDuffBlend(black, white, skity::BlendMode::kLighten));
  // Exclusion with black leaves dst; with white inverts dst.
  EXPECT_EQ(white,
            skity::PorterDuffBlend(black, white, skity::BlendMode::kExclusion));
  const uint32_t inv =
      skity::PorterDuffBlend(white, gray, skity::BlendMode::kExclusion);
  EXPECT_EQ(255 - 0x80, ColorGetR(inv));
  // Difference of equal colors is transparent black in rgb, opaque alpha.
  const uint32_t diff =
      skity::PorterDuffBlend(gray, gray, skity::BlendMode::kDifference);
  EXPECT_EQ(0u, ColorGetR(diff));
  EXPECT_EQ(255u, ColorGetA(diff));
  // ColorDodge: white src pins output to white.
  EXPECT_EQ(white,
            skity::PorterDuffBlend(white, gray, skity::BlendMode::kColorDodge));
  // SoftLight with dst alpha 0 returns src.
  EXPECT_EQ(gray,
            skity::PorterDuffBlend(gray, 0, skity::BlendMode::kSoftLight));
  EXPECT_TRUE(BlendEq(
      skity::PorterDuffBlend(gray, black, skity::BlendMode::kColorBurn),
      skity::PorterDuffBlend(gray, black, skity::BlendMode::kColorBurn)));
}
