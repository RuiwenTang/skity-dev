# Effects

The effect layer (`src/effect/`, `include/skity/effect/`) defines the paint-attachable effects: shaders (fill content), color/image/mask filters, and path effects. All effect classes inherit `Flattenable` for serialization — see [[effect#Flattenable serialization]].

## Shaders

`Shader` (`include/skity/effect/shader.hpp`) produces fill content. Implementations in `src/effect/`:

- `GradientShader` base with `LinearGradientShader`, `RadialGradientShader`, `TwoPointConicalGradientShader`, `SweepGradientShader` (factories `Shader::MakeLinear` etc.).
- `PixmapShader` — image fills; `Shader::MakeShader(image, ...)`.
- Degenerate gradients fall back to solid color via `NeedsFallbackToSolidColor` (`gradient_fallback.hpp`).

On the GPU path, shading dispatch happens through `GenShadingFragment`/`ConfigureShadingFragment` (`src/render/hw/draw/wgx_utils.hpp`), which pick the matching WGSL fragment (see [[render#Draw steps, geometry and fragments]]).

## Color filters

`ColorFilter` implementations (`src/effect/color_filter_base.hpp`): `BlendColorFilter`, `MatrixColorFilter` (4x5 matrix), `SRGBGammaColorFilter`, and `ComposeColorFilter` (flattening composition chains). Factories on `ColorFilters::`.

Their distinguishing property: on GPU they compile into dynamically generated WGSL (`WGXFilterFragment`), so they **do not require an offscreen layer** — `HWCanvas::NeesOffScreenLayer` excludes them on purpose. Semantics vs. the pipeline in [[render#Blend plans and dst read]].

## Image and mask filters

`ImageFilter` implementations live in `src/effect/image_filter_base.hpp`.

Concrete filters: `BlurImageFilter`, `DropShadowImageFilter`, `MorphologyImageFilter` (dilate/erode), `MatrixImageFilter`, `ColorFilterImageFilter`, `ComposeImageFilter`; factories on `ImageFilters::` (Blur/DropShadow/Dilate/Erode/MatrixTransform/ColorFilter/Compose/LocalMatrix).

`MaskFilter` currently only does blur (`MaskFilter::MakeBlur(BlurStyle, radius)`, `BlurStyle {kNormal, kSolid, kOuter, kInner}`) and is treated as a blur filter chain on GPU.

These are the two paint attachments that force offscreen rendering: `HWFilters::ConvertPaintToHWFilter` lowers them into an `HWFilter` DAG executed by `HWFilterLayer` — the DAG shape and phases are in [[render#Filter DAG]]. The software path runs `MaskFilterOnFilter` (`src/effect/mask_filter_priv.hpp`) with stack blur.

## Path effects

`PathEffect` rewrites geometry before rasterization, applied in `HWCanvas::DrawPathInternal`.

Implementations: `DashPathEffect` and `DiscretePathEffect` (`src/effect/dash_path_effect.*`, `discrete_path_effect.*`). Factories: `DashPathEffect::Make(interval, phase)`, `DiscretePathEffect::Make(seg_length, deviation, seed)`.

## Flattenable serialization

Every effect inherits `Flattenable` (`include/skity/io/flattenable.hpp`) so it can be serialized.

`FlattenToBuffer` writes the effect into a `WriteBuffer`, and `ProcName()` returns a Skia-style factory name (e.g. `"SkComposeImageFilter"`), keeping skity's serialized form compatible with Skia flattenable streams. This is what picture/display-list persistence ([[recorder#Serialization]]) and the `module/io` flat codecs build on.
