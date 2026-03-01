# neuTube 2.0 (“NeuTu”) → Atlas migration

Last updated: 2026-03-01

Doc location: `src/img/NEUTUBE2_MIGRATION.md`

## Executive summary

Atlas already vendors the legacy neuTube/NeuTu codebase under `src/neurolabi/` (C + C++/Qt) and has a CLI entrypoint
via `nim::ZRunNeuTuCommand` (`src/neurolabi/gui/zrunneutucommand.*`). This code works for small-ish stacks, but it inherits
neuTube’s historical limitations:

- Large-scale images are not first-class: many paths assume in-memory `Stack`/`ZStack` and 32-bit indexing.
- Several core utilities are C libraries (`src/neurolabi/c`, `src/neurolabi/lib/genelib`) that constrain portability,
  correctness at scale, and modernization.
- JSON/config handling is based on the legacy `ZJsonObject` stack (jansson-based), while Atlas already standardizes on
  `boost::json` (`src/img/zjson.*`).
- Image I/O and format support lag behind Atlas’ current `zimg` / `ZImgPack` ecosystem.
- SWC I/O/processing in neurolabi uses legacy `ZSwcTree`; Atlas has a newer, 64-bit-friendly SWC model in `nim::ZSwc`
  (`src/img/zswc.*`).

**Goal:** incrementally migrate neuTube functionality into Atlas as “neuTube 2.0” by building a modern, Atlas-native
implementation in `src/img/`, while keeping the legacy code available for comparison and regression-checking until parity
is reached.

## Current status (as of 2026-03-01)

✅ Completed (high-level)

- **Goal 1 (CLI parity + modernization):** `--command` routes to `nim::ZRunNeuTuCommand2` and no longer depends on
  `src/neurolabi/` in the production codepath. Legacy is still used for in-process parity tests.
- **Goal 2 (interactive tracing in Atlas GUI):**
  - **Seed tracing** is integrated in both **2D** and **3D** views via the shared **Trace Settings** state.
  - **Auto Trace** is integrated as a **background task** with progress + cancel in the shared **Tasks** dock.
  - **SWC node context menu parity** (neuTube-style 2D + 3D) is migrated, including the key interactive edit modes.
- **Code consolidation:** the ported neuTube v2 implementation (previously in `src/neutube/`) now lives in `src/img/`
  (no separate `neutube` target).
- **SWC file tools:** `Swc → Rescale SWC...` is migrated as a file-based operation (similar to `Swc → Subtract SWCs...`).

⬜ Next (near-term)

- Large-image readiness (separate milestone):
  - implement an image-access abstraction on `ZImgPack` for tracing/skeletonize so very large volumes can be processed
    without requiring full in-RAM materialization (while preserving strict parity).
- Optional cleanups once parity is stable:
  - Rename/refactor `zneutube*` “ported legacy” files into first-class `z*` APIs (keeping A/B tests intact).

## Primary goals (phased)

### Goal 1 (near-term): modernize the CLI tracing pipeline

Deliver a drop-in replacement for the existing CLI entrypoint:

- Keep the existing `nim::ZRunNeuTuCommand` intact for A/B comparison.
- Introduce `nim::ZRunNeuTuCommand2` with implementation in `src/img/`.
- Remove dependencies on neurolabi C libraries along the `ZRunNeuTuCommand2` codepath.
- Switch JSON/config to `boost::json` (Atlas standard).
- Switch image I/O and processing to Atlas’ `zimg` / `ZImgPack` stack (large-image capable, better format support).
- Switch SWC I/O/processing to `nim::ZSwc` (int64 IDs, modern C++ containers/utilities).

Acceptance criteria for Goal 1:

- CLI supports the same user-facing commands we rely on today (`trace`, `skeletonize`, `compare_swc`, `general:trace_neuron`)
  with equivalent outputs for a curated regression suite.
- **Strict parity requirement:** the tracing/skeletonize/compare algorithms must remain *exactly* the legacy ones. This is a
  mechanical migration (C/C++/Qt legacy → modern C++20 + Atlas libs), not an opportunity to change heuristics or behavior.
- No silent truncation or hard-coded size caps.
- Large images that fit in RAM must work reliably (no 32-bit overflow on voxel counts / byte sizes). Streaming/paging is
  a later milestone; it is *not required* to reach Goal 1.
- Zero dependency on `src/neurolabi/` (including `src/neurolabi/c` and `src/neurolabi/lib/genelib`) for the
  `Atlas --command` runtime path.
  - The legacy code is kept intact under `src/neurolabi/` and is used only for in-process A/B parity tests and as the
    reference baseline until we are ready to remove it.

### Goal 2 (mid-term): migrate interactive tracing into Atlas GUI

neuTube’s manual documents a workflow centered around:

- 2D + 3D windows for image + neuron visualization
- interactive tracing and extensive editing operations
- hotkeys and context menus

We will migrate these behaviors into Atlas’ GUI stack, respecting Atlas threading invariants (rendering-thread-only GL,
UI thread owns widgets) and using structured signals (`QMetaObject::invokeMethod`, queued connections) to cross threads.

Acceptance criteria for Goal 2 (first interactive milestone):

- In Atlas GUI, users can run seeded tracing from both:
  - 2D view (context menu first; optional trace tool mode later)
  - 3D view (left-click trace menu, gated behind an explicit “Trace” tool mode to avoid always-on behavior)
- Tracing runs the already-migrated “legacy-like” algorithm (no new heuristics) and applies results as a single undoable
  operation in `ZSwcDoc`/`ZSwcPack`.
- Tracing respects multi-image scenes:
  - trace actions do **not** rely on active/selected object state (too implicit and unstable).
  - instead, tracing prompts the user to explicitly choose:
    - source image (among visible traceable images under the cursor)
    - source channel (for multi-channel images)
    - SWC destination (new SWC vs attach to an existing SWC)
  - the chosen settings are persisted in `ZDoc::traceSettings()` so 2D/3D windows stay in sync.

#### Goal 2 research notes (legacy neuTube → Atlas mapping)

Key legacy reference points in `src/neurolabi/gui/`:

- Context menu plumbing:
  - `ZStackPresenter::traceTube()` (`src/neurolabi/gui/mvc/zstackpresenter.cpp`)
    - Uses the mouse stack position as the seed.
    - Special-cases Z-projection mode (picks `z = maxIntensityDepth(x, y)`).
    - Calls `ZStackDoc::executeTraceSwcBranchCommand(...)`.
  - `ZStackDoc::executeTraceSwcBranchCommand(...)` (`src/neurolabi/gui/mvc/zstackdoc.cpp`)
    - Runs seeded tracing (`ZNeuronTracer::trace(x, y, z)`) to produce a branch.
    - Optionally auto-connects the branch to the existing SWC set via `ZSwcConnector`.
    - Resamples (`ZSwcResampler`) and pushes a composite undo command (add branch, set parent connection, label trace mask).
  - SWC editing context menu composition:
    - `ZStackDocMenuFactory::makeSwcNodeContextMenu(...)` (`src/neurolabi/gui/zstackdocmenufactory.cpp`)
      lists the full editing action set (delete/break/connect/merge/insert, interpolation, selection helpers,
      “advanced” fixes like remove-turn/resolve-crossover, property changes, measurements).

Existing Atlas integration points we can build on immediately:

- 2D view context menus:
  - `ZGraphicsScene::contextMenuEvent(...)` builds the menu and then calls `ZView::appendContextMenuActions(...)`
    (`src/atlas/zgraphicsscene.cpp`, `src/atlas/zview.cpp`).
  - `ZImgView::appendContextMenuActions(...)` is the natural place to add image-specific actions (currently mostly used
    for Neuroglancer segmentation utilities): `src/atlas/zimgview.cpp`.
