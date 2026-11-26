// Copyright 2021 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

#ifndef SRC_RENDER_SW_SW_SPAN_BRUSH_HPP
#define SRC_RENDER_SW_SW_SPAN_BRUSH_HPP

#include <algorithm>
#include <array>
#include <skity/effect/shader.hpp>
#include <skity/geometry/matrix.hpp>
#include <skity/graphic/color.hpp>
#include <skity/graphic/color_space.hpp>
#include <skity/graphic/sampling_options.hpp>
#include <skity/graphic/tile_mode.hpp>
#include <skity/render/sw_context.hpp>
#include <vector>

#ifdef SKITY_SW_ACCEL
#include "src/render/sw/accel/sw_accel.hpp"
#endif

#include "src/graphic/bitmap_sampler.hpp"
#include "src/render/sw/sw_render_target.hpp"
#include "src/render/sw/sw_subpixel.hpp"

namespace skity {

class Bitmap;
class ColorFilter;
enum class BlendMode;

class SWSpanBrush {
 public:
  // context carries the runtime renderer configuration (accel switch,
  // parallelism); brushes read it per draw. Defaults to the process-wide
  // singleton, which is also what SWCanvas passes explicitly.
  SWSpanBrush(std::vector<Span> const& spans, Bitmap* bitmap,
              ColorFilter* color_filter, BlendMode blend, float global_alpha,
              const SWContext* context = nullptr)
      : p_spans_(spans.data()),
        spans_size_(spans.size()),
        bitmap_(bitmap),
        color_filter_(color_filter),
        blend_(blend),
        global_alpha_(static_cast<uint8_t>(255 * global_alpha)),
        render_target_(bitmap_),
        context_(context ? context : SWContext::Get()) {}

  virtual ~SWSpanBrush() = default;

  // Virtual so accelerated brushes can take the whole span list in one pass
  // (the scalar loop below is the fallback they fall back into).
  virtual void Brush();

 protected:
  // premultiplied color
  virtual Color CalculateColor(int32_t x, int32_t y) = 0;

  virtual bool PureColor() const { return false; }

  const Span* GetSpans() const { return p_spans_; }

  size_t GetSpanSize() const { return spans_size_; }

  Bitmap* CurrentBitmap() const { return bitmap_; }

  const SWContext* GetContext() const { return context_; }

  BlendMode GetBlendMode() const { return blend_; }

  SWRenderTarget& GetRenderTarget() { return render_target_; }

  virtual void OnPreBrush() {}
  virtual void OnPostBrush() {}

 private:
  const Span* p_spans_;
  size_t spans_size_;
  Bitmap* bitmap_;
  ColorFilter* color_filter_;
  BlendMode blend_;
  uint8_t global_alpha_;
  SWRenderTarget render_target_;
  // The process-wide renderer configuration (never null; the singleton
  // outlives every brush).
  const SWContext* context_ = nullptr;
  // Dense color row shared by every span of one draw: produced per span and
  // handed to the target in a single blend call, so the legacy path never
  // materializes a heap buffer per span.
  std::vector<PMColor> row_colors_;
};

class SolidColorBrush : public SWSpanBrush {
 public:
  SolidColorBrush(std::vector<Span> const& spans, Bitmap* bitmap,
                  ColorFilter* color_filter, BlendMode blend, Color4f color);

  ~SolidColorBrush() override = default;

 protected:
  Color CalculateColor(int32_t x, int32_t y) override;

  bool PureColor() const override { return true; }

 private:
  Color color_;
};

class GradientColorBrush : public SWSpanBrush {
 public:
  static std::unique_ptr<GradientColorBrush> MakeGradientColorBrush(
      std::vector<Span> const& spans, Bitmap* bitmap, ColorFilter* color_filter,
      BlendMode blend, Shader::GradientInfo info, Shader::GradientType type,
      const Matrix& device_to_local);

  GradientColorBrush(std::vector<Span> const& spans, Bitmap* bitmap,
                     ColorFilter* color_filter, BlendMode blend,
                     Shader::GradientInfo info, Shader::GradientType type);

  ~GradientColorBrush() override = default;

 protected:
  Color CalculateColor(int32_t x, int32_t y) override;

  Color4f LerpColor(float current);

  // Quantizes the interpolated stop color. A non-zero interpolation flag
  // means GenerateBrush already premultiplied the stop table, so the
  // interpolated value is premultiplied already: clamp RGB into [0, A]
  // (the shader's output clamp, which also folds out-of-gamut resolved
  // stops back into the canonical premul range) and quantize. The default
  // domain quantizes first and runs the trailing rounding premul.
  Color FinalizeColor4f(const Color4f& color) const {
    if (info_.gradientFlags == 0) {
      return ColorToPMColor(Color4fToColor(color));
    }
    Color4f c = color;
    c.r = std::clamp(c.r, 0.f, c.a);
    c.g = std::clamp(c.g, 0.f, c.a);
    c.b = std::clamp(c.b, 0.f, c.a);
    c.a = std::clamp(c.a, 0.f, 1.f);
    return Color4fToColor(c);
  }

  Shader::GradientInfo info_ = {};
  Shader::GradientType type_ = {};
};

class PixmapBrush : public SWSpanBrush {
 public:
  PixmapBrush(std::vector<Span> const& spans, Bitmap* bitmap,
              ColorFilter* color_filter, BlendMode mode, float global_alpha,
              std::shared_ptr<Pixmap> pixmap, const Matrix& points_to_unit,
              const SamplingOptions& sampling, TileMode x_tile_mode,
              TileMode y_tile_mode,
              const std::shared_ptr<const ColorSpace>& target_space = {});

  ~PixmapBrush() override = default;

 protected:
  // Hands the whole span list to the SIMD accelerator when the draw is one
  // of the supported configurations; falls back to the per-span legacy path
  // otherwise.
  void Brush() override;

  Color CalculateColor(int32_t x, int32_t y) override;

 private:
  std::unique_ptr<Bitmap> texture_;
  Matrix points_to_unit_ = {};
  TileMode x_tile_mode_ = TileMode::kClamp;
  TileMode y_tile_mode_ = TileMode::kClamp;
  BitmapSampler bitmap_sampler_;
#ifdef SKITY_SW_ACCEL
  // Precomputed accelerator params, filled by the constructor when the draw
  // matches the accelerated configurations (see sw_accel.hpp).
  sw_accel::ImageParams accel_params_ = {};
  bool accel_ok_ = false;
#endif
};

}  // namespace skity

#endif  // SRC_RENDER_SW_SW_SPAN_BRUSH_HPP
