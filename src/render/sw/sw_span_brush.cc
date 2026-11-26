// Copyright 2021 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

#include "src/render/sw/sw_span_brush.hpp"

#include <cmath>

#ifdef SKITY_SW_ACCEL
#include "src/render/sw/accel/sw_accel.hpp"
#endif

#include <skity/effect/color_filter.hpp>
#include <skity/graphic/bitmap.hpp>

#include "src/geometry/geometry.hpp"
#include "src/graphic/color_priv.hpp"
#include "src/tracing.hpp"

namespace skity {

namespace {
float RemapFloatTile(float t, TileMode tile_mode) {
  if (tile_mode == TileMode::kClamp) {
    t = std::clamp(t, 0.0f, 1.0f);
  } else if (tile_mode == TileMode::kRepeat) {
    t = glm::fract(t);
  } else if (tile_mode == TileMode::kMirror) {
    float t1 = t - 1;
    float t2 = t1 - 2 * std::floor(t1 * 0.5) - 1;
    t = std::abs(t2);
  }
  return t;
}
}  // namespace

void SWSpanBrush::Brush() {
  SKITY_TRACE_EVENT(SWSpanBrush_Brush);

  auto i_width = static_cast<int32_t>(bitmap_->Width());
  auto i_height = static_cast<int32_t>(bitmap_->Height());

  OnPreBrush();
  for (size_t i = 0; i < spans_size_; i++) {
    Span const& span = p_spans_[i];

    auto x = span.x;
    auto y = span.y;
    auto len = span.len;

    if (y < 0 || y >= i_height) {
      continue;
    }

    if (x >= i_width || x + len < 0) {
      continue;
    }

    if (x < 0) {
      len += x;
      x = 0;
    }

    if (x + len >= i_width) {
      len = i_width - x;
    }

    if (len <= 0) {
      continue;
    }

    // Coverage and global alpha must MULTIPLY (MulDiv255Round), not AND: the
    // bitwise-and turned every partial-coverage span with a fractional paint
    // alpha into either 0 or the raw alpha bit pattern, so antialiased image
    // edges under a translucent paint came out far too opaque (e.g. cover 128
    // x alpha 128 blended at 128 instead of 64).
    auto u_alpha =
        static_cast<uint8_t>(MulDiv255Round(span.cover, global_alpha_));

    if (len == 1 || PureColor()) {
      PMColor color = CalculateColor(x, y);

      if (u_alpha != 255) {
        color = AlphaMulQ(color, u_alpha);
      }

      if (color_filter_) {
        color = color_filter_->FilterColor(color);
      }
      render_target_.BlendPixelH(x, y, color, len, blend_);
    } else {
      // Produce the dense color row once, blend it in one call: the
      // per-span heap buffer and the per-span virtual hop are gone.
      if (row_colors_.size() < static_cast<size_t>(len)) {
        row_colors_.resize(static_cast<size_t>(len));
      }

      for (int32_t l = 0; l < len; l++) {
        PMColor color = CalculateColor(x + l, y);
        if (u_alpha != 255) {
          color = AlphaMulQ(color, u_alpha);
        }

        if (color_filter_) {
          color = color_filter_->FilterColor(color);
        }

        row_colors_[l] = color;
      }

      render_target_.BlendPixelH(x, y, row_colors_.data(), len, blend_);
    }
  }
  OnPostBrush();
}

SolidColorBrush::SolidColorBrush(std::vector<Span> const& spans, Bitmap* bitmap,
                                 ColorFilter* color_filter, BlendMode blend,
                                 Color4f color)
    : SWSpanBrush(spans, bitmap, color_filter, blend, 1.f),
      color_(Color4fToColor(color)) {
  color_ = ColorToPMColor(color_);
}

Color SolidColorBrush::CalculateColor(int32_t, int32_t) {
  SKITY_TRACE_EVENT(SolidColorBrush_CalculateColor);

  return color_;
}

Color GradientColorBrush::CalculateColor(int32_t x, int32_t y) {
  // for unsupport gradient type
  Color4f c = Color4f{};
  Color color = Color4fToColor(c);
  color = ColorToPMColor(color);
  return color;
}

namespace {

Matrix PointsToUnit(const Shader::GradientInfo& info,
                    Shader::GradientType type) {
  if (type == Shader::GradientType::kLinear) {
    Vec2 start = Vec2{(info.point[0])};
    Vec2 stop = Vec2{info.point[1]};
    Vec2 ss = stop - start;
    float length = ss.Length();
    float scale = length > 0 ? 1.0f / length : 0;
    Vec2 unit_ss = ss * scale;
    float sine = -unit_ss.y;
    float cosine = unit_ss.x;

    Matrix rotate;
    rotate.SetScaleX(cosine);
    rotate.SetSkewX(-sine);
    rotate.SetSkewY(sine);
    rotate.SetScaleY(cosine);

    return rotate *                       //
           Matrix::Scale(scale, scale) *  //
           Matrix::Translate(-start.x, -start.y);

  } else if (type == Shader::GradientType::kRadial) {
    float radius = info.radius[0];
    Vec2 center = FromPoint(info.point[0]);
    float scale = radius > 0 ? 1.0f / radius : 0;
    return Matrix::Scale(scale, scale) *
           Matrix::Translate(-center.x, -center.y);
  } else if (type == Shader::GradientType::kSweep) {
    Vec2 center = FromPoint(info.point[0]);
    return Matrix::Translate(-center.x, -center.y);
  }

  return Matrix{};
}

Matrix PointsToUnit(const Point& p0, const Point& p1) {
  Vec2 start = FromPoint(p0);
  Vec2 stop = FromPoint(p1);
  Vec2 ss = stop - start;
  float length = ss.Length();
  float scale = length > 0 ? 1.0f / length : 0;
  Vec2 unit_ss = ss * scale;
  float sine = -unit_ss.y;
  float cosine = unit_ss.x;

  Matrix rotate;
  rotate.SetScaleX(cosine);
  rotate.SetSkewX(-sine);
  rotate.SetSkewY(sine);
  rotate.SetScaleY(cosine);

  return rotate *                       //
         Matrix::Scale(scale, scale) *  //
         Matrix::Translate(-start.x, -start.y);
}

}  // namespace

GradientColorBrush::GradientColorBrush(
    std::vector<Span> const& spans, Bitmap* bitmap, ColorFilter* color_filter,
    BlendMode blend, Shader::GradientInfo info, Shader::GradientType type)
    : SWSpanBrush(spans, bitmap, color_filter, blend, 1.f),
      info_(std::move(info)),
      type_(type) {}

namespace {

Vec2 inline MapPoint(const Vec2& src, const Matrix& m) {
  return {src.x * m.GetScaleX() + src.y * m.GetSkewX() + m.GetTranslateX(),
          src.x * m.GetSkewY() + src.y * m.GetScaleY() + m.GetTranslateY()};
}

}  // namespace

Color4f GradientColorBrush::LerpColor(float current) {
  if (FloatNearlyZero(current)) {
    current = 0.0f;
  } else if (FloatNearlyZero(current - 1.0f)) {
    current = 1.0f;
  }

  if ((info_.tile_mode == TileMode::kDecal &&
       (current < 0.0 || current >= 1.0))) {
    return Colors::kTransparent;
  }

  current = RemapFloatTile(current, info_.tile_mode);

  int32_t color_count = info_.colors.size();
  int32_t stop_count = info_.color_offsets.size();

  int32_t start_index = 0;
  int32_t end_index = 1;

  float step = 1.f / (color_count - 1);

  if (stop_count > 0 && current <= info_.color_offsets[0]) {
    return info_.colors[0];
  }

  int32_t i = 0;
  float start = 0.f;
  float end = 0.f;

  for (i = 0; i < color_count - 1; i++) {
    if (stop_count > 0) {
      start = info_.color_offsets[i];
      end = info_.color_offsets[i + 1];
    } else {
      start = step * i;
      end = step * (i + 1);
    }

    if (current >= start && current <= end) {
      start_index = i;
      end_index = i + 1;
      break;
    }
  }

  if (i == color_count - 1 && color_count > 0) {
    return info_.colors[color_count - 1];
  }

  float total = end - start;
  float value = current - start;

  float mix_value = 0.5f;
  if (total > 0) {
    mix_value = value / total;
  }

  return info_.colors[start_index] * (1 - mix_value) +
         info_.colors[end_index] * mix_value;
}

class LinearGradientColorBrush : public GradientColorBrush {
 public:
  LinearGradientColorBrush(std::vector<Span> const& spans, Bitmap* bitmap,
                           ColorFilter* color_filter, BlendMode blend,
                           Shader::GradientInfo info, Shader::GradientType type,
                           const Matrix& device_to_local)
      : GradientColorBrush(spans, bitmap, color_filter, blend, info, type),
        points_to_unit_(PointsToUnit(info_, type_) * device_to_local) {}
  ~LinearGradientColorBrush() override = default;

