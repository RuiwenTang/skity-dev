// Copyright 2021 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

#ifndef EXAMPLE_BASIC_EXAMPLE_HPP
#define EXAMPLE_BASIC_EXAMPLE_HPP

#include <skity/skity.hpp>

namespace skity::example::basic {
// Individual scene sections, also drawn (with the same transforms) by
// draw_canvas; exposed for per-section benchmarking of the software backend.
void draw_basic_example(skity::Canvas* canvas);
void draw_path_effect_example(skity::Canvas* canvas);
void draw_dash_start_example(skity::Canvas* canvas);
void draw_simple_text(skity::Canvas* canvas);
void draw_linear_gradient_example(skity::Canvas* canvas);
void draw_even_odd_fill(skity::Canvas* canvas);

void draw_canvas(skity::Canvas* canvas);
}  // namespace skity::example::basic

#endif  // EXAMPLE_BASIC_EXAMPLE_HPP
