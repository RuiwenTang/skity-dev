// Copyright 2021 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

#ifndef INCLUDE_SKITY_RENDER_SW_CONTEXT_HPP
#define INCLUDE_SKITY_RENDER_SW_CONTEXT_HPP

#include <atomic>
#include <cstdint>
#include <skity/macros.hpp>

namespace skity {

/**
 * @class SWContext
 * @brief Process-wide runtime configuration for the software (CPU) renderer.
 *
 * SWContext is the single source of truth for policy flags that used to live
 * in scattered process globals: the SIMD accelerator switch, the band
 * parallelism switch, and the worker thread budget. It is a lazily created
 * process-level singleton; Get() always returns the same instance.
 *
 * The software canvas and its brushes read the context per draw, so runtime
 * toggles take effect on the next draw without recreating canvases. The
 * legacy sw_accel::SetEnabled()/IsEnabled() entry points are thin delegates
 * over this singleton.
 *
 * @note Fields are atomics and may be flipped at any time. No synchronization
 * is provided for the switch point itself: when multiple threads render
 * through the same process, a toggle may take effect mid-frame.
 * @note The Validate() fuse (CPU capability check) stays process-global and
 * is NOT represented here; AcceleratorEnabled() is still gated by it inside
 * the accelerator entry points.
 */
class SKITY_API SWContext {
 public:
  /**
   * @brief Returns the process-wide software rendering context.
   *
   * The instance is created on first use and outlives every canvas.
   *
   * @return the singleton SWContext, never null.
   */
  static SWContext* Get();

  SWContext(const SWContext&) = delete;
  SWContext& operator=(const SWContext&) = delete;

  /**
   * @brief Whether the SIMD accelerator (skity_sw_accel) may be used.
   *
   * On by default. Turning this off makes the software renderer take the
   * legacy scalar paths within the same binary (A/B comparisons).
   *
   * @return true when accelerated paths are enabled.
   */
  bool AccelEnabled() const;

  /**
   * @brief Enables or disables the SIMD accelerator at runtime.
   *
   * Takes effect on the next draw of every software canvas.
   *
   * @param enabled true to enable accelerated paths.
   */
  void SetAccelEnabled(bool enabled);

  /**
   * @brief Whether draw-level band parallelism may be used.
   *
   * Off by default; reserved for the M4 band-parallel executor. No code
   * consumes this flag yet.
   *
   * @return true when parallel execution is enabled.
   */
  bool ParallelEnabled() const;

  /**
   * @brief Enables or disables draw-level band parallelism at runtime.
   *
   * @param enabled true to enable parallel execution.
   */
  void SetParallelEnabled(bool enabled);

  /**
   * @brief Worker thread budget for parallel execution.
   *
   * The stored value is resolved on read: 0 (the default, "auto") maps to
   * std::thread::hardware_concurrency() with a fallback of 1.
   *
   * @return the effective thread count, always >= 1.
   */
  uint32_t ThreadCount() const;

  /**
   * @brief Sets the worker thread budget for parallel execution.
   *
   * @param count number of worker threads; 0 restores the auto value.
   */
  void SetThreadCount(uint32_t count);

 private:
  SWContext();

  std::atomic<bool> accel_enabled_{true};
  std::atomic<bool> parallel_enabled_{false};
  std::atomic<uint32_t> thread_count_{0};
};

}  // namespace skity

#endif  // INCLUDE_SKITY_RENDER_SW_CONTEXT_HPP
