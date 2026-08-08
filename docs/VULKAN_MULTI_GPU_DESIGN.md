# Vulkan Multi-Device Tile Workers

Atlas keeps each `Z3DRenderingEngine` bound to one Vulkan logical device. An
optional, private `Z3DRenderingEngine::ZVulkanTileWorkerPool` schedules complete
spatial tiles across multiple single-device engines and assembles their final
CPU pixels. Filters, renderers, passes, and Vulkan resources remain unaware of
the pool.

The canonical engine is the public facade. It exposes compatible device
selections and `configureVulkanTileWorkers()`; capture routing, state
publication, worker scheduling, and cleanup remain engine implementation
details. Passing an empty selection removes the pool.

Related references: [Developer Guide](DEVELOPER_GUIDE.md) and
[Image Paging and Progressive Rendering](Atlas_Image_Paging_and_Progressive_Rendering.md).

## Scope

The pool is consumed by fixed-size tiled Vulkan capture. A configured pool is
used only when the requested output requires more than one effective tile. A
single-tile capture continues through the canonical engine's direct path.

The application entry point is opt-in headless scene export through
`--atlas_vk_multi_device_tile_worker_indices`. The flag names the complete
participating device set and requires at least two distinct compatible physical
devices. The engine API also accepts a one-device set, which is useful for the
canonical-adapter integration path and tests. The scene exporter
requests mono output; the pool and result types also support a complete stereo
pair per tile.

Interactive rendering and animation export do not configure or use the pool.
Opt-in multi-process animation export instead assigns adjacent frame ranges to
separate single-device Atlas processes. Each process retains one range, document,
renderer, and its caches for that range's lifetime. Ranges are balanced by default;
optional positive integer frame weights change their relative sizes without changing
ownership or continuity. Frames are not distributed through the tile-worker lanes.
OpenGL never constructs or executes tile-worker lanes.

With no pool configured:

- `render()` and `renderFast()` have no pool branch or worker-state work;
- direct and tiled captures use the canonical engine;
- no worker thread, engine, context, logical device, or snapshot is allocated;
- filters and per-draw code perform no worker or device-routing lookup; and
- fixed-size capture performs no worker construction, state publication,
  scheduling, or device routing.

This keeps the ordinary single-device render hot path free of multi-device
scheduling overhead.

## Ownership and Threading

```text
canonical Z3DRenderingEngine (one device, canonical rendering thread)
  `-- optional private Z3DRenderingEngine::ZVulkanTileWorkerPool
        |-- canonical lane, when its device is selected
        |-- QThread -> complete worker Z3DRenderingEngine -> device B
        |-- QThread -> complete worker Z3DRenderingEngine -> device C
        `-- ...
```

The configured selections are the complete participating set. When they
include the canonical device, the existing canonical engine renders as one
lane; the pool does not construct a duplicate engine for that device. When the
canonical device is absent, it publishes state and waits for worker lanes but
does not claim tiles.

Configuration is synchronous on the canonical engine thread. Reconfiguration
releases the current pool before constructing its replacement, so two complete
device sets are never resident at once. A construction failure propagates to
the caller and leaves the canonical engine without a pool.

Every non-canonical selection owns a dedicated `QThread`, dispatcher, complete
Vulkan-only `Z3DRenderingEngine`, and engine-local `ZQtExecutor`. The worker
engine is constructed, initialized, invoked, and destroyed on that thread. It
owns its context, logical device, allocator, frame executor, scratch pool,
filters, compositor, pipeline contexts, descriptors, textures, readback state,
ready buffers, caches, and progressive validity. Native Vulkan objects never
cross device boundaries. Every participating engine retains the ordinary
configured frame-slot count; the one-outstanding-tile lane contract does not
restrict the engine's internal Vulkan submissions to one executor slot.

One queued lane callback performs the complete batch on each worker thread.
The canonical lane executes the same operation inline on the canonical engine
thread. Worker threads and independent Vulkan queues can therefore record,
submit, wait for readback, and claim later tiles concurrently. The caller waits
for every lane before the synchronous capture call returns.

The pool is destroyed on the canonical engine thread. Each worker engine is
deleted on its owning thread before that thread is stopped and joined. The
canonical engine releases the pool before tearing down its own rendering
resources.

