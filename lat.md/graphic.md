# Graphic API

The graphic layer is skity's public drawing vocabulary: `Canvas` (what to draw), `Paint` (how to style it), `Path` (geometry carrier), plus `Image`/`Bitmap`/`Pixmap` pixels. Headers live in `include/skity/graphic/`, implementation in `src/graphic/`.

## Canvas

`Canvas` (`include/skity/render/canvas.hpp`) is a template-method base: public `Draw*` methods manage matrix/clip state, then forward to protected virtual `OnDraw*` hooks.

Public surface: `DrawLine/Circle/Arc/Oval/Rect/RRect/RoundRect/DRRect/Path/Paint`, `DrawColor`, `Clear`, `SaveLayer`, `Flush`, text entry points (`DrawTextBlob`, deprecated `DrawSimpleText`/`DrawSimpleText2`, `SimpleTextBounds`, `DrawGlyphs`), `DrawImage`/`DrawImageRect`, clip ops (`ClipRect/RRect/Path`), save/restore + matrix ops, `QuickReject`, and clip-bounds queries. `Canvas::MakeSoftwareCanvas(Bitmap*)` creates the software backend.

Concrete canvases: `HWCanvas` (GPU, [[render#Canvas backends]]), `SWCanvas` ([[render#Software renderer]]), `RecordingCanvas` ([[recorder#Recording flow]]).

## Paint

`Paint` (`include/skity/graphic/paint.hpp`) styles geometry. Notable deviations from Skia worth knowing before touching style logic:

- `Style` adds `kStrokeThenFill_Style` beyond Skia's three styles.
- Fill and stroke carry **separate colors** (`fill_color_` / `stroke_color_`).
- Effect handles are all `shared_ptr`: `path_effect_`, `shader_`, `typeface_`, `color_filter_`, `image_filter_`, `mask_filter_` (see [[effect]]).
- Text knobs: `text_size_` (default 14), `font_fill_threshold_` (default 256 — above this, glyphs render as filled paths), `sdf_for_small_text_`.

`DrawFillStrokeInPaintOrder` (`src/render/paint_order.hpp`) is the shared helper that honors the style's fill/stroke ordering.

## Path

`Path` (`include/skity/graphic/path.hpp`) is the universal geometry container, storing verbs, points and conic weights in three parallel arrays (Skia lineage).

Chaining constructors: `MoveTo/LineTo/QuadTo/ConicTo/CubicTo/ArcTo/Close`; shape appenders `AddCircle/AddOval/AddRect/AddRoundRect/AddRRect/AddPath`.

Iteration: `Path::Iter` (auto-close state machine), `Path::RawIter` (peek), `Path::RangeIter` (range-for yielding verb + points + weights). Filling: `PathFillType {kWinding, kEvenOdd}`.

Performance-critical property: `IsAType {kGeneral, kRect, kOval, kSimpleRRect}` — the renderer uses it to promote simple shapes onto the analytic RRect pipeline (`NeedsFallbackToPathDraw` guards the way back). Bounds and convexity are computed lazily and cached (`bounds_dirty_`).

## Path utilities in graphic

Path-adjacent services that live in `src/graphic/` rather than `src/geometry/`:

- `PathMeasure` / `ContourMeasure` (`include/skity/graphic/path_measure.hpp`) — length/position/tangent sampling per contour.
- `PathOp` (`include/skity/graphic/path_op.hpp`) — boolean set operations on paths, implemented by `PathOpEngine` over Clipper2 (`src/graphic/pathop/`).
- `PathVisitor` / `PathScanner` (`src/graphic/path_visitor.hpp`, `path_scanner.hpp`) — curve-subdivision walks and winding computation shared by the renderers.

## Images and bitmaps

Pixel containers and their roles:

- `Bitmap` (`include/skity/graphic/bitmap.hpp`) — CPU pixels, also the target of `MakeSoftwareCanvas`.
- `Pixmap` (`include/skity/io/pixmap.hpp`) — read-only view over pixels (`src/io/pixmap.cc`).
- `Image` (`include/skity/graphic/image.hpp`) — drawable image with `ImageType {kCustom, kPixmap, kTexture, kDeferredTexture, kPromiseTexture}`; `DeferredTextureImage` holds a slot filled later, `PromiseTextureImage` resolves textures lazily via callback — the external-texture integration point.
- GPU-side `Texture`/`GPUSurface` abstractions belong to [[gpu#Resources]].

## Shape

`Shape` (`src/render/shape.hpp`) is an internal tagged union of `Path` or `RRect` used by `HWCanvas` to dispatch draws without re-testing the path type.
