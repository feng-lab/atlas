# neuTube 2.0 (“NeuTu”) → Atlas migration

Last updated: 2026-02-24

Doc location: `src/neutube/NEUTUBE2_MIGRATION.md`

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

**Goal:** incrementally migrate neuTube functionality into Atlas as “neuTube 2.0” by building a new implementation under
`src/neutube/`, while keeping the legacy code available for comparison and regression-checking until parity is reached.

## Primary goals (phased)

### Goal 1 (near-term): modernize the CLI tracing pipeline

Deliver a drop-in replacement for the existing CLI entrypoint:

- Keep the existing `nim::ZRunNeuTuCommand` intact for A/B comparison.
- Introduce `nim::ZRunNeuTuCommand2` with implementation in `src/neutube/`.
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
- Zero dependency on `src/neurolabi/c` and `src/neurolabi/lib/genelib` in the new codepath.
  - During migration, `--command2` is allowed to call legacy `neutu` algorithms temporarily (for parity scaffolding), but
    Goal 1 is only considered complete once we have removed the `neutu` dependency from the `neutube` target.

### Goal 2 (mid-term): migrate interactive tracing into Atlas GUI

neuTube’s manual documents a workflow centered around:

- 2D + 3D windows for image + neuron visualization
- interactive tracing and extensive editing operations
- hotkeys and context menus

We will migrate these behaviors into Atlas’ GUI stack, respecting Atlas threading invariants (rendering-thread-only GL,
UI thread owns widgets) and using structured signals (`QMetaObject::invokeMethod`, queued connections) to cross threads.

## Where we are today (repository facts)

### Existing CLI entrypoint

- Atlas `main` special-cases `--command` and directly invokes the legacy runner:
  - `src/atlas/main.cpp` (search for `--command`)
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

- **C argument parsing and legacy helpers**: `tz_utilities.h` provides `Process_Arguments(...)` and friends. This drags in
  neurolabi C code and is hard to evolve safely.
- **Legacy JSON stack**: `ZJsonObject`/`ZJsonParser` are used for config + input parsing; Atlas already standardizes on
  `boost::json` with additional safety affordances (comment/trailing-comma tolerant parsing).
- **Image model**: core tracing reads images through `ZStack` / `ZStackReader` (legacy stack implementation), which is
  not designed for very large datasets or streaming region reads.
- **SWC model mismatch**: outputs are based on `ZSwcTree`, while Atlas is migrating to `nim::ZSwc` (int64 IDs, modern API).

### Target architecture

`ZRunNeuTuCommand2` should be a thin orchestration layer that delegates to small, testable modules:

- `neutube::CommandLine` (parsing + validation; no neurolabi C dependency)
- `neutube::Config` (boost::json parsing, include-expansion, schema validation)
- `neutube::Trace` (core tracing invocation; image access via `ZImg`/`ZImgPack`)
- `neutube::Skeletonize` (binary/object skeletonization on `ZImg` / `ZSkeleton` / `ZSwc`)
- `neutube::CompareSwc` (similarity; should migrate off `ZSwcTreeMatcher` when ready)

### Compatibility strategy

We keep both runners:

- `nim::ZRunNeuTuCommand` (legacy; **unchanged**)
- `nim::ZRunNeuTuCommand2` (new)

Recommended CLI dispatch (proposal):

- Keep `--command` for legacy (no behavior change).
- Add a new top-level switch `--command2` to invoke the new runner.

This avoids “silent behavior drift” for existing pipelines while enabling side-by-side comparison.

Status:

