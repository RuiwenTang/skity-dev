// Copyright 2021 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

#ifndef SRC_RENDER_SW_SW_RENDER_TARGET_HPP
#define SRC_RENDER_SW_SW_RENDER_TARGET_HPP

#include <skity/graphic/bitmap.hpp>
#include <skity/graphic/blend_mode.hpp>
#include <skity/macros.hpp>

namespace skity {

class SWRenderTarget {
 public:
  explicit SWRenderTarget(Bitmap* bitmap)
      : bitmap_(bitmap),
        pixel_addr_(bitmap_ != nullptr ? bitmap->GetPixelAddr() : nullptr) {}

  // blend pixel with premultiplied color
  void BlendPixel(uint32_t x, uint32_t y, Color color,
                  BlendMode blend = BlendMode::kDefault);

  void BlendPixel(uint32_t x, uint32_t y, Color4f color,
                  BlendMode blend = BlendMode::kDefault);

  void BlendPixelH(uint32_t x, uint32_t y, PMColor* pm_colors, uint32_t len,
                   BlendMode blend = BlendMode::kDefault);

  void BlendPixelH(uint32_t x, uint32_t y, PMColor pm_color, uint32_t len,
                   BlendMode blend = BlendMode::kDefault);

 private:
  bool FastBlend(uint32_t x, uint32_t y, Color color, BlendMode blend);

  Bitmap* bitmap_;
  uint8_t* pixel_addr_;
};

// Scalar glyph-mask blend: one coverage byte per pixel folded with the
// global alpha (MulDiv255Round) and into the constant premul color
// (AlphaMulQ, skipped at full coverage), then the real PorterDuffBlend.
// Supports every blend mode and is the reference the accelerated
// BlendMaskRect must match bit-exactly; the rect must already be clipped to
// the surface, and the surface must be premul ColorType::kRGBA (the caller
// gates eligibility).
void BlendGlyphMaskRect(const uint8_t* mask, size_t mask_stride, int32_t x,
                        int32_t y, int32_t width, int32_t height,
                        uint8_t* dst_base, size_t row_bytes, int32_t dst_width,
                        int32_t dst_height, PMColor src_pm,
                        uint8_t global_alpha, BlendMode blend);

}  // namespace skity

#endif  // SRC_RENDER_SW_SW_RENDER_TARGET_HPP