- 3D view context menus + left-click listener:
  - `Z3DImgFilter` already registers a left-click event listener named `"trace"` but the handler is currently empty:
    `src/atlas/z3dimgfilter.cpp` (`leftMouseButtonPressed`).
  - `Z3DImgFilter::contextMenuEvent(...)` already computes an image-space hit position (`get3DPosition(...)`).
  - Note: the existing 3D image right-click menu is currently implemented by connecting
    `Z3DImgFilter::showImgContextMenu` to `ZImgPack::show3DImgContextMenu`. For the first trace integration we should
    follow the existing ownership model (signals out of the filter), and then decide whether to centralize menu building
    in view/controller code as a later refactor.
- SWC rendering + selection + undo are already present in Atlas:
  - `ZSwcDoc` owns SWC objects (`ZSwcPack`) and integrates with `QUndoStack`: `src/atlas/zswcdoc.*`
  - `ZSwcPack::contextMenu()` already provides a basic right-click SWC editing menu: `src/atlas/zswcpack.*`
  - 2D + 3D SWC selection is implemented (`ZSwcFilter`, `Z3DSwcFilter`) and stays in sync through the shared doc model.
- Image sources (including large/paged datasets) already have a canonical abstraction in Atlas:
  - `ZImgPack` supports both in-memory and disk-cached/paged reads, and supports region reads (`readRegionToImgAsync(...)`)
    and point sampling (`value(...)` / `displayValue(...)`): `src/atlas/zimgpack.*`

Algorithm-side readiness:

- The CLI migration already contains a faithful port of the seeded trace behavior (“interactive-like”) in C++ on `ZImg`
  + `ZSwc`:
  - `src/img/zneutubetrace.cpp` contains `runSeededTraceLegacyLike(...)` and `runSeededTraceWithHostSwcLegacyLike(...)`
    which are ports of `ZNeuronTracer::trace(x, y, z)` and the “attach-to-host-SWC” logic.
- For GUI integration, we should refactor these into in-memory APIs (no file I/O) that operate on `ZImgPack`/`ZImg` and
  return `ZSwc` deltas suitable for undoable edits.

#### Goal 2 plan (step-by-step, migration-first)

The plan below intentionally prioritizes:

- Minimal new UI surface at first (start from context menus / a trace tool toggle).
- Reuse of existing Atlas doc/undo/view plumbing.
- Strict algorithm reuse (call the already-ported C++ tracing code; do not re-invent heuristics).
- Clear UX rules for multi-image scenes (Atlas can show multiple volumes simultaneously).

Goal 2 implementation status (as of 2026-03-01):

- ✅ Phase 2A (in-memory tracing APIs): implemented
  - `nim::traceSeedNewSwcLegacyLike` and `nim::traceSeedIntoHostSwcLegacyLike`:
    - `src/img/zneutubetraceinteractive.*`
  - Seeded CLI tracing calls the same in-memory API (keeps behavior centralized):
    - `src/img/zneutubetrace.cpp`
- ✅ Phase 2B (2D seeded tracing UX): implemented
  - Shared trace state is persisted in `ZDoc::traceSettings()` so 2D/3D windows stay in sync:
    - `src/atlas/ztracesettings.*`
  - Trace Settings dock widget (shared by 2D + 3D):
    - `src/atlas/ztracesettingswidget.*`
  - 2D left-click trace menu (gated behind a checkable Trace tool so selection stays first):
    - `src/atlas/zgraphicsscene.cpp`
    - `src/atlas/zimgview.cpp`
- ✅ Phase 2C (3D seeded tracing UX): implemented
  - 3D left-click trace menu and seed selection integrated into the 3D canvas workflow:
    - `src/atlas/z3dcanvas.*`
    - `src/atlas/z3dimgfilter.*`
- ✅ Phase 2D (SWC node editing parity, 2D + 3D): implemented
  - 2D SWC-node context menu + edit-mode entry actions:
    - `src/atlas/zswcfilter.*`, `src/atlas/zgraphicsscene.cpp`
  - Shared doc-level SWC-node editing submenus (Delete/Break/Connect/Merge/Insert/Interpolate/Select/Advanced/Property/Info):
    - `src/atlas/zswcpack.*`
  - 3D SWC-node context menu parity wrapper (view actions + cloned doc submenu):
    - `src/atlas/z3dcanvas.cpp`
- ✅ Phase 2E (Auto Trace + background tasks): implemented
  - Background task manager UI (progress + cancel, shared by 2D + 3D windows):
    - `src/atlas/zbackgroundtaskmanager.*`
    - `src/atlas/zbackgroundtaskmanagerwidget.*`
  - Auto Trace pipeline integrated as a background task with cancellation checks:
    - `src/img/zneutubetraceauto.*`
    - `src/img/zneutubeautotraceprocess.*`

Phase 2A: define the GUI-facing tracing API (no UI yet)

1. Add a small in-memory tracing API in `src/img/` that exposes:
   - `traceSeedNewSwcLegacyLike(signal, position, cfg, c, t) -> std::unique_ptr<ZSwc>`
   - `traceSeedIntoHostSwcLegacyLike(signal, hostSwc, position, cfg, c, t) -> TraceMergeResult`
2. Keep these APIs file-free:
   - Accept `const ZImg&` or `const ZImgPack&` (plus voxel coordinate).
   - Accept `TraceConfig` (already in `src/img/zneutubetraceconfig.*`).
3. Make the API explicit about coordinate frames:
   - Inputs are always in image voxel coordinates (x,y,z in the image’s native index space).
   - If any cropping/region-reading is used, the API must define the mapping unambiguously and must not silently change
     behavior.
4. Define a “destination SWC” policy for GUI:
   - Always ask explicitly: new SWC vs attach to an existing SWC.
   - Persist the choice (and the chosen SWC id, when applicable) in `ZDoc::traceSettings()`.
5. Define a “trace source image” policy for GUI:
   - Always ask explicitly: choose among visible traceable images under the cursor.
   - Persist the last choice in `ZDoc::traceSettings()` (used as a default when still applicable).
6. Implement `maxIntensityDepth(x, y)` utility for MIP/2D projection trace seeds (legacy behavior) using `ZImgPack::value`
   or a region read to avoid loading the full volume when possible.
7. Ensure all public APIs follow Atlas pointer-nullability rules (prefer refs; only use `/*nullable*/` pointers where
   semantically required).

Phase 2B: integrate “Trace here” in Atlas 2D view (context menu first)

8. Add a “Trace” submenu to the 2D context menu through `ZImgView::appendContextMenuActions(...)` (`src/atlas/zimgview.cpp`).
9. The action is enabled only when:
   - View is in Normal slice mode (not MIP/Montage for the initial version), and
   - The clicked point is within at least one visible traceable image’s bounds.
   - Source image/channel and SWC target are chosen explicitly via the seed-trace dialog.
10. On trigger:
   - Convert `scenePos` → image voxel coordinate using the owning `ZImgFilter` (use the same mapping approach as
     `cachedNeuroglancerSegmentationIdAtScenePos`, but for grayscale images).
   - For Normal view: seed z = current slice in image space.
11. Use a background worker to run tracing (QtConcurrent or folly executor), then apply results on the UI thread by
    mutating `ZSwcDoc`/`ZSwcPack` with an undo command.
12. Create/extend `ZSwcEditCommand` usage patterns so that tracing is undoable as a single operation.
13. Auto-select the newly added branch nodes so users can immediately edit them (mirrors neuTube’s “trace then edit” flow).
14. Status bar feedback:
    - “Tracing…” while running, “N nodes added” on success, and a clear error on failure.
14a. Optional (to match neuTube UX more closely): add a dedicated “Trace” tool mode in the 2D toolbar.
    - Add `ZView::State::Trace` and a checkable action next to the ROI tool button (`src/atlas/zview.*`,
      `src/atlas/zmainwindow.cpp`).
    - In `ZGraphicsScene::mousePressEvent(...)`, when in Trace mode and the click is not on an SWC node/ROI control point,
      pop the trace menu at the mouse position (or directly run “Trace (new SWC)”).
    - This keeps tracing from being “always on” while still supporting neuTube’s left-click flow.

Phase 2C: integrate the left-click trace menu in Atlas 3D view

