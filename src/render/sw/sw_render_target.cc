// Copyright 2021 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

#include "src/render/sw/sw_render_target.hpp"

#include <algorithm>

#include "src/graphic/blend_mode_priv.hpp"
#include "src/graphic/color_priv.hpp"

namespace skity {

void SWRenderTarget::BlendPixel(uint32_t x, uint32_t y, Color src,
                                BlendMode blend) {
  if (!pixel_addr_) {
    return;
  }

  if (x >= bitmap_->Width() || y >= bitmap_->Height()) {
    return;
  }

  if (FastBlend(x, y, src, blend)) {
    return;
  }

  PMColor dst_pm = bitmap_->GetAlphaType() == AlphaType::kPremul_AlphaType
                       ? bitmap_->GetPixel(x, y)
                       : ColorToPMColor(bitmap_->GetPixel(x, y));

  PMColor r_pm = PorterDuffBlend(src, dst_pm, blend);
  bitmap_->SetPixel(x, y,
                    bitmap_->GetAlphaType() == AlphaType::kPremul_AlphaType
                        ? r_pm
                        : PMColorToColor(r_pm));
}

void SWRenderTarget::BlendPixel(uint32_t x, uint32_t y, Color4f color,
                                BlendMode blend) {
  this->BlendPixel(x, y, Color4fToColor(color), blend);
}

// Horizontal blending is deliberately scalar on every architecture. The
// legacy NEON run blender differed from the scalar PorterDuffBlend by +/-1 on
// a fraction of (sa, d) combinations, which made arm and x86 legacy output
// disagree and the SIMD accelerator (anchored to the scalar semantics) differ
// from the unaccelerated arm path on the same machine. Unifying on the scalar
// formula makes all three bit-identical for coeff modes; the accelerated
// path is the fast lane, this one only has to agree with it.

// Premul ColorType::kRGBA rows blend straight against the pixel buffer: the
// brush has already clamped its spans, so the per-pixel GetPixel/SetPixel and
// bounds machinery only remains for the formats this lane cannot serve
// (unpremul surfaces keep the per-pixel path). Behavior is unchanged -
// FastBlend's shortcuts are exact special cases of PorterDuffBlend, and every
// blended pixel runs the same scalar formula as before.
bool SWRenderTarget::BlendRowDirect(uint32_t x, uint32_t y, uint32_t len) {
  return pixel_addr_ != nullptr &&
         bitmap_->GetColorType() == ColorType::kRGBA &&
         bitmap_->GetAlphaType() == AlphaType::kPremul_AlphaType &&
         y < bitmap_->Height() && x < bitmap_->Width() &&
         len <= bitmap_->Width() - x;
}

void SWRenderTarget::BlendPixelH(uint32_t x, uint32_t y, PMColor* pm_colors,
                                 uint32_t len, BlendMode blend) {
  if (BlendRowDirect(x, y, len)) {
    uint32_t* dst32 = reinterpret_cast<uint32_t*>(
        pixel_addr_ + static_cast<size_t>(y) * bitmap_->RowBytes() +
        static_cast<size_t>(x) * 4);
    for (uint32_t i = 0; i < len; i++) {
      const PMColor s = pm_colors[i];
      if (!FastBlend(x + i, y, s, blend)) {
        dst32[i] =
            PMColorSwapRB(PorterDuffBlend(s, PMColorSwapRB(dst32[i]), blend));
      }
    }
    return;
  }
  for (uint32_t i = 0; i < len; i++) {
    BlendPixel(x + i, y, pm_colors[i], blend);
  }
}

void SWRenderTarget::BlendPixelH(uint32_t x, uint32_t y, PMColor pm_color,
                                 uint32_t len, BlendMode blend) {
  if (BlendRowDirect(x, y, len)) {
    uint32_t* dst32 = reinterpret_cast<uint32_t*>(
        pixel_addr_ + static_cast<size_t>(y) * bitmap_->RowBytes() +
        static_cast<size_t>(x) * 4);
    for (uint32_t i = 0; i < len; i++) {
      if (!FastBlend(x + i, y, pm_color, blend)) {
        dst32[i] = PMColorSwapRB(
            PorterDuffBlend(pm_color, PMColorSwapRB(dst32[i]), blend));
      }
    }
    return;
  }
  for (uint32_t i = 0; i < len; i++) {
    BlendPixel(x + i, y, pm_color, blend);
  }
}

bool SWRenderTarget::FastBlend(uint32_t x, uint32_t y, Color color,
                               BlendMode blend) {
  // TODO(tangruiwen): Handle other blend mode
  size_t base_index = y * bitmap_->RowBytes() + x * 4;

  if (blend == BlendMode::kClear) {
    pixel_addr_[base_index] = 0;
    pixel_addr_[base_index + 1] = 0;
    pixel_addr_[base_index + 2] = 0;
    pixel_addr_[base_index + 3] = 0;

    return true;
  } else if (blend == BlendMode::kSrc) {
    bitmap_->SetPixel(x, y, color);
    return true;
  } else if (blend == BlendMode::kDst) {
    return true;
  } else if (blend == BlendMode::kSrcOver) {
    auto a = ColorGetA(color);
    if (a == 0) {
      return true;
    } else if (a == 255) {
      bitmap_->SetPixel(x, y, color);
      return true;
    }
  } else if (blend == BlendMode::kDstOver &&
             pixel_addr_[base_index + 3] == 255) {
    return true;
  } else if (blend == BlendMode::kDstIn) {
    auto a = ColorGetA(color);
    if (a == 255) {
      return true;
    } else if (a == 0) {
      pixel_addr_[base_index] = 0;
      pixel_addr_[base_index + 1] = 0;
      pixel_addr_[base_index + 2] = 0;
      pixel_addr_[base_index + 3] = 0;
      return true;
    }
  } else if (blend == BlendMode::kDstOut) {
    auto a = ColorGetA(color);
    if (a == 0) {
      return true;
    } else if (a == 255) {
      pixel_addr_[base_index] = 0;
      pixel_addr_[base_index + 1] = 0;
      pixel_addr_[base_index + 2] = 0;
      pixel_addr_[base_index + 3] = 0;
      return true;
    }
  }

  return false;
}

void BlendGlyphMaskRect(const uint8_t* mask, size_t mask_stride, int32_t x,
                        int32_t y, int32_t width, int32_t height,
                        uint8_t* dst_base, size_t row_bytes, int32_t dst_width,
                        int32_t dst_height, PMColor src_pm,
                        uint8_t global_alpha, BlendMode blend) {
  if (mask == nullptr || dst_base == nullptr || width <= 0 || height <= 0) {
    return;
  }
  // Clip to the destination surface, shifting the mask pointer along - the
  // same bounds handling the accelerated BlendMaskRect applies.
  const int32_t x0 = std::max(x, 0);
  const int32_t y0 = std::max(y, 0);
  const int32_t x1 = std::min(x + width, dst_width);
  const int32_t y1 = std::min(y + height, dst_height);
  if (x0 >= x1 || y0 >= y1) {
    return;
  }
  mask +=
      static_cast<size_t>(y0 - y) * mask_stride + static_cast<size_t>(x0 - x);
  const size_t count = static_cast<size_t>(x1 - x0);
  for (int32_t row = y0; row < y1; row++) {
    uint32_t* dst32 = reinterpret_cast<uint32_t*>(
        dst_base + static_cast<size_t>(row) * row_bytes +
        static_cast<size_t>(x0) * 4);
    const uint8_t* mask_row =
        mask + static_cast<size_t>(row - y0) * mask_stride;
    for (size_t i = 0; i < count; i++) {
      uint32_t m = mask_row[i];
      if (global_alpha != 255) {
        m = MulDiv255Round(m, global_alpha);
      }
      const uint32_t s = m == 255 ? src_pm : AlphaMulQ(src_pm, m);
      dst32[i] =
          PMColorSwapRB(PorterDuffBlend(s, PMColorSwapRB(dst32[i]), blend));
    }
  }
}

}  // namespace skity
