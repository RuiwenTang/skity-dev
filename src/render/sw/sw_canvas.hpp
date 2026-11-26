// Copyright 2021 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

#ifndef SRC_RENDER_SW_SW_CANVAS_HPP
#define SRC_RENDER_SW_SW_CANVAS_HPP

#include <glm/gtc/matrix_transform.hpp>
#include <memory>
#include <skity/graphic/bitmap.hpp>
#include <skity/render/canvas.hpp>
#include <skity/render/sw_context.hpp>

#include "src/render/canvas_state.hpp"
#include "src/render/sw/sw_raster_clip.hpp"
#include "src/render/sw/sw_subpixel.hpp"

#ifndef SKITY_CPU
#error "NOT Enable CPU Backend"
#endif

namespace skity {

class Bitmap;

class SWSpanBrush;
class SWRaster;

class SWCanvas : public Canvas {
  struct State {
    // The resolved raster clip of this save state (run-encoded antialiased
    // coverage + device bounds); null means no clip. save() shares the
    // parent's clip, a clip op builds a new one, restore() drops it.
    std::shared_ptr<const RasterClip> clip;

    bool has_layer = false;

    State() = default;
    State(State const&) = default;
    State& operator=(State const&) = default;

    bool HasClip() const { return clip != nullptr; }
  };

  struct LayerState {
    Rect rel_bounds = {};
    Rect log_bounds = {};

    std::unique_ptr<Bitmap> bitmap = {};
    std::unique_ptr<SWCanvas> canvas = {};

    Paint paint = {};

    LayerState(Rect const& rel_bounds, Rect const& log_bounds)
        : rel_bounds(rel_bounds), log_bounds(log_bounds) {}

    ~LayerState() = default;

    void Init(SWCanvas* parent_canvas, Vec2 offset);
  };

 public:
  explicit SWCanvas(Bitmap* bitmap);
  ~SWCanvas() override = default;

 protected:
  void OnDrawLine(float x0, float y0, float x1, float y1,
                  Paint const& paint) override;

  void OnClipRect(const Rect& rect, ClipOp op) override;

  void OnClipPath(const Path& path, ClipOp op) override;

  // Combines the pending clip with the current state's resolved clip per the
  // op and stores it (see sw_raster_clip.hpp).
  void PushClip(RasterClip clip, ClipOp op);

  // Rasterizes the device-space path into a run-encoded clip and pushes it.
  void ApplyClipPath(const Path& path, ClipOp op);

  void OnDrawPath(const Path& path, const Paint& paint) override;

  void OnDrawPaint(const Paint& paint) override;

  void OnSaveLayer(const Rect& bounds, const Paint& paint) override;

  void OnDrawBlob(const TextBlob* blob, float x, float y,
                  Paint const& paint) override;

  void OnDrawGlyphs(uint32_t count, const GlyphID* glyphs,
                    const float* position_x, const float* position_y,
                    const Font& font, const Paint& paint) override;

  void OnDrawImageRect(std::shared_ptr<Image> image, const Rect& src,
                       const Rect& dst, const SamplingOptions& sampling,
                       Paint const* paint) override;

  void OnSave() override;

  void OnRestore() override;

  void OnRestoreToCount(int saveCount) override;

  void OnFlush() override;

  uint32_t OnGetWidth() const override;

  uint32_t OnGetHeight() const override;

  CanvasState* GetCanvasState() const override {
    if (parent_canvas_) {
      return parent_canvas_->GetCanvasState();
    }
    return Canvas::GetCanvasState();
  }

 private:
  std::unique_ptr<SWSpanBrush> GenerateBrush(std::vector<Span> const& spans,
                                             skity::Paint const& paint,
                                             bool stroke, Rect const& bounds);

  State* CurrentState() { return &state_stack_.back(); }

  void DoBrush(const SWRaster& raster, const Paint& paint, bool stroke);

  void DrawGlyphsInternal(uint32_t count, const GlyphID* glyphs,
                          const float* position_x, const float* position_y,
                          const Font& font, const Paint& paint);

  void FillGlyphs(uint32_t count, const GlyphID* glyphs,
                  const float* position_x, const float* position_y,
                  const Font& font, const Paint& paint);
  void StrokeGlyphs(uint32_t count, const GlyphID* glyphs,
                    const float* position_x, const float* position_y,
                    const Font& font, const Paint& paint);

