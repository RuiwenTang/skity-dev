/*
 * Copyright 2016 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

// Copyright 2021 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

#include <cmath>

#include "src/graphic/blend_mode_priv.hpp"
#include "src/graphic/color_priv.hpp"

namespace skity {

const char* BlendMode_Name(BlendMode bm) {
  switch (bm) {
    case BlendMode::kClear:
      return "Clear";
    case BlendMode::kSrc:
      return "Src";
    case BlendMode::kDst:
      return "Dst";
    case BlendMode::kSrcOver:
      return "SrcOver";
    case BlendMode::kDstOver:
      return "DstOver";
    case BlendMode::kSrcIn:
      return "SrcIn";
    case BlendMode::kDstIn:
      return "DstIn";
    case BlendMode::kSrcOut:
      return "SrcOut";
    case BlendMode::kDstOut:
      return "DstOut";
    case BlendMode::kSrcATop:
      return "SrcATop";
    case BlendMode::kDstATop:
      return "DstATop";
    case BlendMode::kXor:
      return "Xor";
    case BlendMode::kPlus:
      return "Plus";
    case BlendMode::kModulate:
      return "Modulate";
    case BlendMode::kScreen:
      return "Screen";

    case BlendMode::kOverlay:
      return "Overlay";
    case BlendMode::kDarken:
      return "Darken";
    case BlendMode::kLighten:
      return "Lighten";
    case BlendMode::kColorDodge:
      return "ColorDodge";
    case BlendMode::kColorBurn:
      return "ColorBurn";
    case BlendMode::kHardLight:
      return "HardLight";
    case BlendMode::kSoftLight:
      return "SoftLight";
    case BlendMode::kDifference:
      return "Difference";
    case BlendMode::kExclusion:
      return "Exclusion";
    case BlendMode::kMultiply:
      return "Multiply";

    case BlendMode::kHue:
      return "Hue";
    case BlendMode::kSaturation:
      return "Saturation";
    case BlendMode::kColor:
      return "Color";
    case BlendMode::kLuminosity:
      return "Luminosity";
    default:
      return "Unknown";
  }
}

namespace {

// ---------------------------------------------------------------------------
// Advanced (non-coeff) blend modes, premultiplied f32 domain.
//
// These mirror the WGSL functions the GPU backend generates
// (src/render/hw/draw/wgx_programmable_blending.cc) expression for
// expression, so the CPU backend renders advanced modes the way the GPU
// does. Components are premultiplied color channels in [0, 1]; the (sc, sa)
// and (dc, da) pairs correspond to the vec2 (color, alpha) of the shader
// helpers.
// ---------------------------------------------------------------------------

constexpr float kMinNormalHalf = 0.0001f;

inline float OverlayComponent(float sc, float sa, float dc, float da) {
  if (2.f * dc <= da) {
    return 2.f * sc * dc;
  }
  return sa * da - 2.f * (da - dc) * (sa - sc);
}

inline float ColorDodgeComponent(float sc, float sa, float dc, float da) {
  const float dx_scale = dc == 0.f ? 0.f : 1.f;
  const float ratio =
      std::abs(sa - sc) >= kMinNormalHalf ? (dc * sa) / (sa - sc) : da;
  const float delta = dx_scale * std::min(da, ratio);
  return delta * sa + sc * (1.f - da) + dc * (1.f - sa);
}

inline float ColorBurnComponent(float sc, float sa, float dc, float da) {
  const float dy_term = da == dc ? da : 0.f;
  const float delta = std::abs(sc) >= kMinNormalHalf
                          ? da - std::min(da, (da - dc) * sa / sc)
                          : dy_term;
  return delta * sa + sc * (1.f - da) + dc * (1.f - sa);
}

inline float SoftLightComponent(float sc, float sa, float dc, float da) {
  if (2.f * sc <= sa) {
    return dc * dc * (sa - 2.f * sc) / da + (1.f - da) * sc +
           dc * (-sa + 2.f * sc + 1.f);
  }
  if (4.f * dc <= da) {
    const float d_sq = dc * dc;
    const float d_cu = d_sq * dc;
    const float da_sq = da * da;
    const float da_cu = da_sq * da;
    return (da_sq * (sc - dc * (3.f * sa - 6.f * sc - 1.f)) +
            12.f * da * d_sq * (sa - 2.f * sc) - 16.f * d_cu * (sa - 2.f * sc) -
            da_cu * sc) /
           da_sq;
  }
  return dc * (sa - 2.f * sc + 1.f) + sc -
         std::sqrt(da * dc) * (sa - 2.f * sc) - da * sc;
}

inline float ColorLuminance(float r, float g, float b) {
  return 0.3f * r + 0.59f * g + 0.11f * b;
}

struct RGB {
  float r, g, b;
};

inline RGB SetColorLuminance(const RGB& hue_sat, float alpha, const RGB& lum) {
  const float lum_h = ColorLuminance(hue_sat.r, hue_sat.g, hue_sat.b);
  const float lum_l = ColorLuminance(lum.r, lum.g, lum.b);
  float rr = lum_l - lum_h + hue_sat.r;
  float gg = lum_l - lum_h + hue_sat.g;
  float bb = lum_l - lum_h + hue_sat.b;
  const float mn = std::min(std::min(rr, gg), bb);
  const float mx = std::max(std::max(rr, gg), bb);
  if (mn < 0.f && lum_l != mn) {
    const float scale =
        lum_l / ((lum_l - mn) + 1.0e-6f /* kMinNormalHalf, HSL variant */);
    rr = lum_l + (rr - lum_l) * scale;
    gg = lum_l + (gg - lum_l) * scale;
    bb = lum_l + (bb - lum_l) * scale;
  }
  if (mx > alpha && mx != lum_l) {
    const float scale = (alpha - lum_l) / ((mx - lum_l) + 1.0e-6f);
    rr = lum_l + (rr - lum_l) * scale;
    gg = lum_l + (gg - lum_l) * scale;
    bb = lum_l + (bb - lum_l) * scale;
  }
  return {rr, gg, bb};
}

