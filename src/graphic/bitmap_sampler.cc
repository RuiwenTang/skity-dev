// Copyright 2021 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

#include "src/graphic/bitmap_sampler.hpp"

#include "src/geometry/math.hpp"

namespace skity {

namespace {
float RemapFloatTile(float t, TileMode tile_mode) {
  if (tile_mode == TileMode::kClamp) {
    t = std::clamp(t, 0.0f, 1.0f);
  } else if (tile_mode == TileMode::kRepeat) {
    t = FloatFract(t);
  } else if (tile_mode == TileMode::kMirror) {
    float t1 = t - 1;
    float t2 = t1 - 2 * std::floor(t1 * 0.5) - 1;
    t = std::abs(t2);
  }
  return t;
}

// Mitchell-Netravali cubic convolution weight at distance x (|x|), mirroring
// the GPU texture fragment shader (wgsl_texture_fragment.cc cubic_weight).
float CubicWeight(float x, float B, float C) {
  const float ax = glm::abs(x);
  const float ax2 = ax * ax;
  const float ax3 = ax2 * ax;
  const float w01 =
      ((12.0f - 9.0f * B - 6.0f * C) * ax3 +
       (-18.0f + 12.0f * B + 6.0f * C) * ax2 + (6.0f - 2.0f * B)) /
      6.0f;
  const float w12 = ((-B - 6.0f * C) * ax3 + (6.0f * B + 30.0f * C) * ax2 +
                     (-12.0f * B - 48.0f * C) * ax + (8.0f * B + 24.0f * C)) /
                    6.0f;
  // Three polynomial pieces: |x| < 1 -> w01, |x| < 2 -> w12, else 0.
  if (ax < 1.0f) {
    return w01;
  } else if (ax < 2.0f) {
    return w12;
  }
  return 0.0f;
}
}  // namespace

Vec4 BitmapSampler::SampleXY(Vec2 xy) const {
  const uint32_t x = std::clamp<double>(xy.x, 0, bitmap_.Width() - 1);
  const uint32_t y = std::clamp<double>(xy.y, 0, bitmap_.Height() - 1);
  return Color4fFromColor(bitmap_.GetPixel(x, y));
}

Vec4 BitmapSampler::SampleUnitNearest(Vec2 uv) const {
  uint32_t w = bitmap_.Width();
  uint32_t h = bitmap_.Height();
  return SampleXY({uv.x * w, uv.y * h});
}

Vec4 BitmapSampler::SampleUnitLinear(Vec2 uv) const {
  uint32_t w = bitmap_.Width();
  uint32_t h = bitmap_.Height();

  float x = uv.x * w;
  float y = uv.y * h;

  float i0 = glm::floor(x - 0.5f);
  float j0 = glm::floor(y - 0.5f);

  if (x_tile_mode_ == TileMode::kRepeat) {
    i0 = glm::mod(i0, static_cast<float>(w));
  }

  if (y_tile_mode_ == TileMode::kRepeat) {
    j0 = glm::mod(j0, static_cast<float>(h));
  }

  float i1 = i0 + 1.0f;
  float j1 = j0 + 1.0f;

  if (x_tile_mode_ == TileMode::kRepeat) {
    i1 = glm::mod(i1, static_cast<float>(w));
  }

  if (y_tile_mode_ == TileMode::kRepeat) {
    j1 = glm::mod(j1, static_cast<float>(h));
  }

  float a = glm::fract(x - 0.5f);
  float b = glm::fract(y - 0.5f);

  Vec4 ti0j0 = SampleXY({i0, j0});
  Vec4 ti1j0 = SampleXY({i1, j0});
  Vec4 ti0j1 = SampleXY({i0, j1});
  Vec4 ti1j1 = SampleXY({i1, j1});

  return ((1 - a) * (1 - b) * ti0j0) +  //
         (a * (1 - b) * ti1j0) +        //
         ((1 - a) * b * ti0j1) +        //
         (a * b * ti1j1);
}

Vec4 BitmapSampler::SampleUnitCubic(Vec2 uv) const {
  const float w = static_cast<float>(bitmap_.Width());
  const float h = static_cast<float>(bitmap_.Height());
  const float B = sampling_options_.cubic.B;
  const float C = sampling_options_.cubic.C;

  // Fast bicubic, mirroring the GPU fragment shader (cubic_sample_1d): the
  // four cubic taps on each axis fold into two weighted points, and the 2x2
  // points combine through four bilinear samples - 16 nearest taps become 4
  // bilinear fetches. Exact for all-positive kernels (B-spline), a close
  // approximation for kernels with negative lobes (Catmull-Rom).
  const auto fold_axis = [B, C](float coord, float dim, float& uv_a,
                                float& uv_b, float& c_a, float& c_b) {
    const float p = coord * dim;
    const float it = glm::floor(p - 0.5f);
    const float f = p - 0.5f - it;
    const float wm1 = CubicWeight(f + 1.0f, B, C);
    const float w0 = CubicWeight(f, B, C);
    const float w1 = CubicWeight(1.0f - f, B, C);
    const float w2 = CubicWeight(2.0f - f, B, C);
    c_a = wm1 + w0;
    c_b = w1 + w2;
    // Guard division by zero (Catmull-Rom at f == 0 gives c_b == 0), same
    // epsilon as the GPU shader.
    uv_a = (it - 0.5f + w0 / (c_a + 0.000001f)) / dim;
    uv_b = (it + 1.5f + w2 / (c_b + 0.000001f)) / dim;
  };

  float uv_ax, uv_bx, c_ax, c_bx;
  fold_axis(uv.x, w, uv_ax, uv_bx, c_ax, c_bx);
  float uv_ay, uv_by, c_ay, c_by;
  fold_axis(uv.y, h, uv_ay, uv_by, c_ay, c_by);

  // The fold points can land slightly outside [0, 1] (negative kernel lobes),
  // so every bilinear sample is tile-remapped again - the GPU shader applies
  // remap_float_tile per textureSample for the same reason. SampleUnitLinear
  // plays the hardware sampler's role (wrap for kRepeat, edge-clamped taps
  // otherwise).
  const Vec4 sAA = SampleUnitLinear({RemapFloatTile(uv_ax, x_tile_mode_),
                                     RemapFloatTile(uv_ay, y_tile_mode_)});
  const Vec4 sAB = SampleUnitLinear({RemapFloatTile(uv_ax, x_tile_mode_),
                                     RemapFloatTile(uv_by, y_tile_mode_)});
  const Vec4 sBA = SampleUnitLinear({RemapFloatTile(uv_bx, x_tile_mode_),
                                     RemapFloatTile(uv_ay, y_tile_mode_)});
  const Vec4 sBB = SampleUnitLinear({RemapFloatTile(uv_bx, x_tile_mode_),
                                     RemapFloatTile(uv_by, y_tile_mode_)});

  return c_ax * c_ay * sAA + c_ax * c_by * sAB + c_bx * c_ay * sBA +
         c_bx * c_by * sBB;
}

Color BitmapSampler::GetColor(Vec2 uv) const {
  if ((x_tile_mode_ == TileMode::kDecal && (uv.x < 0.0 || uv.x >= 1.0)) ||
      (y_tile_mode_ == TileMode::kDecal && (uv.y < 0.0 || uv.y >= 1.0))) {
    return Color_TRANSPARENT;
  }

  uv = Vec2{RemapFloatTile(uv.x, x_tile_mode_),
            RemapFloatTile(uv.y, y_tile_mode_)};
  Vec4 sampled{};
  if (sampling_options_.UseCubic()) {
    sampled = SampleUnitCubic(uv);
  } else {
    switch (sampling_options_.filter) {
      case FilterMode::kNearest:
        sampled = SampleUnitNearest(uv);
        break;
      case FilterMode::kLinear:
        sampled = SampleUnitLinear(uv);
        break;
    }
  }
  // Convert the sampled color into the target space after filtering, the
  // same semantics as the GPU texture decode (filter in the stored space).
  if (!steps_.IsNoop()) {
    if (alpha_type_ == kPremul_AlphaType) {
      // Transfer functions are defined on unpremultiplied values; feeding
      // premultiplied ones scales the curve by alpha and can push channels
      // above their alpha. Convert in the unpremultiplied domain and scale
      // back; a zero-alpha sample keeps its exact zeros.
      if (sampled.w > 0.f) {
        Color4f unpremul =
            steps_.Apply(Color4f{sampled.x / sampled.w, sampled.y / sampled.w,
                                 sampled.z / sampled.w, sampled.w});
        // Out-of-gamut conversions exceed [0,1] (ColorSpace keeps the
        // extended range by contract), but the software blend path needs
        // canonical premultiplied channels (RGB <= A). Clamp in the
        // unpremultiplied domain before scaling back — the same clamp the
        // GPU decode applies before its re-premultiply.
        unpremul.r = std::clamp(unpremul.r, 0.f, 1.f);
        unpremul.g = std::clamp(unpremul.g, 0.f, 1.f);
        unpremul.b = std::clamp(unpremul.b, 0.f, 1.f);
        sampled = Color4f{unpremul.r * sampled.w, unpremul.g * sampled.w,
                          unpremul.b * sampled.w, sampled.w};
      }
    } else {
      // Unpremultiplied output stays extended-range: the quantization
      // clamp at the end keeps every channel in [0,255], which is enough
      // for the downstream premultiply to preserve RGB <= A.
      sampled = steps_.Apply(sampled);
    }
  }
  return Color4fToColor(sampled);
}

}  // namespace skity
