# Text Stack

The text stack (`src/text/`, `include/skity/text/`) covers fonts, glyph generation, and glyph caching, with per-platform ports under `src/text/ports/`. Its defining choice: **no shaping engine** — see [[text#Shaping model]].

## Typeface and Font

`Typeface` (`include/skity/text/typeface.hpp`) is the core font abstraction: cmap queries, table access, variation instancing, scaler-context creation.

It is an `enable_shared_from_this` base with `On*` virtuals: `UnicharsToGlyphs`, table access, `MakeVariation` (variable fonts are first-class via `FontArguments`/`VariationPosition`), `CreateScalerContext`. A `TypefaceID` (atomic uint32) identifies instances. `Font` (`include/skity/text/font.hpp`) pairs a typeface with size/scaleX/skewX and hinting flags (`FontHinting`, `Edging` incl. subpixel), exposing glyph metrics/paths/bits: `GetWidthsBounds`, `LoadGlyphPath`, `LoadGlyphBitmap`, `LoadGlyphBitmapInfo`.

Port implementations of `Typeface`:

- `TypefaceFreeType` (`src/text/ports/typeface_freetype.hpp`) — default glyph engine on Linux/Emscripten/Android/OHOS/Windows-legacy, with `FreetypeFace`/`FreeTypeLibrary` lifetime helpers.
- `TypefaceDarwin` (`src/text/ports/darwin/`, CMake `SKITY_CT_FONT`) — CoreText, with an offscreen `CGContext` for rasterization; public conversion header `ports/typeface_ct.hpp`.
- `TypefaceDWrite` (`src/text/ports/win/typeface_win.cc`) — DirectWrite, with custom `DataFontFileLoader` COM plumbing for in-memory fonts.

## Font managers by platform

`FontManager` (`include/skity/text/font_manager.hpp`) is the Skia-style family/style/matcher API: `MatchFamilyStyle`, `MatchFamilyStyleCharacter` (per-character fallback with bcp47 languages). Implementations:

| Port | Class | Notes |
| --- | --- | --- |
| Android | `FontManagerAndroid` | parses XML font config (`android_fonts_parser`), NDK wrappers, Khmer weight-axis fix |
| Darwin | `FontManagerDarwin` | CoreText |
| Windows legacy | `FontManagerWin` | DWrite enumeration **but FreeType glyph generation** via `MakeFreeTypeTypefaceFromDWriteFontFace` (variation axes bridged, see [[text#Gamma policy]]) |
| Windows DWrite | `FontManagerDWrite` | full DWrite incl. fallback via `IDWriteTextAnalysis`-based source/renderer |
| Harmony | `FontManagerHarmony` | OHOS config parsing |
| Test / empty | `FontManagerTest` / `FontManagerEmpty` | test-font dir and no-op fallback |

On Windows, `Settings::EnableDWriteFontManager()` (`include/skity/utils/settings.hpp`) selects between the two managers at runtime; both compile into the binary. Style matching is `FontStyleSet::MatchStyleCSS3` (`src/text/font_manager.cc`), a CSS3 weight/stretch/style scorer ported from Skia.

Variation-axis fidelity across the DWrite→FreeType bridge is covered in [[text#Gamma policy]].

## Shaping model

There is **no harfbuzz and no in-library shaping**: glyph mapping is a direct cmap lookup.

`TextBlobBuilder::GenerateTextRuns` (`src/text/text_blob.cc`) maps each code point through `Typeface::UnicharToGlyph` — one glyph per code point, positions accumulated from advances; no ligatures, kerning, or bidi.

Complex-script segmentation is the host's job, injected via `TypefaceDelegate` (`include/skity/text/text_blob.hpp`): `Fallback(Unichar, Paint)` picks fallback fonts per character and `BreakTextRun(const char*)` pre-segments runs. `CreateSimpleFallbackDelegate` provides the built-in simple behavior. The runtime model is deliberately minimal: `TextRun` = font + glyph ids + positions; `TextBlob` aggregates runs.

## Scaler context cache

Glyph rasterization results are cached in three tiers, keyed for byte-level equality:

1. `ScalerContextDesc` (`src/text/scaler_context_desc.hpp`) — the cache key itself: `static_assert`-enforced dense, trivially-copyable struct compared with `memcmp`. It folds in text size/flags, subpixel phase, **foreground color** (for gamma pre-blend), and scaler flags such as `kGenerateRawA8MaskFlag`.
2. `ScalerContextCache` — global `LRUCache` (see `src/utils` LRU) mapping desc → `ScalerContextContainer`, purgeable per `TypefaceID`.
3. `ScalerContextContainer` — per-desc `PackedGlyphID → GlyphData` map (mutex-guarded, thread annotations enforced).

`PackedGlyphID` (`src/text/packed_glyph_id.hpp`) packs 16-bit glyph id + 2+2-bit subpixel phase; bit layout follows Skia's `SkPackedGlyphID`.

## Glyph rendering dispatch

`GlyphRun::Make` (`src/render/text/glyph_run.cc`) splits mixed-format runs into A8/RGBA32 lists, then `TextRenderControl::CanUseDirect`/`CanUseSDF` routes each to:

- `DirectGlyphRun` — atlas path (the common case).
- `SDFGlyphRun` — small-text signed distance fields via `sdf::SdfGen::GenerateSdfImage`.
- `PathGlyphRun` — large text (default threshold 256, `Paint`'s `font_fill_threshold_`) rendered as paths through a `DrawPathFunc` callback.
- `TransformedMaskGlyphRun` — bitmap/color emoji, transformed on GPU.

Atlas machinery (`src/render/text/atlas/`): `Atlas`/`AtlasManager` allocate glyph rectangles and upload (`AtlasAllocator` placement); `AtlasManager` also toggles the large-emoji atlas path (`kMaxLargeEmojiTextSize=1024`). The `HWDynamicTextDraw`/`HWDynamicSdfTextDraw` step generation is covered in [[render#Text rendering integration]].

## Gamma policy

Gamma handling is deliberately platform-asymmetric — "gamma at raster time for masks, linear for distance fields":

- Windows applies **A8 mask gamma correction** on both rasterization paths: `ApplyA8MaskGammaForWindows` (`src/text/ports/win/scaler_context_win.cc`) builds sRGB pre-blend LUTs (`BuildA8MaskGammaTables`) and applies them in `ScalerContextDWrite::GenerateImage` and `ScalerContextFreetype::GenerateImage` (`#if defined(SKITY_WIN)`).
- The **SDF path opts out**: `AtlasManager` sets `kGenerateRawA8MaskFlag` in the desc so distance-field generation receives raw linear coverage.
- The Windows-legacy bridge also carries **DWrite variation axes into FreeType** (`GetDWriteVariationDesignPosition` via `IDWriteFontFace5`, then `MakeVariation`), cached in `DWriteTypefaceCache` — variable-font fidelity across the bridge is an explicit goal (regression: `test/ut/text/dwrite_stability_test.cc`, `test/ut/text/a8_mask_gamma_test.cc`).