15. Use the existing `Z3DImgFilter` left-click listener (`leftMouseButtonPressed`) as the entrypoint for a neuTube-like
    “left click pops trace menu” behavior:
    - Only when Trace mode is enabled (Atlas should not be “always on” like legacy neuTube).
    - Only when at least one visible traceable image can be mapped at the click location (explicit source selection).
    - Only when camera movement is not occurring (check small mouse delta between press/release).
16. Do not build the QMenu inside `ZImgPack` (data object); instead:
    - Add a `Z3DImgFilter` signal like `requestTraceMenu(QPoint globalPos, float x, float y, float z)`.
    - Connect it in the owning 3D view/controller with the corresponding image object id captured.
17. The menu should include at least:
    - “Trace Here...” (opens the shared seed-trace dialog and runs tracing with the chosen source image/channel + SWC target)
    - Optional later: “Trace Settings...” (persistent dock/panel, if we choose to add one)
18. Invoke the same in-memory tracing API as Phase 2B and apply results to `ZSwcDoc`/`ZSwcPack` on completion.

Phase 2D: multi-image UX (Atlas-specific)

19. Add a clear “trace target” indicator in the UI:
    - 2D/3D: display the current `ZDoc::traceSettings()` selection somewhere discoverable (status bar or a future panel).
20. When multiple images are visible:
    - Do not guess a “current” image based on active/selected state.
    - Use the shared seed-trace dialog (`src/atlas/zseedtracedialog.*`) to choose explicitly among candidates.
21. Multi-channel policy:
    - Multi-channel images are supported by selecting a channel in the dialog.
    - No channel-extraction workflow is required.

Phase 2E: SWC editing menu parity expansion (Atlas `ZSwcPack` context menu)

22. Compare neuTube’s SWC node context menu (`src/neurolabi/gui/zstackdocmenufactory.cpp`) to Atlas’ `ZSwcPack` menu and
    port actions incrementally.
23. Add missing “basic editing” actions first:
    - Merge selected nodes
    - Insert node (break edge and insert at interpolated location)
24. Add interpolation actions next (use `ZSwc` geometry utilities, keeping semantics consistent with neuTube):
    - interpolate positions along a branch
    - interpolate radii
    - z-only interpolation
25. Add “advanced editing” actions:
    - remove turn artifacts
    - resolve crossover (if feasible with current `ZSwc` ops; otherwise document as deferred)
    - connect isolated components (Atlas already has MST in `connectSelectedNodes`; can be generalized)
26. Add information/measurement actions:
    - measure path length between selected nodes
    - summarize tree stats
27. Ensure each action is undoable and does not mutate view/render state from the wrong thread.

Phase 2F: large-image readiness (Atlas advantage, but staged)

28. Initial interactive tracing can operate on in-memory `ZImg` (small/medium stacks).
29. Add a staged path for disk-cached/paged images:
    - Use `ZImgPack::readRegionToImgAsync(...)` to fetch a conservative region around the seed.
    - Document clearly if this is an approximation relative to full-volume tracing and guard it behind a preference.
30. Longer-term (post parity): refactor algorithms that fundamentally assume full-volume array access so they can run on
    region readers without changing semantics (this is a separate milestone and may require algorithm-aware paging).

Phase 2G: validation + documentation

31. Add non-GUI tests for the GUI-facing in-memory APIs (seed trace returns deterministic SWC for a fixed `ZImg` fixture).
32. Add a small integration test that simulates the “seed trace” path without GUI by calling the service and checking that
    the resulting SWC matches the legacy in-process baseline for a known test image.
33. Update `docs/USER_GUIDE.md` with the new workflow:
    - selecting a trace target image
    - tracing from 2D/3D
    - editing SWC via context menus
34. Update `docs/DEVELOPER_GUIDE.md` with threading notes for tracing (where the compute runs, how results are applied, how
    undo is recorded).

## Where we are today (repository facts)

### Existing CLI entrypoint

- Atlas `main` special-cases `--command` and invokes the migrated runner:
  - `src/atlas/main.cpp` (search for `--command`)
  - `src/img/zrunneutucommand2.*`
- The legacy runner remains for A/B tests and reference:
  - `src/neurolabi/gui/zrunneutucommand.*`

### Legacy neuTube code location

- C libs: `src/neurolabi/c/`, `src/neurolabi/lib/genelib/`
- Qt/C++ GUI + algorithms: `src/neurolabi/gui/`
- Legacy JSON configs shipped into the app bundle:
  - Source: `src/neurolabi/json/`
  - Copied into Resources via `src/atlas/CMakeLists.txt` custom commands (look for `copy_directory .../neurolabi/json`)

### Legacy JSON config inventory (CLI-relevant)

These files are currently copied to `Resources/json/` and are used (directly or indirectly) by the legacy CLI:

- `command_config.json`
  - Currently only contains sub-objects for `skeletonize` and `trace`, each with an `"include"` key.
  - Command selection is *not* expressed here today; it is primarily selected by CLI flags.
- `trace_config.json`
  - `default` section keys used by the legacy tracer:
    - `minimalScoreAuto`, `minimalScoreManual`, `minimalScoreSeed`, `minimalScore2d`
    - `refit`, `spTest`, `crossoverTest`, `tuneEnd`, `edgePath`, `enhanceMask`
    - `seedMethod`, `recover`, `maxEucDist`
  - `level` section provides per-level overrides (`"1"`, `"2"`, ...).
- `skeletonize.json`
  - `downsampleInterval` (3 integers)
  - `minimalLength`, `finalMinimalLength`, `keepingSingleObject`, `rebase`, `fillingHole`, `maximalDistance`
- Schemas:
  - `skeletonize.schema.json` appears valid and can be used as reference.
  - `trace.schema.json` appears stale/invalid JSON (syntax error) and should be treated as a non-authoritative document
    until repaired.

### Atlas “modern” equivalents we want to converge to

- JSON: `boost::json` helpers `src/img/zjson.h`, `src/img/zjson.cpp`
- Image I/O + region reads: `nim::ZImg`, `nim::ZImgSource` (`src/img/zimg.h`) and `nim::ZImgPack` (`src/atlas/zimgpack.*`)
- SWC: `nim::ZSwc` (`src/img/zswc.h`)
- Large volume paging/rendering reference: `docs/Atlas_Image_Paging_and_Progressive_Rendering.md`

## neuTube manual: GUI feature inventory (for Goal 2 planning)

Reference source: `Manual _ neuTube.html` (downloaded from neutracing.com/manual).

### Terminology (manual)

- Neuron modeled as a tree (nodes + edges), SWC-based.
- “Tracing” = creating a tree model from image data.
- “Stack” = 3D image.
- 2D and 3D windows, with separate “2D editing” and “3D editing”.

### Main menu items explicitly documented (manual)

- `File → Open…` (TIFF or SWC)
- `File → Import → Image Sequence`
- `File → Expand Current → SWC`
- `File → Expand Current → Mask`
- `File → Save Image/Stack`
- `File → Export → Mask`
- `View → Adjust → Brightness/Contrast`
- `View → Object Mode` (Dense / Sparse / Skeleton)
- `View → Autosaved` (crash recovery)
- `View → Information`
- `Tools → Process → Extract Channel`
- `Tools → Process → Invert`
- `Tools → Manage Objects`

### Hotkeys (manual)

General:

- `Cmd/Ctrl + A`: select all SWC nodes
- `Cmd/Ctrl + Z`: undo; `Cmd/Ctrl + Shift + Z`: redo
- `Backspace/Delete`: delete selected objects
- `Space`: extend
- `C`: connect selected SWC nodes
- `N`: connect to nearest SWC node (single selection)
- `B`: break SWC connections
- `V`: enable mouse-move mode for selected objects

2D window:

- `G`: toggle add SWC node
- `A/W/S/D` (+ `Shift` for fast): move image or SWC node
- `=`/`Up`: zoom in; `-`/`Down`: zoom out
- `Left/Right`: move in Z (plane)
- `Q`/`E` (or `<`/`>`): radius down/up
- `F`: move selected nodes to current plane

3D window:

