/*
 * Copyright 2012 The Android Open Source Project
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

// Copyright 2021 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

#include <skity/effect/color_filter.hpp>
#include <skity/effect/mask_filter.hpp>
#include <skity/geometry/matrix.hpp>
#include <skity/graphic/bitmap.hpp>
#include <skity/graphic/paint.hpp>
#include <skity/graphic/path.hpp>
#include <skity/graphic/tile_mode.hpp>
#include <skity/render/canvas.hpp>

#include "src/effect/color_filter_base.hpp"
#include "src/effect/image_filter_base.hpp"

#if defined(SKITY_CPU)
#include <cstring>

#include "src/graphic/color_priv.hpp"
#include "src/graphic/color_transform.hpp"
#include "src/render/sw/sw_stack_blur.hpp"
#endif

namespace skity {

// static
Rect ImageFilterBase::ApproximateFilteredBounds(const Rect& src, float radius_x,
                                                float radius_y) {
  float l = std::floor(src.Left() - radius_x);
  float t = std::floor(src.Top() - radius_y);
  float r = std::ceil(src.Right() + radius_x);
  float b = std::ceil(src.Bottom() + radius_y);

  return Rect::MakeLTRB(l, t, r, b);
}

std::shared_ptr<ImageFilter> ImageFilters::Blur(float sigma_x, float sigma_y) {
  // Only a negative sigma is invalid; sigma == 0 is legal and means no blur on
  // that axis (the filter degenerates to a pass-through).
  if (sigma_x < 0.f || sigma_y < 0.f) {
    return nullptr;
  }
  return std::make_shared<BlurImageFilter>(sigma_x, sigma_y);
}

std::shared_ptr<ImageFilter> ImageFilters::DropShadow(
    float dx, float dy, float sigma_x, float sigma_y, Color color,
    std::shared_ptr<ImageFilter> input, const Rect& crop_rect) {
  // sigma == 0 only skips the blur pass; offset, tinting and the final
  // composite must still run, so it must not be rejected here.
  if (sigma_x < 0.f || sigma_y < 0.f) {
    return nullptr;
  }
  return std::make_shared<DropShadowImageFilter>(dx, dy, sigma_x, sigma_y,
                                                 color, input, crop_rect);
}

std::shared_ptr<ImageFilter> ImageFilters::DropShadow(
    float dx, float dy, float sigma_x, float sigma_y, Color4f color,
    std::shared_ptr<ImageFilter> input, const DropShadowOptions& options) {
  if (sigma_x < 0.f || sigma_y < 0.f) {
    return nullptr;
  }
  return std::make_shared<DropShadowImageFilter>(dx, dy, sigma_x, sigma_y,
                                                 color, options.color_space,
                                                 input, options.crop_rect);
}

DropShadowImageFilter::DropShadowImageFilter(float dx, float dy, float sigma_x,
                                             float sigma_y, Color color,
                                             std::shared_ptr<ImageFilter> input,
                                             const Rect& crop_rect)
    : DropShadowImageFilter(dx, dy, sigma_x, sigma_y, Color4fFromColor(color),
                            nullptr, std::move(input), crop_rect) {}

DropShadowImageFilter::DropShadowImageFilter(
    float dx, float dy, float sigma_x, float sigma_y, Color4f color,
    std::shared_ptr<const ColorSpace> color_space,
    std::shared_ptr<ImageFilter> input, const Rect& crop_rect)
    : ImageFilterBase({input.get()}),
      dx_(dx),
      dy_(dy),
      radius_x_(ConvertSigmaToRadius(sigma_x)),
      radius_y_(ConvertSigmaToRadius(sigma_y)),
      color_(color),
      color_space_(std::move(color_space)),
      crop_rect_(crop_rect) {}

std::shared_ptr<ImageFilter> ImageFilters::Dilate(float radius_x,
                                                  float radius_y) {
  if (radius_x < 0 || radius_y < 0) {
    return nullptr;
  }
  return std::make_shared<MorphologyImageFilter>(ImageFilterType::kDilate,
                                                 radius_x, radius_y);
}

std::shared_ptr<ImageFilter> ImageFilters::Erode(float radius_x,
                                                 float radius_y) {
  if (radius_x < 0 || radius_y < 0) {
    return nullptr;
  }
  return std::make_shared<MorphologyImageFilter>(ImageFilterType::kErode,
                                                 radius_x, radius_y);
}

std::shared_ptr<ImageFilter> ImageFilters::MatrixTransform(
    const Matrix& matrix) {
  return std::make_shared<MatrixImageFilter>(matrix);
}

std::shared_ptr<ImageFilter> ImageFilters::ColorFilter(
    std::shared_ptr<class ColorFilter> color_filter) {
  return std::make_shared<ColorFilterImageFilter>(color_filter);
}

std::shared_ptr<ImageFilter> ImageFilters::Compose(
    std::shared_ptr<ImageFilter> outer, std::shared_ptr<ImageFilter> inner) {
  return std::make_shared<ComposeImageFilter>(outer, inner);
}

std::shared_ptr<ImageFilter> ImageFilters::LocalMatrix(
    std::shared_ptr<ImageFilter> image_filter, const Matrix& local_matrix) {
  if (!image_filter) {
    return nullptr;
  }

  if (local_matrix.IsIdentity()) {
    return image_filter;
  }

  ImageFilterBase* image_filter_base =
      static_cast<ImageFilterBase*>(image_filter.get());
  switch (image_filter_base->GetType()) {
    case ImageFilterType::kIdentity:
      return image_filter;
    case ImageFilterType::kBlur: {
      if (local_matrix.OnlyScaleAndTranslate()) {
        BlurImageFilter* blur_image_filter =
            static_cast<BlurImageFilter*>(image_filter_base);
        float radius_x =
            local_matrix.GetScaleX() * blur_image_filter->GetRadiusX();
        float radius_y =
            local_matrix.GetScaleX() * blur_image_filter->GetRadiusY();
        return ImageFilters::Blur(ConvertRadiusToSigma(radius_x),
                                  ConvertRadiusToSigma(radius_y));
      }
      return nullptr;
    }
    case ImageFilterType::kDropShadow: {
      if (local_matrix.OnlyScaleAndTranslate()) {
        DropShadowImageFilter* drop_shadow_image_filter =
            static_cast<DropShadowImageFilter*>(image_filter_base);
        float scale_x = local_matrix.GetScaleX();
        float scale_y = local_matrix.GetScaleY();
        float dx = scale_x * drop_shadow_image_filter->GetOffsetX();
        float dy = scale_y * drop_shadow_image_filter->GetOffsetY();
        float radius_x = scale_x * drop_shadow_image_filter->GetRadiusX();
        float radius_y = scale_y * drop_shadow_image_filter->GetRadiusY();
        return ImageFilters::DropShadow(
            dx, dy, ConvertRadiusToSigma(radius_x),
            ConvertRadiusToSigma(radius_y),
            drop_shadow_image_filter->GetColor4f(), nullptr,
            DropShadowOptions{drop_shadow_image_filter->GetColorSpace()});
      }
      return nullptr;
    }
    case ImageFilterType::kColorFilter:
      return image_filter;
    case ImageFilterType::kMatix: {
      MatrixImageFilter* matrix_image_filter =
          static_cast<MatrixImageFilter*>(image_filter_base);
      Matrix inv_local_matrx;
      if (local_matrix.Invert(&inv_local_matrx)) {
        Matrix matrix =
            local_matrix * matrix_image_filter->GetMatrix() * inv_local_matrx;
        return ImageFilters::MatrixTransform(matrix);
      }
      return nullptr;
    }
    case ImageFilterType::kCompose: {
      ComposeImageFilter* compose_image_filter =
          static_cast<ComposeImageFilter*>(image_filter_base);
      if (!compose_image_filter->GetInner()) {
        return ImageFilters::LocalMatrix(compose_image_filter->GetOuter(),
                                         local_matrix);
      }
      if (!compose_image_filter->GetOuter()) {
        return ImageFilters::LocalMatrix(compose_image_filter->GetInner(),
                                         local_matrix);
      }

      auto outer = ImageFilters::LocalMatrix(compose_image_filter->GetOuter(),
                                             local_matrix);
      auto inner = ImageFilters::LocalMatrix(compose_image_filter->GetInner(),
                                             local_matrix);

      if (outer && inner) {
        return ImageFilters::Compose(outer, inner);
      }
      return nullptr;
    }
    case ImageFilterType::kDilate:
    case ImageFilterType::kErode:
      return nullptr;
  }
  return nullptr;
}

#if defined(SKITY_CPU)

namespace {

// CPU execution of one image-filter node over a layer bitmap - the shared
// recursive core behind the nested filter types (matrix / color filter /
// compose) and their blur / drop-shadow leaves. `bounds` is the device rect
// the src pixmap covers; out_bounds returns the device rect the filtered
// pixmap covers (a matrix transform or a shadow offset grows it). Interior
// compositing runs with a default paint, mirroring the GPU filter graph
// where inner nodes produce textures and only the final blit carries the
// paint's blend and alpha. Bitmaps are move-disabled, so the result travels
// as its shared Pixmap.
std::shared_ptr<Pixmap> MakeLayerPixmap(
    uint32_t width, uint32_t height,
    const std::shared_ptr<const ColorSpace>& space) {
  Bitmap bitmap(width, height, AlphaType::kPremul_AlphaType);
  bitmap.GetPixmap()->SetColorInfo(AlphaType::kPremul_AlphaType,
                                   ColorType::kRGBA, space);
  return bitmap.GetPixmap();
}

std::shared_ptr<Pixmap> ApplyFilterNode(const ImageFilterBase* filter,
                                        const std::shared_ptr<Pixmap>& src,
                                        const Rect& bounds, Rect& out_bounds);

std::shared_ptr<Pixmap> ApplyDropShadowNode(const DropShadowImageFilter* filter,
                                            const std::shared_ptr<Pixmap>& src,
                                            const Rect& bounds,
                                            Rect& out_bounds) {
  Rect shadow_bounds = bounds;
  shadow_bounds.Offset(filter->GetOffsetX(), filter->GetOffsetY());
  out_bounds = bounds;
  out_bounds.Join(shadow_bounds);
  const int32_t radius_x =
      static_cast<int32_t>(std::lround(std::min(filter->GetRadiusX(), 254.f)));
  const int32_t radius_y =
      static_cast<int32_t>(std::lround(std::min(filter->GetRadiusY(), 254.f)));
  const float pad = static_cast<float>(std::max(radius_x, radius_y));
  // The shadow mask blurs out of a padded layer (SWStackBlur cannot grow
  // its support); the padded rect also spans the halo in the output.
  Rect padded =
      ImageFilterBase::ApproximateFilteredBounds(shadow_bounds, pad, pad);
  out_bounds.Join(padded);
  out_bounds = ImageFilterBase::ApproximateFilteredBounds(out_bounds, 0, 0);

  auto dst = MakeLayerPixmap(static_cast<uint32_t>(out_bounds.Width()),
                             static_cast<uint32_t>(out_bounds.Height()),
                             src->GetColorSpace());
  Bitmap dst_bitmap(dst, false);

  auto padded_pm = MakeLayerPixmap(static_cast<uint32_t>(padded.Width()),
                                   static_cast<uint32_t>(padded.Height()),
                                   src->GetColorSpace());
  Bitmap padded_bitmap(padded_pm, false);
  auto pad_canvas = Canvas::MakeSoftwareCanvas(&padded_bitmap);
  pad_canvas->Translate(-padded.Left(), -padded.Top());
  pad_canvas->DrawImage(Image::MakeImage(src), shadow_bounds, nullptr);

  auto shadow = MakeLayerPixmap(padded_bitmap.Width(), padded_bitmap.Height(),
                                src->GetColorSpace());
  Bitmap shadow_bitmap(shadow, false);
  SWStackBlur(&padded_bitmap, &shadow_bitmap, radius_x, radius_y).Blur();

  // Tint with the declared color resolved into the layer space (the same
  // chain as the top-level DropShadowImageFilter::OnFilter).
  Color4f resolved = filter->GetColor4f();
  auto steps = ColorTransformSteps::Resolve(filter->GetColorSpace(),
                                            src->GetColorSpace());
  if (!steps.IsNoop()) {
    resolved = steps.Apply(resolved);
  }
  const Color shadow_color = ColorToPMColor(Color4fToColor(resolved));
  for (size_t y = 0; y < shadow_bitmap.Height(); ++y) {
    for (size_t x = 0; x < shadow_bitmap.Width(); ++x) {
      auto a = ColorGetA(shadow_bitmap.GetPixel(x, y));
      shadow_bitmap.SetPixel(
          x, y, a > 0 ? AlphaMulQ(shadow_color, a) : Color_TRANSPARENT);
    }
  }

  auto canvas = Canvas::MakeSoftwareCanvas(&dst_bitmap);
  canvas->Translate(-out_bounds.Left(), -out_bounds.Top());
  canvas->DrawImage(Image::MakeImage(shadow), padded, nullptr);
  canvas->DrawImage(Image::MakeImage(src), bounds, nullptr);
  return dst;
}

std::shared_ptr<Pixmap> ApplyFilterNode(const ImageFilterBase* filter,
                                        const std::shared_ptr<Pixmap>& src,
                                        const Rect& bounds, Rect& out_bounds) {
  const auto& space = src->GetColorSpace();
  // The input chain runs first: a serialized DropShadow nests as
  // Matrix{ColorFilter{Blur}} through inputs_, not as a Compose tree.
  auto stage = src;
  Rect stage_bounds = bounds;
  if (!filter->GetInputs().empty() && filter->GetInputs()[0] != nullptr) {
    stage = ApplyFilterNode(As_IFB(filter->GetInputs()[0]), stage, stage_bounds,
                            stage_bounds);
  }
  const std::shared_ptr<Pixmap> node_src = stage;
  const Rect& node_bounds = stage_bounds;
  switch (filter->GetType()) {
    case ImageFilterType::kBlur: {
      const auto* blur = static_cast<const BlurImageFilter*>(filter);
      const int32_t radius_x = static_cast<int32_t>(
          std::lround(std::min(blur->GetRadiusX(), 254.f)));
      const int32_t radius_y = static_cast<int32_t>(
          std::lround(std::min(blur->GetRadiusY(), 254.f)));
      // SWStackBlur cannot grow its support: draw the content layer into a
      // padded layer first (the content centered under the halo).
      Rect padded = ImageFilterBase::ApproximateFilteredBounds(
          node_bounds, static_cast<float>(radius_x),
          static_cast<float>(radius_y));
      out_bounds = padded;
      auto dst = MakeLayerPixmap(static_cast<uint32_t>(padded.Width()),
                                 static_cast<uint32_t>(padded.Height()), space);
      Bitmap dst_bitmap(dst, false);
      auto pad_canvas = Canvas::MakeSoftwareCanvas(&dst_bitmap);
      pad_canvas->Translate(-padded.Left(), -padded.Top());
      pad_canvas->DrawImage(Image::MakeImage(node_src), node_bounds, nullptr);
      auto stage =
          MakeLayerPixmap(dst_bitmap.Width(), dst_bitmap.Height(), space);
      Bitmap stage_bitmap(stage, false);
      SWStackBlur(&dst_bitmap, &stage_bitmap, radius_x, radius_y).Blur();
      return stage;
    }
    case ImageFilterType::kDropShadow:
      return ApplyDropShadowNode(
          static_cast<const DropShadowImageFilter*>(filter), node_src,
          node_bounds, out_bounds);
    case ImageFilterType::kMatix: {
      const auto* matrix_filter = static_cast<const MatrixImageFilter*>(filter);
      Rect mapped;
      matrix_filter->GetMatrix().MapRect(&mapped, node_bounds);
      out_bounds = ImageFilterBase::ApproximateFilteredBounds(mapped, 0, 0);

      auto dst =
          MakeLayerPixmap(static_cast<uint32_t>(out_bounds.Width()),
                          static_cast<uint32_t>(out_bounds.Height()), space);
      Bitmap dst_bitmap(dst, false);
      auto canvas = Canvas::MakeSoftwareCanvas(&dst_bitmap);
      canvas->Translate(-out_bounds.Left(), -out_bounds.Top());
      canvas->Concat(matrix_filter->GetMatrix());
      canvas->DrawImage(Image::MakeImage(node_src), node_bounds, nullptr);
      return dst;
    }
    case ImageFilterType::kColorFilter: {
      const auto* filter_filter =
          static_cast<const ColorFilterImageFilter*>(filter);
      out_bounds = bounds;
      auto dst = MakeLayerPixmap(node_src->Width(), node_src->Height(), space);
      Bitmap src_bitmap(src, false);
      Bitmap dst_bitmap(dst, false);
      // A declared blend color resolves into the layer space first - the
      // same rule GenerateBrush applies to paint color filters (the CPU
      // filter quantizes at construction, without a target context).
      auto color_filter = filter_filter->GetColorFilter();
      auto* base = As_CFB(color_filter.get());
      if (base->GetType() == ColorFilterType::kBlend) {
        auto* blend = static_cast<BlendColorFilter*>(base);
        if (!ColorSpaceEquals(blend->GetColorSpace(), space)) {
          Color4f resolved = blend->GetColor4f();
          auto steps =
              ColorTransformSteps::Resolve(blend->GetColorSpace(), space);
          if (!steps.IsNoop()) {
            resolved = steps.Apply(resolved);
          }
          color_filter =
              ColorFilters::Blend(resolved, blend->GetBlendMode(), nullptr);
        }
      }
      for (size_t y = 0; y < src_bitmap.Height(); ++y) {
        for (size_t x = 0; x < src_bitmap.Width(); ++x) {
          dst_bitmap.SetPixel(x, y,
                              As_CFB(color_filter.get())
                                  ->OnFilterColor(src_bitmap.GetPixel(x, y)));
        }
      }
      return dst;
    }
    case ImageFilterType::kCompose: {
      const auto* compose = static_cast<const ComposeImageFilter*>(filter);
      const auto& outer = compose->GetOuter();
      const auto& inner = compose->GetInner();
      auto stage = src;
      Rect stage_bounds = bounds;
      if (inner) {
        stage = ApplyFilterNode(As_IFB(inner.get()), stage, stage_bounds,
                                stage_bounds);
      }
      if (outer) {
        stage = ApplyFilterNode(As_IFB(outer.get()), stage, stage_bounds,
                                out_bounds);
      } else {
        out_bounds = stage_bounds;
      }
      return stage;
    }
    default:
      out_bounds = bounds;
      return src;
  }
}

}  // namespace

// static
void ImageFilterBase::BlurBitmapToCanvas(Canvas* canvas, Bitmap& bitmap,
                                         const Rect& filter_bounds,
                                         const Paint& paint, float radius_x,
                                         float radius_y) {
  Bitmap filtered_bitmap(bitmap.Width(), bitmap.Height(), kPremul_AlphaType);
  SWStackBlur(&bitmap, &filtered_bitmap, std::round(radius_x),
              std::round(radius_y))
      .Blur();
  canvas->DrawImage(Image::MakeImage(filtered_bitmap.GetPixmap()),
                    filter_bounds, &paint);
}

void ImageFilterBase::FlattenToBuffer(WriteBuffer& buffer) const {
  buffer.WriteInt32(static_cast<int32_t>(inputs_.size()));

  for (size_t i = 0; i < inputs_.size(); ++i) {
    auto input = inputs_[i];
    buffer.WriteBool(input != nullptr);

    if (input != nullptr) {
      buffer.WriteFlattenable(input);
    }
  }
}

void BlurImageFilter::OnFilter(Canvas* canvas, Bitmap& bitmap,
                               const Rect& filter_bounds,
                               const Rect& content_bounds,
                               const Paint& paint) const {
  Rect out_bounds;
  auto dst =
      ApplyFilterNode(this, bitmap.GetPixmap(), content_bounds, out_bounds);
  canvas->DrawImage(Image::MakeImage(dst), out_bounds, &paint);
}

std::string_view BlurImageFilter::ProcName() const {
  return "SkBlurImageFilter";
}

void BlurImageFilter::FlattenToBuffer(WriteBuffer& buffer) const {
  ImageFilterBase::FlattenToBuffer(buffer);

  buffer.WriteFloat(ConvertRadiusToSigma(radius_x_));
  buffer.WriteFloat(ConvertRadiusToSigma(radius_y_));

  // pass default value
  buffer.WriteInt32(static_cast<int32_t>(TileMode::kDecal));
}

void DropShadowImageFilter::OnFilter(Canvas* canvas, Bitmap& bitmap,
                                     const Rect& filter_bounds,
                                     const Rect& content_bounds,
                                     const Paint& paint) const {
  Rect out_bounds;
  auto dst =
      ApplyFilterNode(this, bitmap.GetPixmap(), content_bounds, out_bounds);
  canvas->DrawImage(Image::MakeImage(dst), out_bounds, &paint);
}

std::string_view DropShadowImageFilter::ProcName() const {
  // The last filter used to flatten is MatrixTransformImageFilter so we return
  // SkMatrixTransformImageFilter as ProcName
  return "SkMatrixTransformImageFilter";
}

void DropShadowImageFilter::FlattenToBuffer(WriteBuffer& buffer) const {
  BlurImageFilter blur(ConvertRadiusToSigma(radius_x_),
                       ConvertRadiusToSigma(radius_y_));

  // The declared space rides the blend filter's own serialization: the
  // color is written as sRGB-converted extended-range floats there, so a
  // wide-gamut shadow survives the trip for wide-gamut replay targets.
  ColorFilterImageFilter color_filter(
      &blur, ColorFilters::Blend(color_, BlendMode::kSrcIn, color_space_));

  MatrixImageFilter matrix_filter(
      &color_filter, Matrix::Translate(GetOffsetX(), GetOffsetY()));

  matrix_filter.FlattenToBuffer(buffer);
}

enum class MorphDirection { kX, kY };

static void call_proc_X(MorphologyImageFilter::Proc procX, const Bitmap& src,
                        Bitmap* dst, int radiusX, const Rect& bounds) {
  auto src_addr = reinterpret_cast<const PMColor*>(
      src.GetPixelAddr() + int32_t(bounds.Top()) * src.GetPixmap()->RowBytes() +
      int32_t(bounds.Left()));
  auto dst_addr = reinterpret_cast<PMColor*>(dst->GetPixelAddr());
  procX(src_addr, dst_addr, radiusX, bounds.Width(), bounds.Height(),
        src.GetPixmap()->RowBytes() / 4, dst->GetPixmap()->RowBytes() / 4);
}

static void call_proc_Y(MorphologyImageFilter::Proc procY, const PMColor* src,
                        int srcRowBytesAsPixels, Bitmap* dst, int radiusY,
                        const Rect& bounds) {
  procY(src, reinterpret_cast<PMColor*>(dst->GetPixelAddr()), radiusY,
        bounds.Height(), bounds.Width(), srcRowBytesAsPixels,
        dst->GetPixmap()->RowBytes() / 4);
}

template <ImageFilterType type, MorphDirection direction>
static void morph(const PMColor* src, PMColor* dst, int radius, int width,
                  int height, int srcStride, int dstStride) {
  const int srcStrideX = direction == MorphDirection::kX ? 1 : srcStride;
  const int dstStrideX = direction == MorphDirection::kX ? 1 : dstStride;
  const int srcStrideY = direction == MorphDirection::kX ? srcStride : 1;
  const int dstStrideY = direction == MorphDirection::kX ? dstStride : 1;
  radius = std::min(radius, width - 1);
  const PMColor* upperSrc = src + radius * srcStrideX;
  for (int x = 0; x < width; ++x) {
    const PMColor* lp = src;
    const PMColor* up = upperSrc;
    PMColor* dptr = dst;
    for (int y = 0; y < height; ++y) {
      // If we're maxing (dilate), start from 0; if minning (erode), start
      // from 255.
      const int start = (type == ImageFilterType::kDilate) ? 0 : 255;
      int B = start, G = start, R = start, A = start;
      for (const PMColor* p = lp; p <= up; p += srcStrideX) {
        int b = ColorGetB(*p), g = ColorGetG(*p), r = ColorGetR(*p),
            a = ColorGetA(*p);
        if (type == ImageFilterType::kDilate) {
          B = std::max(b, B);
          G = std::max(g, G);
          R = std::max(r, R);
          A = std::max(a, A);
        } else {
          B = std::min(b, B);
          G = std::min(g, G);
          R = std::min(r, R);
          A = std::min(a, A);
        }
      }
      *dptr = ColorSetARGB(A, R, G, B);
      dptr += dstStrideY;
      lp += srcStrideY;
      up += srcStrideY;
    }
    if (x >= radius) {
      src += srcStrideX;
    }
    if (x + radius < width - 1) {
      upperSrc += srcStrideX;
    }
    dst += dstStrideX;
  }
}

void MorphologyImageFilter::OnFilter(Canvas* canvas, Bitmap& bitmap,
                                     const Rect& filter_bounds,
                                     const Rect& content_bounds,
                                     const Paint& paint) const {
  Bitmap filtered_bitmap(filter_bounds.Width(), filter_bounds.Height());

  Proc procX, procY;

  if (ImageFilterType::kDilate == GetType()) {
    procX = &morph<ImageFilterType::kDilate, MorphDirection::kX>;
    procY = &morph<ImageFilterType::kDilate, MorphDirection::kY>;
  } else {
    procX = &morph<ImageFilterType::kErode, MorphDirection::kX>;
    procY = &morph<ImageFilterType::kErode, MorphDirection::kY>;
  }

  Rect src_bounds = Rect::MakeWH(filter_bounds.Width(), filter_bounds.Height());
  if (radius_x_ > 0 && radius_y_ > 0) {
    Bitmap tmp(filter_bounds.Width(), filter_bounds.Height(),
               AlphaType::kPremul_AlphaType);

    call_proc_X(procX, bitmap, &tmp, radius_x_, src_bounds);
    Rect tmp_bounds = Rect::MakeWH(src_bounds.Width(), src_bounds.Height());
    call_proc_Y(procY, (const skity::PMColor*)tmp.GetPixelAddr(),
                tmp.GetPixmap()->RowBytes() / 4, &filtered_bitmap, radius_y_,
                tmp_bounds);
  } else if (radius_x_ > 0) {
    call_proc_X(procX, bitmap, &filtered_bitmap, radius_x_, src_bounds);
  } else if (radius_y_ > 0) {
    auto src_addr = reinterpret_cast<const skity::PMColor*>(
        bitmap.GetPixelAddr() +
        int32_t(src_bounds.Top()) * bitmap.GetPixmap()->RowBytes() +
        int32_t(src_bounds.Left()) * 4);
    call_proc_Y(procY, src_addr, bitmap.GetPixmap()->RowBytes() / 4,
                &filtered_bitmap, radius_y_, src_bounds);
  }

  canvas->DrawImage(Image::MakeImage(filtered_bitmap.GetPixmap()),
                    filter_bounds, &paint);
}

void MatrixImageFilter::OnFilter(Canvas* canvas, Bitmap& bitmap,
                                 const Rect& filter_bounds,
                                 const Rect& content_bounds,
                                 const Paint& paint) const {
  Rect out_bounds;
  auto dst =
      ApplyFilterNode(this, bitmap.GetPixmap(), content_bounds, out_bounds);
  canvas->DrawImage(Image::MakeImage(dst), out_bounds, &paint);
}

void ColorFilterImageFilter::OnFilter(Canvas* canvas, Bitmap& bitmap,
                                      const Rect& filter_bounds,
                                      const Rect& content_bounds,
                                      const Paint& paint) const {
  Rect out_bounds;
  auto dst =
      ApplyFilterNode(this, bitmap.GetPixmap(), content_bounds, out_bounds);
  canvas->DrawImage(Image::MakeImage(dst), out_bounds, &paint);
}

void ComposeImageFilter::OnFilter(Canvas* canvas, Bitmap& bitmap,
                                  const Rect& filter_bounds,
                                  const Rect& content_bounds,
                                  const Paint& paint) const {
  Rect out_bounds;
  auto dst =
      ApplyFilterNode(this, bitmap.GetPixmap(), content_bounds, out_bounds);
  canvas->DrawImage(Image::MakeImage(dst), out_bounds, &paint);
}

#endif

std::string_view MorphologyImageFilter::ProcName() const {
  return "SkMorphologyImageFilter";
}

void MorphologyImageFilter::FlattenToBuffer(WriteBuffer& buffer) const {
  ImageFilterBase::FlattenToBuffer(buffer);

  buffer.WriteFloat(radius_x_);
  buffer.WriteFloat(radius_y_);

  if (type_ == ImageFilterType::kDilate) {
    buffer.WriteInt32(1);  // MorphType::kDilate
  } else if (type_ == ImageFilterType::kErode) {
    buffer.WriteInt32(0);  // MorphType::kErode
  }
}

MatrixImageFilter::MatrixImageFilter(const Matrix& matrix)
    : ImageFilterBase({nullptr}), matrix_(matrix) {}

MatrixImageFilter::MatrixImageFilter(ImageFilter* input, const Matrix& matrix)
    : ImageFilterBase({input}), matrix_(matrix) {}

std::string_view MatrixImageFilter::ProcName() const {
  return "SkMatrixTransformImageFilter";
}

void MatrixImageFilter::FlattenToBuffer(WriteBuffer& buffer) const {
  ImageFilterBase::FlattenToBuffer(buffer);

  buffer.WriteMatrix(matrix_);
  buffer.WriteSampling(SamplingOptions{});
}

ColorFilterImageFilter::ColorFilterImageFilter(std::shared_ptr<ColorFilter> cf)
    : ImageFilterBase({nullptr}), color_filter_(std::move(cf)) {}

ColorFilterImageFilter::ColorFilterImageFilter(ImageFilter* input,
                                               std::shared_ptr<ColorFilter> cf)
    : ImageFilterBase({input}), color_filter_(cf) {}

std::string_view ColorFilterImageFilter::ProcName() const {
  return "SkColorFilterImageFilter";
}

void ColorFilterImageFilter::FlattenToBuffer(WriteBuffer& buffer) const {
  ImageFilterBase::FlattenToBuffer(buffer);

  buffer.WriteFlattenable(color_filter_.get());
}

}  // namespace skity
