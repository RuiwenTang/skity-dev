# Geometry

The geometry layer (`src/geometry/`, `include/skity/geometry/`) is math, not policy: primitives, curves, and stroking. Its distinguishing trait is that it is dimensioned for 3D from day one.

## 3D-ready primitives

2D today, but the types leave room for perspective:

- `Point` is an alias of `Vec4` (`include/skity/geometry/point.hpp`) — homogeneous coordinates; `Vector` likewise.
- `Matrix` (`include/skity/geometry/matrix.hpp`) is **4x4 column-major** (not Skia's 3x3), with a union offering `m[16]`/`e[4][4]`/vector views, glm-friendly. 2D fast paths (`OnlyTranslate`, `OnlyScale`, `OnlyScaleAndTranslate`, `RectStaysRect`) keep the common case cheap; `InvertZ0Plane` inverts the z=0 projection specifically (cheaper and more stable than a full 4x4 inverse).
- `Camera` + `Quaternion` (`camera.hpp`, `quaternion.hpp`) supply the view matrix (MVP's V) for 3D-flavored clients.

`Rect` and `RRect` (`rect.hpp`, `rrect.hpp`) are conventional; `RRect::Type` spans kEmpty→kComplex and `IsSimpleRRect` feeds the fast-shape pipeline in [[graphic#Path]].

## Curves

Curve machinery shared by rendering and stroking, in `src/geometry/`:

- `QuadCoeff` / `CubicCoeff` / `ConicCoeff` (`geometry.hpp`) — polynomial evaluators; conics evaluate as a ratio of two quad polynomials.
- `Conic` (`conic.hpp`) — `BuildUnitArc` converts an arc into at most `kMaxConicsForArc=5` conics; `ChopIntoQuadsPOW2` converts one conic into up to 2^5 quads. Ovals/circles are represented exactly as 4 conics (see `Path::AddOval`).
- `Cubic` (`cubic.hpp`) — cubic-specific root/extent helpers.
- Wang's formula (`wangs_formula.hpp`, ported from Skia) — adaptive subdivision depth for quad/cubic/conic under a transform (`VectorXform`), used by both CPU rasterization and GPU tessellation.

## Stroking

Stroking lives here, not in the renderer. `Stroke` (`include/skity/geometry/stroke.hpp`) is the public entry with two modes:

- `Stroke::StrokePath(path, out)` — full-quality outline (caps/joins/miter).
- `Stroke::QuadPath(...)` — a variant that **downgrades conics/cubics to quads**, built for the GPU rasterizers; `keep_cubic` toggles retaining cubics.

The implementation is `PathStroker` (`src/geometry/stroke.cc`, ~1000 lines, declared inside the .cc as a friend of `Path`) with its own quad-intersection machinery (`IntersectRay`, `PtInQuadBounds`, `CheckQuadLinear`). The HW stroke rasterizer (`HWPathStrokeRaster`) is a separate, cheaper approach that inflates each subdivided segment into rectangles — see [[render#Path rasterization strategies]].
