// Copyright 2021 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

// Multi-target entry for the SW accelerator. foreach_target.h re-includes
// the kernel body once per enabled SIMD target; the direct include after it
// compiles the static (baseline) target and finalizes the dispatch table.

#undef HWY_TARGET_INCLUDE
#define HWY_TARGET_INCLUDE "src/render/sw/accel/sw_accel_kernels.cc"
#include <hwy/foreach_target.h>

#include <atomic>
#include <mutex>

#include "src/graphic/blend_mode_priv.hpp"
#include "src/graphic/color_priv.hpp"
#include "src/render/sw/accel/sw_accel.hpp"
#include "src/render/sw/accel/sw_accel_kernels.cc"

namespace skity {
namespace sw_accel {

HWY_EXPORT(SolidRunClearKernel);
HWY_EXPORT(SolidRunSrcKernel);
HWY_EXPORT(SolidRunDstKernel);
HWY_EXPORT(SolidRunSrcOverKernel);
HWY_EXPORT(SolidRunDstOverKernel);
HWY_EXPORT(SolidRunSrcInKernel);
HWY_EXPORT(SolidRunDstInKernel);
HWY_EXPORT(SolidRunSrcOutKernel);
HWY_EXPORT(SolidRunDstOutKernel);
HWY_EXPORT(SolidRunSrcATopKernel);
HWY_EXPORT(SolidRunDstATopKernel);
HWY_EXPORT(SolidRunXorKernel);
HWY_EXPORT(SolidRunPlusKernel);
HWY_EXPORT(SolidRunModulateKernel);
HWY_EXPORT(SolidRunScreenKernel);

namespace {

using RunKernelFn = void (*)(uint8_t*, size_t, uint32_t);

RunKernelFn SelectRunKernel(BlendMode mode) {
  switch (mode) {
    case BlendMode::kClear:
      return HWY_DYNAMIC_DISPATCH(SolidRunClearKernel);
    case BlendMode::kSrc:
      return HWY_DYNAMIC_DISPATCH(SolidRunSrcKernel);
    case BlendMode::kDst:
      return HWY_DYNAMIC_DISPATCH(SolidRunDstKernel);
    case BlendMode::kSrcOver:
      return HWY_DYNAMIC_DISPATCH(SolidRunSrcOverKernel);
    case BlendMode::kDstOver:
      return HWY_DYNAMIC_DISPATCH(SolidRunDstOverKernel);
    case BlendMode::kSrcIn:
      return HWY_DYNAMIC_DISPATCH(SolidRunSrcInKernel);
    case BlendMode::kDstIn:
      return HWY_DYNAMIC_DISPATCH(SolidRunDstInKernel);
    case BlendMode::kSrcOut:
      return HWY_DYNAMIC_DISPATCH(SolidRunSrcOutKernel);
    case BlendMode::kDstOut:
      return HWY_DYNAMIC_DISPATCH(SolidRunDstOutKernel);
    case BlendMode::kSrcATop:
      return HWY_DYNAMIC_DISPATCH(SolidRunSrcATopKernel);
    case BlendMode::kDstATop:
      return HWY_DYNAMIC_DISPATCH(SolidRunDstATopKernel);
    case BlendMode::kXor:
      return HWY_DYNAMIC_DISPATCH(SolidRunXorKernel);
    case BlendMode::kPlus:
      return HWY_DYNAMIC_DISPATCH(SolidRunPlusKernel);
    case BlendMode::kModulate:
      return HWY_DYNAMIC_DISPATCH(SolidRunModulateKernel);
    case BlendMode::kScreen:
      return HWY_DYNAMIC_DISPATCH(SolidRunScreenKernel);
    default:
      return nullptr;
  }
}

// Scalar reference of the legacy solid path: fold coverage into src, then
// the real PorterDuffBlend. Takes and returns memory words (0xAABBGGRR,
// ColorType::kRGBA); used by Validate() to check the dispatched kernels.
uint32_t ReferenceBlend(BlendMode mode, uint32_t src_pm, uint32_t dst_mem,
                        uint8_t cover) {
  const uint32_t src = cover == 255 ? src_pm : AlphaMulQ(src_pm, cover);
  return PMColorSwapRB(PorterDuffBlend(src, PMColorSwapRB(dst_mem), mode));
}

std::once_flag g_validate_once;
bool g_available = false;
std::atomic<bool> g_runtime_enabled{true};

void ValidateOnce() {
  // Same deterministic pattern shape as the unit tests: rows longer than a
  // vector plus tail, and alpha edges (0, 128, 255). Dispatches the kernels
  // directly - going through FillSolidSpans would re-enter this call_once.
  struct Entry {
    BlendMode mode;
    RunKernelFn kernel;
  };
  const Entry entries[] = {
      {BlendMode::kClear, HWY_DYNAMIC_DISPATCH(SolidRunClearKernel)},
      {BlendMode::kSrc, HWY_DYNAMIC_DISPATCH(SolidRunSrcKernel)},
      {BlendMode::kDst, HWY_DYNAMIC_DISPATCH(SolidRunDstKernel)},
      {BlendMode::kSrcOver, HWY_DYNAMIC_DISPATCH(SolidRunSrcOverKernel)},
      {BlendMode::kDstOver, HWY_DYNAMIC_DISPATCH(SolidRunDstOverKernel)},
      {BlendMode::kSrcIn, HWY_DYNAMIC_DISPATCH(SolidRunSrcInKernel)},
      {BlendMode::kDstIn, HWY_DYNAMIC_DISPATCH(SolidRunDstInKernel)},
      {BlendMode::kSrcOut, HWY_DYNAMIC_DISPATCH(SolidRunSrcOutKernel)},
      {BlendMode::kDstOut, HWY_DYNAMIC_DISPATCH(SolidRunDstOutKernel)},
      {BlendMode::kSrcATop, HWY_DYNAMIC_DISPATCH(SolidRunSrcATopKernel)},
      {BlendMode::kDstATop, HWY_DYNAMIC_DISPATCH(SolidRunDstATopKernel)},
      {BlendMode::kXor, HWY_DYNAMIC_DISPATCH(SolidRunXorKernel)},
      {BlendMode::kPlus, HWY_DYNAMIC_DISPATCH(SolidRunPlusKernel)},
      {BlendMode::kModulate, HWY_DYNAMIC_DISPATCH(SolidRunModulateKernel)},
      {BlendMode::kScreen, HWY_DYNAMIC_DISPATCH(SolidRunScreenKernel)},
  };
  // Premul src colors (channel values bounded by alpha): the mixed
  // Porter-Duff sums are byte-safe only on premul operands, which is the
  // accelerator's guaranteed input domain.
  const uint32_t srcs[] = {0x00000000u, 0x80808080u, 0xffffffffu, 0xff204060u,
                           0x40302010u};
  // Arbitrary words folded to premul for the same reason.
  uint32_t dst[71];
  uint32_t out[71];
  for (size_t i = 0; i < 71; i++) {
    dst[i] = ColorToPMColor(static_cast<uint32_t>(i * 2654435761u));
    out[i] = dst[i];
  }
  for (const Entry& e : entries) {
    for (uint32_t src_pm : srcs) {
      for (int cover_int = 0; cover_int < 256; cover_int += 5) {
        const uint8_t cover = static_cast<uint8_t>(cover_int);
        const uint32_t scaled =
            cover == 255 ? src_pm : AlphaMulQ(src_pm, cover);
        e.kernel(reinterpret_cast<uint8_t*>(out), 71, scaled);
        for (size_t i = 0; i < 71; i++) {
          if (out[i] != ReferenceBlend(e.mode, src_pm, dst[i], cover)) {
            return;
          }
          out[i] = dst[i];
        }
      }
    }
  }
  g_available = true;
}

}  // namespace

bool Validate() {
  // Run the check once through Available(); Validate itself triggers it.
  Available();
  return g_available;
}

bool Available() {
  std::call_once(g_validate_once, ValidateOnce);
  return g_available;
}

bool SupportsSolidMode(BlendMode mode) {
  return mode <= BlendMode::kLastCoeffMode;
}

void SetEnabled(bool enabled) {
  g_runtime_enabled.store(enabled, std::memory_order_relaxed);
}

bool IsEnabled() { return g_runtime_enabled.load(std::memory_order_relaxed); }

bool FillSolidSpans(BlendMode mode, uint32_t src_pm, uint8_t* dst_base,
                    size_t row_bytes, int32_t width, int32_t height,
                    const Span* spans, size_t span_count) {
  if (!IsEnabled() || !Available()) {
    return false;
  }
  RunKernelFn kernel = SelectRunKernel(mode);
  if (kernel == nullptr) {
    return false;
  }
  for (size_t i = 0; i < span_count; i++) {
    int32_t x = spans[i].x;
    int32_t y = spans[i].y;
    int32_t len = spans[i].len;
    if (y < 0 || y >= height || x >= width || len <= 0) {
      continue;
    }
    if (x < 0) {
      len += x;
      x = 0;
    }
    if (x + len > width) {
      len = width - x;
    }
    if (len <= 0) {
      continue;
    }
    int32_t cover = spans[i].cover;
    if (cover < 0) {
      cover = 0;
    } else if (cover > 255) {
      cover = 255;
    }
    const uint32_t scaled =
        cover == 255 ? src_pm : AlphaMulQ(src_pm, static_cast<unsigned>(cover));
    kernel(dst_base + static_cast<size_t>(y) * row_bytes +
               static_cast<size_t>(x) * 4,
           static_cast<size_t>(len), scaled);
  }
  return true;
}

}  // namespace sw_accel
}  // namespace skity