 protected:
  Color CalculateColor(int32_t x, int32_t y) override {
    SKITY_TRACE_EVENT(LinearGradientColorBrush_CalculateColor);

    Vec2 src{x + 0.5f, y + 0.5f};
    Color4f c = LerpColor(MapPoint(src, points_to_unit_).x);
    Color color = FinalizeColor4f(c);
    return color;
  }

 private:
  Matrix points_to_unit_ = {};
};

class SweepGradientColorBrush : public GradientColorBrush {
 public:
  SweepGradientColorBrush(std::vector<Span> const& spans, Bitmap* bitmap,
                          ColorFilter* color_filter, BlendMode blend,
                          Shader::GradientInfo info, Shader::GradientType type,
                          const Matrix& device_to_local)
      : GradientColorBrush(spans, bitmap, color_filter, blend, info, type),
        points_to_unit_(PointsToUnit(info_, type_) * device_to_local) {}
  ~SweepGradientColorBrush() override = default;

 protected:
  Color CalculateColor(int32_t x, int32_t y) override {
    SKITY_TRACE_EVENT(SweepGradientColorBrush_CalculateColor);

    Vec2 src{x + 0.5f, y + 0.5f};
    auto mapped = MapPoint(src, points_to_unit_);

    float angle = std::atan2(-mapped.y, -mapped.x);

    auto bias = info_.radius[0];
    auto scale = info_.radius[1];

    constexpr static float k1Over2Pi = 0.1591549430918;
    float t = (angle * k1Over2Pi + 0.5 + bias) * scale;
    Color4f c = LerpColor(t);
    Color color = FinalizeColor4f(c);
    return color;
  }