inline float ColorSaturation(const RGB& c) {
  return std::max(std::max(c.r, c.g), c.b) - std::min(std::min(c.r, c.g), c.b);
}

inline RGB SetColorSaturation(const RGB& color, const RGB& sat_color) {
  const float mn = std::min(std::min(color.r, color.g), color.b);
  const float mx = std::max(std::max(color.r, color.g), color.b);
  if (mx > mn) {
    const float sat = ColorSaturation(sat_color);
    return {((color.r - mn) * sat) / (mx - mn),
            ((color.g - mn) * sat) / (mx - mn),
            ((color.b - mn) * sat) / (mx - mn)};
  }
  return {0.f, 0.f, 0.f};
}

// flip_sat / flip_lum mirror the flipSat selector of the shader helper.
inline RGB BlendHSLC(bool flip_sat, bool flip_lum, const RGB& s, float sa,
                     const RGB& d, float da) {
  const float alpha = da * sa;
  const RGB sda{s.r * da, s.g * da, s.b * da};
  const RGB dsa{d.r * sa, d.g * sa, d.b * sa};
  RGB l = flip_sat ? dsa : sda;
  RGB r = flip_sat ? sda : dsa;
  if (flip_lum) {
    l = SetColorSaturation(l, r);
    r = dsa;
  }
  return SetColorLuminance(l, alpha, r);
}

// Quantizes a formula output back to a premul byte; the clamp absorbs float
// rounding overshoot, and the +0.5 floor rounds to nearest. The accelerator
// kernels mirror this exactly.
inline uint8_t QuantizeChannel(float v) {
  v = std::min(1.f, std::max(0.f, v));
  return static_cast<uint8_t>(v * 255.f + 0.5f);
}