- `=`/`-`: zoom in/out
- arrows: rotate (`Shift + arrows`: move)
- `<`/`>`: radius down/up
- `Cmd/Ctrl + S`: save SWC
- `Cmd/Ctrl +/-`: SWC size scale up/down
- `Cmd/Ctrl + M`: change SWC display mode
- `Z`: locate selected nodes in 2D

This inventory is *not yet an Atlas GUI specification*; it’s a minimum checklist to ensure we don’t lose core neuTube
interaction behaviors during Goal 2.

## Goal 1 design: `ZRunNeuTuCommand2` (CLI modernization)

### What the legacy runner does today (baseline behavior)

Legacy implementation: `src/neurolabi/gui/zrunneutucommand.cpp`.

Key behaviors to preserve (until we intentionally evolve them):

- Command selection via flags (`--trace`, `--skeletonize`, `--compare_swc`, `--general <json|path>`)
- Default config lookup under `Resources/json` (Atlas injects the json dir path via `nim::ZSystemInfo::jsonDirPath()`)
- Optional “json input” mode: `input[0] == "json"` interprets `input[1]` as either a JSON string or a JSON file path
- “general” command wrapper that currently only accepts `{"command":"trace_neuron", ...}`
- Note: the legacy CLI spec string mentions `--compute_seed`, but the current Atlas-integrated runner does not implement
  that command (it resolves to “unknown command”).

### Root causes of current limitations (what we are fixing)

- **Legacy CLI/helper layer**: neurolabi/genelib utilities (e.g. `Process_Arguments(...)` used by some legacy CLI tools in
  `src/neurolabi/cpp/`) pull in legacy C dependencies and are harder to evolve safely than a small, explicit C++ parser.
- **Legacy JSON stack**: `ZJsonObject`/`ZJsonParser` are used for config + input parsing; Atlas already standardizes on
  `boost::json` with additional safety affordances (comment/trailing-comma tolerant parsing).
- **Image model**: core tracing reads images through `ZStack` / `ZStackReader` (legacy stack implementation), which is
  not designed for very large datasets or streaming region reads.
- **SWC model mismatch**: outputs are based on `ZSwcTree`, while Atlas is migrating to `nim::ZSwc` (int64 IDs, modern API).

### Target architecture

`ZRunNeuTuCommand2` should be a thin orchestration layer that delegates to small, testable modules:

- `nim::ZRunNeuTuCommand2` (parsing + validation + dispatch; no neurolabi C dependency)
- Config helpers:
  - `command_config.json` include resolution in `src/img/zrunneutucommand2.cpp`
  - tracing config parsing: `nim::TraceConfig` (`src/img/zneutubetraceconfig.*`)
- Tracing: `nim::runTrace(...)` (`src/img/zneutubetrace.*`, `src/img/zneutubetraceauto.*`)
- Skeletonize: `nim::runSkeletonize(...)` (`src/img/zneutubeskeletonize.*`, `src/img/zneutubeskeletonizer.*`)
- Compare SWC: `nim::runCompareSwc(...)` (`src/img/zneutubecompareswc.*`)

### Compatibility strategy

We keep both runners:

- `nim::ZRunNeuTuCommand` (legacy; **unchanged**; used for in-process parity tests and as the baseline reference)
- `nim::ZRunNeuTuCommand2` (new; backs Atlas `--command`)

Implemented CLI dispatch (current):

- `Atlas --command ...` invokes `nim::ZRunNeuTuCommand2` (new runner).
- The legacy runner is intentionally not exposed as a CLI switch (single clean CLI); invoke it directly in tests/tools
  when you need the baseline.

Status (Goal 1 / CLI):

- `--command` dispatch is implemented in Atlas and runs the v2 runner (`nim::ZRunNeuTuCommand2`).
  - v2 runner now performs its own argument parsing (no `tz_utilities.h`) and uses `boost::json` for parsing:
    - `--general` config JSON
    - `json ...` input payloads
    - `command_config.json` include resolution for skeletonize/trace config file locations
  - Config search path is injected by the host app:
    - `src/atlas/main.cpp` passes `ZSystemInfo::jsonDirPath()` into `ZRunNeuTuCommand2::run(...)`.
    - For standalone testing, v2 runner also accepts `--json_dir <Resources/json>` and/or `--config <command_config.json>`.
  - `--command --skeletonize` is implemented in `src/img/` using Atlas-native image/SWC types:
    - Image I/O: `nim::ZImg` (no `ZStack` / no `Stack`).
    - Algorithm: `nim::ZNeutubeSkeletonizer` (`src/img/zneutubeskeletonizer.*`).
    - Output: `nim::ZSwc` + legacy-format writer (`src/img/zswcwriter.*`) for byte-identical SWC files.
    - The skeletonize implementation path does not include or call into neurolabi C (`tz_*`) or genelib.
  - `--command --trace` is fully ported in `src/img/` (strictly legacy-equivalent behavior):
    - Config parsing: `TraceConfig` (`src/img/zneutubetraceconfig.*`, Boost.JSON; legacy tag + per-level semantics)
    - Image access: `nim::ZImg` (no `ZStack` / no `Stack`)
    - Output: `nim::ZSwc` + legacy-format writer (`src/img/zswcwriter.*`) for byte-identical SWC files
    - Supports the legacy feature set for the CLI entrypoint (seeded trace, host-SWC attach, diagnosis mode, auto trace).
  - `--command --general {"command":"trace_neuron", ...}` is implemented in `src/img/` and runs the ported auto-trace
    pipeline (including legacy return-code semantics and optional predefined mask handling).
  - `src/img/zrunneutucommand2.cpp` remains orchestration-only (CLI + config parsing + dispatch) and the production
    `Atlas --command` codepath has no dependency on neurolabi headers or libraries.
  - API hygiene (non-null parameters):
    - Migrated many legacy-style `T*` parameters to C++ references for required inputs/outputs across the ported code.
    - Remaining pointer parameters are kept only where they are semantically required (nullable optional inputs/outputs,
      C-style arrays, legacy callback signatures) and are annotated/documented (e.g., `/*nullable*/`).

## Detailed plan (step-by-step)

This plan is intentionally incremental; each step should keep the build green and preserve an A/B path.

### Phase A: scaffolding and “no behavior change” plumbing

1. Add tracing-module structure and CMake wiring under `src/img/` (library sources + small unit tests).
2. Implement `nim::ZRunNeuTuCommand2` that:
   - parses CLI arguments without `tz_utilities.h`
   - loads/merges config using `boost::json` (`src/img/zjson.*`)
   - logs equivalent “effective configuration” output for debugging
3. Wire `--command` in `src/atlas/main.cpp` to run the new runner.
4. Preserve the legacy runner unchanged for parity tests, but keep Atlas’ CLI surface clean (single `--command` entrypoint).

Deliverable: new runner exists and can execute end-to-end on the migrated `--command` path.

### Phase B: remove neurolabi C libraries from the command path

We will **not** modify `src/neurolabi/` during this migration. The legacy `neutu` + `neurolabi` codebase remains the
baseline for strict A/B comparisons.

Instead, we progressively **port** the required algorithm code into `src/img/` and then switch `--command` to use the new
implementation module-by-module.

Completed steps in this phase:

5. Remove `tz_utilities.h` usage entirely (arg parsing, file existence, string utilities).
6. Port the required neurolabi C/C++ algorithm code into clean C++ in `src/img/` (and shared pieces into `src/img/`)
   and remove neurolabi dependencies from the production `--command` codepath.

7. Port CLI-used neurolabi C algorithm utilities into clean C++ under `src/img/`:
   - Add `nim::neighborhoodLegacyOrder(int)` (`src/img/zneutubeneighborhood.*`) to represent legacy
     connectivity tables using Atlas' `ZNeighborhood` offsets (in the exact legacy order).
   - This unlocks reusing existing `ZImgNeighborhood*Iterator` helpers (`src/img/zimgneighborhood*iterator.h`) for
     neighbor traversal in the forthcoming C→C++ algorithm ports, avoiding hand-written bound tests while preserving
     legacy ordering (important for strict parity).

