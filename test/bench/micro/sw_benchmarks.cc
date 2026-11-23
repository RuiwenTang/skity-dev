// Copyright 2021 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

#include <benchmark/benchmark.h>

#include <skity/skity.hpp>

#include "case/basic/example.hpp"
#include "src/render/sw/sw_raster.hpp"
#include "src/render/sw/sw_span_brush.hpp"

#ifdef SKITY_SW_ACCEL
#include "src/graphic/blend_mode_priv.hpp"
#include "src/graphic/color_priv.hpp"
#include "src/render/sw/accel/sw_accel.hpp"

// Scopes one A/B arm of the accelerator runtime switch; the global state is
// restored on exit so other benchmarks are unaffected.
class AccelToggle {
 public:
  explicit AccelToggle(bool enabled)
      : previous_(skity::sw_accel::IsEnabled()) {
    skity::sw_accel::SetEnabled(enabled);
  }
  ~AccelToggle() { skity::sw_accel::SetEnabled(previous_); }

 private:
  bool previous_;
};
#endif

static void RunExamplePremulScene(benchmark::State& state) {
  skity::Bitmap bitmap(1000, 800, skity::AlphaType::kPremul_AlphaType);
  auto canvas = skity::Canvas::MakeSoftwareCanvas(&bitmap);
  skity::Paint paint;
  paint.SetColor(skity::Color_WHITE);
  for (auto _ : state) {
    canvas->DrawPaint(paint);
    skity::example::basic::draw_canvas(canvas.get());
  }
}

static void BM_SWExamplePremulAlpha(benchmark::State& state) {
  RunExamplePremulScene(state);
}
BENCHMARK(BM_SWExamplePremulAlpha)->Unit(benchmark::kMicrosecond);

#ifdef SKITY_SW_ACCEL
// Same scene with the SIMD kernels disabled: the legacy scalar brush runs in
// the same binary. Compare against BM_SWExamplePremulAlpha for the
// accelerator's end-to-end effect (the scene still contains gradient and
// text draws which never take the solid-fill fast path).
static void BM_SWExamplePremulAlphaLegacy(benchmark::State& state) {
  AccelToggle toggle(false);
  RunExamplePremulScene(state);
}
BENCHMARK(BM_SWExamplePremulAlphaLegacy)->Unit(benchmark::kMicrosecond);
#endif

// Per-section breakdown of the example scene. Each section is drawn with the
// same transform as in draw_canvas, on the same premul target, so the times
// add up to the full scene and show where the software backend spends its
// time (and which draws the solid-fill accelerator can and cannot reach).
namespace {

using SectionFn = void (*)(skity::Canvas*);

template <SectionFn kSection, int32_t kTx, int32_t kTy>
void RunExampleSection(benchmark::State& state) {
  skity::Bitmap bitmap(1000, 800, skity::AlphaType::kPremul_AlphaType);
  auto canvas = skity::Canvas::MakeSoftwareCanvas(&bitmap);
  for (auto _ : state) {
    canvas->Save();
    canvas->Translate(static_cast<float>(kTx), static_cast<float>(kTy));
    kSection(canvas.get());
    canvas->Restore();
  }
}

}  // namespace