PMColor AdvancedBlend(PMColor src, PMColor dst, BlendMode mode) {
  const float sr = ColorGetR(src) / 255.f;
  const float sg = ColorGetG(src) / 255.f;
  const float sb = ColorGetB(src) / 255.f;
  const float sa = ColorGetA(src) / 255.f;
  const float dr = ColorGetR(dst) / 255.f;
  const float dg = ColorGetG(dst) / 255.f;
  const float db = ColorGetB(dst) / 255.f;
  const float da = ColorGetA(dst) / 255.f;
  float r = 0.f;
  float g = 0.f;
  float b = 0.f;
  const float a = sa + (1.f - sa) * da;

  const auto overlay_like = [&](bool swap) {
    r = OverlayComponent(swap ? dr : sr, swap ? da : sa, swap ? sr : dr,
                         swap ? sa : da);
    g = OverlayComponent(swap ? dg : sg, swap ? da : sa, swap ? sg : dg,
                         swap ? sa : da);
    b = OverlayComponent(swap ? db : sb, swap ? da : sa, swap ? sb : db,
                         swap ? sa : da);
    // extra = dst * (1 - sa) + src * (1 - da), alpha zeroed.
    r += dr * (1.f - sa) + sr * (1.f - da);
    g += dg * (1.f - sa) + sg * (1.f - da);
    b += db * (1.f - sa) + sb * (1.f - da);
  };

  switch (mode) {
    case BlendMode::kOverlay:
      overlay_like(false);
      break;
    case BlendMode::kHardLight:
      overlay_like(true);
      break;
    case BlendMode::kDarken: {
      r = sr + (1.f - sa) * dr;
      g = sg + (1.f - sa) * dg;
      b = sb + (1.f - sa) * db;
      r = std::min(r, (1.f - da) * sr + dr);
      g = std::min(g, (1.f - da) * sg + dg);
      b = std::min(b, (1.f - da) * sb + db);
      break;
    }
    case BlendMode::kLighten: {
      r = sr + (1.f - sa) * dr;
      g = sg + (1.f - sa) * dg;
      b = sb + (1.f - sa) * db;
      r = std::max(r, (1.f - da) * sr + dr);
      g = std::max(g, (1.f - da) * sg + dg);
      b = std::max(b, (1.f - da) * sb + db);
      break;
    }
    case BlendMode::kColorDodge:
      r = ColorDodgeComponent(sr, sa, dr, da);
      g = ColorDodgeComponent(sg, sa, dg, da);
      b = ColorDodgeComponent(sb, sa, db, da);
      break;
    case BlendMode::kColorBurn:
      r = ColorBurnComponent(sr, sa, dr, da);
      g = ColorBurnComponent(sg, sa, dg, da);
      b = ColorBurnComponent(sb, sa, db, da);
      break;
    case BlendMode::kSoftLight:
      if (da == 0.f) {
        return src;
      }
      r = SoftLightComponent(sr, sa, dr, da);
      g = SoftLightComponent(sg, sa, dg, da);
      b = SoftLightComponent(sb, sa, db, da);
      break;
    case BlendMode::kDifference:
      r = sr + dr - 2.f * std::min(sr * da, dr * sa);
      g = sg + dg - 2.f * std::min(sg * da, dg * sa);
      b = sb + db - 2.f * std::min(sb * da, db * sa);
      break;
    case BlendMode::kExclusion:
      r = dr + sr - 2.f * dr * sr;
      g = dg + sg - 2.f * dg * sg;
      b = db + sb - 2.f * db * sb;
      break;
    case BlendMode::kMultiply:
      r = (1.f - sa) * dr + (1.f - da) * sr + sr * dr;
      g = (1.f - sa) * dg + (1.f - da) * sg + sg * dg;
      b = (1.f - sa) * db + (1.f - da) * sb + sb * db;
      break;
    case BlendMode::kHue: {
      const RGB out =
          BlendHSLC(false, true, {sr, sg, sb}, sa, {dr, dg, db}, da);
      r = out.r + dr - dr * sa + sr - sr * da;
      g = out.g + dg - dg * sa + sg - sg * da;
      b = out.b + db - db * sa + sb - sb * da;
      return ColorSetARGB(QuantizeChannel(sa + da - da * sa),
                          QuantizeChannel(r), QuantizeChannel(g),
                          QuantizeChannel(b));
    }
    case BlendMode::kSaturation: {
      const RGB out = BlendHSLC(true, true, {sr, sg, sb}, sa, {dr, dg, db}, da);
      r = out.r + dr - dr * sa + sr - sr * da;
      g = out.g + dg - dg * sa + sg - sg * da;
      b = out.b + db - db * sa + sb - sb * da;
      return ColorSetARGB(QuantizeChannel(sa + da - da * sa),
                          QuantizeChannel(r), QuantizeChannel(g),
                          QuantizeChannel(b));
    }
    case BlendMode::kColor: {
      const RGB out =
          BlendHSLC(false, false, {sr, sg, sb}, sa, {dr, dg, db}, da);
      r = out.r + dr - dr * sa + sr - sr * da;
      g = out.g + dg - dg * sa + sg - sg * da;
      b = out.b + db - db * sa + sb - sb * da;
      return ColorSetARGB(QuantizeChannel(sa + da - da * sa),
                          QuantizeChannel(r), QuantizeChannel(g),
                          QuantizeChannel(b));
    }
    case BlendMode::kLuminosity: {
      const RGB out =
          BlendHSLC(true, false, {sr, sg, sb}, sa, {dr, dg, db}, da);
      r = out.r + dr - dr * sa + sr - sr * da;
      g = out.g + dg - dg * sa + sg - sg * da;
      b = out.b + db - db * sa + sb - sb * da;
      return ColorSetARGB(QuantizeChannel(sa + da - da * sa),
                          QuantizeChannel(r), QuantizeChannel(g),
                          QuantizeChannel(b));
    }
    default:
      return dst;
  }
  return ColorSetARGB(QuantizeChannel(a), QuantizeChannel(r),
                      QuantizeChannel(g), QuantizeChannel(b));
}

}  // namespace

