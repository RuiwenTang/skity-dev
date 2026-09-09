// Copyright 2021 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

#include "src/render/canvas_state.hpp"

namespace skity {

void LayerState::Save() { elements_.emplace_back(CurrentElement()); }

void LayerState::Restore() {
  if (!CanRestore()) {
    return;
  }
  elements_.pop_back();
}

void LayerState::Translate(float dx, float dy) {
  Concat(Matrix::Translate(dx, dy));
}

void LayerState::Scale(float sx, float sy) { Concat(Matrix::Scale(sx, sy)); }

void LayerState::Rotate(float degree) {
  Concat(Matrix::RotateDeg(degree, Vec2{0, 0}));
}

void LayerState::Rotate(float degree, float px, float py) {
  Concat(Matrix::RotateDeg(degree, Vec2{px, py}));
}

void LayerState::Skew(float sx, float sy) { Concat(Matrix::Skew(sx, sy)); }

void LayerState::Concat(const Matrix& matrix) {
  auto& element = CurrentElement();
  element.local_matrix = element.local_matrix * matrix;
  element.total_matrix = element.total_matrix * matrix;
}

void LayerState::SetMatrix(const Matrix& matrix) {
  auto& element = CurrentElement();
  element.total_matrix = matrix;
  if (world_matrix_.IsIdentity()) {
    element.local_matrix = matrix;
    return;
  }

  Matrix world_to_layer;
  if (world_matrix_.InvertZ0Plane(&world_to_layer)) {
    element.local_matrix = world_to_layer * matrix;
  } else {
    // A singular layer transform cannot represent an arbitrary total matrix
    // in layer-local coordinates. Keep the local matrix valid; the exact total
    // matrix is still tracked separately.
    element.local_matrix = matrix;
  }
}

void LayerState::ResetMatrix() { SetMatrix(Matrix{}); }

}  // namespace skity