 private:
  Matrix points_to_unit_ = {};
};

class RadialGradientColorBrush : public GradientColorBrush {
 public:
  RadialGradientColorBrush(std::vector<Span> const& spans, Bitmap* bitmap,
                           ColorFilter* color_filter, BlendMode blend,
                           Shader::GradientInfo info, Shader::GradientType type,
                           const Matrix& device_to_local)
      : GradientColorBrush(spans, bitmap, color_filter, blend, info, type),
        points_to_unit_(PointsToUnit(info_, type_) * device_to_local) {}
  ~RadialGradientColorBrush() override = default;

 protected:
  Color CalculateColor(int32_t x, int32_t y) override {
    SKITY_TRACE_EVENT(RadialGradientColorBrush_CalculateColor);

    Vec2 src{x + 0.5f, y + 0.5f};
    Color4f c = LerpColor(MapPoint(src, points_to_unit_).Length());
    Color color = FinalizeColor4f(c);
    return color;
  }

 private:
  Matrix points_to_unit_ = {};
};

class ConicalGradientColorBrush : public GradientColorBrush {
 public:
  ConicalGradientColorBrush(std::vector<Span> const& spans, Bitmap* bitmap,
                            ColorFilter* color_filter, BlendMode blend,
                            Shader::GradientInfo info,
                            Shader::GradientType type,
                            const Matrix& device_to_local)
      : GradientColorBrush(spans, bitmap, color_filter, blend, info, type),
        device_to_local_(device_to_local),
        points_to_unit_(PointsToUnit(info_, type_)) {}
  ~ConicalGradientColorBrush() override = default;

