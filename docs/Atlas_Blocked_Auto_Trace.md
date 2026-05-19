# Atlas Blocked Auto Trace (Large Image Support)

## 0. Problem Statement (Root Cause)

Atlas’ current “Auto Trace” workflow materializes the entire signal volume as a dense `ZImg` and then runs the migrated
legacy-like whole-volume auto tracer. This fails (or becomes unusable) for TB/PB-scale datasets because:

- It requires building a full dense stack (`imgPack->assembleChannelTime(...)`), which is not feasible for disk-cached /
  Neuroglancer-backed volumes.
- The legacy tracing algorithm assumes full-image availability and uses a dense trace mask (voxel label image) to enforce
  continuity and avoid retracing.

We need a blocked/paged approach that preserves tracing continuity and supports crash-safe resume.

## 1. Goals and Non-Goals

### 1.1 Goals

1) **Large-image full auto trace** that works with:
   - Disk-cached datasets (tiled on disk)
   - Neuroglancer precomputed datasets (HTTP-backed)

2) **Continuity across block switches**:
   - Switching blocks must not “break” tracing; the tracer must continue as-if the needed signal was always available.

3) **Crash-safe resume**:
   - Blocked auto trace writes results into an **output directory** (session folder).
   - After finishing each block, the system writes a new immutable checkpoint (“append-only”).
   - If a crash occurs during writing a checkpoint, the next run ignores the incomplete checkpoint and resumes from the
     previous fully committed one.
   - In the GUI, the legacy in-memory tracer outputs individual files (SWC + log). Blocked tracing outputs a folder.

4) **No adaptive per-block preprocessing** that makes results depend on block selection order.
   - Preprocessing policy must be deterministic and resumable:
     - `threshold_mode="fixed"`: subtract a user-provided fixed threshold/background value (`subtract_constant`).
     - `threshold_mode="auto"`: subtract background per block ROI using the legacy neuTube algorithm, treating the ROI as
       if it were the whole image.
   - The policy is block-invariant (the same rule is applied to every block), even though the auto mode can produce
     different background values per block depending on local intensity distributions.
   - Runtime controls (Abseil flags):
     - `--atlas_autotrace_block_threshold_mode=auto|fixed` (default: `auto`; parsed case-insensitively and written back lowercase)
     - `--atlas_autotrace_block_subtract_constant=<double>` (default: `0`; used only when mode is `fixed`)

5) **Respect the existing downsample tracing option**:
   - If the user chooses an XY/Z downsample ratio, blocked tracing must request ROIs at that ratio and produce output SWC
     in the original base voxel coordinates (rescaled at the end), matching whole-volume semantics.

### 1.2 Non-Goals (Phase 1)

- Exact parity with the legacy whole-volume tracer is **not** required for the blocked tracer.
- We do not require a persistent voxel trace-mask for the whole volume.
- We do not attempt to “stitch” results by revisiting every block twice.

## 2. High-Level Algorithm

We trace the dataset in 3D blocks:

- **Core block** size: `(Bx, By, Bz)` (configurable).
  - In the GUI we expose a single “Block size” and use cubic blocks (`Bx=By=Bz`).
  - All block sizes are specified in **tracing voxel coordinates** (after downsample).
  - Hard minimum (enforced): `Bx,By,Bz >= 1024`.
- **Halo / padding**: `P` voxels (configurable).
  - In the GUI we expose a single “Padding” value.
  - Halo/padding is also in **tracing voxel coordinates** (after downsample).
  - Hard minimum (enforced): `P >= 128`.
- For a block with core bounds `[x0..x1]×[y0..y1]×[z0..z1]`, we load the **ROI**:
  `[x0-P..x1+P]×[y0-P..y1+P]×[z0-P..z1+P]`, clamped to the dataset bounds.
- ROI loading outcomes are explicit:
  - **Ok**: a valid single-channel ROI image is returned.
  - **AllZero**: the ROI is known to be valid and entirely zero-valued.
  - **Unavailable / fetch failure**: treated as an error so the block can be retried; Atlas must not silently commit it as an empty block.

