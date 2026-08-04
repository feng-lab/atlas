# Vulkan Tile-Worker Architecture

This document describes Atlas's in-process Vulkan tile-worker infrastructure.
`ZVulkanMultiDeviceTileCoordinator` owns one complete headless
`Z3DRenderingEngine` created on the canonical engine's selected physical
adapter. `renderTile()` synchronously renders one caller-supplied tile and
returns owned final pixels.

The coordinator does not select multiple physical devices, run workers
concurrently, assemble or publish multi-worker images, or route application
interactive rendering or export. Atlas's existing multi-process multi-GPU
export is a separate path.

Related: [Developer Guide](DEVELOPER_GUIDE.md) and
[Image Paging and Progressive Rendering](Atlas_Image_Paging_and_Progressive_Rendering.md).

## Current Scope

The coordinator supports:

- one complete headless Vulkan worker;
- exact reuse of the canonical engine's selected physical adapter;
- synchronous mono or stereo tile rendering;
- final-quality rendering with synchronous final-pixel readback;
- guard removal and owned CPU pixel results; and
- repeated tile calls while the worker remains healthy.

It does not provide:

- selection or use of additional physical devices;
- concurrent or asynchronous tile execution;
- multi-tile assembly or publication;
- integration with interactive rendering or export routing;
- a coherent state snapshot spanning several tile calls;
- automatic worker reconstruction or physical-device-loss recovery; or
- worker CPU threads independent from the canonical rendering thread.

Native Vulkan resources remain local to the logical device that created them.

## Architecture and Ownership

```text
canonical Z3DRenderingEngine
  `-- optional ZVulkanMultiDeviceTileCoordinator
        `-- one complete headless Z3DRenderingEngine
              `-- independent logical device on the canonical adapter