PMColor PorterDuffBlend(PMColor src, PMColor dst, BlendMode mode) {
  // Note: Porter-Duff modes are only defined to work properly with
  // premultiplied alpha. That means that none of the R, G or B components can
  // exceed the alpha value.
  if (mode > BlendMode::kScreen) {
    // Advanced modes evaluate in premul f32 with the WGSL formulas the GPU
    // backend uses (previously these fell back to kSrcOver).
    return AdvancedBlend(src, dst, mode);
  }
  switch (mode) {
    case BlendMode::kClear:  // r = 0
      return Color_TRANSPARENT;
    case BlendMode::kSrc:  // r = s
      return src;
    case BlendMode::kDst:  // r = d
      return dst;
    case BlendMode::kSrcOver:  // r = s + (1-sa)*d
      if (ColorGetA(src) == 0) {
        return dst;
      }
      return PMSrcOver(src, dst);
    case BlendMode::kDstOver:  // r = d + (1-da)*s
      if (ColorGetA(dst) == 255) {
        return dst;
      }
      return PMSrcOver(dst, src);
    case BlendMode::kSrcIn:  // r = s * da
      return (ColorGetA(dst) == 255)
                 ? src
                 : AlphaMulQ(src, Alpha255To256(ColorGetA(dst)));
    case BlendMode::kDstIn:  // r = d * sa
      if (ColorGetA(src) == 255) {
        return dst;
      }
      return AlphaMulQ(dst, Alpha255To256(ColorGetA(src)));
    case BlendMode::kSrcOut:  // r = s * (1-da)
      return (ColorGetA(dst) == 0)
                 ? src
                 : AlphaMulQ(src, Alpha255To256(255 - ColorGetA(dst)));
    case BlendMode::kDstOut:  // r = d * (1-sa)
      if (ColorGetA(src) == 0) {
        return dst;
      }
      return AlphaMulQ(dst, Alpha255To256(255 - ColorGetA(src)));
    case BlendMode::kSrcATop:  // r = s*da + d*(1-sa)
      return AlphaMulQ(src, Alpha255To256(ColorGetA(dst))) +
             AlphaMulQ(dst, Alpha255To256(255 - ColorGetA(src)));
    case BlendMode::kDstATop:  // r = d*sa + s*(1-da)
      return AlphaMulQ(dst, Alpha255To256(ColorGetA(src))) +
             AlphaMulQ(src, Alpha255To256(255 - ColorGetA(dst)));
    case BlendMode::kXor:  // r = s*(1-da) + d*(1-sa)
      return AlphaMulQ(src, Alpha255To256(255 - ColorGetA(dst))) +
             AlphaMulQ(dst, Alpha255To256(255 - ColorGetA(src)));
    case BlendMode::kPlus:  // r = min(s + d, 1)
      return ColorSetARGB(std::min(ColorGetA(src) + ColorGetA(dst), 255u),
                          std::min(ColorGetR(src) + ColorGetR(dst), 255u),
                          std::min(ColorGetG(src) + ColorGetG(dst), 255u),
                          std::min(ColorGetB(src) + ColorGetB(dst), 255u));
    case BlendMode::kModulate:  // r = s*d
      return PMColorMul(src, dst);
    case BlendMode::kScreen:  // r = s + d - s*d
      return src + dst - PMColorMul(src, dst);
    default:
      return dst;
  }
}

}  // namespace skity