 protected:
  Color CalculateColor(int32_t x, int32_t y) override {
    SKITY_TRACE_EVENT(ConicalGradientColorBrush_CalculateColor);

    Color4f c = CalculateConical(x, y);
    Color color = FinalizeColor4f(c);
    return color;
  }

  void OnPreBrush() override {
    SKITY_TRACE_EVENT(ConicalGradientColorBrush_OnPreBrush);

    c0_ = info_.point[0];
    c1_ = info_.point[1];
    r0_ = info_.radius[0];
    r1_ = info_.radius[1];
    delta_center_ = (c1_ - c0_).Length();
    delta_radius_ = std::abs(r1_ - r0_);

    if (r0_ < 0 || r1_ < 0) {
      return;
    }

    radial_ = delta_center_ < kNearlyZero;
    strip_ = delta_radius_ < kNearlyZero;

    if (radial_) {
      if (!strip_) {
        scale_ = 1.0 / delta_radius_;
        scale_sign_ = delta_radius_ < 0 ? -1.f : 1.f;
        bias_ = r0_ / delta_radius_;
      }
    } else if (strip_) {
      c0c1_transform_ = PointsToUnit(c0_, c1_);
    } else {
      swap_01_ = r1_ < kNearlyZero;
      if (swap_01_) {
        std::swap(c0_, c1_);
        std::swap(r0_, r1_);
      }
      f_ = r0_ / (r0_ - r1_);
      cf_ = c0_ * (1.f - f_) + c1_ * f_;
      r1_ = r1_ / (c1_ - cf_).Length();
      r1_square_ = r1_ * r1_;

      cfc1_tranform_ = PointsToUnit(cf_, c1_);
    }
  }
  void OnPostBrush() override {}

 private:
  Color4f CalculateConical(int32_t x, int32_t y) {
    if (r0_ < 0 || r1_ < 0) {
      return Colors::kTransparent;
    }

    float t = 0;
    Vec2 p{x + 0.5f, y + 0.5f};
    p = MapPoint(p, device_to_local_);
    if (radial_) {
      // degenerate case 1: codes from shader
      if (strip_) {
        return Colors::kTransparent;
      }
      Vec2 pt = (p - FromPoint(c0_)) * scale_;
      t = pt.Length() * scale_sign_ - bias_;
    } else if (strip_) {
      // degenerate case 2: codes from shader
      float r = r0_ / delta_center_;
      float r_2 = r * r;

      p = MapPoint(p, c0c1_transform_);
      t = r_2 - p.y * p.y;
      if (t < 0.0) {
        return Colors::kTransparent;
      }
      t = p.x + sqrt(t);
    } else {
      // general case: codes from https://skia.org/docs/dev/design/conical/
      p = MapPoint(p, cfc1_tranform_);

      float xt = -1.f;
      if (std::abs(r1_ - 1.f) < kNearlyZero) {
        xt = (p.x * p.x + p.y * p.y) / 2;
      } else if (r1_ > 1.f) {
        float m = r1_square_ - 1.f;
        float delta = m * p.y * p.y + r1_square_ * p.x * p.x;
        xt = (std::sqrt(delta) - p.x) / m;
      } else {
        float m = r1_square_ - 1.f;
        float delta = m * p.y * p.y + r1_square_ * p.x * p.x;
        if (delta > 0) {
          float xt1 = (std::sqrt(delta) - p.x) / m;
          float xt2 = (-std::sqrt(delta) - p.x) / m;
          xt = 1.f - f_ < 0 ? std::min(xt1, xt2) : std::max(xt1, xt2);
        }
      }

      if (xt < 0) {
        return Colors::kTransparent;
      }

      t = f_ + (1.f - f_) * xt;
      if (swap_01_) {
        t = 1.0 - t;
      }
    }

    return LerpColor(t);
  }