- `--command2` dispatch is implemented in Atlas and runs a dedicated v2 runner (`nim::ZRunNeuTuCommand2`).
  - v2 runner now performs its own argument parsing (no `tz_utilities.h`) and uses `boost::json` for parsing:
    - `--general` config JSON
    - `json ...` input payloads
    - `command_config.json` include resolution for skeletonize/trace config file locations
  - Config search path is injected by the host app:
    - `src/atlas/main.cpp` passes `ZSystemInfo::jsonDirPath()` into `ZRunNeuTuCommand2::run(...)`.
    - For standalone testing, v2 runner also accepts `--json_dir <Resources/json>` and/or `--config <command_config.json>`.
  - `--command2 --skeletonize` is now implemented in `src/neutube/` using Atlas-native image/SWC types:
    - Image I/O: `nim::ZImg` (no `ZStack` / no `Stack`).
    - Algorithm: `nim::neutube::ZNeutubeSkeletonizer` (`src/neutube/zneutubeskeletonizer.*`).
    - Output: `nim::ZSwc` + legacy-format writer (`src/neutube/zneutubeswcwriter.*`) for byte-identical SWC files.
    - The skeletonize implementation path does not include or call into neurolabi C (`tz_*`) or genelib.
  - `--command2 --trace` is now **partially ported** (strictly legacy-equivalent behavior):
    - Seeded trace (`position` provided, no host SWC, diagnosis disabled) runs entirely in `src/neutube/`:
      - Config parsing: `TraceConfig` (`src/neutube/zneutubetraceconfig.*`, Boost.JSON; legacy tag + per-level semantics)
      - Tracing core: ported `Trace_Locseg` pipeline (`src/neutube/zneutubelocsegchaintrace.*`)
      - Image access: `nim::ZImg` (no `ZStack` / no `Stack`)
      - Output: `nim::ZSwc` + legacy-format writer (`src/neutube/zneutubeswcwriter.*`)
    - Auto trace (no position), host-SWC attach (`input[1]`), and diagnosis output are still routed through the legacy
      adapter (temporary scaffolding for parity).
      - Legacy adapter: `src/neutube/zneutubelegacy.*` (the only place that includes neurolabi headers)
      - Legacy-only modes still materialize a `ZStack` via `src/neutube/zimgstackinterface.*` to feed NeuTu algorithms.
  - `--command2 --general {"command":"trace_neuron", ...}` is still routed through the legacy tracer today (until the auto
    tracing pipeline is ported).
  - `src/neutube/zrunneutucommand2.cpp` remains orchestration-only (CLI + config parsing + dispatch); all remaining
    neurolabi dependencies are isolated to `src/neutube/zneutubelegacy.*`.

## Detailed plan (step-by-step)

This plan is intentionally incremental; each step should keep the build green and preserve an A/B path.

### Phase A: scaffolding and “no behavior change” plumbing

1. Add `src/neutube/` directory structure and CMake target(s) (library + small unit tests).
2. Implement `nim::ZRunNeuTuCommand2` that:
   - parses CLI arguments without `tz_utilities.h`
   - loads/merges config using `boost::json` (`src/img/zjson.*`)
   - logs equivalent “effective configuration” output for debugging
3. Wire `--command2` in `src/atlas/main.cpp` to run the new runner.
4. Initially, `ZRunNeuTuCommand2` may call into the legacy tracing implementation *behind a narrow adapter*, but the
   adapter must be the only place that touches neurolabi types. This creates a controlled seam for later replacement.

Deliverable: new runner exists and can execute end-to-end, even if it still uses legacy tracer internally.

### Phase B: remove neurolabi C libraries from the command path

We will **not** modify `src/neurolabi/` during this migration. The legacy `neutu` + `neurolabi` codebase remains the
baseline for strict A/B comparisons.

Instead, we progressively **port** the required algorithm code into `src/neutube/` (and shared pieces into `src/img/`)
and then switch `--command2` to use the new implementation module-by-module.

Completed steps in this phase:

5. Remove `tz_utilities.h` usage entirely (arg parsing, file existence, string utilities).
6. Isolate all legacy algorithm calls behind a single adapter layer (`src/neutube/zneutubelegacy.*`) so that
   `src/neutube/zrunneutucommand2.cpp` stays clean and does not directly depend on neurolabi headers.

