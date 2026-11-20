// Copyright 2021 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

#include "src/render/sw/sw_coverage_row_buffer.hpp"

#include <algorithm>

namespace skity {

CoverageRowBuffer::CoverageRowBuffer(int left, int width, int top)
    : row_(static_cast<size_t>(width > 0 ? width : 0), 0),
      left_(left),
      width_(width > 0 ? width : 0),
      top_(top),
      min_x_(width > 0 ? width : 0),
      max_x_(0) {}

void CoverageRowBuffer::AddRun(int x, int y, int len, uint8_t alpha) {
  if (len <= 0 || alpha == 0 || y < top_) {
    return;
  }
  if (y != curr_y_) {
    SwitchRow(y);
  }
  int32_t begin = std::max<int32_t>(x, left_);
  int32_t end = std::min<int32_t>(x + len, left_ + width_);
  if (begin >= end) {
    return;
  }
  for (int32_t i = begin; i < end; i++) {
    row_[i - left_] = static_cast<uint16_t>(row_[i - left_] + alpha);
  }
  min_x_ = std::min(min_x_, begin - left_);
  max_x_ = std::max(max_x_, end - left_);
}

void CoverageRowBuffer::AddAlphas(int x, int y, const uint8_t* alphas,
                                  int len) {
  if (len <= 0 || y < top_) {
    return;
  }
  if (y != curr_y_) {
    SwitchRow(y);
  }
  int32_t begin = std::max<int32_t>(x, left_);
  int32_t end = std::min<int32_t>(x + len, left_ + width_);
  for (int32_t i = begin; i < end; i++) {
    uint8_t alpha = alphas[i - x];
    if (alpha != 0) {
      row_[i - left_] = static_cast<uint16_t>(row_[i - left_] + alpha);
    }
  }
  if (begin < end) {
    min_x_ = std::min(min_x_, begin - left_);
    max_x_ = std::max(max_x_, end - left_);
  }
}

std::vector<Span> CoverageRowBuffer::TakeSpans() {
  if (curr_y_ != SW_NaN32) {
    FlushRow();
    curr_y_ = SW_NaN32;
  }
  return std::move(spans_);
}

void CoverageRowBuffer::SwitchRow(int y) {
  if (curr_y_ != SW_NaN32) {
    FlushRow();
    // Clear only the dirty extent so the next row starts at zero.
    for (int32_t i = min_x_; i < max_x_; i++) {
      row_[i] = 0;
    }
    min_x_ = width_;
    max_x_ = 0;
  }
  curr_y_ = y;
}

void CoverageRowBuffer::FlushRow() {
  if (curr_y_ == SW_NaN32 || max_x_ <= min_x_) {
    return;
  }
  for (int32_t i = min_x_; i < max_x_;) {
    uint16_t value = row_[i];
    if (value == 0) {
      i++;
      continue;
    }
    // Clamp once; see the class comment for why this matches the legacy
    // per-step saturating add.
    uint8_t cover = value >= 255 ? 255 : static_cast<uint8_t>(value);
    int32_t start = i;
    i++;
    while (i < max_x_) {
      uint16_t next = row_[i];
      if (next == 0) {
        break;
      }
      uint8_t next_cover = next >= 255 ? 255 : static_cast<uint8_t>(next);
      if (next_cover != cover) {
        break;
      }
      i++;
    }
    spans_.push_back(Span{left_ + start, curr_y_, i - start, cover});
  }
}

}  // namespace skity
