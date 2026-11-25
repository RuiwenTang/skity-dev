// Copyright 2021 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

#include "src/render/sw/sw_glyph_atlas.hpp"

#include <cstdlib>

#include "src/render/text/atlas/atlas_bitmap.hpp"
#include "src/text/scaler_context_cache.hpp"
#include "src/text/scaler_context_container.hpp"
#include "src/tracing.hpp"

namespace skity {

SWGlyphAtlas::SWGlyphAtlas() : config_(AtlasFormat::A8, false) {}

SWGlyphAtlas* SWGlyphAtlas::GetInstance() {
  static SWGlyphAtlas atlas;
  return &atlas;
}

SWGlyphMask SWGlyphAtlas::GetGlyphMask(const Font& font, const Paint& paint,
                                       const ScalerContextDesc& desc,
                                       PackedGlyphID packed_glyph_id) {
  SKITY_TRACE_EVENT(SWGlyphAtlas_GetGlyphMask);

  std::lock_guard<std::mutex> lock(mutex_);

  const GlyphKey key(packed_glyph_id, desc);
  const size_t page_stride =
      static_cast<size_t>(config_.max_bitmap_size);  // A8: one byte per pixel

  for (const auto& page : pages_) {
    if (!page) {
      continue;
    }
    const GlyphRegion region = page->GetGlyphRegion(key);
    if (region.loc == INVALID_LOC) {
      continue;
    }
    SWGlyphMask mask;
    mask.status = AtlasGlyphStatus::kReady;
    mask.mask = page->MemData() +
                static_cast<size_t>(region.loc.y) * page_stride +
                static_cast<size_t>(region.loc.x);
    mask.stride = page_stride;
    mask.width = static_cast<uint32_t>(region.loc.z);
    mask.height = static_cast<uint32_t>(region.loc.w);
    mask.origin_x = region.origin_x;
    mask.origin_y = region.origin_y;
    return mask;
  }

  // Miss: render the mask through the shared scaler context (FreeType), the
  // same production the GPU atlas uses.
  auto scaler_context =
      ScalerContextCache::GlobalScalerContextCache()->FindOrCreateScalerContext(
          desc, font.GetTypeface());
  OwnedGlyphImage image =
      scaler_context->PrepareOwnedImage(packed_glyph_id, paint);
  if (image.GetStatus() != OwnedGlyphImage::Status::kReady) {
    SWGlyphMask mask;
    mask.status = image.GetStatus() == OwnedGlyphImage::Status::kNoImage
                      ? AtlasGlyphStatus::kNoImage
                      : AtlasGlyphStatus::kInvalidImage;
    return mask;
  }

  // Insert into the current page; a full page opens the next one. One retry
  // on a freshly allocated page covers the page-full case without
  // regenerating the image (mirrors Atlas::GenerateGlyphRegionInternal).
  for (int attempt = 0; attempt < 2; attempt++) {
    if (current_page_ >= pages_.size()) {
      pages_.resize(current_page_ + 1);
    }
    if (!pages_[current_page_]) {
      pages_[current_page_] = std::make_unique<AtlasBitmap>(
          config_.max_bitmap_size, config_.max_bitmap_size, 1);
    }
    const AtlasGlyphResult result =
        pages_[current_page_]->GenerateGlyphRegion(key, image.Image());
    if (result.status == AtlasGlyphStatus::kPageFull) {
      current_page_++;
      continue;
    }
    SWGlyphMask mask;
    mask.status = result.status;
    if (result.status == AtlasGlyphStatus::kReady) {
      const GlyphRegion& region = result.region;
      mask.mask = pages_[current_page_]->MemData() +
                  static_cast<size_t>(region.loc.y) * page_stride +
                  static_cast<size_t>(region.loc.x);
      mask.stride = page_stride;
      mask.width = static_cast<uint32_t>(region.loc.z);
      mask.height = static_cast<uint32_t>(region.loc.w);
      mask.origin_x = region.origin_x;
      mask.origin_y = region.origin_y;
    }
    return mask;
  }
  SWGlyphMask mask;
  mask.status = AtlasGlyphStatus::kInvalidImage;
  return mask;
}

}  // namespace skity
