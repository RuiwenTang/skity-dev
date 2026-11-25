// Copyright 2021 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

#include "src/render/sw/sw_raster_clip.hpp"

#include <algorithm>
#include <cstring>
#include <limits>

#include "src/graphic/color_priv.hpp"

namespace skity {

namespace {

// Fetches the coverage segments of `clip` on row `y` into `out`. The opaque
// tier synthesizes one full-coverage segment for the row, so the combine
// algebra never special-cases the tiers.
void FetchRow(const RasterClip& clip, int32_t y, std::vector<Span>& out) {
  out.clear();
  if (y < clip.bounds.Top() || y >= clip.bounds.Bottom()) {
    return;
  }
  if (clip.opaque) {
    out.push_back(Span{static_cast<int32_t>(clip.bounds.Left()), y,
                       static_cast<int32_t>(clip.bounds.Width()), 255});
    return;
  }
  // Runs are sorted by (y, x); find the first segment of row y.
  auto it = std::lower_bound(
      clip.runs.begin(), clip.runs.end(), y,
      [](const Span& s, int32_t value) { return s.y < value; });
  while (it != clip.runs.end() && it->y == y) {
    out.push_back(*it);
    ++it;
  }
}

// Merges two sorted segment lists of the same row, multiplying coverages on
// the overlaps and splitting segments where the composition changes.
void IntersectRow(const Span* a, size_t na, const Span* b, size_t nb, int32_t y,
                  std::vector<Span>& out) {
  size_t i = 0;
  size_t j = 0;
  while (i < na && j < nb) {
    const int32_t x0 = std::max(a[i].x, b[j].x);
    const int32_t x1 = std::min(a[i].x + a[i].len, b[j].x + b[j].len);
    if (x0 < x1) {
      const int32_t cover = MulDiv255Round(a[i].cover, b[j].cover);
      if (cover > 0) {
        out.push_back(Span{x0, y, x1 - x0, cover});
      }
    }
    if (a[i].x + a[i].len <= b[j].x + b[j].len) {
      ++i;
    } else {
      ++j;
    }
  }
}

// Subtracts the sorted `sub` segments from one sorted `cur` segment of the
// same row: the remaining coverage is cur - sub clamped at zero, and `cur`
// splits around the subtracted intervals.
void DifferenceRow(const Span& cur, const Span* sub, size_t nsub, int32_t y,
                   std::vector<Span>& out) {
  const int32_t cur_end = cur.x + cur.len;
  size_t j = 0;
  // Skip segments entirely left of this run.
  while (j < nsub && sub[j].x + sub[j].len <= cur.x) {
    ++j;
  }
  int32_t x = cur.x;
  while (x < cur_end && j < nsub) {
    const Span& s = sub[j];
    if (s.x >= cur_end) {
      break;
    }
    const int32_t overlap_start = std::max(x, s.x);
    if (overlap_start > x) {
      out.push_back(Span{x, y, overlap_start - x, cur.cover});
    }
    const int32_t overlap_end = std::min(cur_end, s.x + s.len);
    const int32_t remaining = cur.cover - s.cover;
    if (remaining > 0) {
      out.push_back(
          Span{overlap_start, y, overlap_end - overlap_start, remaining});
    }
    x = overlap_end;
    if (cur_end <= s.x + s.len) {
      break;
    }
    ++j;
  }
  if (x < cur_end) {
    out.push_back(Span{x, y, cur_end - x, cur.cover});
  }
}

}  // namespace

RasterClip MakeBoundsClip(const Rect& device_bounds) {
  RasterClip clip;
  clip.bounds = device_bounds;
  clip.opaque = true;
  return clip;
}

RasterClip MakeRunsClip(const Rect& device_bounds, std::vector<Span> runs) {
  RasterClip clip;
  clip.bounds = device_bounds;
  clip.runs = std::move(runs);
  clip.opaque = false;
  if (clip.runs.empty()) {
    clip.bounds.SetEmpty();
  }
  return clip;
}

RasterClip IntersectClip(const RasterClip& cur, const RasterClip& add) {
  RasterClip out;
  out.bounds = cur.bounds;
  if (!out.bounds.Intersect(add.bounds)) {
    return MakeBoundsClip(Rect::MakeEmpty());
  }
  if (cur.opaque && add.opaque) {
    out.opaque = true;
    return out;
  }
  out.opaque = false;

  std::vector<Span> cur_row;
  std::vector<Span> add_row;
  for (int32_t y = static_cast<int32_t>(out.bounds.Top());
       y < static_cast<int32_t>(out.bounds.Bottom()); y++) {
    FetchRow(cur, y, cur_row);
    FetchRow(add, y, add_row);
    IntersectRow(cur_row.data(), cur_row.size(), add_row.data(), add_row.size(),
                 y, out.runs);
  }
  if (out.runs.empty()) {
    out.bounds.SetEmpty();
  }
  return out;
}

RasterClip DifferenceClip(const RasterClip& cur, const RasterClip& sub) {
  // Nothing to subtract outside the intersection of the bounds.
  Rect overlap = cur.bounds;
  if (!overlap.Intersect(sub.bounds)) {
    return cur;
  }
  RasterClip out;
  out.bounds = cur.bounds;
  out.opaque = false;

  std::vector<Span> cur_row;
  std::vector<Span> sub_row;
  for (int32_t y = static_cast<int32_t>(cur.bounds.Top());
       y < static_cast<int32_t>(cur.bounds.Bottom()); y++) {
    FetchRow(cur, y, cur_row);
    FetchRow(sub, y, sub_row);
    for (const Span& run : cur_row) {
      DifferenceRow(run, sub_row.data(), sub_row.size(), y, out.runs);
    }
  }
  if (out.runs.empty()) {
    // The subtraction removed everything: fully clipped.
    out.bounds.SetEmpty();
  }
  return out;
}

std::vector<Span> ClipSpans(const RasterClip& clip,
                            std::vector<Span> const& spans) {
  // An empty clip draws nothing (an opaque clip with empty bounds reaches
  // here - the raster window alone does not express it).
  if (clip.IsEmpty()) {
    return {};
  }
  // The opaque tier is bounds-only: keep the spans inside the bounds with
  // their coverage untouched. (The rasterizer usually already bounded them
  // via GetScanClipBounds; this makes the function standalone-correct.)
  if (clip.opaque || spans.empty()) {
    std::vector<Span> out;
    if (clip.opaque) {
      out.reserve(spans.size());
      const int32_t left = static_cast<int32_t>(clip.bounds.Left());
      const int32_t right = static_cast<int32_t>(clip.bounds.Right());
      const int32_t top = static_cast<int32_t>(clip.bounds.Top());
      const int32_t bottom = static_cast<int32_t>(clip.bounds.Bottom());
      for (const Span& span : spans) {
        if (span.y < top || span.y >= bottom) {
          continue;
        }
        const int32_t x0 = std::max(span.x, left);
        const int32_t x1 = std::min(span.x + span.len, right);
        if (x0 < x1) {
          out.push_back(Span{x0, span.y, x1 - x0, span.cover});
        }
      }
      return out;
    }
    return spans;
  }
  std::vector<Span> out;
  out.reserve(spans.size());
  std::vector<Span> clip_row;
  int32_t cursor_row = std::numeric_limits<int32_t>::min();
  for (const Span& span : spans) {
    if (span.y != cursor_row) {
      cursor_row = span.y;
      FetchRow(clip, cursor_row, clip_row);
    }
    if (clip_row.empty()) {
      continue;
    }
    // Fill spans of one row ascend and the clip segments ascend; intersect
    // each fill span with every overlapping clip segment, multiplying the
    // coverages and splitting where the clip coverage changes.
    for (const Span& clip_run : clip_row) {
      const int32_t x0 = std::max(clip_run.x, span.x);
      const int32_t x1 = std::min(clip_run.x + clip_run.len, span.x + span.len);
      if (x0 < x1) {
        const int32_t cover = MulDiv255Round(span.cover, clip_run.cover);
        if (cover > 0) {
          out.push_back(Span{x0, span.y, x1 - x0, cover});
        }
      }
    }
  }
  return out;
}

void StageClipRow(const RasterClip& clip, int32_t y, int32_t x0, int32_t x1,
                  uint8_t* staged) {
  std::memset(staged, 0, static_cast<size_t>(x1 - x0));
  if (y < clip.bounds.Top() || y >= clip.bounds.Bottom()) {
    return;
  }
  if (clip.opaque) {
    const int32_t s = std::max(x0, static_cast<int32_t>(clip.bounds.Left()));
    const int32_t e = std::min(x1, static_cast<int32_t>(clip.bounds.Right()));
    if (s < e) {
      std::memset(staged + (s - x0), 255, static_cast<size_t>(e - s));
    }
    return;
  }
  auto it = std::lower_bound(
      clip.runs.begin(), clip.runs.end(), y,
      [](const Span& s, int32_t value) { return s.y < value; });
  while (it != clip.runs.end() && it->y == y) {
    const int32_t s = std::max(x0, it->x);
    const int32_t e = std::min(x1, it->x + it->len);
    if (s < e) {
      // A run has constant coverage.
      std::memset(staged + (s - x0), static_cast<uint8_t>(it->cover),
                  static_cast<size_t>(e - s));
    }
    ++it;
  }
}

}  // namespace skity
