# Vulkan Multi-Device Tile-Worker Architecture

This document describes Atlas's in-process Vulkan tile-worker infrastructure.
`ZVulkanMultiDeviceTileCoordinator` owns one or more complete headless
`Z3DRenderingEngine` workers. Each worker is bound to one exact compatible
physical device and returns owned final pixels for spatial tiles.

The public batch operation is synchronous. Internally, the coordinator submits
one tile per worker without waiting for final-pixel readback, polls completion
on the canonical rendering thread, and refills the first available worker.
Independent Vulkan device queues can therefore execute tile submissions at the
same time without introducing worker CPU threads.

Headless scene export constructs an operation-scoped coordinator when
`--atlas_vk_multi_device_tile_worker_indices` is non-empty. Multi-tile fixed-size
capture uses its workers and saves the assembled CPU frame. Interactive
rendering, GUI capture, headless animation export, OpenGL rendering, and
single-tile scene capture use the canonical engine's direct path. Atlas's
multi-process animation-export path is separate.

Related: [Developer Guide](DEVELOPER_GUIDE.md) and
[Image Paging and Progressive Rendering](Atlas_Image_Paging_and_Progressive_Rendering.md).

## Current Scope

The coordinator provides:

- explicit selection of one or more distinct compatible Vulkan devices;
- one complete headless rendering engine and logical device per selection;
- a synchronous `renderFrame()` batch path with overlapping submit/collect;
- dynamic spatial-tile refill based on observed completion;
- mono or complete stereo-pair tiles;
- one canonical state snapshot for every coordinated batch;
- worker-side full-output mesh-LOD preparation for the batch lifetime;
- final-quality rendering, guard removal, and streaming assembly into one
  save-oriented, top-left-origin CPU frame;
- opt-in routing for tiled headless Vulkan scene export; and
- all-or-nothing worker cleanup on synchronization, rendering, or cancellation
  failure.

The coordinator does not provide:

- automatic multi-device-set selection or performance policy;
- interactive-rendering or headless-animation routing;
- publication into canonical compositor ready buffers or the interactive
  canvas;
- more than one outstanding tile per worker;
- independent worker CPU threads;
- automatic worker reconstruction;
- continuation on a reduced worker set after failure; and
- physical-device-loss recovery.

Native Vulkan resources remain local to the logical device that created them.
Device compatibility is a correctness gate, not a performance admission
policy. Each selected worker receives one initial tile when work is available;
dynamic refill adapts later assignments but cannot retract a slow outstanding
tile or remove worker construction and state-synchronization cost. A
substantially slower adapter can therefore make a batch slower than the fastest
adapter alone. Device sets and tile settings must be benchmarked on
representative scenes before performance-sensitive use.

## Architecture and Ownership

```text
canonical Z3DRenderingEngine
  `-- optional ZVulkanMultiDeviceTileCoordinator
        |-- worker Z3DRenderingEngine -> logical device A
        |-- worker Z3DRenderingEngine -> logical device B
        `-- ...
```

Each active rendering engine uses exactly one logical device. A tile worker has
immutable Vulkan-backend and exact-device-selection affinity. Filters, passes,
and draw calls do not select devices.

`ZVulkanDevice` owns the allocator, descriptor state, staging resources,
`ZVulkanFrameExecutor`, and completion state for one logical device. Every
worker is a complete `Z3DRenderingEngine` and owns its Vulkan context, device,
scratch pool, parameters, views, filters, compositor, ready buffers, readback
resources, engine-local caches, and progressive state.

The canonical engine and workers refer to the same document-owned objects and
data packs. They do not share filter or parameter QObjects, scratch leases,
descriptors, textures, ready buffers, native GPU resources, or progressive
validity state. Only the canonical engine may propagate view-originated
selection or visibility changes back to the shared document.

`Z3DGpuInfo` remains the process-global capability record published by the
canonical engine. Worker initialization does not replace it. Volume planning
that reads `Z3DGpuInfo` therefore uses the canonical adapter's limits for every
worker. Worker selection excludes devices whose 2D/3D texture dimensions,
array-layer count, or GPU-memory planning capacity are below the canonical
record. Using the least-capable adapter as the canonical device provides the
conservative configuration for a heterogeneous worker set.