  void HandleFilter(uint32_t count, const GlyphID* glyphs,
                    const float* position_x, const float* position_y,
                    const Font& font, const Paint& paint);

  void HandleFilter(Path const& path, Paint const& paint);

  LayerState* PeekLayerStack();

  std::unique_ptr<LayerState> PopLayerStack();

  LayerState* GenerateLayer(Rect const& rel_bounds, Rect const& log_bounds,
                            Vec2 const& offset);

  void OnLayerRestore();

  std::unique_ptr<SWCanvas> CreateSubCanvas(Bitmap* bitmap, const Vec2& offset);

  const Rect& GetGlobalClipBounds() const override {
    if (parent_canvas_) {
      return parent_canvas_->GetGlobalClipBounds();
    }
    return Canvas::GetGlobalClipBounds();
  }

  Rect GetScanClipBounds() const {
    Rect clip_bounds = GetGlobalClipBounds();
    clip_bounds.Offset(-global_offset_.x, -global_offset_.y);
    // Keep the scan window inside the device even with no clip: unclipped
    // paths (or coordinates inflated before the perspective fold) would
    // otherwise overflow the rasterizer's 16.16 fixed-point scan range.
    clip_bounds.Intersect(Rect::MakeWH(static_cast<float>(bitmap_->Width()),
                                       static_cast<float>(bitmap_->Height())));
    if (const auto& clip = state_stack_.back().clip) {
      // The resolved clip bounds bound every scan and blit window.
      clip_bounds.Intersect(clip->bounds);
    }
    return clip_bounds;
  }

  bool IsDrawingLayer() const {
    if (parent_canvas_) {
      return parent_canvas_->drawing_layer_;
    }
    return drawing_layer_;
  }

  void SetDrawingLayer(bool drawing_layer) {
    if (parent_canvas_) {
      parent_canvas_->drawing_layer_ = drawing_layer;

    } else {
      drawing_layer_ = drawing_layer;
    }
  }

  Matrix CurrentTransform() {
    Matrix transform = Matrix::Translate(-global_offset_.x, -global_offset_.y) *
                       GetTotalMatrix();
    // A projective CTM whose w row is constant (independent of x/y) has an
    // exact affine equivalent on the z=0 drawing plane: fold it in here so
    // every consumer - the rasterizer's path transform, the brush texture
    // and gradient inverse mappings - sees plain affine coordinates
    // instead of glm's homogeneous (w-scaled) results. Truly projective
    // CTMs (w depends on x/y) are left untouched; the software pipeline
    // has no per-pixel divide for those yet.
    const float w_x = transform[0][3];
    const float w_y = transform[1][3];
    const float w_w = transform[3][3];
    if (w_x == 0.f && w_y == 0.f && w_w != 0.f && w_w != 1.f) {
      const float inv_w = 1.f / w_w;
      for (int col = 0; col < 4; col++) {
        transform[col][0] *= inv_w;
        transform[col][1] *= inv_w;
      }
      // The z row/column carries no 2D information (points live on z=0 and
      // nothing consumes the z output), but a non-standard z row would skew
      // glm's full inverse into the 2x3 consumers (MapPoint): normalize it.
      transform[2][0] = 0.f;
      transform[2][1] = 0.f;
      transform[2][2] = 1.f;
      transform[2][3] = 0.f;
      transform[0][2] = 0.f;
      transform[1][2] = 0.f;
      transform[3][2] = 0.f;
      transform[0][3] = 0.f;
      transform[1][3] = 0.f;
      transform[3][3] = 1.f;
    }
    return transform;
  }

 private:
  Bitmap* bitmap_;
  // Process-wide renderer configuration (accel / parallel switches); read
  // per draw so runtime toggles take effect without recreating the canvas.
  SWContext* context_ = SWContext::Get();
  // TODO(tangruiwen) state can use copy on write time template
  std::vector<State> state_stack_;
  std::vector<std::unique_ptr<LayerState>> layer_stack_;
  SWCanvas* parent_canvas_ = nullptr;
  Vec2 global_offset_ = Vec2{0.f, 0.f};
  bool drawing_layer_ = false;
  // Holds the target-resolved blend filter for the in-flight brush; brushes
  // keep a raw pointer, so the replacement must outlive GenerateBrush.
  std::shared_ptr<ColorFilter> resolved_color_filter_;
};

}  // namespace skity

#endif  // SRC_RENDER_SW_SW_CANVAS_HPP