We maintain two primary global structures:

- **SWC**: the canonical traced skeleton output (and the persisted resume artifact).
- **Frontier (pending tasks)**: a set of continuation tasks emitted when a trace reaches the halo boundary.

We also persist **block-scan progress** so resume never re-runs seed detection for blocks that were already scanned.

### 2.x Single-Visit Policy (Important)

Blocked auto trace is designed for TB/PB-scale data, where reloading a block ROI is the dominant cost.
Therefore, **each core block is processed exactly once**:

- A block visit includes: tracing all pending tasks assigned to the block, then running seed detection + tracing for that
  block’s core.
- After a block is committed, it is marked **visited** and will never be loaded again.
- Any continuation task that would “naturally” point to a visited block is treated as an invariant violation; the
  implementation will either:
  - drop it only if the endpoint is already inside traced SWC geometry (task is already satisfied), or
  - reassign it to a different **unvisited** block whose ROI intersects the continuation path (rare, edge-case recovery), or
  - terminate it only when the continuation already leaves the dataset bounds or no admissible unvisited handoff block exists.

### 2.0 Coordinate System (Base vs Tracing Space)

Blocked tracing supports the existing UI downsample ratios. That introduces two coordinate spaces:

- **Base voxel coordinates**: the original dataset coordinates (full resolution).
- **Tracing voxel coordinates**: coordinates of the signal actually fed into the tracer, after applying a downsample ratio
  (e.g. `ratio=[2,2,1]`).

Policy:
- All persisted session state (`swc_delta.swc`, `frontier.json`) lives in **tracing voxel coordinates**, because it must be
  internally self-consistent for continuation and resume.
- The user-facing output SWC written to the chosen output path is a **rescaled copy** back into base voxel coordinates.

### 2.1 Pending Tasks

A pending task represents “continue tracing from this endpoint when the next block is available”.

Each task stores the minimum state needed for continuation:

- `taskId` (monotonic)
- `attachSwcNodeId` (where to attach new nodes)
- `direction` (which end to extend): `Forward` (tail) or `Backward` (head)
- `endLocseg` (`LocalNeuroseg`: full params + position, in **global** voxel coordinates)
  - This is the **only locseg** we need to persist for continuity. Interior segments are committed into SWC as normal nodes.
- `reason` (e.g. `OutOfBlockHalo`, `OutOfImage`, `PausedForBudget`) — primarily diagnostic
- `suggestedBlockId`:
  - derived from `endLocseg` top/bottom position
  - used by the scheduler as the **task assignment target**
  - must always refer to an **unvisited** block under the single-visit policy

We do **not** persist the full chain of locseg nodes:
the chain interior is committed into SWC, and “already traced?” checks are answered via SWC geometry.

### 2.2 Block Selection / Scheduling

For each iteration:

1) Choose the next **unvisited** core block to process:
   - Prefer blocks with the most pending tasks assigned.
   - Tie-break deterministically by the smallest linear block index (stable/resumable).
   - Never revisit a visited block.

2) Load ROI (core + halo) signal for the selected channel/time.

3) **Trace pending tasks first**:
   - For each task assigned to this block, continue tracing until:
     - it terminates normally (low score, hit traced structure, etc.), or
     - it reaches the halo boundary again → emit a new pending task.
   - Commit new traced geometry into SWC immediately.

4) **Seed detection** in the ROI:
   - Run the *exact same seed detection algorithm* as whole-volume auto trace, but applied to the ROI as-if it were the
     whole image.
   - Drop seeds detected in the halo (keep only seeds whose coordinates are inside the core).
   - Drop seeds that hit already traced structure (via SWC geometry).

5) Trace new seeds (within this block), again committing into SWC and emitting new pending tasks as needed.

Repeat until:
- no pending tasks remain and no seeds remain, or
- user cancels, or
- a configured budget is reached.

## 3. “Already Traced?” Without a Global Voxel Mask

Instead of a global trace mask image, we use a **continuous-geometry SWC spatial index**:

