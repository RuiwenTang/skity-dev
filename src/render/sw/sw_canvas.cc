// Copyright 2021 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

#include "src/render/sw/sw_canvas.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <skity/effect/mask_filter.hpp>
#include <skity/effect/path_effect.hpp>
#include <skity/geometry/stroke.hpp>
#include <skity/graphic/bitmap.hpp>
#include <skity/text/font.hpp>
#include <skity/text/text_blob.hpp>
#include <skity/text/text_run.hpp>

#ifdef SKITY_SW_ACCEL
#include "src/render/sw/accel/sw_accel.hpp"
#endif
#include "src/effect/color_filter_base.hpp"
#include "src/effect/gradient_fallback.hpp"
#include "src/effect/image_filter_base.hpp"
#include "src/effect/mask_filter_priv.hpp"
#include "src/effect/pixmap_shader.hpp"
#include "src/graphic/color_priv.hpp"
#include "src/graphic/color_transform.hpp"
#include "src/render/paint_order.hpp"
#include "src/render/sw/sw_glyph_atlas.hpp"
#include "src/render/sw/sw_raster.hpp"
#include "src/render/sw/sw_span_brush.hpp"
#include "src/render/sw/sw_stack_blur.hpp"
#include "src/render/text/glyph_position.hpp"
#include "src/text/scaler_context_cache.hpp"
#include "src/tracing.hpp"

