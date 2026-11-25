// Copyright 2021 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

#ifndef SRC_RENDER_SW_SW_GLYPH_ATLAS_HPP
#define SRC_RENDER_SW_SW_GLYPH_ATLAS_HPP

#include <memory>
#include <mutex>
#include <skity/graphic/paint.hpp>
#include <skity/text/font.hpp>
#include <vector>

#include "src/render/text/atlas/atlas_bitmap.hpp"
#include "src/render/text/atlas/atlas_glyph.hpp"
#include "src/text/packed_glyph_id.hpp"
#include "src/text/scaler_context_desc.hpp"

namespace skity {

// A cached A8 glyph mask plus its placement origin. The mask rows live in the
// atlas page storage (stride = the page width); origin_x/origin_y are the
// FreeType bitmap origin with the packed subpixel phase canceled, so the mask
// blits at the integer physical-pixel grid position
// floor(quantized pen) + (origin_x, -origin_y).
struct SWGlyphMask {
  AtlasGlyphStatus status = AtlasGlyphStatus::kNoImage;
  // Valid when status == kReady: mask.width x mask.height coverage bytes,
  // one byte per pixel, mask.stride bytes between rows.
  const uint8_t* mask = nullptr;
  size_t stride = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  float origin_x = 0.f;
  float origin_y = 0.f;

  bool IsReady() const { return status == AtlasGlyphStatus::kReady; }
};

// Process-wide memory atlas of A8 glyph masks for the software renderer.
// Reuses AtlasBitmap pages (packed allocation, GlyphKey hashing) and the
// scaler context's PrepareOwnedImage (FreeType mask generation) - the same
// production chain as the GPU glyph atlas, minus the GPU upload. The cache
// key is the GlyphKey (glyph id + subpixel phases + the full scaler desc
// including the device transform), so scaled or rotated text caches per
// transform bucket exactly like the GPU path. Thread-safe: text draws may
// come from several software canvases on different threads.
class SWGlyphAtlas {
 public:
  static SWGlyphAtlas* GetInstance();

  // Looks up the mask for (packed glyph id + scaler desc), rendering and
  // inserting it on a miss. Returns kReady with the mask view, kNoImage for
  // inkless glyphs (spaces), or kTooLarge / kInvalidImage when the mask
  // cannot be cached and the caller must fall back to the path pipeline.
  SWGlyphMask GetGlyphMask(const Font& font, const Paint& paint,
                           const ScalerContextDesc& desc,
                           PackedGlyphID packed_glyph_id);

 private:
  SWGlyphAtlas();

  std::mutex mutex_;
  // A8 pages of AtlasConfig::max_bitmap_size; one more is opened when the
  // current page runs full. Pages are never freed (glyph working sets are
  // small and stable).
  std::vector<std::unique_ptr<AtlasBitmap>> pages_;
  uint32_t current_page_ = 0;
  const AtlasConfig config_;
};

}  // namespace skity

#endif  // SRC_RENDER_SW_SW_GLYPH_ATLAS_HPP