- Each SWC edge (parent→child) is treated as a **tapered cylinder** (radius interpolated between endpoints).
- Root nodes are treated as **spheres** (or degenerate segments).
- A point `(x,y,z)` is “inside traced structure” if its distance to any SWC primitive is ≤ the primitive’s radius at the
  closest point.

Acceleration structure:

- Use **Boost.Geometry R-tree** over primitive AABBs expanded by radius.
- Point queries:
  - query rtree for candidate primitives whose AABB contains the point
  - exact distance test against those candidates

This avoids allocating a global dense or sparse voxel mask for huge datasets.

### 3.1 Parity Guard

Continuous-geometry hit tests can differ from legacy voxel-label semantics.
We keep legacy behavior available behind Abseil flags so parity tests (and any regression comparisons) can force the old
paths.

Policy:
- Default: **new behavior enabled**
- Parity tests: force legacy paths

Flags:
- `--atlas_trace_use_swc_geometry_mask=false` forces legacy voxel-mask semantics for host-SWC mask queries in the seeded
  tracing path.

## 4. Crash-Safe Resume via Append-Only Block Checkpoints

The output is a directory with immutable per-block checkpoints.

### 4.1 Directory Layout

```
<session_dir>/
  result_tracing.swc    # rolling full SWC mirror (tracing voxel coordinates; atomically replaced after each block commit)
  result.swc            # final output (base voxel coordinates; written at the end)
  log.txt               # trace log (written during tracing)
  manifest.json
  blocks/
    commit_000001/   # fully committed checkpoint
      commit.json    # small “done marker”, written last
      swc_delta.swc   # append-only SWC fragment for this commit
      swc_full.swc    # full SWC snapshot at this commit (tracing voxel coordinates)
      seed_scanned_blocks.json  # all core blocks already seed-scanned at this commit
      frontier.json
      scheduler.json
    commit_000002/
      ...
```

Write protocol for commit `N`:

1) Write everything into a staging directory:
   `blocks/.staging_<uuid>_commit_<N>/...`
2) Close files.
3) Write `commit.json` last inside staging.
4) Atomically rename staging → `commit_<N>/`.

Resume protocol:

- Scan `blocks/commit_*`.
- Choose the highest `N` whose checkpoint directory loads successfully.
- Ignore any `.staging_*` directories or incomplete commits.
- A later broken commit does not prevent resuming from an earlier good one.

GUI note:
- If the user selects an output folder that already contains `manifest.json`, Atlas treats it as an existing blocked
  tracing session and **locks** block size / padding and downsample ratio to the manifest. This prevents accidental
  “resume with different parameters” mismatches.

### 4.2 Persisted Contents (Minimum)

- `manifest.json` (static):
  - exact dataset identity (`ZImgSource` JSON)
  - selected `(c,t)`
  - `signal_downsample_ratio` (tracing ratio; defines session coordinate system)
  - `z_scale` (explicit tracing anisotropy in tracing coordinates)
  - block size + halo
  - preprocessing policy:
    - `threshold_mode`: `"auto"` or `"fixed"`
    - `subtract_constant`: fixed threshold value (used only when `threshold_mode="fixed"`)
  - algorithm config (TraceConfig, plus “blocked tracer” parameters)
- `blocks/commit_*/swc_delta.swc`: SWC node fragments appended at each commit (IDs are global within the session and must
  remain stable; do not resort during the session).
- `blocks/commit_*/swc_full.swc`: full SWC snapshot for that commit (canonical resume skeleton for that checkpoint).
- `blocks/commit_*/seed_scanned_blocks.json`: full visited-block snapshot for that commit.
- `blocks/commit_*/frontier.json`: pending tasks (each with `endLocseg`).
- `blocks/commit_*/scheduler.json`: minimal scheduling cursor (e.g. next linear scan index) for deterministic resume.
- `result_tracing.swc`: rolling “full SWC so far” convenience artifact (an atomic mirror of the latest committed
  `swc_full.swc`). It is not required for correctness; the commit directories are the resume source of truth.