#### Phase B dependency inventory (historical; now fully ported)

During the migration, we tracked which legacy neurolabi C surfaces were being pulled in by the tracing/skeletonize CLI
path so we could port them incrementally and verify strict A/B parity as we went.

As of 2026-03-01, the ported neuTube v2 implementation is fully self-contained within `src/img/`: it does not link
against `neutu` and does not include any headers from `src/neurolabi/` in the production `--command` path. The inventory
below is kept as a record of what was ported.

The list below identifies the **minimum** legacy C object-file surfaces that were pulled in by the legacy CLI-used code
path before the port. This served as a practical “what to port first” inventory.

Empirically (from `nm -u` on the legacy `neutu` object files used by the CLI path, mapped via `nm -gA` on
`build/Release/src/neurolabi/c/libneurolabi.a`), the undefined symbol set resolved to the following object files in the
legacy archive (highest impact first):

- `tz_swc_tree.c.o` (SWC tree core; largest dependency surface)
- `tz_stack_bwmorph.c.o` (thinning / morphology)
- `tz_stack_lib.c.o` (core stack ops)
- `tz_locseg_chain.c.o` (local segment chain)
- `tz_trace_utils.c.o` (trace workspace + tracing helpers)
- `tz_stack_threshold.c.o` (threshold / binarize)
- `tz_neuron_structure.c.o` (neuron structure utilities)
- `tz_sp_grow.c.o` (region growing)
- `tz_stack_math.c.o` (stack math utilities)
- `tz_stack_attribute.c.o` (stack attribute helpers)
- `tz_locseg_chain_com.c.o` (chain composition / conversion)
- `image_lib.c.o` (genelib `Stack` / `Image` allocation + lifecycle; used by `Make_Stack`, `Kill_Stack`, etc.)
- `tz_stack_watershed.c.o` (watershed workspace + ops)
- `tz_stack_stat.c.o` (stack statistics)
- `tz_stack_graph.c.o` (stack graph helpers)
- `tz_geo3d_utils.c.o`, `tz_geo3d_scalar_field.c.o` (geometry utilities)
- `tz_local_neuroseg.c.o` (local neuroseg optimize/label)
- `tz_stack_objlabel.c.o` (object labeling)
- `tz_stack_neighborhood.c.o` (neighbor offsets/bounds)
- `tz_mc_stack.c.o` (multi-channel stack helpers)
- `tz_workspace.c.o` (workspace helpers)
- `tz_voxel_linked_list.c.o` (voxel list helpers)
- `tz_stack_sampling.c.o` (sampling)
- `tz_stack_utils.c.o` (misc utilities)
- `tz_fmatrix.c.o` (matrix helpers)
- `tz_int_arraylist.c.o` (arraylist helpers)
- `tz_pixel_array.c.o` (pixel array helpers)
- `tz_locseg_node.c.o` (locseg node helpers)
- `tz_neuron_component.c.o` (neuron component helpers)
- `tz_objdetect.c.o` (object detection helpers)
- `tz_stack.c.o` (basic stack helpers)
- `tz_stack_relation.c.o` (stack relation helpers)
- `tz_voxel_graphics.c.o` (voxel graphics helpers)

This list is kept as a record of the initial dependency surface. If future work (especially Goal 2 GUI migration) pulls
new legacy functionality into the migration path, regenerate the inventory and port incrementally (preserving strict
parity tests as you go).

Deliverable: `--command` is “C-lib free” (on the linking boundary), even if some legacy C++ remains temporarily.

### Phase C: migrate image I/O and large-image support

8. Replace `ZStackReader::Read(...)` / `ZStack::load(...)` usage with:
   - `nim::ZImg` for in-memory small/medium volumes, and/or
   - `nim::ZImgPack` for large volumes and on-demand region reads.
9. Introduce an image-access abstraction used by the tracer:
   - provides voxel reads, small neighborhood reads, and min/max or normalization support
   - uses `size_t`/`int64_t` indices and never assumes `width*height*depth` fits in 32-bit
10. Validate on datasets that exceed 4GB total voxel storage (streaming/paging path).

Deliverable: tracing no longer requires reading the full volume into RAM; large images supported.

### Phase D: migrate SWC model and analysis

11. Switch outputs to `nim::ZSwc` (`src/img/zswc.h`) for:
   - SWC read/write
   - node/edge editing operations used by tracing
12. Port any missing SWC processing/analysis utilities from neurolabi into `src/img/` (or keep tracer-only pieces local),
    keeping `src/img/` as the shared/common layer.

Deliverable: the new CLI produces modern SWC outputs without `ZSwcTree`.

### Phase E: algorithms (tracing + skeletonization)

13. Replace legacy tracer internals with a new tracer implemented in `src/img/` on top of the new image/SWC APIs.
14. Replace skeletonize with a new implementation that:
   - handles binary volumes (including large datasets via tiling)
   - supports similar configuration knobs to legacy (downsample intervals, length thresholds, etc.)
   - does not rely on neurolabi C morphology / distance-map code

Deliverable: no neurolabi dependency on the `--command` path at all.

### Phase F: clean-up and deprecation

15. Once parity + performance are confirmed, decide whether to:
   - keep legacy neurolabi code for other features (FlyEM, etc.), or
   - split it into an optional build feature, or
   - remove it entirely after all features are migrated.

## Validation & regression strategy

We need objective, automated checks to keep the migration safe:

- **Golden-file tests**: run legacy CLI and new CLI on the same small input volumes and compare resulting SWC outputs
  (with tolerances where appropriate).
- **In-tree parity tests (current)**:
  - `zneutubecommand2paritytest`:
    - `NeutubeCommand2Parity.SkeletonizeAndTrace_TiffMatchesLegacy`
    - `NeutubeCommand2Parity.Trace_WithHostSwc_MatchesLegacy`
    - `NeutubeCommand2Parity.Trace_WithHostSwc_NoConnection_MatchesLegacy`
    - `NeutubeCommand2Parity.Trace_DiagnosisSeeded_MatchesLegacy`
    - `NeutubeCommand2Parity.Trace_DiagnosisWithHostSwc_MatchesLegacy`
    - `NeutubeCommand2Parity.Trace_Auto_FromTestData_MatchesLegacy`
    - `NeutubeCommand2Parity.CompareSwc_MatchesLegacy`
    - Invokes the legacy runner and v2 runner in-process and asserts the produced SWC files are byte-identical.
    - Verifies the ported `--compare_swc` path matches the legacy `ZSwcTreeMatcher`-based implementation.
- **Property tests / invariants**:
  - SWC must be connected where expected; no NaNs; IDs unique; parents valid; radii non-negative.
  - No silent truncation for large inputs; operations must fail fast with clear errors if user inputs are invalid.
- **Performance smoke**: time and memory usage on representative datasets; confirm streaming region reads are used for large inputs.

## Open questions (need product/engineering decisions)

1. Config schema: keep legacy JSON schema as-is, or introduce a new schema and a migration layer?
2. Large-image support beyond “fits in RAM”: when to invest in streaming/paging for tracing (Goal 1 does not require this).
3. Legacy removal: when to delete `src/neurolabi/` after we have sufficient GUI + CLI parity confidence.

## Progress tracker (living checklist)

Legend: ⬜ not started, 🟨 in progress, ✅ done