 private:
  Matrix device_to_local_ = {};
  Matrix points_to_unit_ = {};
  // All filed names are from https://skia.org/docs/dev/design/conical/
  Point c0_ = {};
  Point c1_ = {};
  float r0_ = {};
  float r1_ = {};
  float r1_square_ = {};
  float delta_center_ = {};
  float delta_radius_ = {};
  bool radial_ = {};
  bool strip_ = {};
  float scale_ = {};
  float scale_sign_ = {};
  float bias_ = {};
  Matrix c0c1_transform_ = {};
  bool swap_01_ = {};
  float f_ = {};
  Point cf_ = {};
  Matrix cfc1_tranform_ = {};
};

#ifdef SKITY_SW_ACCEL
// Gradient fill through the SIMD accelerator: precomputes the stop table in
// OnPreBrush and hands the whole span list to sw_accel::FillGradientSpans in
// one pass, for every blend mode (kSrc/kSrcOver fused, the rest staged).
// The LUT fetch matches the scalar chain within about 2 per channel, so the
// accepted configurations are the ones that tolerate that headroom;
// everything else (sweep/conical, unpremul or non-RGBA surfaces, color
// filters, degenerate gradients) keeps the legacy path.
class AcceleratedGradientColorBrush : public GradientColorBrush {
 public:
  AcceleratedGradientColorBrush(std::vector<Span> const& spans, Bitmap* bitmap,
                                ColorFilter* color_filter, BlendMode blend,
                                Shader::GradientInfo info,
                                Shader::GradientType type,
                                const Matrix& device_to_local)
      : GradientColorBrush(spans, bitmap, color_filter, blend, info, type),
        device_to_local_(device_to_local) {
    // The scalar twin that renders when the accelerator is disabled, trips
    // its Validate fuse, or rejects this configuration at draw time. This
    // factory only creates the accelerated brush for linear/radial
    // gradients, so the fallback is always one of the real legacy brushes -
    // never the transparent unsupported-type base.
    if (type == Shader::GradientType::kRadial) {
      fallback_ = std::make_unique<RadialGradientColorBrush>(
          spans, bitmap, color_filter, blend, info, type, device_to_local);
    } else {
      fallback_ = std::make_unique<LinearGradientColorBrush>(
          spans, bitmap, color_filter, blend, info, type, device_to_local);
    }
  }

  ~AcceleratedGradientColorBrush() override = default;

  void Brush() override {
    // Brush() runs before the base class would call OnPreBrush(), so the
    // params (including the LUT build) are constructed here, once, lazily -
    // and only when the accelerator would actually run, so the legacy
    // fallback arm never pays for them.
    if (!params_built_ && sw_accel::IsEnabled() && sw_accel::Available()) {
      params_built_ = true;
      use_accel_ = BuildParams();
    }
    if (use_accel_) {
      Bitmap* bitmap = CurrentBitmap();
      if (sw_accel::FillGradientSpans(
              params_, GetBlendMode(), bitmap->GetPixelAddr(),
              bitmap->RowBytes(), static_cast<int32_t>(bitmap->Width()),
              static_cast<int32_t>(bitmap->Height()), GetSpans(),
              GetSpanSize())) {
        return;
      }
      // Disabled or unavailable at draw time; nothing has been written yet.
    }
    fallback_->Brush();
  }