```

Each active Vulkan pipeline uses one logical device. A tile worker has immutable
Vulkan-backend and exact-device-selection affinity. Filters, passes, and draw
calls do not select a device.

`ZVulkanDevice` owns the allocator, descriptor state, staging resources,
`ZVulkanFrameExecutor`, and completion state for one logical device. A tile
worker is a complete `Z3DRenderingEngine` and owns its Vulkan context and
logical device, scratch pool, parameters, views, filters, compositor, ready
buffers, readback resources, engine/filter-local caches, and progressive state.

The canonical engine and worker refer to the same document-owned objects and
data packs and borrow the same render-thread `ZQtExecutor`; each logical device
owns a separate `ZVulkanFrameExecutor`. They do not share filter or parameter
QObjects, scratch leases, descriptors, textures, ready buffers, native GPU
resources, or progressive validity state.

The worker uses ordinary document-to-view connections for object and data
lifetime updates. View-originated selection and visibility changes are not
propagated from a worker back to the shared document.

### Direct One-Device Rendering

Atlas application entry points do not construct the coordinator. Interactive
rendering, single-shot capture, and tiled export execute on the canonical
engine. Direct tiled export and worker rendering use the same checked tile
descriptor geometry, but direct export submits every tile through the canonical
engine.

Interactive and untiled direct rendering perform no coordinator lookup, worker
selection, or per-draw device routing. Direct tiled export allocates its checked
descriptor list and submits each tile through the canonical engine. OpenGL does
not construct or consult the Vulkan coordinator or worker.

Single-device Vulkan submission adds a constant-time sticky-failure check at
backend entry and frame acquisition. While healthy, the checks perform no lock,
device enumeration, or allocation.

## Tile Descriptor and Result Contract

The unit of work is a spatial tile.

- A tile runs the complete active filter and compositor pipeline.
- Stereo remains inside one attempt; the eyes are not separate tasks.
- Transparency resolves inside the tile; no OIT state crosses devices.
- A tile returns final display pixels rather than native GPU resources or an
  intermediate render target.
- Returned images contain only the guard-free valid region.

`Z3DTileDescriptor` is a device-independent geometry value. It stores:

- the full output extent;
- a bottom-left-origin, half-open valid output rectangle; and
- guard width in pixels.

Checked accessors derive the expanded attachment extent and origin, normalized
frustum bounds, valid attachment crop, and top-left assembly origin. Guards may
extend beyond the full output so edge tiles retain the same filtering context.
The valid output rectangles returned by `makeZ3DTileDescriptors()` cover the
full output exactly once, including odd extents and edge tiles. Descriptors are
returned in bottom-row-first serpentine order.

`Z3DRenderedTile` owns RGBA, top-left-origin pixels. `primaryColor` contains the
mono or left-eye result; `rightColor` is present only when the same attempt
rendered a stereo pair. A caller that assembles several results uses
`Z3DTileDescriptor::topLeftAssemblyOrigin()`.

The descriptor contains no engine, device, QObject, scheduler, callback, or
pixel storage.

## Worker Construction and State Synchronization

Worker construction requires the canonical engine to be initialized with the
Vulkan backend on its rendering thread. The worker receives the canonical
context's preference-sorted physical-device index and UUID. Independent device
enumeration must resolve both values to the same compatible adapter; worker
construction never falls back to another adapter or to OpenGL.

A worker:

- initializes an independent Vulkan context and logical device;
- creates no OpenGL surface or canvas;
- does not overwrite process-global GPU capability state;
- borrows the canonical engine's rendering-thread executor; and
- propagates reported filter or view initialization failures to coordinator
  construction.

Construction is transactional. A worker remains local to the constructor until
initialization and the complete initial state transfer succeed.

Before each tile attempt, the coordinator reapplies:

1. every canonical 3D object-view state exposed by `write(objectId, ...)`;
2. device pixel ratio;
3. global and compositor parameters; and
4. the complete runtime camera, including derived state omitted from scene
   serialization.

Object state is applied before global state so bound-dependent parameters use
the worker's final canonical object ranges. The complete camera is the last
write so its clipping range is not recomputed from worker-local provisional
bounds. Full-frame output size is not synchronized; each tile attempt installs
its descriptor's attachment extent.

A tile call is synchronous on the canonical rendering thread and does not pump
Qt events, so queued document changes cannot interleave with that attempt. The
coordinator does not capture one state revision across several calls. Results
from separate calls must not be treated as one coherent image if canonical
state may change between them.

## Worker Tile Execution

`renderTile()` synchronizes the worker and then `renderVulkanTile()`:

1. checks that the worker's logical device accepts submissions;
2. observes cancellation before opening the rendering transaction;
3. sets the attachment extent, full-frame viewport, guarded tile frustum, and
   compositor rendering region;
4. renders the complete mono or stereo pipeline at final quality;
5. waits for final readback completion;
6. verifies that every accepted ready buffer belongs to the current render
   frame token;
7. copies and crops owned pixels into `Z3DRenderedTile`; and
8. clears the temporary tile frustum and compositor rendering region on both
   success and failure.

The worker retains the most recent attachment output extent and synchronous
readback policy. Each later attempt overwrites its output extent before
rendering. Canonical ready buffers, camera, picking state, and progressive state
are not used as worker output and are not mutated by the attempt.

Any synchronization, rendering, or cancellation exception destroys the worker
and is rethrown. The coordinator does not reconstruct it, and a later call
fails the non-null worker invariant. The coordinator must be destroyed while
the canonical engine and shared executor are still alive.

## Vulkan Submission and Completion Invariants

These contracts apply to ordinary one-device Vulkan rendering and to worker
engines.

### Cancellation Boundary

Cancellation is polled before command-buffer setup and between completed filter
calls; recording callbacks do not poll it. Once recording starts, that attempt
either submits a complete command buffer or takes the definitive-unsubmitted
abort path.

Submitted GPU work is not physically cancelled and retains its resources until
normal fence completion. A submission that explicitly requires synchronous
completion defers cancellation until its completion safe point; an ordinary
asynchronous submission may reach a later cancellation boundary before its
fence signals.

### Definitive-Unsubmitted Abort

`beginRender()` publishes a public active frame only after acquisition and
backend setup succeed. If setup, recording, readback-copy recording, command
buffer finalization, or completion arming fails before queue ownership
transfers, Atlas can prove that the command buffer was not submitted. The abort
path:

- closes recording ownership;
- releases CPU-side pins;
- drains every resource-release hook even if one hook fails;
- discards callbacks that require GPU completion;
- marks each readback producer complete without data;
- drains scratch and resource-release hooks and schedules descriptor-arena
  reset for slot reuse;
- releases the frame lease after cleanup can no longer access its slot; and
- reports the definitive-unsubmitted failure after cleanup.

The logical device stores the first failed render-frame token and submission ID
as a sticky record. Host-side layout or static-cache state may already describe
commands that were discarded, so later submissions on that logical device are
rejected until its engine, backends, and device are recreated.

### Uncertain Submission Ownership

A queue-submission exception does not prove whether the driver accepted the
command buffer. A fence-observation error likewise prevents Atlas from proving
whether submitted resources remain in use. Queue-submission and
fence-observation failures therefore terminate at the observation point.

Atlas does not recover a logical device after these failures and does not
continue on a reduced device set.

Normal backend teardown starts only after active recording has either been
submitted or completed the definitive-unsubmitted abort path. It stops new
work, waits for known submissions, runs completion safe points, and then
destroys backend and device resources.

### Readback Retirement

A readback staging slot has two independent owners:

1. the producer, which finishes after the submission fence is safe or after a
   definitive-unsubmitted abort; and
2. the consumer, which owns the ticket until pixels are copied, discarded, or
   the ticket is destroyed.

The slot is reusable only after both owners finish, in either order. Abort-aware
waiters receive an unavailable result instead of stale bytes. Producer and
consumer completion are independent: fence completion may precede the CPU copy,
and ticket destruction may precede fence completion.

### Drain-All Cleanup

Completion callbacks and safe-point hooks release independent resources. Atlas
drains every registered action, remembers the first ordinary exception, and
reports it after the drain. Normal teardown may retry the drain safely.

Detached backend tasks are drained before their owners are destroyed. A
reusable backend replaces a drained task scope before accepting more work.

## Required Invariants for Additional Workers

Additional workers may be introduced only if the following correctness
invariants are preserved:

- identify every selected physical device by preference index and UUID and
  reject changed or duplicate identities;
- verify that every selected device supports the complete active pipeline
  before accepting the worker set;
- keep native resources, device-local caches, scratch state, and completion
  state local to the device that created them;
- capture one immutable logical state and revision for every set of tiles that
  may form one published image;
- overlap devices without mutating engine QObjects from additional CPU threads;
- retire stale submissions without publishing their pixels;
- assemble and publish only complete results with matching revision, output
  extent, camera, quality, and eye identity; and
- stop assignment and drain worker activity before rebuilding for document
  topology changes.

## Validation

CPU-only tests cover:

- exact, non-overlapping descriptor coverage, including odd extents and
  unclipped guards;
- perspective and orthographic tile projection;
- output-to-image orientation;
- invalid descriptor and overflow invariants;
- readback producer and consumer completion in either order;
- duplicate or unowned readback completion invariants;
- readback publication identity and ownership transfer; and
- exact index-and-UUID Vulkan device-selection policy.

`ZVulkanMultiDeviceTileCoordinatorTest.SynchronousPpllTilesPreserveParityAndCanonicalEngine`
is an opt-in Vulkan smoke test. With `ATLAS_ENABLE_VULKAN_SMOKE_TEST=1` and a
usable Vulkan ICD, it checks same-adapter worker construction, synchronized
object and global state, worker-assembled PPLL output parity with canonical
tiled export, worker teardown, and continued canonical-engine rendering.

When the environment variable is unset, the smoke test skips before constructing
`QApplication`, a rendering engine, or `ZVulkanContext`. The descriptor,
selection, retirement, and publication tests construct no Vulkan context or
logical device and require no usable ICD or GPU at runtime. The build retains
Atlas's normal Vulkan SDK and shader-compiler prerequisites.