- ✅ Create tracing module structure in `src/img/` (consolidated; no separate `neutube` target)
- ✅ Add `nim::ZRunNeuTuCommand2` entrypoint + `--command` dispatch
- ✅ Migrate JSON parsing to `boost::json` for v2 runner
- ✅ Remove `tz_utilities.h` from v2 runner
- ✅ Use `ZImg` for image I/O in v2 runner
- ✅ No stack adapters: tracing/skeletonize operate directly on `ZImg`/`ZSwc` (production `--command` path).
- ✅ Remove `ZSystemInfo` dependency from v2 runner (json dir injected by host / `--json_dir`)
- ✅ Remove neurolabi dependency from the production `--command` codepath (no `src/neurolabi/` headers/libraries)
- ✅ Add in-process A/B parity tests for `--command` (skeletonize + trace + compare_swc)
- ✅ Consolidate A/B tests by invoking both runners in-process (legacy + v2)
- ✅ Port the CLI-used neurolabi C algorithms to clean C++ (exact behavior, 64-bit-safe sizes)
  - ✅ Neighborhood/connectivity tables: `src/img/zneutubeneighborhood.*`
  - ✅ Binary morphology helpers (`Stack_Not`, `Stack_Majority_Filter`, `Stack_Fill_Hole_N`): `src/img/zneutubeimgbwmorph.*`
  - ✅ Point sampling + mask hit (`Stack_Point_Sampling`, `Stack_Point_Hit_Mask`): `src/img/zneutubeimgsampling.*`
  - ✅ Geo3d scalar field sampling + scoring (`Geo3d_Scalar_Field_Stack_Sampling*`, `Geo3d_Scalar_Field_Stack_Score*`):
    `src/img/zneutubegeo3dscalarfield.*`, `src/img/zneutubestackfitscore.*`
  - ✅ Geo3d scalar field centroid (`Geo3d_Scalar_Field_Centroid`): `src/img/zneutubegeo3dscalarfield.*`
  - ✅ Geo3d orientation utilities (`Vector_Angle`, `Geo3d_*_Orientation`, `Geo3d_Rotate_Orientation`):
    `src/img/zneutubegeo3dutils.*`, `src/img/zneutube3dgeom.*`
  - ✅ Legacy optimizer core (`Fit_Perceptor`, line search, conjugate updates):
    `src/img/zneutubecontfun.h`, `src/img/zneutubeoptimizeutils.*`, `src/img/zneutubeperceptor.*`
    - Note: Atlas already has Ceres-based optimizers (e.g. `src/img/zregistrationoptimizer.*`), but we intentionally keep
      the legacy Perceptor algorithm here to preserve byte-identical tracing outputs for Goal 1.
  - ✅ Trace workspace mask/bounds helpers (`Trace_Workspace_Mask_Value`, `Trace_Workspace_Point_In_Bound`):
    `src/img/zneutubetraceworkspace.*`
  - ✅ Trace record + score containers (`Trace_Record`, `Stack_Fit_Score` structs + setters/getters):
    `src/img/zneutubetracerecord.*`
  - ✅ Stack fit score switches (`STACK_FIT_*` option scoring on sampled arrays, including masked-score semantics):
    `src/img/zneutubestackfitscore.*`, `src/img/zneutubestackfitoptions.h`
  - ✅ Large-object labeling (`Stack_Label_Large_Objects_*`): `src/img/zneutubeobjlabel.*` (parity-tested)
  - ✅ 3D squared EDT (`Stack_Bwdist_L_U16` / `dt3d_binary_mu16`): `src/img/zneutubeedt3d.*` (parity-tested)
  - ✅ Planar squared EDT (`Stack_Bwdist_L_U16P`): `src/img/zneutubeplanaredt.*`
  - ✅ Local maxima (`Stack_Local_Max`, `Stack_Locmax_Region`): `src/img/zneutubeimglocmax.*` (parity-tested)
  - ✅ Sp-grow (`Stack_Sp_Grow` + parser): `src/img/zneutubespgrow.*`, `src/img/zneutubespgrowparser.*`
  - ✅ Neuroseg field generation (`Neuroseg_Slice_Field`, `Neuroseg_Slice_Field_P`, `Neuroseg_Field_S_Fast`, `Neuroseg_Field_Sp`):
    `src/img/zneutubeneuroseg.*`, `src/img/zneutube3dgeom.*`, `src/img/zneutubegeo3dpointarray.*`
  - ✅ Local neuroseg scoring + fit (`Local_Neuroseg_Field_S*`, `Local_Neuroseg_Score_*`, `Fit_Local_Neuroseg_W`):
    `src/img/zneutubelocalneuroseg.*`
  - ✅ Local neuroseg optimize loop (`Local_Neuroseg_Position_Adjust`, `Local_Neuroseg_Orientation_Search_C`,
    `Local_Neuroseg_R_Scale_Search`, `Local_Neuroseg_Optimize_W`): `src/img/zneutubelocalneuroseg.*`
  - ✅ Legacy `darray_qsort(...)` parity helper: `src/img/zneutubedarrayqsort.*`
  - ✅ darray math helpers (`darray_dot_n`, `darray_dot_nw`, `darray_sum_n`, `darray_mean_n`, `darray_corrcoef_n`, `darray_max`):
    `src/img/zneutubedarraymath.*`
- ⬜ Implement image-access abstraction on `ZImg`/`ZImgPack` (streaming for very large volumes)
- ✅ Port/implement skeletonize on modern image types (exact behavior)
  - ✅ Port `ZStackSkeletonizer::makeSkeletonWithoutDs(Stack*, const int*)` behavior into
    `nim::ZNeutubeSkeletonizer` (`src/img/zneutubeskeletonizer.*`) using only `ZImg` + `ZSwc`.
  - ✅ Switch `--command --skeletonize` dispatch to the v2 runner implementation (`src/img/zneutubeskeletonize.*`).
  - ✅ Port SWC primitives used by skeletonize:
    `src/img/zswcops.*`, `src/img/zswcpointdist.*`, `src/img/zneutubeswcreconnect.*`,
    `src/img/zswcresampler.*`, `src/img/zneutubeswcregionsampling.*`, `src/img/zswcwriter.*`
- ✅ Port `--compare_swc` to `nim::ZSwc` (exact legacy `ZSwcTreeMatcher::matchAllG` semantics)
  - Implementation: `src/img/zneutubecompareswc.*`
  - Note: legacy `ZSwcLayerTrunkAnalyzer::extractTrunk(...)` is a stub (returns an empty path). The port preserves this
    behavior exactly, including the `-1.0` gap-penalty score contributions that fall out of matching empty branches.
  - Parity: `test/zneutubecommand2paritytest.cpp` (`NeutubeCommand2Parity.CompareSwc_MatchesLegacy`)
- ✅ Switch SWC I/O + processing to `nim::ZSwc`
  - ✅ Skeletonize output uses `nim::ZSwc` + a legacy-format writer for byte-identical SWC files
  - ✅ Trace output migration status
    - ✅ Seeded trace (position provided, no host SWC) uses `nim::ZSwc` + legacy-format writer
    - ✅ Seeded trace with host SWC attach (position provided, host SWC provided) uses `nim::ZSwc`
      end-to-end (legacy SWC parse ordering, SWC->mask labeling, and branch attach/connector semantics)
    - ✅ Auto trace and diagnosis use the ported tracer on `ZImg` + `ZSwc` (`src/img/zneutubetraceauto.*`)
- ✅ Add parity tests and sample datasets for regression checking (in-tree A/B parity + `atlas_test_data`)
- ✅ Document GUI parity targets and migrate the initial Atlas GUI integration (Goal 2 baseline)
  - ✅ Trace Settings + seeded tracing in 2D and 3D views
  - ✅ Background Tasks dock + Auto Trace integration
  - ✅ SWC node context menu parity (2D + 3D)
- ✅ Consolidate former `src/neutube/` implementation into `src/img/` and remove the `neutube` target
- ✅ Add `Swc → Rescale SWC...` (file-based) to match neuTube's SWC-wide utilities

## Appendix: neuTube → Atlas SWC Node Context Menu Parity

Merged from `docs/NEUTUBE_SWC_CONTEXT_MENU_PARITY.md` on 2026-03-01.

This section maps neuTube’s **SWC node** context menus (2D + 3D) to Atlas and tracks migration parity.

Scope:
- Right-click context menu shown when interacting with **SWC nodes**.
- neuTube reference: `~/code/neutu/neurolabi/gui` (not the `src/neurolabi` folder inside this repo).

Non-goals (for this section):
- Non-node SWC menus (tree/forest menus).
- Non-SWC menus (image/process/body/puncta).

### neuTube 2D SWC node context menu (ZStackPresenter)