#define SKITY_BENCH_SECTION(name, fn, tx, ty)                             \
  static void BM_SWSection_##name(benchmark::State& state) {              \
    RunExampleSection<skity::example::basic::fn, tx, ty>(state);          \
  }                                                                      \
  BENCHMARK(BM_SWSection_##name)->Unit(benchmark::kMicrosecond);          \
  static void BM_SWSection_##name##_Legacy(benchmark::State& state) {     \
    AccelToggle toggle(false);                                             \
    RunExampleSection<skity::example::basic::fn, tx, ty>(state);          \
  }                                                                      \
  BENCHMARK(BM_SWSection_##name##_Legacy)->Unit(benchmark::kMicrosecond);

#ifdef SKITY_SW_ACCEL
SKITY_BENCH_SECTION(BasicShapes, draw_basic_example, 0, 0)
SKITY_BENCH_SECTION(PathEffect, draw_path_effect_example, 300, 0)
SKITY_BENCH_SECTION(DashStroke, draw_dash_start_example, 0, 300)
SKITY_BENCH_SECTION(Text, draw_simple_text, 520, 0)
SKITY_BENCH_SECTION(Gradient, draw_linear_gradient_example, 400, 300)
SKITY_BENCH_SECTION(EvenOddFill, draw_even_odd_fill, 800, 0)
#endif

// Pure blend cost, isolated from rasterization: a fixed span list is pushed
// through FillSolidSpans, and through the per-pixel scalar blend the legacy
// brush performs (PorterDuffBlend on each pixel). Measures the kernels
// themselves rather than what fraction of a scene reaches them.
#ifdef SKITY_SW_ACCEL
namespace {

constexpr int32_t kBlendW = 1000;
constexpr int32_t kBlendH = 800;

std::vector<skity::Span> BlendBenchSpans() {
  // Long interior runs with a short AA-style tail, i.e. the span shape the
  // walker emits for an opaque shape edge.
  std::vector<skity::Span> spans;
  spans.reserve(kBlendH * 2);
  for (int32_t y = 0; y < kBlendH; y++) {
    spans.push_back(skity::Span{8, y, 980, 255});
    spans.push_back(skity::Span{988, y, 3, 128});
  }
  return spans;
}

template <bool kUseAccel, skity::BlendMode kMode, bool kOpaqueSrc>
void RunSolidBlend(benchmark::State& state) {
  std::vector<uint32_t> pixels(kBlendW * kBlendH);
  const std::vector<skity::Span> spans = BlendBenchSpans();
  // Half-transparent src keeps the blend arithmetic from being short-
  // circuited; opaque src exercises the common opaque-fill case.
  const uint32_t src_pm = kOpaqueSrc ? 0xff3366ccu : 0x803366ccu;
  uint8_t* base = reinterpret_cast<uint8_t*>(pixels.data());

  for (auto _ : state) {
    if constexpr (kUseAccel) {
      skity::sw_accel::FillSolidSpans(kMode, src_pm, base, kBlendW * 4, kBlendW,
                                      kBlendH, spans.data(), spans.size());
    } else {
      for (const skity::Span& span : spans) {
        uint32_t* row = pixels.data() + static_cast<size_t>(span.y) * kBlendW +
                        span.x;
        const uint32_t src =
            span.cover == 255 ? src_pm : skity::AlphaMulQ(src_pm, span.cover);
        for (int32_t i = 0; i < span.len; i++) {
          // Memory words are kRGBA; blend in the PMColor convention.
          row[i] = skity::PMColorSwapRB(skity::PorterDuffBlend(
              src, skity::PMColorSwapRB(row[i]), kMode));
        }
      }
    }
    // Passing an lvalue binds DoNotOptimize to the non-deprecated overload
    // and keeps the buffer writes observable across iterations.
    uint32_t* sink = pixels.data();
    benchmark::DoNotOptimize(sink);
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(state.iterations() *
                          static_cast<int64_t>(spans.size()) * 983);
}

}  // namespace

#define SKITY_BENCH_BLEND(name, mode, opaque)                               \
  static void BM_SWSolidBlend_##name(benchmark::State& state) {             \
    RunSolidBlend<true, skity::BlendMode::mode, opaque>(state);             \
  }                                                                        \
  BENCHMARK(BM_SWSolidBlend_##name)->Unit(benchmark::kMicrosecond);         \
  static void BM_SWSolidBlend_##name##_Scalar(benchmark::State& state) {    \
    RunSolidBlend<false, skity::BlendMode::mode, opaque>(state);            \
  }                                                                        \
  BENCHMARK(BM_SWSolidBlend_##name##_Scalar)->Unit(benchmark::kMicrosecond);

SKITY_BENCH_BLEND(SrcOver, kSrcOver, false)
SKITY_BENCH_BLEND(SrcOverOpaque, kSrcOver, true)
SKITY_BENCH_BLEND(SrcIn, kSrcIn, false)
SKITY_BENCH_BLEND(Modulate, kModulate, false)
#endif

static void BM_SWExampleUnpremulAlpha(benchmark::State& state) {
  skity::Bitmap bitmap(1000, 800, skity::AlphaType::kUnpremul_AlphaType);
  auto canvas = skity::Canvas::MakeSoftwareCanvas(&bitmap);
  skity::Paint paint;
  paint.SetColor(skity::Color_WHITE);
  for (auto _ : state) {
    canvas->DrawPaint(paint);
    skity::example::basic::draw_canvas(canvas.get());
  }
}
BENCHMARK(BM_SWExampleUnpremulAlpha)->Unit(benchmark::kMicrosecond);

static void BM_SWExampleUnpremulAlphaWithClip(benchmark::State& state) {
  skity::Bitmap bitmap(1000, 800, skity::AlphaType::kUnpremul_AlphaType);
  auto canvas = skity::Canvas::MakeSoftwareCanvas(&bitmap);
  skity::Paint paint;
  paint.SetColor(skity::Color_WHITE);
  for (auto _ : state) {
    canvas->ClipRect(skity::Rect::MakeLTRB(100, 100, 200, 200));
    canvas->DrawPaint(paint);
    skity::example::basic::draw_canvas(canvas.get());
  }
}
BENCHMARK(BM_SWExampleUnpremulAlphaWithClip)->Unit(benchmark::kMicrosecond);

static void BM_SWRasterBigTriangle(benchmark::State& state) {
  skity::Bitmap bitmap(1000, 800, skity::AlphaType::kUnpremul_AlphaType);
  auto canvas = skity::Canvas::MakeSoftwareCanvas(&bitmap);
  skity::Path path;
  path.MoveTo(500, 0);
  path.LineTo(0, 800);
  path.LineTo(1000, 800);
  path.Close();

  for (auto _ : state) {
    skity::SWRaster raster;
    raster.RastePath(path, skity::Matrix{});
  }
}
BENCHMARK(BM_SWRasterBigTriangle)->Unit(benchmark::kMicrosecond);

static void BM_SWRasterStar(benchmark::State& state) {
  skity::Bitmap bitmap(1000, 800, skity::AlphaType::kUnpremul_AlphaType);
  auto canvas = skity::Canvas::MakeSoftwareCanvas(&bitmap);
  skity::Path star1;
  star1.MoveTo(100, 10);
  star1.LineTo(40, 180);
  star1.LineTo(190, 60);
  star1.LineTo(10, 60);
  star1.LineTo(160, 180);
  star1.Close();

  skity::Path star2 = star1;
  star2.SetFillType(skity::Path::PathFillType::kEvenOdd);

  for (auto _ : state) {
    skity::SWRaster raster1;
    raster1.RastePath(star1, skity::Matrix{});
    skity::SWRaster raster2;
    raster2.RastePath(star2, skity::Matrix{});
  }
}
BENCHMARK(BM_SWRasterStar)->Unit(benchmark::kMicrosecond);

static void BM_SWDrawBigImage(benchmark::State& state) {
  skity::Bitmap bitmap1(1000, 800, skity::AlphaType::kPremul_AlphaType);
  auto canvas1 = skity::Canvas::MakeSoftwareCanvas(&bitmap1);

  skity::Paint paint;
  paint.SetColor(skity::Color_WHITE);
  canvas1->DrawPaint(paint);
  skity::example::basic::draw_canvas(canvas1.get());

  skity::Bitmap bitmap2(1000, 800, skity::AlphaType::kPremul_AlphaType);
  auto canvas2 = skity::Canvas::MakeSoftwareCanvas(&bitmap2);
  std::shared_ptr<skity::Image> image =
      skity::Image::MakeImage(bitmap1.GetPixmap());

  for (auto _ : state) {
    canvas2->DrawImage(image,
                       skity::Rect::MakeWH(image->Width(), image->Height()));
  }
}
BENCHMARK(BM_SWDrawBigImage)->Unit(benchmark::kMicrosecond);

static void BM_SWDrawBigImageLinear(benchmark::State& state) {
  skity::Bitmap bitmap1(1000, 800, skity::AlphaType::kPremul_AlphaType);
  auto canvas1 = skity::Canvas::MakeSoftwareCanvas(&bitmap1);

  skity::Paint paint;
  paint.SetColor(skity::Color_WHITE);
  canvas1->DrawPaint(paint);
  skity::example::basic::draw_canvas(canvas1.get());

  skity::Bitmap bitmap2(1000, 800, skity::AlphaType::kPremul_AlphaType);
  auto canvas2 = skity::Canvas::MakeSoftwareCanvas(&bitmap2);
  std::shared_ptr<skity::Image> image =
      skity::Image::MakeImage(bitmap1.GetPixmap());

  skity::SamplingOptions options{skity::FilterMode::kLinear,
                                 skity::MipmapMode::kNone};

  for (auto _ : state) {
    canvas2->DrawImage(
        image, skity::Rect::MakeWH(image->Width(), image->Height()), options);
  }
}
BENCHMARK(BM_SWDrawBigImageLinear)->Unit(benchmark::kMicrosecond);

static void BM_SWDrawBigImageWithBlur(benchmark::State& state) {
  skity::Bitmap bitmap1(1000, 800, skity::AlphaType::kPremul_AlphaType);
  auto canvas1 = skity::Canvas::MakeSoftwareCanvas(&bitmap1);

  skity::Paint paint;
  paint.SetColor(skity::Color_WHITE);
  canvas1->DrawPaint(paint);
  skity::example::basic::draw_canvas(canvas1.get());

  skity::Bitmap bitmap2(1000, 800, skity::AlphaType::kPremul_AlphaType);
  auto canvas2 = skity::Canvas::MakeSoftwareCanvas(&bitmap2);
  std::shared_ptr<skity::Image> image =
      skity::Image::MakeImage(bitmap1.GetPixmap());
  auto mask_filter = skity::MaskFilter::MakeBlur(skity::BlurStyle::kNormal, 20);
  paint.SetMaskFilter(mask_filter);
  for (auto _ : state) {
    canvas2->DrawImage(
        image, skity::Rect::MakeWH(image->Width(), image->Height()), &paint);
  }
}
BENCHMARK(BM_SWDrawBigImageWithBlur)->Unit(benchmark::kMicrosecond);

class GradientSpanTest : public skity::GradientColorBrush {
 public:
  GradientSpanTest(skity::Shader::GradientInfo info,
                   skity::Shader::GradientType type)
      : GradientColorBrush(std::vector<skity::Span>{}, nullptr, nullptr,
                           skity::BlendMode::kDefault, std::move(info), type) {}

  ~GradientSpanTest() override = default;

  void TestLoop(float x, float y) { this->CalculateColor(x, y); }
};

static void BM_SWGradientSpanBrush(benchmark::State& state) {
  skity::Vec4 colors[] = {
      skity::Vec4{0.f, 1.f, 1.f, 0.f},
      skity::Vec4{0.f, 0.f, 1.f, 1.f},
      skity::Vec4{1.f, 0.f, 0.f, 1.f},
  };
  float positions[] = {0.f, 0.65f, 1.f};

  float blockX = 100.f;
  float blockY = 100.f;

  std::vector<skity::Point> pts = {
      skity::Point{blockX, blockY, 0.f, 1.f},
      skity::Point{blockX + 50, blockY + 100, 0.f, 1.f},
  };

  auto lgs = skity::Shader::MakeLinear(pts.data(), colors, positions, 3,
                                       skity::TileMode::kClamp);

  skity::Shader::GradientInfo info;

  auto type = lgs->AsGradient(&info);

  GradientSpanTest test_brush(info, type);

  for (auto _ : state) {
    for (size_t i = 0; i < 500 * 500; i++) {
      test_brush.TestLoop(105.f, 105.f);
    }
  }
}

BENCHMARK(BM_SWGradientSpanBrush)->Unit(benchmark::kMicrosecond);