Started (next step):

7. Begin porting CLI-used neurolabi C algorithm utilities into clean C++ under `src/neutube/`/`src/img/`:
   - Add `nim::neutube::neighborhoodLegacyOrder(int)` (`src/neutube/zneutubeneighborhood.*`) to represent legacy
     connectivity tables using Atlas' `ZNeighborhood` offsets (in the exact legacy order).
   - This unlocks reusing existing `ZImgNeighborhood*Iterator` helpers (`src/img/zimgneighborhood*iterator.h`) for
     neighbor traversal in the forthcoming C→C++ algorithm ports, avoiding hand-written bound tests while preserving
     legacy ordering (important for strict parity).

#### Phase B dependency inventory (what we must port for Goal 1)

The v2 runner currently calls the remaining legacy algorithms via `src/neutube/zneutubelegacy.*` (through small
`src/neutube/` entrypoints like `src/neutube/zneutubetrace.*`), which in turn uses `neutu`
(`src/neurolabi/gui/`). Those C++ algorithms depend on a subset of symbols from the legacy C library (`neurolabi`) and
genelib.

To reach Goal 1 (no `neutu` / no neurolabi C libs), we must ultimately port the CLI-used algorithm stack into `src/neutube/`.
This likely involves porting (as C++20) the same underlying C routines that `neutu` relies on today, but *without*
depending on `src/neurolabi/c` or genelib.

The list below identifies the **minimum** legacy C object-file surfaces that are currently pulled in by the CLI-used
neutu objects. This is a practical “what to port first” inventory for the eventual clean implementation.

Empirically (from `nm -u` on the `neutu` object files used by the CLI path, mapped via `nm -gA` on
`build/Release/src/neurolabi/c/libneurolabi.a`), the current CLI-relevant undefined symbol set resolves to the following
object files in the legacy archive (highest impact first):

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

This list is a **starting point**: as we port, we should keep regenerating the symbol inventory to ensure we are not
accidentally relying on additional parts of the C library.

Deliverable: `--command2` is “C-lib free” (on the linking boundary), even if some legacy C++ remains temporarily.

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
12. Port any missing SWC processing/analysis utilities from neurolabi into `src/img/` (or `src/neutube/` if tracer-only),
    keeping `src/img/` as the shared/common layer.

Deliverable: the new CLI produces modern SWC outputs without `ZSwcTree`.

### Phase E: algorithms (tracing + skeletonization)

13. Replace legacy tracer internals with a new tracer implemented in `src/neutube/` on top of the new image/SWC APIs.
14. Replace skeletonize with a new implementation that:
   - handles binary volumes (including large datasets via tiling)
   - supports similar configuration knobs to legacy (downsample intervals, length thresholds, etc.)
   - does not rely on neurolabi C morphology / distance-map code

Deliverable: no neurolabi dependency on `--command2` path at all.

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
    - `NeutubeCommand2Parity.CompareSwc_MatchesLegacy`
    - Exercises `--command` vs `--command2` on synthetic TIFF inputs and asserts the produced SWC files are
      byte-identical (skeletonize + seeded trace in one consolidated test).
    - Verifies the ported `--compare_swc` path matches the legacy `ZSwcTreeMatcher`-based implementation.
  - Note: the legacy Atlas runner no longer relies on the genelib `Process_Arguments(...)` helper, so it can be invoked
    multiple times within a single test process (required for consolidated A/B tests).
- **Property tests / invariants**:
  - SWC must be connected where expected; no NaNs; IDs unique; parents valid; radii non-negative.
  - No silent truncation for large inputs; operations must fail fast with clear errors if user inputs are invalid.
- **Performance smoke**: time and memory usage on representative datasets; confirm streaming region reads are used for large inputs.

## Open questions (need product/engineering decisions)