Source:
- Menu composition: `neurolabi/gui/mvc/zstackpresenter.cpp` (`ZStackPresenter::createSwcNodeContextMenu`)
- Presenter (view) actions: `neurolabi/gui/zmenufactory.cpp` (`ZMenuFactory::makeSwcNodeContextMenu(ZStackPresenter*, ...)`)
- Doc actions/submenus: `neurolabi/gui/zmenufactory.cpp` (`ZMenuFactory::makeSwcNodeContextMenu(ZStackDoc*, ...)`)
- Labels/shortcuts: `neurolabi/gui/zactionfactory.cpp`

Order (top → bottom):
1) Extend (Space)
2) Connect to (C)
3) Move to Current Plane (F)
4) Move Selected (Shift+Mouse) (V)
5) Estimate Radius
6) Delete (X)
7) Delete Unselected
8) Break (B)
9) Connect (C)
10) Merge
11) Insert (I)
12) Interpolate >
    - Position and Radius
    - Z
    - Position
    - Radius
13) Select >
    - Downstream
    - Upstream
    - Neighbors
    - Host branch
    - All connected nodes
    - All nodes (Ctrl+A)
14) Advanced Editing >
    - Remove turn
    - Resolve crossover
    - Join isolated branch
    - Join isolated brach (across trees)
    - Reset branch point
15) Change Property >
    - Translate
    - Change size
    - Set as a root
16) Information >
    - Summary
    - Path length
    - Scaled Path length
17) Separator
18) Add Neuron Node (G)
19) Locate node(s) in 3D

Notes:
- Items 1–5 are **interaction-mode entry** actions in neuTube (they switch the presenter into an SWC edit mode). They are
  not checkable in the menu; neuTube shows a status message and exits the mode on right-click.

#### 2D view-action semantics (neuTube reference)

These are not just “commands” in neuTube: they enter an interactive mode managed by `ZInteractiveContext`
and then react to subsequent clicks/drags.

- **Extend** (`ACTION_EXTEND_SWC_NODE`, `ZStackPresenter::enterSwcExtendMode()`):
  - Enters `ZInteractiveContext::SWC_EDIT_EXTEND`.
  - Status tip (neuTube): “Left click to extend. Path calculation is off when 'CTRL' is pressed. Right click to exit extending mode.”
  - Next **left-click release**:
    - If `Ctrl` held: `OP_SWC_EXTEND` → `ZStackDoc::executeSwcNodeExtendCommand(center, radius)` (plain extend).
    - Else, if click hits an SWC node: selection changes (no extend).
    - Else: `OP_SWC_SMART_EXTEND` → `ZStackDoc::executeSwcNodeSmartExtendCommand(center, radius)` (path computation).
- **Connect to** (`ACTION_CONNECT_TO_SWC_NODE`, `ZStackPresenter::enterSwcConnectMode()`):
  - Enters `ZInteractiveContext::SWC_EDIT_CONNECT`.
  - Next left-click release: `OP_SWC_CONNECT_TO` → `ZStackDoc::executeConnectSwcNodeCommand(prevNode, targetNode)`,
    then exits SWC edit mode.
- **Move to Current Plane** (`ACTION_CHANGE_SWC_NODE_FOCUS`, `ZStackPresenter::changeSelectedSwcNodeFocus()`):
  - Immediate command: `ZStackDoc::executeSwcNodeChangeZCommand(currentSliceZ)`.
- **Move Selected (Shift+Mouse)** (`ACTION_MOVE_SWC_NODE`, `ZStackPresenter::enterSwcMoveMode()`):
  - Enters `ZInteractiveContext::SWC_EDIT_MOVE_NODE`.
  - User holds `Shift` and drags with left button: mapper emits `OP_MOVE_OBJECT` while in `INTERACT_SWC_MOVE_NODE`.
- **Estimate Radius** (`ACTION_ESTIMATE_SWC_NODE_RADIUS`, `ZStackPresenter::estimateSelectedSwcRadius()`):
  - Immediate command: `ZStackDoc::executeSwcNodeEstimateRadiusCommand()`.

#### Atlas implementation pointers (current)

These pointers are here so reviewers can quickly find the current Atlas ports and compare with the neuTube reference above:

- 2D node context menu composition: `src/atlas/zswcfilter.cpp` (`popupSwcNodeContextMenu`).
- 2D SWC edit modes (Extend/Connect-to/Add-node/Move-selected): `src/atlas/zgraphicsscene.cpp`.
- 2D SWC pack edit helpers (undoable): `src/atlas/zswcpack.cpp`.
- Shared doc-level SWC submenus (Delete/Break/Connect/Merge/Insert/Interpolate/Select/Advanced/Change Property/Information):
  `src/atlas/zswcpack.cpp` (`createContextMenu`).
- 3D node context menu composition (UI thread): `src/atlas/z3dcanvas.cpp` (`Z3DCanvas::showSwcNodeContextMenu`).
- 3D SWC interaction modes (render thread): `src/atlas/z3dswcfilter.cpp` (`Z3DSwcFilter::selectSwc`).

### neuTube 3D SWC node context menu (Z3DWindow)

Source:
- Menu composition: `neurolabi/gui/z3dwindow.cpp` (`Z3DWindow::makeSwcContextMenu`)
- View actions: `neurolabi/gui/zmenufactory.cpp` (`ZMenuFactory::makeSwcNodeContextMenu`)
- Doc actions/submenus: `neurolabi/gui/zmenufactory.cpp` (`ZStackDocMenuFactory::makeSwcNodeContextMenu`)
- Labels/shortcuts: `neurolabi/gui/zactionfactory.cpp`

Order (top → bottom):
1) Extend (Space) (toggle)
2) Connect to (C)
3) Move Selected (Shift+Mouse) (V) (toggle)
4) Doc actions/submenus (same as 2D doc section, items 6–16 above)
5) Separator
6) Locate node(s) in 2D
7) Change type
8) Add neuron node (toggle)

Notes:
- Items 1, 3, and 8 are **toggle** actions that represent active interaction modes.
- `Tree` action is added in code but hidden by `customizeContextMenu()` in the default app; it does not appear in typical UX.
- 3D menu does **not** include “Move to Current Plane” or “Estimate Radius” (2D-only).
- neuTube 3D builds the doc submenu title as **"Intepolate"** (typo) in `ZStackDocMenuFactory`; 2D uses **"Interpolate"**.

#### 3D view-action semantics (neuTube reference)

- **Extend** (toggle, `Z3DWindow::toogleSmartExtendSelectedSwcNodeMode(bool)`):
  - Sets `Z3DSwcFilter::EInteractionMode` to `SmartExtendSwcNode` when stack data exists, otherwise `PlainExtendSwcNode`.
  - Sets canvas `ZInteractiveContext::SWC_EDIT_SMART_EXTEND`.
  - Mutually exclusive with “Add neuron node” mode (turns it off when enabled).
  - Actual execution is driven by a **volume click** callback (`Z3DWindow::pointInVolumeLeftClicked(...)`):
    - Preconditions (legacy): `hasVolume()`, `channelNumber() == 1`, extend toggle checked, and exactly one SWC node selected.
    - If `Ctrl` held: `ZStackDoc::executeSwcNodeExtendCommand(clickPos)`.
    - Else: `ZStackDoc::executeSwcNodeSmartExtendCommand(clickPos)`.
- **Connect to** (`Z3DWindow::startConnectingSwcNode()`):
  - Sets `Z3DSwcFilter::EInteractionMode::ConnectSwcNode`, `SWC_EDIT_CONNECT`.
  - Next click on target node triggers `Z3DWindow::connectSwcTreeNode(tn)` which runs
    `ZStackDoc::executeConnectSwcNodeCommand(closestSelectedNode, tn)` and exits mode.
- **Move Selected** (toggle, `Z3DWindow::toogleMoveSelectedObjectsMode(bool)`):
  - Delegates to the interaction handler (`setMoveObjects(checked)`); used with `Shift+Mouse`.
  - The interaction handler emits `objectsMoved(dx, dy, dz)` continuously while dragging; neuTube pushes
    `ZStackDocCommand::ObjectEdit::MoveSelected` and relies on `mergeWith()` to coalesce the drag into a single undo item.