namespace skity {
namespace {
constexpr ColorType ToColorType(BitmapFormat bitmap_format) {
  switch (bitmap_format) {
    case BitmapFormat::kBGRA8:
      return ColorType::kBGRA;
    case BitmapFormat::kRGBA8:
      return ColorType::kRGBA;
    case BitmapFormat::kGray8:
      return ColorType::kA8;
    case BitmapFormat::kUnknown:
      return ColorType::kUnknown;
  }
}

// A blend filter color recorded in a declared space resolves into the
// target (bitmap) space at draw time, mirroring the GPU uniform
// resolution; the CPU filter otherwise quantizes the declared bytes as
// sRGB (it has no target context at construction time).
std::shared_ptr<ColorFilter> ResolveColorFilterForTarget(
    const std::shared_ptr<ColorFilter>& filter,
    const std::shared_ptr<const ColorSpace>& target_space) {
  if (!filter) {
    return {};
  }
  auto* base = As_CFB(filter.get());
  if (base->GetType() != ColorFilterType::kBlend) {
    return filter;
  }
  auto* blend = static_cast<BlendColorFilter*>(base);
  const auto& declared = blend->GetColorSpace();
  if (ColorSpaceEquals(declared, target_space)) {
    return filter;
  }
  Color4f resolved = blend->GetColor4f();
  auto steps = ColorTransformSteps::Resolve(declared, target_space);
  if (!steps.IsNoop()) {
    resolved = steps.Apply(resolved);
  }
  return ColorFilters::Blend(resolved, blend->GetBlendMode(), nullptr);
}
}  // namespace

#ifdef SKITY_SW_ACCEL
// Fast path: constant-color fills (no shader / color filter) on premul
// RGBA8888 targets go straight to the SIMD kernels; falls back to the
// legacy brush when anything is unsupported.
static bool TryAccelerateSolidFill(std::vector<Span> const& spans,
                                   const Paint& paint, bool stroke,
                                   Bitmap* bitmap, const SWContext* context) {
  if (!context->AccelEnabled() || spans.empty() || paint.GetShader() ||
      paint.GetColorFilter() ||
      !sw_accel::SupportsSolidMode(paint.GetBlendMode()) ||
      bitmap->GetAlphaType() != AlphaType::kPremul_AlphaType ||
      bitmap->GetColorType() != ColorType::kRGBA) {
    return false;
  }
  // Resolve the fill/stroke color exactly like GenerateBrush's solid branch.
  const auto& bitmap_space = bitmap->GetPixmap()->GetColorSpace();
  Color4f color = stroke ? paint.GetStrokeColor(bitmap_space)
                         : paint.GetFillColor(bitmap_space);
  uint32_t src_pm = ColorToPMColor(Color4fToColor(color));
  return sw_accel::FillSolidSpans(
      paint.GetBlendMode(), src_pm, bitmap->GetPixelAddr(), bitmap->RowBytes(),
      static_cast<int32_t>(bitmap->Width()),
      static_cast<int32_t>(bitmap->Height()), spans.data(), spans.size());
}
#endif

static Rect ComputeBoundsIfStroke(Rect bounds, const Paint& paint) {
  if (paint.GetStyle() != Paint::kFill_Style) {
    float stroke_width = paint.GetStrokeWidth();
    bounds.SetLTRB(glm::floor(bounds.Left() - stroke_width),
                   glm::floor(bounds.Top() - stroke_width),
                   glm::floor(bounds.Right() + stroke_width),
                   glm::floor(bounds.Bottom() + stroke_width));
  }
  return bounds;
}

std::unique_ptr<Canvas> Canvas::MakeSoftwareCanvas(Bitmap* bitmap) {
  if (bitmap == nullptr) {
    return {};
  }

  if (bitmap->Width() == 0 || bitmap->Height() == 0) {
    return {};
  }

  return std::make_unique<SWCanvas>(bitmap);
}

void SWCanvas::LayerState::Init(SWCanvas* parent_canvas, Vec2 offset) {
  this->bitmap = std::make_unique<Bitmap>(
      static_cast<uint32_t>(std::ceil(rel_bounds.Width())),
      static_cast<uint32_t>(std::ceil(rel_bounds.Height())),
      AlphaType::kPremul_AlphaType);
  // Layers live in the target canvas' color space: content drawn into the
  // layer and the layer composited back must not gain conversions.
  this->bitmap->GetPixmap()->SetColorInfo(
      AlphaType::kPremul_AlphaType, ColorType::kRGBA,
      parent_canvas->bitmap_->GetPixmap()->GetColorSpace());

  this->canvas = parent_canvas->CreateSubCanvas(
      bitmap.get(), offset + Vec2{rel_bounds.Left(), rel_bounds.Top()});

  // The layer canvas renders through the full CTM, so layer pixels are
  // device-aligned like the GPU's layer texture: regular content lands at
  // the same device position it would without a layer, and SetMatrix
  // inside the layer addresses device coordinates. The composite blits
  // the layer back 1:1 without re-applying the CTM.
  this->canvas->Concat(parent_canvas->CurrentTransform());

  if (bitmap->GetPixelAddr() != nullptr) {
    std::memset(bitmap->GetPixelAddr(), 0,
                bitmap->RowBytes() * bitmap->Height());
  }
}

SWCanvas::SWCanvas(Bitmap* bitmap) : Canvas(), bitmap_(bitmap) {
  state_stack_.emplace_back(State());
}

void SWCanvas::OnDrawLine(float x0, float y0, float x1, float y1,
                          Paint const& paint) {
  SKITY_TRACE_EVENT(SWCanvas_OnDrawLine);

  Path path;
  path.MoveTo(x0, y0);
  path.LineTo(x1, y1);

  this->OnDrawPath(path, paint);
}

void SWCanvas::PushClip(RasterClip clip, ClipOp op) {
  const auto& current = state_stack_.back().clip;
  if (!current) {
    // The first clip op of the state. A leading kDifference subtracts from
    // the whole surface (the implicit base of the clip stack).
    if (op == ClipOp::kDifference) {
      clip =
          DifferenceClip(MakeBoundsClip(Rect::MakeWH(Width(), Height())), clip);
    }
    state_stack_.back().clip =
        std::make_shared<const RasterClip>(std::move(clip));
    return;
  }
  RasterClip resolved = op == ClipOp::kIntersect
                            ? IntersectClip(*current, clip)
                            : DifferenceClip(*current, clip);
  state_stack_.back().clip =
      std::make_shared<const RasterClip>(std::move(resolved));
}

void SWCanvas::OnClipRect(const Rect& rect, ClipOp op) {
  SKITY_TRACE_EVENT(SWCanvas_OnClipRect);

  if (PeekLayerStack()) {
    PeekLayerStack()->canvas->ClipRect(rect, op);
    return;
  }

  const Matrix ctm = CurrentTransform();
  Rect device_rect;
  ctm.MapRect(&device_rect, rect);

  if (op == ClipOp::kIntersect && ctm.OnlyScaleAndTranslate() &&
      device_rect.Left() == std::floor(device_rect.Left()) &&
      device_rect.Top() == std::floor(device_rect.Top()) &&
      device_rect.Right() == std::ceil(device_rect.Right()) &&
      device_rect.Bottom() == std::ceil(device_rect.Bottom())) {
    // Tier 0: an integer-aligned rect intersection is pure bounds; the
    // global clip bounds (updated by Canvas::ClipRect) already carry it.
    PushClip(MakeBoundsClip(device_rect), op);
    return;
  }

  // Tier 1: rasterize the mapped rect - antialiased edges for fractional
  // rects, exact subtractive coverage for kDifference, and correct coverage
  // under non-scale-and-translate transforms.
  Path path;
  path.AddRect(rect);
  ApplyClipPath(path, op);
}

void SWCanvas::ApplyClipPath(const Path& path, ClipOp op) {
  SWRaster raster;
  raster.RastePath(path, CurrentTransform(), GetScanClipBounds());

  PushClip(MakeRunsClip(raster.GetBounds(), raster.CurrentSpans()), op);
}

void SWCanvas::OnClipPath(const Path& path, ClipOp op) {
  SKITY_TRACE_EVENT(SWCanvas_OnClipPath);

  if (PeekLayerStack()) {
    PeekLayerStack()->canvas->ClipPath(path, op);
    return;
  }

  ApplyClipPath(path, op);
}

void SWCanvas::DoBrush(const SWRaster& raster, const Paint& paint,
                       bool stroke) {
  SKITY_TRACE_EVENT(SWCanvas_DoBrush);

  std::vector<Span> clipped;
  std::vector<Span> const* spans = nullptr;
  if (const auto& clip = state_stack_.back().clip) {
    // The resolved clip composes its antialiased coverage into the fill
    // spans (multiplying at clip edges); the brush sees final coverages.
    clipped = ClipSpans(*clip, raster.CurrentSpans());
    if (clipped.empty()) {
      return;
    }
    spans = &clipped;
  } else {
    spans = &raster.CurrentSpans();
  }

#ifdef SKITY_SW_ACCEL
  if (TryAccelerateSolidFill(*spans, paint, stroke, bitmap_, context_)) {
    return;
  }
#endif

  auto brush = GenerateBrush(*spans, paint, stroke, raster.GetBounds());
  brush->Brush();
}

void SWCanvas::OnDrawPath(const Path& path, const Paint& paint) {
  SKITY_TRACE_EVENT(SWCanvas_OnDrawPath);

  if (PeekLayerStack()) {
    PeekLayerStack()->canvas->DrawPath(path, paint);
    return;
  }

  if (paint.GetMaskFilter() || paint.GetImageFilter()) {
    // post processing
    HandleFilter(path, paint);
    return;
  }

  bool need_fill = paint.GetStyle() != Paint::kStroke_Style;
  bool need_stroke = paint.GetStyle() != Paint::kFill_Style;

  auto draw_fill = [&]() {
    SWRaster raster;

    Path temp;
    if (paint.GetPathEffect() &&
        paint.GetPathEffect()->FilterPath(&temp, path, false, paint)) {
      raster.RastePath(temp, CurrentTransform(), GetScanClipBounds());
    } else {
      raster.RastePath(path, CurrentTransform(), GetScanClipBounds());
    }

    DoBrush(raster, paint, false);
  };

  auto draw_stroke = [&]() {
    Stroke stroke(paint);

    Path temp;
    Path quad;
    Path outline;
    if (paint.GetPathEffect() &&
        paint.GetPathEffect()->FilterPath(&temp, path, true, paint)) {
      stroke.QuadPath(temp, &quad);
      stroke.StrokePath(quad, &outline);
    } else {
      stroke.QuadPath(path, &quad);
      stroke.StrokePath(quad, &outline);
    }

    SWRaster raster;
    raster.RastePath(outline, CurrentTransform(), GetScanClipBounds());

    DoBrush(raster, paint, true);
  };

  DrawFillStrokeInPaintOrder(paint.GetStyle(), need_fill, need_stroke,
                             draw_fill, draw_stroke);
}

void SWCanvas::OnDrawPaint(const Paint& paint) {
  SKITY_TRACE_EVENT(SWCanvas_OnDrawPaint);

  if (PeekLayerStack()) {
    PeekLayerStack()->canvas->DrawPaint(paint);
    return;
  }

  Path path;
  path.AddRect(Rect::MakeWH(Width(), Height()));

  SWRaster raster;
  raster.RastePath(path, Matrix{}, GetScanClipBounds());

  DoBrush(raster, paint, false);
}

void SWCanvas::OnSaveLayer(const Rect& bounds, const Paint& paint) {
  SKITY_TRACE_EVENT(SWCanvas_OnSaveLayer);

  state_stack_.emplace_back(state_stack_.back());
  if (PeekLayerStack()) {
    PeekLayerStack()->canvas->OnSave();
  }

  CurrentState()->has_layer = true;

  Paint work_paint{paint};
  work_paint.SetStyle(Paint::kFill_Style);
  auto layer_bounds = work_paint.ComputeFastBounds(bounds);

  SWCanvas* target_canvas = this;
  if (PeekLayerStack()) {
    target_canvas = static_cast<SWCanvas*>(PeekLayerStack()->canvas.get());
  }

  auto canvas_matrix = target_canvas->CurrentTransform();
  Rect device_bounds =
      Rect::MakeWH(target_canvas->Width(), target_canvas->Height());
  Rect scan_clip_bounds = target_canvas->GetScanClipBounds();
  if (!device_bounds.Intersect(scan_clip_bounds)) {
    device_bounds.SetEmpty();
  }

  Matrix device_to_local;
  if (canvas_matrix.Invert(&device_to_local)) {
    Rect local_clip_bounds;
    device_to_local.MapRect(&local_clip_bounds, device_bounds);
    if (!layer_bounds.Intersect(local_clip_bounds)) {
      layer_bounds.SetEmpty();
    }
  }

  Rect rel_bounds{};
  canvas_matrix.MapRect(&rel_bounds, layer_bounds);

  auto layer =
      GenerateLayer(rel_bounds, layer_bounds, target_canvas->global_offset_);

  if (layer == nullptr) {
    // Failed allocation: no layer to restore, keep drawing on the target.
    CurrentState()->has_layer = false;
    return;
  }

  layer->paint = paint;
}

void SWCanvas::OnDrawBlob(const TextBlob* blob, float x, float y,
                          Paint const& paint) {
  SKITY_TRACE_EVENT(SWCanvas_OnDrawBlob);

  if (PeekLayerStack()) {
    PeekLayerStack()->canvas->OnDrawBlob(blob, x, y, paint);
    return;
  }

  for (auto const& run : blob->GetTextRun()) {
    auto typeface = run.LockTypeface();
    auto const& glyphs = run.GetGlyphInfo();
    if (!typeface || glyphs.empty()) {
      continue;
    }

    Font font(typeface, run.GetFontSize());
    std::vector<float> pos_y{};
    std::vector<float> pos_x{};
    if (run.GetPosX().empty()) {
      // this this slow, but cpu raster is not performance sensitive
      // std::vector<GlyphData> glyphs(run.GetGlyphInfo().size());
      std::vector<const GlyphData*> glyph_data(glyphs.size());
      font.LoadGlyphMetrics(glyphs.data(), glyphs.size(), glyph_data.data(),
                            paint);

      for (auto const& glyph : glyph_data) {
        pos_x.emplace_back(x + glyph->GetHoriBearingX());
        pos_y.emplace_back(y);

        x += glyph->AdvanceX();
      }
    } else {
      pos_y.resize(run.GetPosX().size(), y);
      for (auto rx : run.GetPosX()) {
        pos_x.emplace_back(rx + x);
      }
    }

    this->DrawGlyphsInternal(static_cast<int32_t>(glyphs.size()), glyphs.data(),
                             pos_x.data(), pos_y.data(), font, paint);
  }
}

void SWCanvas::OnDrawGlyphs(uint32_t count, const GlyphID* glyphs,
                            const float* position_x, const float* position_y,
                            const Font& font, const Paint& paint) {
  if (PeekLayerStack()) {
    PeekLayerStack()->canvas->OnDrawGlyphs(count, glyphs, position_x,
                                           position_y, font, paint);
  } else {
    this->DrawGlyphsInternal(count, glyphs, position_x, position_y, font,
                             paint);
  }
}

void SWCanvas::DrawGlyphsInternal(uint32_t count, const GlyphID* glyphs,
                                  const float* position_x,
                                  const float* position_y, const Font& font,
                                  const Paint& paint) {
  SKITY_TRACE_EVENT(SWCanvas_DrawGlyphsInternal);

  bool need_fill = paint.GetStyle() != Paint::kStroke_Style;
  bool need_stroke = paint.GetStyle() != Paint::kFill_Style;

  if (paint.GetMaskFilter() || paint.GetImageFilter()) {
    HandleFilter(count, glyphs, position_x, position_y, font, paint);
    return;
  }

  bool draw_fill = need_fill || font.GetTypeface()->ContainsColorTable();
  auto fill_glyphs = [&]() {
    if (draw_fill) {
      FillGlyphs(count, glyphs, position_x, position_y, font, paint);
    }
  };
  auto stroke_glyphs = [&]() {
    if (need_stroke) {
      StrokeGlyphs(count, glyphs, position_x, position_y, font, paint);
    }
  };

  DrawFillStrokeInPaintOrder(paint.GetStyle(), draw_fill, need_stroke,
                             fill_glyphs, stroke_glyphs);
}

void SWCanvas::FillGlyphs(uint32_t count, const GlyphID* glyphs,
                          const float* position_x, const float* position_y,
                          const Font& font, const Paint& paint) {
  if (font.GetTypeface()->ContainsColorTable()) {
    // Color glyphs render from their own RGBA images, unchanged.
    std::vector<const GlyphData*> glyphs_data(count);
    font.LoadGlyphPath(glyphs, count, glyphs_data.data());
    for (uint32_t k = 0; k < count; k++) {
      float x = position_x[k] + glyphs_data[k]->GetHoriBearingX();
      float y = position_y[k] - glyphs_data[k]->GetHoriBearingY();

      float h = glyphs_data[k]->GetHeight();
      float w = glyphs_data[k]->GetWidth();

      const GlyphID id = glyphs_data[k]->Id();
      const auto& transform = CurrentTransform();
      const Matrix22 transform22{transform.GetScaleX(), transform.GetSkewX(),
                                 transform.GetSkewY(), transform.GetScaleY()};
      const auto desc =
          ScalerContextDesc::MakeTransformed(font, Paint(), 1.f, transform22);
      auto context = ScalerContextCache::GlobalScalerContextCache()
                         ->FindOrCreateScalerContext(desc, font.GetTypeface());
      auto glyph_image = context->PrepareOwnedImage(PackedGlyphID(id), Paint());
      const auto& glyph_bitmap = glyph_image.Image();
      if (!glyph_bitmap.buffer) {
        continue;
      }

      const size_t row_bytes = glyph_bitmap.RowBytes();

      auto pixmap = std::make_shared<Pixmap>(
          skity::Data::MakeWithCopy(
              glyph_bitmap.buffer,
              row_bytes * static_cast<size_t>(glyph_bitmap.height)),
          row_bytes, glyph_bitmap.width, glyph_bitmap.height,
          AlphaType::kOpaque_AlphaType, ToColorType(glyph_bitmap.format));

      this->DrawImage(Image::MakeImage(pixmap),
                      skity::Rect::MakeXYWH(x, y, w, h), SamplingOptions{},
                      nullptr);
    }
    return;
  }

  // Run-level eligibility for the A8 glyph mask path: solid-color fills of
  // outline fonts on premul RGBA targets. Shader, color filter and stroke
  // text keep the path pipeline. Clips of every tier are supported: bounds
  // through the blit window, run-encoded clips through per-row staging.
  const bool mask_eligible =
      paint.GetShader() == nullptr && paint.GetColorFilter() == nullptr &&
      bitmap_->GetAlphaType() == AlphaType::kPremul_AlphaType &&
      bitmap_->GetColorType() == ColorType::kRGBA;

  const Matrix ctm = CurrentTransform();
  const Matrix22 transform22{ctm.GetScaleX(), ctm.GetSkewX(), ctm.GetSkewY(),
                             ctm.GetScaleY()};
  GlyphPositionRoundingSpec rounding_spec;
  rounding_spec.is_subpixel = font.IsSubpixel();
  rounding_spec.axis_alignment =
      ComputeAxisAlignmentForHorizontalText(font.IsBaselineSnap(), ctm);
  const ScalerContextDesc desc =
      mask_eligible
          ? ScalerContextDesc::MakeTransformed(font, paint, 1.f, transform22)
          : ScalerContextDesc{};
  const auto& bitmap_space = bitmap_->GetPixmap()->GetColorSpace();
  const PMColor color_pm =
      ColorToPMColor(Color4fToColor(paint.GetFillColor(bitmap_space)));
  const uint8_t global_alpha = static_cast<uint8_t>(255 * paint.GetAlphaF());
  const Rect clip = GetScanClipBounds();
  // The rasterizer's clip window: floor on the left/top, ceil on the
  // right/bottom (SWRaster::RastePath floors and ceils the intersected
  // scan bounds the same way).
  const int32_t clip_left = static_cast<int32_t>(std::floor(clip.Left()));
  const int32_t clip_top = static_cast<int32_t>(std::floor(clip.Top()));
  const int32_t clip_right = static_cast<int32_t>(std::ceil(clip.Right()));
  const int32_t clip_bottom = static_cast<int32_t>(std::ceil(clip.Bottom()));
  const int32_t surface_w = static_cast<int32_t>(bitmap_->Width());
  const int32_t surface_h = static_cast<int32_t>(bitmap_->Height());

  std::vector<uint32_t> path_glyphs;
  for (uint32_t k = 0; k < count; k++) {
    if (mask_eligible) {
      const Vec2 pen{position_x[k] * ctm.GetScaleX() +
                         position_y[k] * ctm.GetSkewX() + ctm.GetTranslateX(),
                     position_x[k] * ctm.GetSkewY() +
                         position_y[k] * ctm.GetScaleY() + ctm.GetTranslateY()};
      const auto position = QuantizeGlyphPosition(pen, 1.f, rounding_spec);
      const SWGlyphMask mask = SWGlyphAtlas::GetInstance()->GetGlyphMask(
          font, paint, desc,
          PackedGlyphID(glyphs[k], position.x_phase, position.y_phase));
      if (mask.IsReady()) {
        // The mask origin has the packed phase canceled inside, so the blit
        // lands on the integer physical-pixel grid (position + origin is
        // exact in float).
        const int32_t glyph_left = static_cast<int32_t>(
            std::lround(position.position.x + mask.origin_x));
        const int32_t glyph_top = static_cast<int32_t>(
            std::lround(position.position.y - mask.origin_y));
        int32_t x0 = std::max(glyph_left, clip_left);
        int32_t y0 = std::max(glyph_top, clip_top);
        int32_t x1 =
            std::min(glyph_left + static_cast<int32_t>(mask.width), clip_right);
        int32_t y1 = std::min(glyph_top + static_cast<int32_t>(mask.height),
                              clip_bottom);
        x0 = std::max(x0, 0);
        y0 = std::max(y0, 0);
        x1 = std::min(x1, surface_w);
        y1 = std::min(y1, surface_h);
        if (x0 < x1 && y0 < y1) {
          const uint8_t* mask_ptr =
              mask.mask + static_cast<size_t>(y0 - glyph_top) * mask.stride +
              static_cast<size_t>(x0 - glyph_left);
          const auto& clip = state_stack_.back().clip;
          if (clip && clip->HasRuns()) {
            // The clip coverage varies per pixel: compose it into the glyph
            // mask row by row (one staged clip row at a time) and blend the
            // combined coverage.
            std::vector<uint8_t> combined(static_cast<size_t>(x1 - x0));
            for (int32_t row = y0; row < y1; row++) {
              StageClipRow(*clip, row, x0, x1, combined.data());
              const uint8_t* glyph_row =
                  mask_ptr + static_cast<size_t>(row - y0) * mask.stride;
              for (int32_t i = 0; i < x1 - x0; i++) {
                combined[i] = static_cast<uint8_t>(
                    MulDiv255Round(glyph_row[i], combined[i]));
              }
#ifdef SKITY_SW_ACCEL
              bool blended = false;
              if (context_->AccelEnabled()) {
                blended = sw_accel::BlendMaskRect(
                    paint.GetBlendMode(), color_pm, global_alpha,
                    bitmap_->GetPixelAddr(), bitmap_->RowBytes(), surface_w,
                    surface_h, x0, row, combined.data(), x1 - x0, x1 - x0, 1);
              }
              if (!blended)
#endif
              {
                BlendGlyphMaskRect(
                    combined.data(), x1 - x0, x0, row, x1 - x0, 1,
                    bitmap_->GetPixelAddr(), bitmap_->RowBytes(), surface_w,
                    surface_h, color_pm, global_alpha, paint.GetBlendMode());
              }
            }
          } else {
#ifdef SKITY_SW_ACCEL
            bool blended = false;
            if (context_->AccelEnabled()) {
              blended = sw_accel::BlendMaskRect(
                  paint.GetBlendMode(), color_pm, global_alpha,
                  bitmap_->GetPixelAddr(), bitmap_->RowBytes(), surface_w,
                  surface_h, x0, y0, mask_ptr, mask.stride, x1 - x0, y1 - y0);
            }
            if (!blended)
#endif
            {
              BlendGlyphMaskRect(mask_ptr, mask.stride, x0, y0, x1 - x0,
                                 y1 - y0, bitmap_->GetPixelAddr(),
                                 bitmap_->RowBytes(), surface_w, surface_h,
                                 color_pm, global_alpha, paint.GetBlendMode());
            }
          }
        }
        continue;
      }
      if (mask.status == AtlasGlyphStatus::kNoImage) {
        continue;  // inkless glyph (space)
      }
      // kTooLarge / kInvalidImage: fall through to the path pipeline.
    }

    path_glyphs.push_back(k);
  }

  if (path_glyphs.empty()) {
    return;
  }
  // Paths are only materialized for the glyphs the mask path rejected.
  std::vector<GlyphID> path_ids;
  path_ids.reserve(path_glyphs.size());
  for (uint32_t k : path_glyphs) {
    path_ids.push_back(glyphs[k]);
  }
  std::vector<const GlyphData*> glyphs_data(path_glyphs.size());
  font.LoadGlyphPath(path_ids.data(), path_ids.size(), glyphs_data.data());
  for (size_t i = 0; i < path_glyphs.size(); i++) {
    const uint32_t k = path_glyphs[i];
    const auto& path = glyphs_data[i]->GetPath();
    auto transform = Matrix::Translate(position_x[k], position_y[k]);

    SWRaster raster;

    raster.RastePath(path, CurrentTransform() * transform, GetScanClipBounds());

    DoBrush(raster, paint, false);
  }
}

void SWCanvas::StrokeGlyphs(uint32_t count, const GlyphID* glyphs,
                            const float* position_x, const float* position_y,
                            const Font& font, const Paint& paint) {
  std::vector<const GlyphData*> glyphs_data(count);
  font.LoadGlyphPath(glyphs, count, glyphs_data.data());
  for (uint32_t k = 0; k < count; k++) {
    auto& path = glyphs_data[k]->GetPath();
    auto transform = Matrix::Translate(position_x[k], position_y[k]);

    Stroke stroke(paint);
    Path quad;
    Path outline;
    stroke.QuadPath(path, &quad);
    stroke.StrokePath(quad, &outline);

    SWRaster raster;

    raster.RastePath(outline, CurrentTransform() * transform,
                     GetScanClipBounds());

    DoBrush(raster, paint, true);
  }
}

void SWCanvas::OnDrawImageRect(std::shared_ptr<Image> image, const Rect& src,
                               const Rect& dst, const SamplingOptions& sampling,
                               Paint const* paint) {
  SKITY_TRACE_EVENT(SWCanvas_OnDrawImageRect);

  if (!image) {
    return;
  }

  if (src.Width() == 0 || src.Height() == 0 || dst.Width() == 0 ||
      dst.Height() == 0) {
    return;
  }

  Paint work_paint = (paint == nullptr) ? Paint() : *paint;
  work_paint.SetStyle(Paint::kFill_Style);
  Matrix local_matrix;

  if (IsDrawingLayer()) {
    local_matrix = Matrix::Scale(1.f / src.Width(), 1.f / src.Height()) *
                   Matrix::Translate(-src.Left(), -src.Top());
  } else {
    local_matrix =
        Matrix::Translate(dst.Left(), dst.Top()) *
        Matrix::Scale(dst.Width() / src.Width(), dst.Height() / src.Height()) *
        Matrix::Translate(-src.Left(), -src.Top());
  }

  auto shader = Shader::MakeShader(std::move(image), sampling, TileMode::kDecal,
                                   TileMode::kDecal, local_matrix);
  work_paint.SetShader(std::move(shader));

  Path path;
  path.AddRect(dst);

  this->OnDrawPath(path, work_paint);
}

void SWCanvas::OnSave() {
  if (PeekLayerStack()) {
    PeekLayerStack()->canvas->Save();
    return;
  }

  state_stack_.emplace_back(state_stack_.back());
}

void SWCanvas::OnRestore() {
  if (state_stack_.size() == 1) {
    return;
  }

  if (PeekLayerStack()) {
    auto sub_canvas = static_cast<SWCanvas*>(PeekLayerStack()->canvas.get());

    if (sub_canvas->state_stack_.size() > 1) {
      sub_canvas->Restore();

      return;
    }
  }

  if (state_stack_.back().has_layer) {
    OnLayerRestore();
  }

  state_stack_.pop_back();
}

void SWCanvas::OnRestoreToCount(int saveCount) {
  if (saveCount < 1) {
    // invalid count
    return;
  }

  while (state_stack_.size() > static_cast<size_t>(saveCount)) {
    this->OnRestore();
  }
}

void SWCanvas::OnFlush() {}

uint32_t SWCanvas::OnGetWidth() const { return bitmap_->Width(); }

uint32_t SWCanvas::OnGetHeight() const { return bitmap_->Height(); }

std::unique_ptr<SWSpanBrush> SWCanvas::GenerateBrush(
    std::vector<Span> const& spans, skity::Paint const& paint, bool stroke,
    Rect const& bounds) {
  // Brushes keep a raw ColorFilter*; the resolved filter outlives the call
  // through this member (brushes are consumed synchronously).
  resolved_color_filter_ = ResolveColorFilterForTarget(
      paint.GetColorFilter(), bitmap_->GetPixmap()->GetColorSpace());
  auto color_filter = resolved_color_filter_;
  auto shader = paint.GetShader();
  if (shader) {
    const auto* image_ptr = paint.GetShader()->AsImage();
    Shader::GradientInfo info{};
    Shader::GradientType type = shader->AsGradient(&info);

    // Gradient stops resolve from their declared space (all stops share
    // one, default sRGB) into the target (bitmap) color space, so
    // interpolation happens in the space the surface encodes.
    {
      ColorTransformSteps stop_steps = ColorTransformSteps::Resolve(
          info.color_space, bitmap_->GetPixmap()->GetColorSpace());
      if (!stop_steps.IsNoop()) {
        for (auto& stop : info.colors) {
          stop = stop_steps.Apply(stop);
        }
      }
    }

    // A non-zero interpolation flag premultiplies the resolved stops before
    // interpolation (the GPU uploads premultiplied stops for that flag);
    // the brushes then skip the trailing premultiply so both domains land
    // on canonical premul outputs.
    if (info.gradientFlags != 0) {
      for (auto& stop : info.colors) {
        stop.r *= stop.a;
        stop.g *= stop.a;
        stop.b *= stop.a;
      }
    }

    if (type == Shader::kLinear || type == Shader::kRadial ||
        type == Shader::kConical || type == Shader::kSweep) {
      Color4f fallback_color;
      if (NeedsFallbackToSolidColor(type, info, fallback_color)) {
        fallback_color.a *= paint.GetAlphaF();
        return std::make_unique<SolidColorBrush>(
            spans, bitmap_, color_filter.get(), paint.GetBlendMode(),
            fallback_color);
      }
      Matrix device_to_local;
      shader->GetLocalMatrix().Invert(&device_to_local);

      if (IsDrawingLayer()) {
        device_to_local =
            device_to_local *
            Matrix::Scale(1.0f / bounds.Width(), 1.0f / bounds.Height()) *
            Matrix::Translate(-bounds.Left(), -bounds.Top());
      } else {
        Matrix layer_to_local;
        CurrentTransform().Invert(&layer_to_local);
        device_to_local = device_to_local * layer_to_local;
      }

      return GradientColorBrush::MakeGradientColorBrush(
          spans, bitmap_, color_filter.get(), paint.GetBlendMode(), info, type,
          device_to_local);
    } else if (image_ptr) {
      const auto& image = *image_ptr;
      assert(image->GetPixmap());
      auto pixmap_shader =
          std::static_pointer_cast<PixmapShader>(paint.GetShader());

      auto pixmap = *(image->GetPixmap());

      uint32_t img_width = pixmap->Width();
      uint32_t img_height = pixmap->Height();

      Matrix inverse;
      shader->GetLocalMatrix().Invert(&inverse);

      Matrix matrix =
          Matrix::Scale(1.f / img_width, 1.f / img_height) * inverse;

      if (IsDrawingLayer()) {
        matrix = matrix *
                 Matrix::Scale(1.0f / bounds.Width(), 1.0f / bounds.Height()) *
                 Matrix::Translate(-bounds.Left(), -bounds.Top());
      } else {
        Matrix layer_to_local;
        CurrentTransform().Invert(&layer_to_local);
        matrix = matrix * layer_to_local;
      }

      return std::make_unique<PixmapBrush>(
          spans, bitmap_, color_filter.get(), paint.GetBlendMode(),
          paint.GetAlphaF(), std::move(pixmap), matrix,
          *pixmap_shader->GetSamplingOptions(), pixmap_shader->GetXTileMode(),
          pixmap_shader->GetYTileMode(), bitmap_->GetPixmap()->GetColorSpace());
    }
  }

  // Recorded paint colors resolve into the target (bitmap) color space at
  // draw time, mirroring the GPU geometry uniform resolution. The per-slot
  // getters resolve each color with its OWN recorded space: a stroke pass of
  // a stroke-and-fill paint must not resolve through the fill color's space.
  const auto& bitmap_space = bitmap_->GetPixmap()->GetColorSpace();
  Color4f color = stroke ? paint.GetStrokeColor(bitmap_space)
                         : paint.GetFillColor(bitmap_space);

  return std::make_unique<SolidColorBrush>(spans, bitmap_, color_filter.get(),
                                           paint.GetBlendMode(), color);
}

void SWCanvas::HandleFilter(Path const& path, Paint const& paint) {
  Paint work_paint = paint;
  work_paint.SetMaskFilter(nullptr);
  work_paint.SetImageFilter(nullptr);

  auto mask_filter = paint.GetMaskFilter();
  auto image_filter = As_IFB(paint.GetImageFilter().get());
  auto bounds = ComputeBoundsIfStroke(path.GetBounds(), paint);

  Rect filter_bounds;
  if (mask_filter) {
    filter_bounds = ImageFilterBase::ApproximateFilteredBounds(
        bounds, mask_filter->GetBlurRadius(), mask_filter->GetBlurRadius());
  } else {
    // The image filter's own fast bounds: a matrix transform or a shadow
    // offset grows the layer beyond the blur radii.
    filter_bounds = ImageFilterBase::ApproximateFilteredBounds(
        image_filter->ComputeFastBounds(bounds), 0, 0);
  }

  // The layer covers the unexpanded content rect: filters grow their own
  // output (blur/drop-shadow pads the halo, matrix maps the rect), and it
  // lives in the target canvas' color space so filters that resolve
  // declared colors (drop shadow) land in the right space.
  Bitmap bitmap(bounds.Width(), bounds.Height(), AlphaType::kPremul_AlphaType);
  bitmap.GetPixmap()->SetColorInfo(AlphaType::kPremul_AlphaType,
                                   ColorType::kRGBA,
                                   bitmap_->GetPixmap()->GetColorSpace());

  auto temp_canvas = Canvas::MakeSoftwareCanvas(&bitmap);
  temp_canvas->Translate(-bounds.Left(), -bounds.Top());
  temp_canvas->DrawPath(path, work_paint);

  if (mask_filter) {
    MaskFilterOnFilter(this, bitmap, filter_bounds, work_paint,
                       mask_filter.get());
  } else {
    image_filter->OnFilter(this, bitmap, filter_bounds, bounds, work_paint);
  }
}

void SWCanvas::HandleFilter(uint32_t count, const GlyphID* glyphs,
                            const float* position_x, const float* position_y,
                            const Font& font, const Paint& paint) {
  Paint work_paint = paint;
  work_paint.SetMaskFilter(nullptr);
  work_paint.SetImageFilter(nullptr);

  auto mask_filter = paint.GetMaskFilter();
  auto image_filter = As_IFB(paint.GetImageFilter().get());
  auto bounds = TextBlob::ComputeBounds(count, glyphs, position_x, position_y,
                                        font, paint);
  Rect filter_bounds;
  if (mask_filter) {
    filter_bounds = ImageFilterBase::ApproximateFilteredBounds(
        bounds, mask_filter->GetBlurRadius(), mask_filter->GetBlurRadius());
  } else {
    filter_bounds = ImageFilterBase::ApproximateFilteredBounds(
        image_filter->ComputeFastBounds(bounds), 0, 0);
  }

  Bitmap bitmap(bounds.Width(), bounds.Height(), AlphaType::kPremul_AlphaType);
  bitmap.GetPixmap()->SetColorInfo(AlphaType::kPremul_AlphaType,
                                   ColorType::kRGBA,
                                   bitmap_->GetPixmap()->GetColorSpace());

  auto temp_canvas = Canvas::MakeSoftwareCanvas(&bitmap);
  temp_canvas->Translate(-bounds.Left(), -bounds.Top());
  static_cast<SWCanvas*>(temp_canvas.get())
      ->DrawGlyphsInternal(count, glyphs, position_x, position_y, font,
                           work_paint);

  if (mask_filter) {
    ImageFilterBase::BlurBitmapToCanvas(this, bitmap, filter_bounds, work_paint,
                                        mask_filter->GetBlurRadius(),
                                        mask_filter->GetBlurRadius());
  } else {
    image_filter->OnFilter(this, bitmap, filter_bounds, bounds, work_paint);
  }
}

SWCanvas::LayerState* SWCanvas::PeekLayerStack() {
  if (layer_stack_.empty()) {
    return nullptr;
  }

  return layer_stack_.back().get();
}

std::unique_ptr<SWCanvas::LayerState> SWCanvas::PopLayerStack() {
  auto layer = std::move(layer_stack_.back());

  layer_stack_.pop_back();

  return layer;
}

SWCanvas::LayerState* SWCanvas::GenerateLayer(Rect const& rel_bounds,
                                              Rect const& log_bounds,
                                              Vec2 const& offset) {
  layer_stack_.emplace_back(
      std::make_unique<LayerState>(rel_bounds, log_bounds));

  layer_stack_.back()->Init(this, offset);

  if (layer_stack_.back()->bitmap->GetPixelAddr() == nullptr) {
    // The layer bitmap allocation failed (degenerate huge bounds): drop
    // the layer and let the content draw straight onto the current
    // target - the GPU's failed-layer path, which also keeps SetMatrix
    // device-absolute.
    layer_stack_.pop_back();
    return nullptr;
  }

  return layer_stack_.back().get();
}

void SWCanvas::OnLayerRestore() {
  auto layer = PopLayerStack();

  if (PeekLayerStack()) {
    PeekLayerStack()->canvas->OnRestore();
  }

  // The layer bitmap is device-aligned; blit it back 1:1 (the content
  // already carries the CTM it was recorded with). SetMatrix instead of
  // Save/Restore - a saved state carries has_layer and its restore would
  // recurse into OnLayerRestore - and draw outside the drawing-layer flag:
  // its shader fast path drops the destination translate, which only
  // matched the old layer-local composite rect.
  {
    Matrix ctm = CurrentTransform();
    SetMatrix(Matrix{});
    this->DrawImage(Image::MakeImage(layer->bitmap->GetPixmap()),
                    layer->rel_bounds, &layer->paint);
    SetMatrix(ctm);
  }
  SetDrawingLayer(false);
}

std::unique_ptr<SWCanvas> SWCanvas::CreateSubCanvas(Bitmap* bitmap,
                                                    const Vec2& global_offset) {
  auto sub_canvas = std::make_unique<SWCanvas>(bitmap);
  sub_canvas->SetTracingCanvasState(false);
  sub_canvas->parent_canvas_ = this;
  sub_canvas->global_offset_ = global_offset;

  return sub_canvas;
}

}  // namespace skity