All coordinator and worker operations run on the canonical rendering thread.
Workers borrow the canonical engine's installed `ZQtExecutor`; the coordinator
does not create CPU threads or pump Qt events. Command recording and queue
submission are serialized on that thread, while already-submitted work on
independent device queues may overlap.

The coordinator must be destroyed on the canonical rendering thread while the
canonical engine and borrowed executor are still alive.

### Direct Single-Device Rendering

The normal rendering and capture APIs do not allocate or consult a coordinator.
Interactive rendering, GUI screenshots, headless animation export, OpenGL
rendering, and headless scene export with an empty
`--atlas_vk_multi_device_tile_worker_indices` flag execute on the canonical
engine. `takeFixedSizeScreenShot()` passes no coordinator, so direct tiled
export submits every tile through that engine.

The opt-in headless scene route passes an externally owned coordinator to
`takeFixedSizeScreenShotWithVulkanTileCoordinator()`. The engine verifies that
the coordinator belongs to that exact canonical engine. If the output fits in
one effective tile, pixel capture still executes directly on the canonical
engine; the requested worker set is nevertheless validated and initialized
before that size branch. Otherwise the coordinator renders the spatial tile
batch.

Direct rendering performs no worker selection or per-draw device routing.
OpenGL never constructs or consults the Vulkan coordinator. Healthy
single-device Vulkan submission performs only constant-time sticky-failure
observations at backend entry and frame acquisition.

## Device Selection and Worker Construction

Worker construction requires an initialized canonical Vulkan engine on its
rendering thread. `compatibleVulkanTileWorkerSelections()` returns each
physical device that satisfies the Vulkan pipeline contract and exposes
planning-relevant limits at least as large as the canonical capability record,
represented by a preference-sorted index plus expected device UUID.

The explicit coordinator constructor accepts a caller-supplied sequence of
these selections. It requires:

- at least one selection;
- every selection to belong to the canonical compatible-device set;
- unique preference indices; and
- unique physical-device UUIDs.

Each worker independently enumerates devices and requires both parts of its
selection to resolve to the same compatible adapter. Exact worker selection
never falls back to another adapter or to OpenGL. A worker:

- initializes an independent Vulkan context and logical device;
- creates no OpenGL surface or canvas;
- does not overwrite process-global GPU capability state;
- borrows the canonical rendering-thread executor; and
- propagates reported filter or view initialization failures.

Partially constructed worker sets are destroyed if any worker fails to
initialize. The one-argument coordinator constructor creates one worker on the
canonical engine's selected adapter; it executes the same batch path as an
explicit multi-device worker set.

For headless scene export,
`--atlas_vk_multi_device_tile_worker_indices` is a comma-separated list of
preference-sorted Vulkan indices and requires at least two entries. Every entry
must resolve to a compatible device, and both preference indices and physical
device UUIDs must be unique. Invalid, unavailable, incompatible, or duplicate
entries fail export without selecting a substitute device.

The flag contains the complete worker set. The canonical adapter renders tiles
only when its index is included in that list. `--atlas_vk_device_index` and the
single Linux scene-export value accepted by `--use_gpu_devices` select the
canonical engine independently.

## Tile Descriptor and Result Contract

The unit of work is a spatial tile.

- A tile runs the complete active filter and compositor pipeline.
- Stereo remains inside one attempt; eyes are not separate tasks.
- Transparency resolves inside the tile; no OIT state crosses devices.
- A tile returns final display pixels rather than native GPU resources or an
  intermediate render target.
- Returned images contain only the guard-free valid region.

`Z3DTileDescriptor` stores the full output extent, a bottom-left-origin
half-open valid output rectangle, and guard width. Checked accessors derive the
expanded attachment extent and origin, normalized frustum bounds, valid
attachment crop, and top-left assembly origin. Guards may extend beyond the
full output so edge tiles retain the same filtering context.

`makeZ3DTileDescriptors()` covers the complete output exactly once, including
odd extents and edge tiles, in bottom-row-first serpentine order.