- **Add neuron node** (toggle, `Z3DWindow::toogleAddSwcNodeMode(bool)`):
  - Sets `Z3DSwcFilter::EInteractionMode::AddSwcNode`, `SWC_EDIT_ADD_NODE`.
  - Mutually exclusive with “Extend” (turns it off when enabled).
  - On background click (hit nothing) with node picking enabled, neuTube picks a nearby SWC node to define a depth,
    projects that node position onto the click ray, and adds a new SWC node at the projected position with the
    picked node's radius (`Z3DSwcFilter::selectSwc` emits `addNewSwcTreeNode(x, y, z, r)`).

#### 3D "Change type" dialog (neuTube reference)

Source:
- Dialog: `neurolabi/gui/dialogs/swctypedialog.*` (`SwcTypeDialog`)
- 3D invocation + apply logic: `neurolabi/gui/z3dwindow.cpp` (`Z3DWindow::changeSelectedSwcNodeType`)

UI elements:
- Title: "Change Swc Type"
- Type: `QSpinBox` (`0..65535`)
- Picking modes (radio buttons):
  - Individual (default)
  - Downstream
  - Connection
  - Main trunk
  - Trunk level
  - Branch level
  - Traffic
  - Longest leaf
  - Furthest leaf
  - Root
  - Subtree

Visibility rules:
- When invoked with `ZSwcTree::SWC_NODE`:
  - **Shown**: Individual, Downstream, Connection, Branch level, Longest leaf, Furthest leaf
  - **Hidden**: Main trunk, Traffic, Trunk level, Root, Subtree
- When invoked with `ZSwcTree::WHOLE_TREE`:
  - **Shown**: Individual, Main trunk, Traffic, Trunk level, Root, Subtree
  - **Hidden**: Connection, Downstream

Apply semantics used by the 3D SWC-node context menu:
- `INDIVIDUAL`: set selected node(s) type to `dlg.type()`
- `DOWNSTREAM`: set downstream type to `dlg.type()`
- `CONNECTION`: set upstream type to `dlg.type()` until common ancestor
- `LONGEST_LEAF`: set path type from selected node to furthest node (geodesic)
- Other dialog modes may be visible in the dialog but are not handled in `Z3DWindow::changeSelectedSwcNodeType()`.

### Atlas parity checklist

Legend:
- ✅ implemented (behavior + UI parity)
- 🟡 implemented but not parity
- ❌ missing

#### Shared doc-level SWC actions
- Delete: ✅ (ported legacy semantics: children become new roots)
- Delete Unselected: ✅ (ported legacy semantics: children become new roots)
- Break: ✅ (ported legacy semantics: selected-child links detach to master-root/forest root)
- Connect: ✅ (ported legacy ZSwcConnector MST + minDist semantics)
- Merge: ✅ (ported legacy MergeSwcNode semantics)
- Insert: ✅ (ported legacy insert-between-adjacent-selected semantics)
- Interpolate submenu: ✅ (ported legacy plane-path interpolation semantics)
- Select submenu: ✅ (ported legacy downstream/upstream/branch/connected semantics)
- Advanced Editing submenu:
  - Remove turn: ✅
  - Resolve crossover: ✅ (ported legacy matching/rewire)
  - Join isolated branch: ✅
  - Join isolated brach (across trees): 🟡 (logic parity; Atlas does not auto-remove empty SWC objects)
  - Reset branch point: ✅
- Change Property submenu:
  - Translate: ✅ (ZSwcSkeletonTransformDialog)
  - Change size: ✅ (ZSwcSizeDialog; single dialog)
  - Set as a root: ✅
- Information submenu:
  - Summary: ✅ (ZInformationDialog)
  - Path length: ✅ (ZInformationDialog)
  - Scaled Path length: ✅ (ZResolutionDialog + ZInformationDialog)

#### 2D-only SWC actions
- Extend: ✅ (plain extend + smart extend/path computation)
- Connect to: ✅
- Move to Current Plane: ✅
- Move Selected (Shift+Mouse): ✅
- Estimate Radius: ✅ (signal-fit legacy-like)
- Add Neuron Node: ✅
- Locate node(s) in 3D: ✅

#### 3D-only SWC actions
- Extend (toggle): ✅ (smart vs plain matches stack-data presence)
- Move Selected (toggle): ✅ (continuous move merges undo like neuTube)
- Locate node(s) in 2D: ✅
- Change type (SwcTypeDialog): ✅ (apply semantics match neuTube 3D)
- Add neuron node (toggle): ✅ (depth from nearby node, project onto click ray)

### Atlas regression tests

- `test/zswcpackundomergetest.cpp`:
  - Verifies neuTube-like undo merge behavior for repeated move-selected edits.
  - Verifies doc-level SWC node context menu action ordering/labels (Delete/Break/Connect/…).

### Atlas 3D implementation note (threading)

In Atlas, `Z3DRenderingEngine` runs on a dedicated rendering thread (see `src/atlas/z3dmainwindow.cpp`), so any 3D
context menus and dialogs must be created on the UI thread. The current 2D SWC node menu is already UI-thread
safe, but the 3D SWC node menu needs to be implemented via a signal/request to `Z3DCanvas` (similar to how the
3D seed-trace menu is shown) to avoid creating `QMenu`/`QDialog` on the rendering thread.

Status:
- Atlas now shows the 3D SWC node context menu via `Z3DCanvas::showSwcNodeContextMenu()` (UI thread).

### Migration plan (step-by-step)

This is the concrete plan/checklist used to reach parity. Items are marked complete as of this change.

1. [x] Locate neuTube 2D SWC node menu composition (`ZStackPresenter::createSwcNodeContextMenu`).
2. [x] Extract the ordered action list and shortcuts (via `ZActionFactory`).
3. [x] Locate neuTube doc-level submenu construction (`ZMenuFactory::makeSwcNodeContextMenu(ZStackDoc*, ...)`).
4. [x] Locate neuTube 3D SWC node menu composition (`Z3DWindow::createContextMenu`).
5. [x] Locate neuTube 3D doc-level SWC menu factory (`ZStackDocMenuFactory::makeSwcNodeContextMenu`).
6. [x] Write this parity mapping doc (2D + 3D menus, order, semantics, sources).
7. [x] Implement Atlas 2D node context menu composition (`ZSwcFilter::popupSwcNodeContextMenu`).
8. [x] Implement Atlas 2D interactive SWC edit modes (Extend/Connect-to/Move-selected/Add-node) with neuTube semantics.
9. [x] Implement/port shared doc-level SWC actions: Delete / Delete Unselected / Break / Connect / Merge / Insert.
10. [x] Implement/port Interpolate submenu actions (position/radius/Z).
11. [x] Implement/port Select submenu actions (downstream/upstream/branch/connected/all).
12. [x] Implement/port Advanced Editing: Remove turn.
13. [x] Implement/port Advanced Editing: Resolve crossover.
14. [x] Implement/port Advanced Editing: Join isolated branch.
15. [x] Implement/port Advanced Editing: Join isolated brach (across trees).
16. [x] Implement/port Advanced Editing: Reset branch point.
17. [x] Implement/port Change Property: Translate (single dialog).
18. [x] Implement/port Change Property: Change size (single dialog).
19. [x] Implement/port Change Property: Set as a root.
20. [x] Implement/port Information: Summary / Path length.
21. [x] Implement/port Information: Scaled path length (resolution prompt + report).
22. [x] Implement Atlas 3D node context menu on UI thread (`Z3DCanvas::showSwcNodeContextMenu`).
23. [x] Implement Atlas 3D interaction-mode toggles (Extend / Move-selected / Add-node) matching neuTube behavior.
24. [x] Implement 2D↔3D locate actions (Locate node(s) in 3D / Locate node(s) in 2D).
25. [x] Add regression tests (undo-merge + menu ordering/labels) and wire them into CTest.
26. [x] Build and run `ctest --output-on-failure`.
