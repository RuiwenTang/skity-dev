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

void CoverageRowBuffer::AddDirectRun(int x, int y, int len, uint8_t alpha) {
  if (len <= 0 || alpha == 0 || y < top_) {
    return;
  }
  if (y != curr_y_) {
    SwitchRow(y);
  }
  // Runs of one row are emitted in ascending x; merge with the previous run
  // when it is directly adjacent and has equal coverage.
  if (!direct_runs_.empty()) {
    Span& last = direct_runs_.back();
    if (last.cover == alpha && last.x + last.len == x) {
      last.len += len;
      return;
    }
  }
  direct_runs_.push_back(Span{x, y, len, alpha});
}

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
    direct_runs_.clear();
    // Clear only the dirty extent so the next row starts at zero.
    for (int32_t i = min_x_; i < max_x_; i++) {
      row_[i] = 0;
    }
    min_x_ = width_;
    max_x_ = 0;
  }
  curr_y_ = y;
}

void CoverageRowBuffer::EmitSpan(int x, int y, int len, uint8_t cover) {
  // Dense segments and direct runs are emitted in ascending x; merge into
  // the previous span when adjacent with equal coverage.
  if (!spans_.empty()) {
    Span& last = spans_.back();
    if (last.y == y && last.cover == cover && last.x + last.len == x) {
      last.len += len;
      return;
    }
  }
  spans_.push_back(Span{x, y, len, cover});
}

void CoverageRowBuffer::FlushRow() {
  if (curr_y_ == SW_NaN32) {
    return;
  }
  // Merge the dense dirty extent and the direct runs, both ascending in x,
  // into spans_. Direct runs win ties to keep the legacy emission order.
  //
  // Fast path: rows without dense contributions (the common case for opaque
  // fills) are appended in bulk; merging into spans_ is impossible across
  // rows.
  if (max_x_ <= min_x_) {
    spans_.insert(spans_.end(), direct_runs_.begin(), direct_runs_.end());
    return;
  }
  size_t run_index = 0;
  int32_t i = min_x_;
  while (true) {
    // Next dense run start (skipping zeros) or end-of-extent.
    while (i < max_x_ && row_[i] == 0) {
      i++;
    }
    if (i >= max_x_) {
      for (; run_index < direct_runs_.size(); run_index++) {
        const Span& run = direct_runs_[run_index];
        EmitSpan(run.x, run.y, run.len, static_cast<uint8_t>(run.cover));
      }
      break;
    }
    // Take the direct run first while it starts before the dense segment.
    if (run_index < direct_runs_.size() &&
        direct_runs_[run_index].x <= left_ + i) {
      const Span& run = direct_runs_[run_index];
      EmitSpan(run.x, run.y, run.len, static_cast<uint8_t>(run.cover));
      run_index++;
      continue;
    }
    // Emit one constant segment of the dense extent.
    uint16_t value = row_[i];
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
    EmitSpan(left_ + start, curr_y_, i - start, cover);
  }
}

}  // namespace skity