## Document and Render State

All engines observe the same `ZDoc`. Worker views use their normal document
subscriptions for object and data-pack lifetime changes, and document-owned
payloads are shared directly. Only the canonical engine may propagate
view-originated selection or visibility mutations back to the document.

Mutable rendering state is engine-local. Workers do not share filter,
parameter, compositor, camera, scratch, descriptor, or ready-buffer QObjects
with the canonical engine or with one another.

At the start of every batch with a noncanonical lane, the canonical engine
publishes one immutable `Z3DRenderingEngine::VulkanTileRenderState` containing:

- compositor and global-parameter JSON;
- every available per-object view state;
- device pixel ratio; and
- the complete runtime camera.

All non-canonical lanes share that immutable publication. Before claiming a
tile, each worker applies object states, device pixel ratio, general state, and
finally the complete camera. Object state is applied before bound-dependent
global parameters, and the camera is last so worker-local bound updates do not
replace the canonical clipping range. The canonical lane already owns the
published values and does not serialize them back into itself.

Each lane then installs the common full-output viewport and prepares mesh LOD
for that extent. The prepared LOD remains active for the lane's batch. Every
tile independently installs its attachment size, guarded frustum, and
compositor rendering region; full-frame output-size state is not copied among
engines.

The lane callback does not pump its Qt event loop. Consequently, engine-local
filter and parameter state is fixed for the callback after state application.
The immutable publication does not freeze or version shared document payloads;
the fixed-size export route requires the document scene to remain
stable for the synchronous batch.

`Z3DGpuInfo` remains the canonical engine's process-wide planning record.
Worker initialization does not replace it. Compatible worker candidates must
satisfy the Vulkan pipeline contract and the canonical record's relevant image
and memory-planning limits.

## Device Selection

`compatibleVulkanTileWorkerSelections()` returns exact, preference-sorted
device selections from the initialized canonical Vulkan context. Each
selection contains both an index and the expected physical-device UUID.

Pool configuration requires every selection to belong to that compatible set,
with unique indices and UUIDs. A non-canonical worker independently enumerates
devices and requires both values to identify the requested adapter. There is no
substitute-device or OpenGL fallback.

Compatibility is a correctness gate, not a speed policy. A slower device can
hold the batch tail after claiming a tile, and workers add construction, state
synchronization, and device-local resource costs. Concurrent devices can also
contend for host CPU, driver, and shared-memory bandwidth. Representative
scenes and tile sizes determine whether a selected set improves throughput.

## Tile and Batch Contract

`Z3DTileDescriptor` is pure spatial geometry. It stores a full output extent,
a bottom-left-origin valid output rectangle, and guard width. Attachment
extent, guarded frustum, crop region, and top-left assembly origin are derived
from those values. `makeZ3DTileDescriptors()` covers odd and edge extents
exactly once in bottom-row-first serpentine order.

Before scheduling, the pool verifies that the descriptor list is non-empty,
uses one full output extent, contains no overlapping valid rectangles, and
covers every output pixel exactly once.

A tile runs the complete active filter and compositor pipeline on one engine.
Transparency resolves locally inside that tile. Stereo eyes are not separate
tasks: a stereo assignment renders and returns both eyes before the lane claims
more work. Results contain owned, guard-free, top-left-origin RGBA pixels and
no device, engine, QObject, or native-resource handle.

The batch owns:

- an atomic next-tile index for dynamic claims;
- an atomic stop flag;
- the first observed failure and first non-cancellation failure;
- one full-size `Z3DRenderedFrame`;
- completion state for every descriptor; and
- a mutex protecting frame assembly.

Each lane claims one index at a time. Faster lanes collect their current result
and atomically claim another tile, so work distribution follows completed
throughput rather than a fixed partition. Each engine permits exactly one
outstanding tile.

## Submission, Readback, and Assembly

At batch entry, each participating engine saves its existing readback
completion policy and selects `ReturnAfterSubmit`. Mesh export state and the
original readback policy are restored when the lane finishes or abandons the
batch.

For each claimed tile, the engine:

