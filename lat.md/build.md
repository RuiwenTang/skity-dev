# Build System

CMake is the source of truth; GN files are generated from it, habitat pins dependencies, and `platform/` wraps the results for Android and Apple consumption.

## CMake targets and options

Top-level targets live in the root `CMakeLists.txt`: product libraries, test binaries, and per-case examples.

Product libraries: `skity` (shared; static under Emscripten), `skity-capi`, `skity-codec`, `skity-io` (static), `wgsl-cross` (static), `skity_framework` (Apple, under `SKITY_GENERATE_FRAMEWORK`). Tests: `skity_unit_test`, `skity_vulkan_unit_test`, `skity_golden_test_shape/text`, and the benches.

Backend/feature options live in `cmake/Options.cmake`:

| Option | Default | Effect |
| --- | --- | --- |
| `SKITY_HW_RENDERER` / `SKITY_SW_RENDERER` | ON | hardware / software renderer (`-DSKITY_CPU`) |
| `SKITY_GL_BACKEND` | ON | `-DSKITY_OPENGL` |
| `SKITY_MTL_BACKEND` | ON on Apple | `-DSKITY_METAL` |
| `SKITY_VK_BACKEND` | OFF | `-DSKITY_VULKAN`; Debug adds `SKITY_VK_DEBUG_RUNTIME` |
| `WGX_SPIRV_BACKEND` | with VK | required by `SKITY_VK_BACKEND` ([[wgx#Integration points]]) |
| `SKITY_CODEC_MODULE` / `SKITY_IO_MODULE` / `SKITY_CAPI_MODULE` | ON | satellite modules ([[capi]]) |
| `SKITY_TEST`, `SKITY_TEST_UT/BENCH/GOLDEN`, `SKITY_GOLDEN_GUI` | OFF (tests) | test suites ([[tests]]) |
| `SKITY_CT_FONT` | OFF | CoreText font port ([[text#Font managers by platform]]) |
| `SKITY_HARMONY` / `SKITY_WASM` | off | OHOS / Emscripten paths |

Release mobile builds strip aggressively: LTO, `--gc-sections`, `--icf=safe`, `-Oz`, link maps (Linux/Android). Core code compiles `-fno-exceptions -fno-rtti -fvisibility=hidden` ([[architecture#Cross-cutting decisions]]). Platform branches live in `cmake/Platform.cmake` (Emscripten, Android NDK, macOS/iOS, OHOS, Linux GL/VK probing).

## Habitat dependencies

Dependencies are managed by [habitat](https://github.com/lynx-family/habitat) (`./tools/hab sync --target dev,module`), not by git submodules. `.habitat` points at `hab/DEPS*`:

- `hab/DEPS` — core third-party, pinned to commits with patches applied from `patches/`
- `hab/DEPS.dev` — dev-only (gtest, glfw, benchmark, perfetto, fonts, libSwAngle…)
- `hab/DEPS.ci` — CI toolchains (llvm, cmake, ninja via buildtools)
- `hab/DEPS.module` — codec extras (libjpeg-turbo, wuffs, libwebp)

Third-party rules: never edit `third_party/` directly; changes go through `patches/` (AGENTS.md repository boundary).

## GN generation

The `*.gni` files at the repo root are **generated**, not hand-maintained — editing them by hand fights the generator.

With `SKITY_CMAKE_TO_GN=ON`, `cmake/CMakeToGN.cmake` reflects over CMake targets (`cmake_to_gni()`, helper `tools/gen_target_gni.py`) to emit source lists/includes/defines/links for Lynx's GN build. Generated today: `skity.gni`, `skity-codec.gni`, `wgsl-cross.gni`, `freetype2.gni`, `jsoncpp_static.gni`, `pugixml-static.gni`.

## Platform packaging

How the built libraries ship to mobile platforms.

- `platform/android/` — a minimal Gradle project producing a prefab AAR (`skity-native.aar`) consumable via CMake `find_package`.
- `platform/darwin/` — `SKITY_GENERATE_FRAMEWORK` builds `skity.xcframework` (macOS arm64/x86_64, iOS arm64, simulator), packaged by `tools/pack_skity_framework.py`; static libraries only, no module maps/Swift support. CI caches these build deps.

## Third-party inventory

What ships and why (subset).

freetype2 (fonts, patched), glm (math), glad (GL loader), jsoncpp + pugixml (config parsing), fmt (logging), volk (Vulkan loaders in tests), Vulkan-Headers + VMA (VK backend), SPIRV-Headers (wgx SPIR-V), libpng/libjpeg-turbo/wuffs/libwebp/nanopng (codecs), glfw (examples/tests), gtest + google_benchmark (testing), perfetto (tracing), libSwAngle/libSwiftShader (software GPUs for tests).

Notably absent: harfbuzz (see [[text#Shaping model]]), shaderc/glslang/SPIRV-Cross (see [[wgx]]).
