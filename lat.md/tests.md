# Tests

Test organization under `test/`: unit tests mirror the source tree, golden tests compare rendered pixels per backend, plus benchmarks and a font harness.

All validation goes through `tools/test-runner.py` (mandatory per AGENTS.md), which emits a structured `<agent-test-report>` JSON on failure — see [[tests#Test runner]].

## Unit tests

`test/ut/` builds one binary, `skity_unit_test` (GoogleTest + gmock), organized by subsystem directory mirroring the source tree.

Subsystem directories: `base/ capi/ codec/ effect/ geometry/ gpu/{gl,vk}/ graphic/ io/ recorder/ render/{hw,hw/draw}/ text/ utils/ wgx/`. Conditional sources attach per feature: codec tests need `SKITY_CODEC_MODULE`, Windows-specific font tests (`dwrite_stability_test.cc`, A8 gamma: `text/a8_mask_gamma_test.cc`) gate on platform macros, WGX SPIR-V tests need `WGX_SPIRV_BACKEND`, and font-dir-dependent tests take `-DSKITY_FONT_DIR`.

Vulkan gets a dedicated binary `skity_vulkan_unit_test` (`gpu/vk/gpu_context_vk_test.cc`, `wgx/wgx_vulkan_pipeline_test.cc`), linked by default against the bundled SwiftShader/ANGLE loader (`libSwAngle` on macOS, `libvulkan` on Linux); `SKITY_VK_TEST_USE_SYSTEM_LOADER` switches to the system loader. Known pre-existing failures there (SIGSEGV in RepositoryStyleTextureShaders, UniformHelper vertex shader cases, arena ×2) are documented as main-branch baseline, not regressions.

## Golden tests

`test/golden/` renders scenes and compares PNGs pixel-for-pixel: two binaries, `skity_golden_test_shape` and `skity_golden_test_text`, sharing `golden_test.cc` and the `skity_golden_test` object library (`common/`).

- **Backend matrix** — `--backend gl|vulkan|metal` (default Metal). One `Backend` abstraction (`golden_test_env.hpp`) with per-backend environments in `common/{gl,mtl,vk}/`. On GL, the runner sets SwiftShader/ANGLE env (`ANGLE_DEFAULT_PLATFORM`, `DYLD_LIBRARY_PATH` to libSwAngle).
- **Golden lookup is backend-suffixed** — GL first tries `foo_gl.png` before `foo.png`, so backends can share or override baselines.
- **Case registration** — `add_golden_test[_shape|_text]()` CMake functions over `cases/` (shape, gradient, clip, image, image_filter, save_layer, simple_shape, skp, text, blend_mode, recorder groups).
- **Updating baselines** — `SKITY_UPDATE_MISSING_GOLDEN=1` writes missing goldens; `SKITY_GOLDEN_GUI` opens an interactive diff window.
- **Sandbox rule** — always run through `tools/test-runner.py`; direct sandboxed runs of the golden binaries can fail at `texture != nullptr` before comparison.
- **Failure analysis** — the report JSON carries `expected_image`, `actual_image`, `diff_pixels_file`, `diff_bbox`; read `diff_pixels_file` before touching rendering logic.

## Benchmarks

`test/bench/` holds `skity_hw_render_bench` (optionally perfetto-instrumented, `SKITY_BENCH_ENABLE_PERFETTO`) and `skity_micro_bench` under `bench/micro/` (google_benchmark).

## Font harness

`harness/font/` (enabled by `SKITY_ENABLE_FONT_HARNESS`) builds the static `skity_font_harness` plus the `skity-font` CLI.

The CLI is a cross-platform font metrics/glyph probe (CoreText/DWrite branches) emitting jsoncpp reports — artifact/compare/probe/manifests workflow, with a `font_harness_cli_smoke` test. It exists to catch font-stack regressions like the DWrite stability and variation-axis issues in [[text#Gamma policy]].

## Test runner

`tools/test-runner.py` is the single entry point for all suites.

Flags: `--suite=unit|golden-shape|golden-text|...`, `--backend=` (golden), `--filter="<TestName>"`. It configures the environment per suite (ANGLE/SwiftShader for GL goldens), reports a JSON block to the terminal, and writes `build/agent_test_report.json`. CI (codecov + GitHub Actions) consumes the same suites.
