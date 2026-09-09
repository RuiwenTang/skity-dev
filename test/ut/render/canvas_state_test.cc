// Copyright 2021 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

#include "src/render/canvas_state.hpp"

#include <gtest/gtest.h>

#include <skity/skity.hpp>

TEST(CanvasState, CanCreate) {
  skity::CanvasState state;
  EXPECT_TRUE(state.GetTotalMatrix().IsIdentity());
}

TEST(CanvasState, CanScale) {
  skity::CanvasState state;
  state.Scale(1.5, 2.5);
  EXPECT_EQ(state.GetTotalMatrix(), skity::Matrix::Scale(1.5, 2.5));
}

TEST(CanvasState, CanTranslate) {
  skity::CanvasState state;
  state.Translate(1.6, 3.5);
  EXPECT_EQ(state.GetTotalMatrix(), skity::Matrix::Translate(1.6, 3.5));
}

TEST(CanvasState, CanSkew) {
  skity::CanvasState state;
  state.Skew(2.5, 2.0);
  EXPECT_EQ(state.GetTotalMatrix(), skity::Matrix::Skew(2.5, 2.0));
}

TEST(CanvasState, CanRotate) {
  skity::CanvasState state;
  state.Rotate(30);
  EXPECT_EQ(state.GetTotalMatrix(),
            skity::Matrix::RotateDeg(30, skity::Vec2{0, 0}));
}

TEST(CanvasState, CanRotateDeg) {
  skity::CanvasState state;
  state.Rotate(30, 1.0, 3.0);
  EXPECT_EQ(state.GetTotalMatrix(),
            skity::Matrix::RotateDeg(30, skity::Vec2{1.0, 3.0}));
}

TEST(CanvasState, CanConcat) {
  auto skew = skity::Matrix::Skew(2.5, 4.0);
  auto translate = skity::Matrix::Translate(10, 20);
  skity::CanvasState state;
  state.Concat(skew);
  state.Concat(translate);
  EXPECT_EQ(state.GetTotalMatrix(), skew * translate);
}

TEST(CanvasState, CanSetMatrix) {
  skity::CanvasState state;
  state.Translate(100, 100);
  state.SetMatrix(skity::Matrix::Scale(5, 10));
  EXPECT_EQ(state.GetTotalMatrix(), skity::Matrix::Scale(5, 10));
}

TEST(CanvasState, CanResetMatrix) {
  skity::CanvasState state;
  state.Translate(100, 100);
  state.ResetMatrix();
  EXPECT_EQ(state.GetTotalMatrix(), skity::Matrix{});
}

TEST(CanvasState, CanSaveAndRestore) {
  skity::CanvasState state;
  state.Save();
  state.Rotate(30, 1.0, 3.0);
  EXPECT_EQ(state.GetTotalMatrix(),
            skity::Matrix::RotateDeg(30, skity::Vec2{1.0, 3.0}));
  state.Restore();
  EXPECT_EQ(state.GetTotalMatrix(), skity::Matrix{});
}

TEST(CanvasState, CurrentLayerMatrix) {
  skity::CanvasState state;
  state.Rotate(30, 1.0, 3.0);
  skity::Paint paint;
  state.SaveLayer(skity::Rect::MakeLTRB(100, 100, 500, 500), paint);
  state.Rotate(20, 1.0, 3.0);
  EXPECT_EQ(state.CurrentLayerMatrix(),
            skity::Matrix::RotateDeg(20, skity::Vec2{1.0, 3.0}));
  EXPECT_EQ(state.GetTotalMatrix(),
            skity::Matrix::RotateDeg(30, skity::Vec2{1.0, 3.0}) *
                skity::Matrix::RotateDeg(20, skity::Vec2{1.0, 3.0}));
  state.Restore();
}

TEST(CanvasState, SetAndResetMatrixInsideSaveLayer) {
  skity::CanvasState state;
  const skity::Matrix layer_world_matrix =
      skity::Matrix::Translate(10, 20) * skity::Matrix::Scale(2, 3);
  state.Concat(layer_world_matrix);
  state.SaveLayer(skity::Rect::MakeLTRB(0, 0, 100, 100), skity::Paint{});

  const skity::Matrix matrix = skity::Matrix::Translate(30, 40);
  state.SetMatrix(matrix);
  EXPECT_EQ(state.GetTotalMatrix(), matrix);

  skity::Matrix world_to_layer;
  ASSERT_TRUE(layer_world_matrix.InvertZ0Plane(&world_to_layer));
  EXPECT_EQ(state.CurrentLayerMatrix(), world_to_layer * matrix);

  const skity::Matrix concat_matrix = skity::Matrix::Scale(2, 4);
  state.Concat(concat_matrix);
  EXPECT_EQ(state.GetTotalMatrix(), matrix * concat_matrix);
  EXPECT_EQ(state.CurrentLayerMatrix(),
            world_to_layer * matrix * concat_matrix);

  state.ResetMatrix();
  EXPECT_TRUE(state.GetTotalMatrix().IsIdentity());
  EXPECT_EQ(state.CurrentLayerMatrix(), world_to_layer);

  state.Restore();
  EXPECT_EQ(state.GetTotalMatrix(), layer_world_matrix);
}

TEST(CanvasState, SingularLayerSetMatrixKeepsRequestedTotalMatrix) {
  skity::CanvasState state;
  const skity::Matrix singular_matrix = skity::Matrix::Scale(0, 1);
  ASSERT_FALSE(singular_matrix.InvertZ0Plane(nullptr));
  state.SetMatrix(singular_matrix);
  state.SaveLayer(skity::Rect::MakeWH(100, 100), skity::Paint{});

  const skity::Matrix matrix = skity::Matrix::Translate(30, 40);
  const skity::Matrix concat_matrix = skity::Matrix::Scale(2, 4);
  state.SetMatrix(matrix);
  state.Concat(concat_matrix);

  const skity::Matrix expected_matrix = matrix * concat_matrix;
  EXPECT_EQ(state.CurrentLayerMatrix(), expected_matrix);
  EXPECT_EQ(state.GetTotalMatrix(), expected_matrix);

  state.SaveLayer(skity::Rect::MakeWH(50, 50), skity::Paint{});
  EXPECT_TRUE(state.CurrentLayerMatrix().IsIdentity());
  EXPECT_EQ(state.GetTotalMatrix(), expected_matrix);
  state.Restore();
  state.Restore();
  EXPECT_EQ(state.GetTotalMatrix(), singular_matrix);
}
