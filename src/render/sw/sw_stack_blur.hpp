// Copyright 2021 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

#ifndef SRC_RENDER_SW_SW_STACK_BLUR_HPP
#define SRC_RENDER_SW_SW_STACK_BLUR_HPP

#include <skity/graphic/color.hpp>

namespace skity {

class Bitmap;

// My implement about
// Mario Kingemann's StackBlur
// http://underdestruction.com/2004/02/25/stackblur-2004/
//
// The blur is separable: the horizontal pass runs with radius_x and the
// vertical pass with radius_y, so anisotropic kernels (e.g. a 10x5 blur)
// stay anisotropic instead of folding to max(radius_x, radius_y).
class SWStackBlur final {
 public:
  SWStackBlur(Bitmap* src, Bitmap* dst, int32_t blur_radius);

  SWStackBlur(Bitmap* src, Bitmap* dst, int32_t blur_radius_x,
              int32_t blur_radius_y);

  ~SWStackBlur() = default;

  void Blur();

 private:
  static int32_t GetMulSum(int32_t radius);
  static int32_t GetShrSum(int32_t radius);

  void BlurHorizontal(int32_t radius);
  void BlurVertical(int32_t radius);

 private:
  Bitmap* src_;
  Bitmap* dst_;
  int32_t blur_radius_x_;
  int32_t blur_radius_y_;
};

}  // namespace skity

#endif  // SRC_RENDER_SW_SW_STACK_BLUR_HPP