 private:
  bool BuildParams() {
    if (!sw_accel::Available()) {
      return false;
    }
    const auto& offsets = info_.color_offsets;
    const int32_t count = static_cast<int32_t>(info_.colors.size());
    // A short offsets table would read out of bounds below (the legacy scan
    // has the same assumption); keep such gradients on the scalar path.
    if (count < 2 ||
        (!offsets.empty() && offsets.size() < info_.colors.size())) {
      return false;
    }
    const Matrix m = PointsToUnit(info_, type_) * device_to_local_;
    params_.type = type_ == Shader::GradientType::kRadial ? 1 : 0;
    params_.m00 = m.GetScaleX();
    params_.m01 = m.GetSkewX();
    params_.m02 = m.GetTranslateX();
    params_.m10 = m.GetSkewY();
    params_.m11 = m.GetScaleY();
    params_.m12 = m.GetTranslateY();
    const float values[6] = {params_.m00, params_.m01, params_.m02,
                             params_.m10, params_.m11, params_.m12};
    for (float v : values) {
      // Degenerate transforms make t non-finite.
      if (!std::isfinite(v)) {
        return false;
      }
    }
    params_.tile_mode = static_cast<int32_t>(info_.tile_mode);
    params_.use_offsets = !offsets.empty();
    params_.premul_stops = info_.gradientFlags != 0;
    if (params_.use_offsets) {
      params_.first_offset = offsets.front();
    }
    params_.color_count = count;
    // Segment table with the legacy values: declared offsets, or the uniform
    // step grid (LerpColor computes step * i with the same float ops).
    const float step = 1.f / (count - 1);
    seg_start_.resize(static_cast<size_t>(count) - 1);
    seg_end_.resize(static_cast<size_t>(count) - 1);
    for (int32_t i = 0; i < count - 1; i++) {
      if (params_.use_offsets) {
        seg_start_[i] = offsets[i];
        seg_end_[i] = offsets[i + 1];
      } else {
        seg_start_[i] = step * i;
        seg_end_[i] = step * (i + 1);
      }
    }
    params_.seg_start = seg_start_.data();
    params_.seg_end = seg_end_.data();
    params_.colors = &info_.colors.front().e[0];
    // Fetch the baked stop chain from the per-thread LUT cache (keyed on
    // the stop table only - animated transforms reuse one entry). The
    // pointer is valid for this draw, which is exactly its lifetime here.
    params_.lut = sw_accel::AcquireGradientLUT(params_);
    if (params_.lut == nullptr) {
      return false;
    }
    params_.lut_entries = 512;
    return true;
  }