### 4.x Why Commits Now Store Full Snapshots

Each commit directory is intentionally self-contained:

- Users can resume from the last good commit even if some earlier or later commit directories are missing or corrupted.
- Resume does not depend on replaying the entire history just to recover the current SWC and visited-block set.

The trade-off is higher disk usage, because each commit repeats the current SWC and visited-block snapshot. We keep
`swc_delta.swc` as well because it is still useful for debugging and inspection.

To make “current full SWC so far” easy to inspect (and to speed up crash recovery), we also maintain:

- `result_tracing.swc`: an atomic mirror of the latest committed `swc_full.swc`.

These rolling artifacts are convenience/optimization only; the immutable `blocks/commit_*/` self-contained checkpoints
remain the resume source of truth.

### 4.2.x Resume Identity Contract

Blocked auto trace resumes only when the output folder belongs to the **same source image**:

- Atlas persists `dataset_id = imgPack->imgSource().toString()`.
- This is the JSON serialization of `ZImgSource` (including canonicalized filenames / source parameters).
- On resume, the manifest must also match `(channel,time)`, `signal_downsample_ratio`, `z_scale`, dataset shape, block
  geometry, preprocessing mode, and effective `TraceConfig`.
- Any mismatch is a hard error; Atlas does not attempt cross-dataset reuse of a session folder.

## 4.3 File Format Notes (SWC vs ESWC vs JSON)

We intentionally keep the on-disk skeleton as **plain SWC**:

- SWC remains the primary user-facing artifact (loadable in Atlas/NeuTu/others).
- We avoid “ESWC everywhere” because:
  - the extra fields are not uniformly supported by downstream tools,
  - we only need extra state for a *small subset* of nodes (frontier endpoints).

Instead, we persist *non-SWC state* in explicit sidecar JSON files:

- `frontier.json` holds `LocalNeuroseg` and continuation metadata required for correctness.
- This is more explicit and less fragile than hiding state in optional SWC columns.

If we later want ESWC for debugging/inspection, we can emit it as an additional optional artifact without making it the
resume source of truth.

## 4.4 Final Output Reconstruction

The resumable session SWC stays append-only and may contain multiple roots while tracing is in progress.
Before Atlas writes the final `result.swc`, it:

- keeps the traced result as a forest instead of forcing all roots into one tree,
- uses exact `attachSwcNodeId` continuation tasks to preserve branch continuity across block boundaries, and
- for fresh seed-started chains, immediately tries the same local host-attachment heuristic used by interactive trace
  against the current global SWC, using ROI-aware sampling in the current block, before leaving the chain as a new
  root.

After those topology decisions, Atlas applies the same legacy SWC postprocess family used by whole-volume auto trace
(zigzag removal, branch tuning, spur removal, close-node merge, overshoot removal, optional optimal resampling,
orphan-blob pruning).

This preserves append-only IDs inside the resumable session artifacts while keeping separate neurons as separate roots
unless a fresh branch has an explicit local host connection.

## 5. Implementation Roadmap

Phase 1 (infrastructure + first consumer):
1) Implement `ZSwcSpatialIndex` (Boost.Geometry rtree).
2) Add `ZSwcGeometryMaskVolume` adapter to plug into existing tracing helpers.
3) Add gflag guard for geometry-mask behavior and set parity tests to legacy.
4) Use geometry-mask in host-SWC seeded trace (interactive attach path).

Phase 2 (blocked tracer core):
1) Implement session directory writer + resume loader.
2) Implement block scheduler + ROI loader for disk + Neuroglancer via `ZImgPack`.
3) Implement pending task continuation and per-block seed detection/tracing.
4) Integrate into UI Auto Trace for disk-cached datasets.

Phase 3 (optimization + robustness):
1) Incremental SWC index updates (avoid full rebuild per commit).
2) Tune scheduling heuristics (pending-task locality, network batching).
3) Optional per-block SWC voxelization for legacy-like overlap suppression where needed.