`Z3DRenderedTile` owns top-left-origin RGBA pixels. `primaryColor` contains the
mono or left-eye result; `rightColor` is present only when the same attempt
rendered a stereo pair. These are save-oriented straight-alpha pixels after
premultiplied-color correction, not interactive premultiplied ready-buffer
data.

`renderFrame()` requires a non-empty descriptor sequence that shares one full
output extent, contains no overlapping valid rectangles, and covers every
output pixel exactly once. It allocates one `Z3DRenderedFrame`, collects tiles
in completion order, validates each tile's size, RGBA8 type, and eye mode, and
immediately pastes it at `topLeftAssemblyOrigin()`. Completed tile images are
released instead of being retained for a second full-frame assembly pass. The
returned primary and optional right-eye images are top-left-origin and ready to
save without another vertical flip.

Descriptors and results contain no engine, device, QObject, scheduler, or
native resource handle.

## Batch State Snapshot

Before a coordinated batch submits any tile, the coordinator captures:

1. global and compositor JSON state;
2. every available canonical object-view state;
3. device pixel ratio; and
4. the complete runtime camera, including fields omitted from scene JSON.

It then applies the same captured values to every worker in this order:

1. object-view states;
2. device pixel ratio;
3. global and compositor state; and
4. complete camera state.

Object state precedes global state so bound-dependent parameters use final
canonical object ranges. Camera state is applied last so worker-local
provisional bounds cannot replace its clipping range. Full-frame output size is
not copied; every tile submission installs its own attachment extent while the
camera receives the descriptor's full output extent.

State capture and application are synchronous on the canonical rendering
thread and do not pump Qt events. No queued document or parameter update can
interleave with a batch. Synchronization is allowed only when no worker has an
outstanding tile. If capture or application fails for any worker, the entire
worker set is discarded and no tile is submitted.

After synchronization, every worker sets the common full-output camera
viewport and prepares mesh filters for that full view. The prepared export LOD
remains fixed while the worker renders its assigned tiles and is released after
the complete frame has been assembled. Each submitted descriptor must match
the extent used for that preparation.

Workers retain ordinary document-to-view subscriptions for object and data
lifetime changes between batches. The coordinator does not expose a batch
ticket that survives across rendering-thread event-loop turns.

## Submit, Collect, and Dynamic Refill

Each worker accepts exactly one outstanding tile. `submitVulkanTile()`:

1. validates worker role, thread affinity, device ownership, and healthy
   submission state;
2. observes cancellation before recording;
3. installs the attachment extent, full-output viewport, guarded tile frustum,
   and compositor rendering region;
4. records and submits the complete final-quality mono or stereo pipeline with
   `ReadbackCompletionPolicy::ReturnAfterSubmit`; and
5. retains the descriptor, eye mode, and render-frame token until collection.

The tile projection and output extent remain unchanged while final-pixel
readback is pending. `isVulkanTileReady()` pumps completion safe points and
accepts readiness only when the published eye token or tokens exactly match the
outstanding render-frame token. A newer publication while an older tile remains
outstanding is an invariant violation.

`collectVulkanTile()` validates the ready buffers, copies and crops the valid
region into owned images, normalizes image orientation, clears the outstanding
assignment, and restores the worker's full-frame projection.

`renderFrame()` uses a dynamic shared queue:

1. submit one tile to each worker while unassigned tiles remain;
2. poll every assigned worker on the canonical rendering thread;
3. collect completed pixels for the original tile index;
4. paste the owned result into the output frame and release the tile image;
5. immediately give that worker the next unassigned tile; and
6. return only after every output pixel has a result.

Workers therefore receive work according to observed completion rate instead
of a fixed equal partition. Polling does not process Qt events. Independent
device queues may overlap, although mandatory synchronization inside an
individual rendering pipeline can limit the amount of overlap for that tile.

## Cancellation and Failure Cleanup

Cancellation is observed before initial submission, between assignments, and
between completion-poll iterations. Once cancellation is observed, the
coordinator submits no new tiles. Worker teardown drains already-submitted work
to its completion boundary; partial results are not returned.

