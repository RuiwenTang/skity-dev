// Copyright 2021 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

#ifndef SRC_GRAPHIC_BLEND_MODE_PRIV_HPP
#define SRC_GRAPHIC_BLEND_MODE_PRIV_HPP

#include <skity/graphic/blend_mode.hpp>
#include <skity/graphic/color.hpp>
#include <skity/macros.hpp>

namespace skity {

PMColor PorterDuffBlend(PMColor src, PMColor dst, BlendMode mode);

constexpr bool IsAdvancedBlendMode(BlendMode mode) {
  return mode > BlendMode::kLastCoeffMode;
}

}  // namespace skity
#endif  // SRC_GRAPHIC_BLEND_MODE_PRIV_HPP