  Matrix device_to_local_ = {};
  sw_accel::GradientParams params_ = {};
  std::vector<float> seg_start_;
  std::vector<float> seg_end_;
  std::unique_ptr<SWSpanBrush> fallback_;
  bool params_built_ = false;
  bool use_accel_ = false;
};
#endif

std::unique_ptr<GradientColorBrush> GradientColorBrush::MakeGradientColorBrush(
    std::vector<Span> const& spans, Bitmap* bitmap, ColorFilter* color_filter,
    BlendMode blend, Shader::GradientInfo info, Shader::GradientType type,
    const Matrix& device_to_local) {
#ifdef SKITY_SW_ACCEL
  if (color_filter == nullptr && sw_accel::SupportsGradientMode(blend) &&
      (type == Shader::GradientType::kLinear ||
       type == Shader::GradientType::kRadial) &&
      info.colors.size() >= 2 &&
      bitmap->GetAlphaType() == AlphaType::kPremul_AlphaType &&
      bitmap->GetColorType() == ColorType::kRGBA) {
    return std::make_unique<AcceleratedGradientColorBrush>(
        spans, bitmap, color_filter, blend, info, type, device_to_local);
  }
#endif
  switch (type) {
    case Shader::GradientType::kLinear:
      return std::make_unique<LinearGradientColorBrush>(
          spans, bitmap, color_filter, blend, info, type, device_to_local);
    case Shader::GradientType::kRadial:
      return std::make_unique<RadialGradientColorBrush>(
          spans, bitmap, color_filter, blend, info, type, device_to_local);
    case Shader::GradientType::kConical:
      return std::make_unique<ConicalGradientColorBrush>(
          spans, bitmap, color_filter, blend, info, type, device_to_local);
    case Shader::GradientType::kSweep:
      return std::make_unique<SweepGradientColorBrush>(
          spans, bitmap, color_filter, blend, info, type, device_to_local);
    default:
      return std::make_unique<GradientColorBrush>(spans, bitmap, color_filter,
                                                  blend, info, type);
  }
}

PixmapBrush::PixmapBrush(std::vector<Span> const& spans, Bitmap* bitmap,
                         ColorFilter* color_filter, BlendMode blend,
                         float global_alpha, std::shared_ptr<Pixmap> pixmap,
                         const Matrix& points_to_unit,
                         const SamplingOptions& sampling, TileMode x_tile_mode,
                         TileMode y_tile_mode,
                         const std::shared_ptr<const ColorSpace>& target_space)
    : SWSpanBrush(spans, bitmap, color_filter, blend, global_alpha),
      texture_(new Bitmap(std::move(pixmap))),
      points_to_unit_(points_to_unit),
      x_tile_mode_(x_tile_mode),
      y_tile_mode_(y_tile_mode),
      bitmap_sampler_(*texture_.get(), sampling, x_tile_mode, y_tile_mode,
                      target_space) {
#ifdef SKITY_SW_ACCEL
  // Accelerated configurations: kRGBA textures without a color transform,
  // nearest/linear filtering, kSrc/kSrcOver, a finite transform and a
  // texture whose byte offsets fit the kernel's u32 math. Cubic sampling,
  // color filters, other blend modes, exotic texture formats and
  // degenerate textures keep the scalar sampler path.
  const bool offsets_fit =
      texture_->RowBytes() >= static_cast<size_t>(texture_->Width()) * 4 &&
      texture_->RowBytes() * texture_->Height() <= 0xffffffffull;
  const float matrix[6] = {
      points_to_unit.GetScaleX(),     points_to_unit.GetSkewX(),
      points_to_unit.GetTranslateX(), points_to_unit.GetSkewY(),
      points_to_unit.GetScaleY(),     points_to_unit.GetTranslateY()};
  bool finite = true;
  for (float entry : matrix) {
    finite = finite && std::isfinite(entry);
  }
  accel_ok_ = color_filter == nullptr && sw_accel::SupportsImageMode(blend) &&
              texture_->Width() >= 1 && texture_->Height() >= 1 &&
              offsets_fit && finite && !sampling.UseCubic() &&
              texture_->GetColorType() == ColorType::kRGBA &&
              bitmap_sampler_.ColorTransformIsNoop();
  if (accel_ok_) {
    accel_params_.texels = texture_->GetPixelAddr();
    accel_params_.tex_row_bytes = texture_->RowBytes();
    accel_params_.tex_width = static_cast<int32_t>(texture_->Width());
    accel_params_.tex_height = static_cast<int32_t>(texture_->Height());
    accel_params_.m00 = matrix[0];
    accel_params_.m01 = matrix[1];
    accel_params_.m02 = matrix[2];
    accel_params_.m10 = matrix[3];
    accel_params_.m11 = matrix[4];
    accel_params_.m12 = matrix[5];
    accel_params_.x_tile_mode = static_cast<int32_t>(x_tile_mode);
    accel_params_.y_tile_mode = static_cast<int32_t>(y_tile_mode);
    accel_params_.bilinear = sampling.filter == FilterMode::kLinear;
    accel_params_.src_premul =
        texture_->GetAlphaType() == AlphaType::kPremul_AlphaType;
    accel_params_.global_alpha = static_cast<uint8_t>(255 * global_alpha);
  }
#endif
}

Color PixmapBrush::CalculateColor(int32_t x, int32_t y) {
  SKITY_TRACE_EVENT(PixmapBrush_CalculateColor);

  auto uv = MapPoint(Vec2{x + 0.5f, y + 0.5f}, points_to_unit_);
  Color color = bitmap_sampler_.GetColor(uv);
  if (texture_->GetAlphaType() == kUnpremul_AlphaType) {
    color = ColorToPMColor(color);
  }

  return color;
}

void PixmapBrush::Brush() {
#ifdef SKITY_SW_ACCEL
  // The whole span list goes through the SIMD fetch + blend kernels in one
  // pass when the draw matches the accelerated configurations built by the
  // constructor; the kernel reproduces the scalar chain bit-exactly.
  if (accel_ok_ && sw_accel::IsEnabled() && sw_accel::Available()) {
    Bitmap* bitmap = CurrentBitmap();
    if (sw_accel::FillImageSpans(accel_params_, GetBlendMode(),
                                 bitmap->GetPixelAddr(), bitmap->RowBytes(),
                                 static_cast<int32_t>(bitmap->Width()),
                                 static_cast<int32_t>(bitmap->Height()),
                                 GetSpans(), GetSpanSize())) {
      return;
    }
    // Disabled or unavailable at draw time; nothing has been written yet.
  }
#endif
  SWSpanBrush::Brush();
}

}  // namespace skity
