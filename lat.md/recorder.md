# Recorder and Display Lists

The recorder (`src/recorder/`, `include/skity/recorder/`) captures Canvas calls into a compact, replayable display list. It is the cross-thread artifact: record anywhere, replay on the render thread.

The format supports spatial queries, partial redraw, and persistence — see the sections below.

## Recording flow

`PictureRecorder` (`include/skity/recorder/picture_recorder.hpp`) drives the lifecycle: `BeginRecording(bounds, options)` creates a `RecordingCanvas`, drawing happens through `GetRecordingCanvas()`, `FinishRecording()` returns a `DisplayList`.

`RecordingCanvas` (`src/recorder/recording_canvas.cc`) overrides every `OnDraw*`/`OnClip*`/matrix/save/restore hook and packs each op into a `DisplayListBuilder`-owned buffer via a private `Push<T>` template. `DisplayListBuildOptions` currently exposes `build_rtree`.

## Display list format

The serialized form is a single contiguous byte stream, not an array of op objects:

- `RecordedOp` (`src/recorder/recorded_op.hpp`) is a variable-length header — `type : 8` bits, `size : 24` bits — walked linearly by size. The op catalog is the `FOR_EACH_RECORDED_OP` X-macro listing 27 ops (Save/Restore/matrix ops/clips/Draw*/SaveLayer/DrawTextBlob/DrawImage/DrawGlyphs).
- `DisplayListStorage` (`src/recorder/display_list.cc`) owns the malloc'd, realloc-grown buffer; `DisplayList` walks it to `Draw(Canvas*)`, to dispose ops, or to fetch a paint by op offset.
- `DisplayListBuilder` (`src/recorder/display_list_builder.hpp`) tracks `save_op_stack_` and back-fills `SetSaveRestoreOffset()` so Save ops jump straight to their matching Restore; `spatial_ops_` (rect→offset pairs) feeds the R-tree.

Why bytes instead of virtual op objects: replay is a linear scan, ops stay cache-friendly, and the buffer is one allocation — cheaper to move across threads than an object graph.

## Partial redraw

Two indexing structures turn the flat stream into spatially queryable data:

- `DisplayListRTree` (`src/recorder/display_list_rtree.cc`) — bounding-box R-tree over ops, built from `spatial_ops_`; powers `DisplayList::Search(Rect)` and cull-rect replay (`Draw` accepts a cull rect, exposed in the C API as the display-list partial-redraw entry).
- `DisplayListRegion` (`src/recorder/display_list_region.cc`) — span-based dirty region (Flutter-flow lineage, dual copyright header) with span buffers and ordered accumulation, for computing what to redraw.

`DisplayList::Property` bit flags (kSaveLayer/kShader/kColorFilter/kMaskFilter/kImageFilter) let consumers cheaply test whether a list uses a given effect class.

## Serialization

Display lists persist in Skia-picture ("SKP"-adjacent) form via `module/io`.

Picture assembly lives in `module/io/src/picture.cc`, op-tagged playback in `module/io/src/record/` (`record_playback.cc`, `draw_type.hpp`), and flattenable codecs for paint/path/shader/matrix/rrect/vertices/font under `module/io/src/io/flat/`. Effect flattening relies on [[effect#Flattenable serialization]]. Golden coverage of record/replay lives in `test/golden/cases/recorder` (see [[tests#Golden tests]]).