1. validates role, thread, device, batch extent, and submission health;
2. observes cancellation before recording;
3. installs the tile attachment size and guarded projection;
4. records and submits the complete mono or stereo frame;
5. retains the descriptor, eye mode, and immutable render-frame token;
6. pumps device-local completion safe points until final readback publication;
7. accepts ready pixels only when the published eye token or tokens exactly
   match the outstanding frame token; and
8. materializes only the valid attachment rectangle from mapped RGBA8 readback,
   directly deinterleaving, correcting premultiplied color, and applying the
   required row orientation.

The lane keeps its tile projection for the batch. The next submission replaces
that projection directly, and normal completion or failure abandonment restores
the full-frame projection once at the batch boundary.

Deferred image paging or preview warnings emitted while processing a tile are
treated as tile failures. The canonical lane can borrow the enclosing capture
error scope; an independent worker owns a tile-local scope.

A submitted tile keeps its Vulkan and readback resources alive through the
normal device-local fence and publication lifecycle. A stop request does not
discard an outstanding submission. The lane waits for publication, collects
the result to close its ownership boundary, and only then stops or claims more
work.

Completed tiles are pasted immediately into the batch frame under the assembly
mutex and then released. Per-tile dimensions, RGBA8 type, eye mode, placement,
and one-time completion are checked. The returned frame is save-oriented and
requires no additional vertical flip.

Direct tiled Vulkan capture uses the same valid-region conversion. Guard pixels
are not copied into an intermediate `ZImg`, and conversion does not allocate a
full-attachment double-precision alpha image or a second cropped image.

## Cancellation and Failure

Cancellation is checked before batch publication, before tile submission, and
between assignments. Once a lane observes cancellation or the shared stop
flag, it makes no further claims. A claim already past that observation point
can still submit; it is drained and collected before the lane exits. Partial
pixels are never saved or returned.

Every lane catches its local exception, records the first observed failure,
sets the shared stop flag, and restores or abandons its engine-side export
state. Any observed non-cancellation failure, whether raised while rendering or
cleaning up, takes precedence over an ordinary cancellation so an unsafe pool
cannot be retained merely because another lane observed cancellation first.
Other lanes finish any outstanding tile and stop. The caller joins all worker
tasks before rethrowing the selected failure.

Normal cancellation preserves the configured pool after all lanes have
cleanly restored their state. A non-cancellation batch failure removes the
pool, destroys non-canonical engines on their owning threads, and requires
explicit reconfiguration before another pooled tiled capture. Backend-owned
resources from submitted work remain governed by each device's normal
completion or failed-device teardown rules. Abandonment advances the
compositor's readback-owner revision, so a late completion retires its resources
without publishing pixels from the failed attempt. The batch never continues
with a reduced device set and never returns a partially assembled frame.

## Heterogeneous Output and Device-Local Image Binding

Atlas does not normalize floating-point, depth, fragment-order, or
transparency results across drivers. Dynamic claims can assign a tile to a
different adapter on another run. Cross-device pixels and complete image
hashes are therefore not guaranteed to be identical, even though guard pixels
preserve the required spatial context.

Each Vulkan engine selects image-binding variants from its own logical-device
capabilities. A dense, non-paged single-channel slice uses the two-descriptor
push variant when `VK_KHR_push_descriptor` is enabled and
`maxPushDescriptors >= 2`. Raycaster and slice layer-array merges keep color
bindless and use the samplerless depth-array push descriptor when the extension
is enabled and `maxPushDescriptors >= 1`. The corresponding bindless variant is
used when a required capability is unavailable. Selection contains no vendor
branch, and descriptor state is not shared across engines. See the Developer
Guide's Slices Path and Compositor Integration sections for the binding
contracts.

## Validation

CPU-only tile tests cover geometry, projection, exact coverage, odd extents,
guard behavior, orientation, mono/stereo allocation, assembly, invalid
descriptor extents, and attachment overflow.

Complete-engine worker tests are opt-in through
`ATLAS_ENABLE_VULKAN_SMOKE_TEST=1`. They cover canonical-device participation,
same-adapter PPLL parity, a two-physical-device batch, immutable state transfer,
assembled-frame contracts, worker teardown, and continued canonical rendering.
Without the environment variable, the tests skip before constructing a Vulkan
engine, so ordinary CI does not require a Vulkan ICD or GPU.