1. CLI compatibility: should `--command2` be introduced, or should `--command` switch to v2 with a `--legacy_command` escape hatch?
2. Config schema: keep legacy JSON schema as-is, or introduce a new schema and a migration layer?
3. Tracing algorithm parity: **must** match the legacy tracer exactly for baseline A/B testing. “Close enough” is not acceptable for Goal 1.

## Progress tracker (living checklist)

Legend: ⬜ not started, 🟨 in progress, ✅ done

- ✅ Create `src/neutube/` module structure and targets
- ✅ Add `nim::ZRunNeuTuCommand2` entrypoint + `--command2` dispatch
- ✅ Migrate JSON parsing to `boost::json` for v2 runner
- ✅ Remove `tz_utilities.h` from v2 runner
- ✅ Use `ZImg` for image I/O in v2 runner
  - Note: `src/neutube/zimgstackinterface.*` remains as a temporary bridge for legacy algorithms (trace path), but
    skeletonize is now fully `ZImg`-native and does not use the adapter.
- ✅ Remove `ZSystemInfo` dependency from v2 runner (json dir injected by host / `--json_dir`)
- ✅ Isolate legacy algorithm calls behind `src/neutube/zneutubelegacy.*`
- ✅ Add A/B parity tests for `--command2` (skeletonize + seeded trace)
- ✅ Remove genelib `Process_Arguments(...)` from the legacy runner (enables consolidated in-process A/B tests)
- 🟨 Port neurolabi C algorithms to clean C++ (exact behavior, 64-bit-safe sizes)
  - ✅ Neighborhood/connectivity tables: `src/neutube/zneutubeneighborhood.*`
  - ✅ Binary morphology helpers (`Stack_Not`, `Stack_Majority_Filter`, `Stack_Fill_Hole_N`): `src/neutube/zneutubeimgbwmorph.*`
  - ✅ Point sampling + mask hit (`Stack_Point_Sampling`, `Stack_Point_Hit_Mask`): `src/neutube/zneutubeimgsampling.*`
  - ✅ Geo3d scalar field sampling + scoring (`Geo3d_Scalar_Field_Stack_Sampling*`, `Geo3d_Scalar_Field_Stack_Score*`):
    `src/neutube/zneutubegeo3dscalarfield.*`, `src/neutube/zneutubestackfitscore.*`
  - ✅ Geo3d scalar field centroid (`Geo3d_Scalar_Field_Centroid`): `src/neutube/zneutubegeo3dscalarfield.*`
  - ✅ Geo3d orientation utilities (`Vector_Angle`, `Geo3d_*_Orientation`, `Geo3d_Rotate_Orientation`):
    `src/neutube/zneutubegeo3dutils.*`, `src/neutube/zneutube3dgeom.*`
  - ✅ Legacy optimizer core (`Fit_Perceptor`, line search, conjugate updates):
    `src/neutube/zneutubecontfun.h`, `src/neutube/zneutubeoptimizeutils.*`, `src/neutube/zneutubeperceptor.*`
    - Note: Atlas already has Ceres-based optimizers (e.g. `src/img/zregistrationoptimizer.*`), but we intentionally keep
      the legacy Perceptor algorithm here to preserve byte-identical tracing outputs for Goal 1.
  - ✅ Trace workspace mask/bounds helpers (`Trace_Workspace_Mask_Value`, `Trace_Workspace_Point_In_Bound`):
    `src/neutube/zneutubetraceworkspace.*`
  - ✅ Trace record + score containers (`Trace_Record`, `Stack_Fit_Score` structs + setters/getters):
    `src/neutube/zneutubetracerecord.*`
  - ✅ Stack fit score switches (`STACK_FIT_*` option scoring on sampled arrays, including masked-score semantics):
    `src/neutube/zneutubestackfitscore.*`, `src/neutube/zneutubestackfitoptions.h`
  - ✅ Large-object labeling (`Stack_Label_Large_Objects_*`): `src/neutube/zneutubeobjlabel.*` (parity-tested)
  - ✅ Planar squared EDT (`Stack_Bwdist_L_U16P`): `src/neutube/zneutubeplanaredt.*`
  - ✅ Sp-grow (`Stack_Sp_Grow` + parser): `src/neutube/zneutubespgrow.*`, `src/neutube/zneutubespgrowparser.*`
  - ✅ Neuroseg field generation (`Neuroseg_Slice_Field`, `Neuroseg_Slice_Field_P`, `Neuroseg_Field_S_Fast`, `Neuroseg_Field_Sp`):
    `src/neutube/zneutubeneuroseg.*`, `src/neutube/zneutube3dgeom.*`, `src/neutube/zneutubegeo3dpointarray.*`
  - ✅ Local neuroseg scoring + fit (`Local_Neuroseg_Field_S*`, `Local_Neuroseg_Score_*`, `Fit_Local_Neuroseg_W`):
    `src/neutube/zneutubelocalneuroseg.*`
  - ✅ Local neuroseg optimize loop (`Local_Neuroseg_Position_Adjust`, `Local_Neuroseg_Orientation_Search_C`,
    `Local_Neuroseg_R_Scale_Search`, `Local_Neuroseg_Optimize_W`): `src/neutube/zneutubelocalneuroseg.*`
  - ✅ Legacy `darray_qsort(...)` parity helper: `src/neutube/zneutubedarrayqsort.*`
  - ✅ darray math helpers (`darray_dot_n`, `darray_dot_nw`, `darray_sum_n`, `darray_mean_n`, `darray_corrcoef_n`, `darray_max`):
    `src/neutube/zneutubedarraymath.*`