Any synchronization, rendering, collection, or cancellation exception makes
the batch fail as a unit. The coordinator:

- stops assigning work;
- destroys every worker on the canonical rendering thread;
- lets engine teardown drain known submissions and completion safe points;
- discards all partial results; and
- rethrows the failure.

The coordinator does not continue on a reduced device set and does not reuse a
partially synchronized or failed worker set. A later call observes that no
healthy workers remain.

## Headless Scene Export

`ZRunExport3DScene` owns the coordinator for one export operation. With an
empty worker-index flag, it calls the ordinary fixed-size screenshot path. With
a non-empty valid worker list, it creates exact-device workers and passes the
coordinator to the fixed-size screenshot API. A multi-tile capture renders
final-quality tiles, assembles them as they complete, and writes the same mono
image output used by direct scene export.

The scene runner requests `MonoView`. The coordinator and frame types preserve
complete stereo-pair tile semantics, but no application entry point routes
stereo export through the coordinator. Headless animation export does not use
the in-process coordinator.

Atlas does not normalize numerical results across physical devices. Each tile
contains the pixels produced by its assigned adapter and driver.
Device-dependent floating-point, depth, fragment-order, and backend feature
behavior can therefore produce per-tile differences, especially for
transparency modes. Guard pixels preserve spatial filtering context but do not
reconcile cross-device numerical differences. Every selected adapter must be
able to render the scene correctly and satisfy the canonical adapter's resource
plan. Direct rendering on each adapter verifies device-local output but does
not by itself verify a plan derived from a stronger canonical adapter. Use the
least-capable adapter as canonical, validate the complete worker set on the
target scene, or use equivalent adapters. Use direct rendering when uniform
output across tile boundaries is required.

Dynamic refill assigns later tiles according to the completion order observed
by the coordinator. When heterogeneous adapters produce different pixels, a
later tile can be assigned to a different adapter on another run. Exact
tile-to-adapter assignment, assembled pixels, and image hashes are therefore
not guaranteed to repeat across runs.

The ordinary Vulkan submission lifecycle also applies:

- recording either submits a complete command buffer or enters the
  definitive-unsubmitted abort path;
- definitive-unsubmitted cleanup drains resource-release hooks, discards
  fence-completion callbacks, retires unavailable readbacks, and records the
  first sticky device failure;
- queue-submission or fence-observation failures are fatal because submission
  ownership is uncertain;
- submitted resources remain alive until fence completion; and
- a readback staging slot is reusable only after both its producer and consumer
  owners finish.

## Validation

CPU-only tests cover tile geometry and projection, output orientation,
descriptor invariants, mono/stereo frame allocation and odd-grid top-left
placement, exact index-and-UUID selection, readback ownership, and publication
identity without constructing a Vulkan context or logical device.

The opt-in Vulkan coordinator smoke tests run only when
`ATLAS_ENABLE_VULKAN_SMOKE_TEST=1`:

- `SingleAdapterPpllBatchPreservesParityAndCanonicalEngine` validates the
  one-worker batch path, synchronized state, exact same-adapter PPLL tile
  parity, teardown, and continued canonical rendering. Its synthetic scene
  contains opaque and translucent geometry and verifies a non-empty translucent
  contribution before comparing tiled results.
- `DistinctPhysicalDevicesCompleteBatchAndPreserveCanonicalEngine` requires at
  least two distinct physical-device UUIDs in the canonical engine's
  planning-compatible worker set, otherwise it skips. It constructs exact
  workers on two devices, executes the dynamically refilled PPLL batch,
  validates the assembled frame contract, records cross-device pixel deltas as
  diagnostics, and verifies that canonical state and rendering remain intact.
  Device-dependent deltas are not a pass/fail threshold.

With the environment variable unset, the smoke tests skip before constructing
`QApplication`, `Z3DRenderingEngine`, or `ZVulkanContext`. Normal CI therefore
does not require a Vulkan ICD or GPU. Atlas still retains its normal Vulkan SDK
and shader-compiler build prerequisites.
