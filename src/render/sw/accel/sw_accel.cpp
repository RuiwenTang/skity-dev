// Copyright 2021 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

// Multi-target entry for the SW accelerator. foreach_target.h re-includes
// the kernel body once per enabled SIMD target; the direct include after it
// compiles the static (baseline) target and finalizes the dispatch table.

#undef HWY_TARGET_INCLUDE
#define HWY_TARGET_INCLUDE "src/render/sw/accel/sw_accel_kernels.cc"
#include <hwy/foreach_target.h>

#include <mutex>

#include "src/graphic/color_priv.hpp"
#include "src/render/sw/accel/sw_accel.hpp"
#include "src/render/sw/accel/sw_accel_kernels.cc"

namespace skity {
namespace sw_accel {

HWY_EXPORT(SolidRunClearKernel);
HWY_EXPORT(SolidRunSrcKernel);
HWY_EXPORT(SolidRunSrcOverKernel);

namespace {

using RunKernelFn = void (*)(uint8_t*, size_t, uint32_t);

RunKernelFn SelectRunKernel(BlendMode mode) {
  switch (mode) {
    case BlendMode::kClear:
      return HWY_DYNAMIC_DISPATCH(SolidRunClearKernel);
    case BlendMode::kSrc:
      return HWY_DYNAMIC_DISPATCH(SolidRunSrcKernel);
    case BlendMode::kSrcOver:
      return HWY_DYNAMIC_DISPATCH(SolidRunSrcOverKernel);
    default:
      return nullptr;
  }
}

// Scalar reference of the legacy solid path: fold coverage into src, then
// blend. Takes and returns memory words (0xAABBGGRR, ColorType::kRGBA);
// used by Validate() to check the dispatched kernels.
uint32_t ReferenceBlend(BlendMode mode, uint32_t src_pm, uint32_t dst_mem,
                        uint8_t cover) {
  uint32_t dst = PMColorSwapRB(dst_mem);
  uint32_t src = cover == 255 ? src_pm : AlphaMulQ(src_pm, cover);
  uint32_t result;
  switch (mode) {
    case BlendMode::kClear:
      result = Color_TRANSPARENT;
      break;
    case BlendMode::kSrc:
      result = src;
      break;
    case BlendMode::kSrcOver:
      if (ColorGetA(src) == 0) {
        result = dst;
      } else {
        result = PMSrcOver(src, dst);
      }
      break;
    default:
      result = dst;
      break;
  }
  return PMColorSwapRB(result);
}

std::once_flag g_validate_once;
bool g_available = false;

void ValidateOnce() {
  // Same deterministic pattern shape as the unit tests: rows longer than a
  // vector plus tail, and alpha edges (0, 128, 255). Dispatches the kernels
  // directly - going through FillSolidSpans would re-enter this call_once.
  const RunKernelFn src_over = HWY_DYNAMIC_DISPATCH(SolidRunSrcOverKernel);
  const RunKernelFn src = HWY_DYNAMIC_DISPATCH(SolidRunSrcKernel);
  const uint32_t srcs[] = {0x00000000u, 0x80808080u, 0xffffffffu, 0xff204060u,
                           0x40302010u};
  uint32_t dst[71];
  uint32_t out[71];
  for (size_t i = 0; i < 71; i++) {
    dst[i] = static_cast<uint32_t>(i * 2654435761u);
    out[i] = dst[i];
  }
  for (uint32_t src_pm : srcs) {
    for (int cover_int = 0; cover_int < 256; cover_int += 5) {
      const uint8_t cover = static_cast<uint8_t>(cover_int);
      const uint32_t scaled =
          cover == 255 ? src_pm : AlphaMulQ(src_pm, cover);
      src(reinterpret_cast<uint8_t*>(out), 71, scaled);
      if (out[0] != ReferenceBlend(BlendMode::kSrc, src_pm, dst[0], cover)) {
        return;
      }
      for (size_t i = 0; i < 71; i++) {
        out[i] = dst[i];
      }
      src_over(reinterpret_cast<uint8_t*>(out), 71, scaled);
      for (size_t i = 0; i < 71; i++) {
        if (out[i] != ReferenceBlend(BlendMode::kSrcOver, src_pm, dst[i],
                                     cover)) {
          return;
        }
        out[i] = dst[i];
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
  switch (mode) {
    case BlendMode::kClear:
    case BlendMode::kSrc:
    case BlendMode::kSrcOver:
      return true;
    default:
      return false;
  }
}

bool FillSolidSpans(BlendMode mode, uint32_t src_pm, uint8_t* dst_base,
                    size_t row_bytes, int32_t width, int32_t height,
                    const Span* spans, size_t span_count) {
  if (!Available()) {
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