- ⬜ Implement image-access abstraction on `ZImg`/`ZImgPack` (streaming for very large volumes)
- ✅ Port/implement skeletonize on modern image types (exact behavior)
  - ✅ Port `ZStackSkeletonizer::makeSkeletonWithoutDs(Stack*, const int*)` behavior into
    `nim::neutube::ZNeutubeSkeletonizer` (`src/neutube/zneutubeskeletonizer.*`) using only `ZImg` + `ZSwc`.
  - ✅ Switch `--command2 --skeletonize` dispatch to the neutube-owned runner (`src/neutube/zneutubeskeletonize.*`).
  - ✅ Port SWC primitives used by skeletonize: `src/neutube/zneutubeswcops.*`, `src/neutube/zneutubeswcpointdist.*`,
    `src/neutube/zneutubeswcreconnect.*`, `src/neutube/zneutubeswcresampler.*`, `src/neutube/zneutubeswcregionsampling.*`,
    `src/neutube/zneutubeswcwriter.*`
- ✅ Port `--compare_swc` to `nim::ZSwc` (exact legacy `ZSwcTreeMatcher::matchAllG` semantics)
  - Implementation: `src/neutube/zneutubecompareswc.*`
  - Note: legacy `ZSwcLayerTrunkAnalyzer::extractTrunk(...)` is a stub (returns an empty path). The port preserves this
    behavior exactly, including the `-1.0` gap-penalty score contributions that fall out of matching empty branches.
  - Parity: `test/zneutubecommand2paritytest.cpp` (`NeutubeCommand2Parity.CompareSwc_MatchesLegacy`)
- 🟨 Switch SWC I/O + processing to `nim::ZSwc`
  - ✅ Skeletonize output uses `nim::ZSwc` + a legacy-format writer for byte-identical SWC files
  - 🟨 Trace output migration status
    - ✅ Seeded trace (position provided, no host SWC, diagnosis disabled) uses `nim::ZSwc` + legacy-format writer
    - ⬜ Auto trace and host-SWC attach still use legacy `ZSwcTree` via `neutube_legacy` (temporary scaffolding)
- ⬜ Add parity tests and sample datasets for regression checking
- ⬜ Document GUI parity targets and begin Atlas GUI integration (Goal 2)
