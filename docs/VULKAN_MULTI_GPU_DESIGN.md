# Atlas Vulkan Multi-GPU Tile Rendering Design

> Status: Phase 0 export baselines and one interactive correctness seed are
> captured, and the Phase 1 single-device foundation is implemented locally.
> Multi-device execution remains disabled; the Phase 2 tile runtime, secondary
> device domains, and pipeline replicas are still a proposed architecture.
>
> Scope: Independent-device, in-process Vulkan rendering using whole-pipeline
> screen tiles for both interactive frames and scene/animation export. OpenGL
> and ordinary single-device Vulkan rendering retain their current direct paths.
>
> Deliberate exclusions: no stereo-eye scheduling, object/layer partitioning,
> alternate-frame rendering, generic task graph, Vulkan device group, external
> memory transport, or cross-device GPU composition in the first implementation.
>
> Related references:
> [Developer Guide](DEVELOPER_GUIDE.md) and
> [Image Paging and Progressive Rendering](Atlas_Image_Paging_and_Progressive_Rendering.md).

## Contents

- [0. Decision](#0-decision)
- [1. Pre-Phase-1 Baseline Findings](#1-pre-phase-1-baseline-findings)
- [2. Goals, Non-Goals, and Product Boundary](#2-goals-non-goals-and-product-boundary)
- [3. Zero-Cost Single-Device Contract](#3-zero-cost-single-device-contract)
- [4. Minimal Object Model](#4-minimal-object-model)
- [5. Shared Tile Runtime](#5-shared-tile-runtime)
- [6. Device-Local State and Vulkan Completion](#6-device-local-state-and-vulkan-completion)
- [7. Transparency, Paging, and Stereo](#7-transparency-paging-and-stereo)
- [8. Failure, Supersession, and Teardown](#8-failure-supersession-and-teardown)
- [9. Device Selection, Memory, and Performance](#9-device-selection-memory-and-performance)
- [10. Delivery Phases](#10-delivery-phases)
- [11. Validation and Benchmark Plan](#11-validation-and-benchmark-plan)
- [12. Expected Code Impact](#12-expected-code-impact)
- [13. Deferred Alternatives](#13-deferred-alternatives)
- [14. Final Architecture Test](#14-final-architecture-test)
- [15. References](#15-references)

## 0. Decision

Atlas should use one concrete, tile-only Vulkan runtime for multi-device work.
That runtime serves both interactive rendering and export. Export is the first
deterministic validation vehicle; it is not the architectural boundary and
interactive rendering is not a later generalization.

The earlier design introduced more layers than whole-pipeline tiles need. A
tile has a deliberately simple dependency shape:

```text
immutable frame inputs
        |
        +---- complete tile pipeline on device 0 ---- final host pixels --+
        +---- complete tile pipeline on device 1 ---- final host pixels --+-- coherent join
        `---- complete tile pipeline on device N ---- final host pixels --+
```

Tiles do not consume another tile's GPU output. Native resources never cross a
device boundary. The only join is final pixel assembly. The first implementation
therefore needs only three new concrete lifetime owners:

1. `ZVulkanTileRuntime final`: the optional engine-owned scheduler, generation,
   completion, and assembly owner while multi-device rendering is enabled.
2. `ZVulkanDeviceDomain final`: the RAII owner for one selected secondary
   logical device and its device-level state.
3. `ZVulkanTilePipelineReplica final`: one independent physical realization of
   the complete pipeline for one worker/device.

Everything else is a plain value or a private runtime struct:

- `ZVulkanContext::DeviceSelection` identifies an adapter explicitly;
- `Z3DTileRequirements` reports tile eligibility and guard requirements;
- `Z3DTileDescriptor` contains pure tile geometry and camera transforms;
- `WorkerSlot`, `GenerationState`, and `TileAttempt` remain private, stable
  implementation structs inside `ZVulkanTileRuntime`.

There is no public ticket hierarchy, render-fleet interface, task graph,
transport policy, resource registry, or abstract frame sink. The runtime exposes
two concrete entry points backed by the same private scheduling state machine:

```cpp
requestInteractiveFrame(/* current logical inputs */);
renderExportFrame(/* frozen logical inputs and output request */);
```

The output action is an ordinary enum/switch at the join boundary: publish a
coherent interactive buffer or return a complete export image. It is not a
virtual interface.

### 0.1 Work decomposition

The only GPU scheduling unit is a spatial tile. This rule applies equally to
interactive frames and exports.

- A tile runs the whole active filter/compositor pipeline.
- A tile contains all requested eyes; stereo eyes are never separate tasks.
- A tile returns final display pixels, never an intermediate attachment for
  another GPU to reduce.
- Interactive progressive state stays with the tile's worker.
- Export tiles use next-free assignment because they are one-shot,
  non-progressive jobs.

No object, layer, eye, geometry range, volume brick, or alternate frame becomes
a scheduler task in this design.

### 0.2 Top-level routing

`Z3DRenderingEngine::processFrame()` remains the direct one-device implementation.
Multi-device routing occurs once in the engine's frame/export entry code:

```text
OpenGL, multi-device disabled, fewer than two usable selected devices,
or current pipeline not tileable
    -> existing direct processFrame()/export path

compatible Vulkan multi-device mode active
    -> ZVulkanTileRuntime
    -> complete independent tiles on persistent device-local replicas
    -> coherent host assembly
    -> interactive ready-buffer publication or export result
```

An interactive multi-device frame bypasses the live direct pipeline just as a
multi-device export does. The engine's primary device may participate, but it
uses an isolated tile pipeline replica. The live primary filters/compositor
remain the canonical logical/UI state and stay idle as physical render state
while the runtime is active.

### 0.3 Abstractions deliberately rejected

The following remain removed or deferred:

- `ZVulkanPlatform` and `ZVulkanTopology`;
- `ZVulkanExecutionNode` and `ZVulkanRenderFleet`;
- a general frame-plan builder or dependency DAG;
- polymorphic tile tasks or output sinks;
- generic frame/resource registries;
- transport-policy interfaces;
- process-wide domain-incarnation keys;
- device-group, peer-memory, and external-memory implementations.

Their correctness concerns are handled by explicit ownership, immutable device
affinity, generation IDs, and direct owner checks. They do not need to become
architectural layers in Phase 1 or Phase 2.

## 1. Pre-Phase-1 Baseline Findings

### 1.1 Existing single-device ownership

At the pre-Phase-1 baseline, each `Z3DRenderingEngine` owned one:

- `ZVulkanContext` and `ZVulkanDevice`;
- mixed-backend scratch pool;
- live filter pipeline and compositor;
- Vulkan backend per renderer base;
- set of per-filter/per-eye output leases and progressive state.

`ZVulkanContext` enumerates and evaluates physical devices but selects one and
creates one logical device. `ZVulkanDevice` is already the correct fundamental
Vulkan ownership island: it owns allocator, residency, descriptors/bindless
state, executor, staging, and completion state. Multi-device support should
replicate that island rather than make a device wrapper retargetable.

### 1.2 Existing interactive state machine

The current interactive path is not simply a call to a renderer. It has an
event, cancellation, progressive, completion, and publication contract:

```text
camera/parameter/data/resize change
  -> filter/compositor invalidation
  -> low-priority UpdateRequest
  -> renderFast(): one progressive processFrame() step
  -> first preview may become ready asynchronously
  -> lower-priority LayoutRequest
  -> render(): progressive steps until complete
  -> compositor swaps a ready host buffer
  -> renderingFinished
  -> Z3DCanvas consumes the current ready buffer under targetSwitchMutex
```

`UpdateRequest` outranks progressive `LayoutRequest`. A new input cancels or
coalesces older work. Vulkan completion is pumped by the render-thread timer,
and current compositor callbacks use render tokens to reject stale readbacks.
The canvas already consumes host-visible RGBA data through
`Z3DLocalColorBuffer`; initial multi-device presentation therefore does not need
a cross-device image or an Atlas-managed upload to the primary GPU.

A multi-device runtime must preserve these visible semantics:

- responsive camera, parameter, and resize invalidation;
- a coherent first preview followed by coherent refinement;
- no old camera or old-size publication after a newer request;
- one `renderingFinished` publication per completed visible wave;
- deterministic progress and cancellation;
- generation-consistent synchronous picking.

It cannot reuse the export assumption that mutations are frozen until an entire
session ends.

### 1.3 Existing tiled export is sequential mutable state

The current tiled screenshot code provides useful semantics:

- output partitioning;
- normalized tile frusta;
- border expansion;
- valid-region cropping;
- CPU image assembly;
- mono and stereo file formatting.

It is not a concurrent scheduler. Before each tile it mutates the engine output
size, shared camera viewport/frustum, compositor rendering region, filter output
sizes, and retained output targets. It calls `processFrame()`, converts/crops/
pastes the result, and reclaims Vulkan scratch before continuing.

The layout, projection, crop, orientation, and stereo formatting rules should
be preserved, but the shared mutable pipeline cannot be invoked concurrently.
Tile geometry must first be extracted into a pure helper.

### 1.4 Physical-resource discovery was globally coupled

Vulkan buffer and texture wrappers already retain their owning
`ZVulkanDevice&`, which is sufficient for direct native-owner validation.
However, the baseline Vulkan backend discovered the device through the global
scratch pool, rendering code directly consulted the global pool in many places,
and renderer bases shared singleton mutable renderer state. Those assumptions
could not identify two simultaneous physical pipelines safely.

Phase 1 replaced discovery with constructor-injected concrete references:

```cpp
Z3DRendererBase(/* existing state references */,
                Z3DScratchResourcePool& scratchPool,
                RenderBackend initialBackend);

Z3DRendererVulkanBackend(Z3DScratchResourcePool& scratchPool,
                         ZVulkanDevice& device);
```

This is dependency injection, not a device-provider abstraction. The primary
path receives direct references to the objects it already owns. A backend's
device affinity becomes immutable.

### 1.5 Device-local physical pipeline state is unavoidable

It is unsafe to temporarily point the live pipeline at another device, even
when all command recording happens on one CPU thread. Earlier commands can
remain in flight while the next device records. Every active device needs its
own:

- Vulkan renderer backends and pipeline caches;
- descriptor/bindless state;
- scratch and output leases;
- compositor targets and readback resources;
- progressive accumulators and per-eye validity;
- PPLL buffers and continuation state;
- static geometry and dense-image GPU caches;
- eventually, paged-image mappings and uploader state.

This applies to the primary adapter too. The multi-device worker borrowing the
primary device/scratch pool owns an isolated tile replica and never aliases the
live camera, filter validity, progressive leases, compositor ready buffers,
picking callbacks, or publication callbacks.

Replicas persist across interactive frames and animation frames so device,
pipeline, descriptor, geometry, and texture setup is amortized. Persistence is
required for interactive feasibility, not an optional future optimization.

### 1.6 Final quality and synchronous waiting were coupled

The baseline Vulkan backend treated non-progressive/final rendering with
readback as a synchronous wait in `Z3DRendererVulkanBackend::endRender()`.
Reusing that behavior for tiles would submit worker 0 and wait for its readback
before worker 1 is submitted, eliminating GPU overlap.

Phase 1 separated two independent decisions:

- render quality: progressive step versus final/non-progressive evaluation;
- completion policy: return an asynchronous completion attempt versus wait for
  required readbacks.

Existing direct paths retain their original default behavior. Only isolated
tile replicas will request final-quality asynchronous attempts. This
prerequisite belongs below the runtime and does not add a scheduler branch to
draw code.

### 1.7 Source lifetime cannot rely on raw live-filter pointers

Interactive invalidations and structural document changes are distributed
through current QObject connections; there is no single mutation gate that can
freeze every change safely. A persistent runtime must therefore be isolated
from live QObject lifetime:

- generation synchronization copies scalar values and camera/view state;
- canonical data is retained through immutable pack generations or explicit
  owner pins;
- CPU spans are consumed during immediate lowering or their owner is pinned by
  the attempt;
- a replica never retains an unowned pointer into a removable live filter;
- topology changes cancel current assignment and rebuild/version replicas at a
  per-worker safe point.

A generic deep copy of the entire scene is unnecessary, but a small immutable
per-generation capture and exact source ownership are mandatory.

### 1.8 Logical input change was conflated with physical validity

At the baseline, `Z3DFilter::invalidate()` and `Z3DCompositor::invalidate()`
only exposed an edge-triggered notification when a previously valid live output
became invalid. That was correct for the direct pipeline, but it would block a
runtime while the live physical pipeline stayed idle: its validity bits would
remain invalid after the first request, so a later camera or parameter change
could be logically new without producing another edge-triggered notification.

The runtime must not mark the live primary pipeline valid after replicas render;
doing so would let a later direct fallback reuse physical outputs that the live
pipeline never produced. Phase 1 therefore added a separate unconditional
logical-change notification, `renderInputChanged()`:

- emit it whenever render-affecting invalidation is requested, even when the
  relevant live validity bit is already invalid;
- keep the existing `invalidated()` signal edge-triggered for direct-path
  physical-validity semantics;
- while multi-device mode is active, coalesce the unconditional notification
  into the newest desired input revision and cancel the active generation;
- explicitly bump the desired revision for pipeline-topology and output-size
  changes;
- connect/disconnect the signal as filters enter/leave the logical pipeline.

This is a separation of existing concepts, not a scheduler abstraction. It is
required to make repeated interaction observable while keeping direct fallback
correct.

## 2. Goals, Non-Goals, and Product Boundary

### 2.1 Goals

- Use two or more independent Vulkan logical devices to render spatial tiles
  concurrently for interactive and export requests.
- Preserve the existing direct OpenGL and single-device Vulkan implementations.
- Preserve complete output coverage; never omit a tile, pixel, eye, object, or
  page request.
- Preserve Atlas' progressive preview-to-final interaction model.
- Coalesce rapid camera/parameter/resize changes to the newest generation and
  never publish a stale or wrong-sized frame.
- Publish interactive pixels only as a coherent wave; never mix generations,
  extents, eyes, or progressive stages.
- Keep GPU resources and completion state inside the device that created them.
- Support non-paged opaque, blend-delayed, DDP, weighted, and exact PPLL modes
  through whole-tile execution, gated separately by parity.
- Read final tile pixels to host and assemble them directly into reusable host
  buffers, using the current canvas-ready-buffer route for initial presentation.
- Submit work to all free selected devices before waiting so GPU execution
  overlaps while CPU recording stays on the rendering thread.
- Reuse current export and interactive benchmarks and extend them for burst
  invalidation, resize, and generation-aware publication tests.
- Keep early implementation types concrete, final, and non-polymorphic.

### 2.2 Non-goals

- Multi-GPU OpenGL.
- A distributed renderer or generic task framework.
- Stereo eyes as tasks; a spatial tile is the only scheduling unit.
- Object, filter, layer, geometry, or volume-brick partitioning.
- Alternate-frame rendering.
- Cross-device GPU composition or intermediate attachment transfer.
- Device groups, peer memory, external memory, or external semaphores.
- Concurrent worker CPU threads or mutation of engine QObjects off the rendering
  thread.
- A generic immutable copy of the entire scene.
- Aggregating multiple devices' VRAM into one apparent budget.
- Paged-volume multi-device execution in Phase 2a.
- Automatic rebuilding of a failed multi-device runtime in the first release.
- Partial-tile interactive publication.

### 2.3 Product boundary

Both interactive rendering and scene/animation export are Phase 2a consumers of
the same architecture. Export should land first inside that phase because hashes,
large images, and controlled source lifetime make it the best correctness oracle.
Phase 2a is not complete until interactive camera motion, resize, cancellation,
progressive refinement, coherent publication, and picking pass their gates.

The initial host-readback assembly route is intentionally portable and matches
Atlas' current Vulkan-to-canvas boundary. It must still prove an end-to-end
interactive speedup. If readback, assembly, Qt handoff, or synchronization erases
the GPU gain, the feature remains opt-in while transport is revisited; the
design must not claim general acceleration from summed GPU time alone.

## 3. Zero-Cost Single-Device Contract

“Zero-cost” means no multi-device scheduler/domain lookup, allocation, routing
branch, container indexing, or extra virtual dispatch in filter, pass, batch,
or draw hot paths. Fixed hard ownership checks remain at resource acquisition,
native-realization, descriptor-priming, and surface-conversion boundaries; they
enforce physical-pipeline contracts without selecting or routing a device.
Supporting runtime-selectable multi-device rendering necessarily needs one
predictable branch at the frame/export routing boundary; claiming literally
zero additional instructions at that boundary would be misleading.

### 3.1 Direct path

When multi-device rendering is disabled, resolves to fewer than two usable
devices, or is ineligible for the current pipeline:

- `Z3DRenderingEngine::processFrame()` executes the existing filter loop;
- `Z3DRendererBase::m_backend` remains singular;
- a renderer never indexes a backend/device vector;
- filters do not look up domains, workers, generations, or tasks;
- render commands do not carry a device ID or scheduler pointer;
- no tile/generation/task storage is allocated;
- no scene or geometry snapshot is copied;
- no multi-device virtual call exists;
- no multi-device branch is added per filter, pass, batch, or draw.

The engine's `renderFast()`, `render()`, and export entry points contain one
top-level route to the nullable runtime. The direct branch immediately executes
the current code. A predictable pointer/state test at this coarse boundary is
less costly and clearer than an indirect strategy call.

### 3.2 Primary state stays direct

The engine's primary context, device, scratch pool, live pipeline, and compositor
remain directly owned. They are not wrapped in a one-element fleet, domain,
worker array, or task graph for symmetry.

Phase 1 introduces no tile runtime and does not require a device domain. It only
makes existing primary dependencies explicit and immutable. The domain owner is
introduced in Phase 2 for secondary devices.

### 3.3 Lazy persistent multi-device storage

`Z3DRenderingEngine` holds a nullable `std::unique_ptr<ZVulkanTileRuntime>` only
after Phase 2 is compiled. The object is constructed when Vulkan multi-device
mode is activated with at least two compatible selected devices. It owns
secondary domains and all tile replicas while that mode remains active.

Persistent across frames:

- selected device domains and executor state;
- worker slots and physical pipeline replicas;
- pipeline/descriptor/static geometry/dense texture caches;
- reusable readback and host assembly buffers;
- per-device timing history needed only for reporting.

Transient per request/generation:

- immutable captured input values and pins;
- tile descriptors/status;
- progressive wave state;
- active attempts and cancellation/supersession state;
- export result formatting state.

The runtime is destroyed on multi-device disable, selected-device change,
backend switch, engine teardown, or unrecoverable runtime/device failure. It is
not recreated for each interactive frame, screenshot, or animation frame.

### 3.4 Acceptance gate

Before Phase 2 is enabled, compare pre/post Phase 1 and Phase 2 builds with
multi-device mode disabled:

- no new allocation in `processFrame()`;
- no new inner-loop dispatch, lookup, or branch;
- identical renderer/filter call structure;
- identical deterministic output hashes;
- no statistically meaningful regression in one-device export time;
- no statistically meaningful regression in interactive request-to-preview,
  preview-to-final, visible FPS, resize recovery, or cancellation behavior.

Owner-device `CHECK`s occur when a native resource enters or changes a
device-local binding/cache. Bindless registration and descriptor priming
validate ownership before recording; recording-time bindless lookup does not
repeat the check for the same unchanged resource on every draw.

## 4. Minimal Object Model

### 4.1 Ownership overview

```text
Z3DRenderingEngine                                      [rendering thread]
  +-- primary context/device/scratch                    [always direct]
  +-- live logical filter pipeline/compositor           [canonical UI state]
  `-- optional ZVulkanTileRuntime                       [only multi-device mode]
        +-- stable WorkerSlot 0
        |     +-- borrows primary device/scratch/executor
        |     `-- owns ZVulkanTilePipelineReplica
        +-- stable WorkerSlot 1 ... N
        |     +-- owns ZVulkanDeviceDomain
        |     `-- owns ZVulkanTilePipelineReplica
        +-- at most one active TileAttempt per worker
        +-- at most one physically active GenerationState
        +-- one coalesced newest pending logical revision
        +-- cancelled attempts that only retire their own resources
        +-- reusable interactive ready/building host buffers
        `-- export assembly buffers while an export request is active
```

`WorkerSlot`, `GenerationState`, and `TileAttempt` are private structs. Worker
slots use stable-address storage created before work submission. Deferred
callbacks capture runtime incarnation, input generation, progressive wave,
worker index, tile ID, device identity, and immutable token/timing values. They
never retain a pointer to a movable vector element or query mutable “current
frame” globals.

Only one generation is physically active. A new interactive input immediately
makes it unschedulable and records only the newest desired logical revision.
Already-submitted Vulkan work may remain physically in flight, but it can do
nothing except release its own resources. The runtime captures and starts the
new generation only after all attempts from the cancelled wave reach terminal
safe points. This deliberate barrier avoids simultaneous replica versions or
two scene generations across workers in the first implementation.

### 4.2 `ZVulkanContext::DeviceSelection`

This small value makes device selection an explicit constructor input rather
than a read of only `--atlas_vk_device_index`. It contains the
preference-sorted requested index and expected physical-device UUID. UUID is the
authoritative identity across independently created Vulkan instances;
construction rejects an index that no longer resolves to the expected UUID.

The existing default startup behavior remains for the primary engine. An
acceptable first multi-device implementation creates one Vulkan context/instance
per selected secondary adapter. The cost is paid once when multi-device mode is
activated. A shared platform/instance layer is deferred unless profiling shows
material startup or memory cost.

### 4.3 `ZVulkanDeviceDomain final`

This concrete RAII aggregate exists only for secondary workers. It owns:

- one explicitly selected `ZVulkanContext`;
- one `ZVulkanDevice`;
- one device-bound Vulkan scratch pool;
- executor/completion, allocator, residency, bindless, and staging state already
  owned through the device;
- immutable per-device support information.

Worker 0 borrows the primary device and scratch pool directly; it is not wrapped
in a domain merely for symmetry. Secondary initialization must not overwrite
the process-global/primary `Z3DGpuInfo` capability view.

### 4.4 `ZVulkanTilePipelineReplica final`

Each worker owns one independent physical realization of the complete active
pipeline. It contains typed filter/renderer/compositor state and device-local
resources, not cloned live QObjects and not a generic resource map.

At an accepted generation boundary:

1. The runtime captures camera, viewport, eye/view mode, scalar parameters,
   topology version, canonical data-generation references, and cancellation/
   performance identity once on the rendering thread.
2. After every attempt from the preceding generation is terminal, each worker
   synchronizes its replica from that immutable capture.
3. No worker starts the new capture while another worker still executes the old
   generation.
4. `recordTile()` derives replica-owned camera/frustum/output state from the
   descriptor and records the complete pipeline.
5. CPU spans are lowered immediately or retained with explicit owner pins.

The replica never mutates live filter validity, global progress, camera state,
ready buffers, picking manager, or UI signals. A topology change increments the
captured topology generation. Each worker rebuilds its typed physical list only
at its safe point; its stale attempt keeps any old physical state and pins it
needs until completion.

Static caches survive input generations when their canonical resource
generation is unchanged. The first implementation conservatively resets
tile-output validity and progressive accumulators for every new input generation;
it may retain pipelines, descriptors, geometry, and texture caches. Finer
invalidation reuse can be added inside typed replica code after measurement,
without changing the runtime architecture.

### 4.5 Plain tile values

`Z3DTileRequirements` reports:

- whether every active pass/filter is safely tileable;
- the combined finite guard at the requested scale;
- whether full-frame logical coordinates are required;
- a reason when tiling is unsupported.

Requirements are evaluated when multi-device mode activates and again when
pipeline topology, relevant effect parameters, output scale/extent, or paging
mode changes. Automatic mode falls back to the matching direct interactive or
export path; an explicitly forced request reports the reason.

`Z3DTileDescriptor` is a trivially movable value such as:

```cpp
struct Z3DTileDescriptor {
  uint64_t tileId;
  QSize fullOutputExtent;
  QRect validOutputRect;
  QRect expandedRenderRect;
  QSize attachmentExtent;
  Z3DScreenCoordinateTransform attachmentLocalToFullFrame;
  Z3DCameraSnapshot camera;
};
```

Extents and rectangles are per-eye. Valid rectangles cover every requested
pixel exactly once. Expanded rectangles include the combined guard. Camera and
screen transforms are derived without mutating the live camera.
`Z3DScreenShotType` is not part of tile geometry; export formatting remains an
export-entry concern.

### 4.6 `ZVulkanTileRuntime final`

The runtime is the only scheduler and assembly owner. Its public behavior is
small:

- activate/deactivate selected devices at an engine safe point;
- request or supersede an interactive input generation;
- advance/poll all domains from the existing render-thread completion pump;
- render one export image through the same private tile state machine;
- expose current ready host buffers to the existing engine/canvas accessors;
- produce one exact, generation-matched primary-device picking target;
- report progress, failure, and per-device/tile measurements.

It does not expose workers or domains to filters. It does not implement a generic
render interface. Its private `GenerationState` contains only what this tile
renderer needs:

```text
purpose: Interactive | Export
inputGeneration
progressiveWave
immutable camera/view/extent/scalar values and source pins
flat descriptors and statuses
tile-to-worker affinity for interactive progressive work
per-tile progress and complete coverage accounting
building/published host buffer identity
cancellation/supersession/failure state
one render/performance token for the coherent wave/output
```

Input generation and progressive wave are deliberately separate. Camera,
parameter, scene, or resize changes increment `inputGeneration` and reset wave
zero. Refinement of identical inputs increments `progressiveWave`, preserving
the tile's device-local progressive accumulator.

## 5. Shared Tile Runtime

### 5.1 Activation and deactivation

Entering multi-device mode is a rendering-thread safe-point operation:

1. stop accepting a new direct primary frame;
2. drain direct primary submissions, callbacks, and Vulkan scratch leases;
3. construct secondary domains and all worker replicas;
4. synchronize no frame yet; wait for the next explicit frame request;
5. route the existing Vulkan completion timer to the runtime;
6. invalidate the full logical frame and post `UpdateRequest`.

The primary live pipeline remains constructed as canonical logical state but
does not physically render while the runtime is active. There is exactly one
completion-poll owner per device.

Leaving multi-device mode stops new assignment, supersedes the current
generation, retires/drains attempts, destroys replicas before their devices,
restores the ordinary primary completion route, invalidates the direct pipeline,
and posts the newest pending `UpdateRequest`.

Backend switching must test runtime activity directly; the current direct-render
cancellation source is not a sufficient proxy for secondary submissions. Record
the requested switch, cancel the runtime, and apply the switch only after every
attempt/callback is terminal and the completion-pump lease is released.

### 5.2 Interactive event integration

The direct path remains unchanged. In multi-device mode only, the coarse engine
entry points behave as follows:

```text
UpdateRequest -> renderFast()
  -> runtime records/coalesces the newest desired logical revision
  -> if no cancelled wave is retiring, capture immutable inputs
  -> submit one tile on every worker
  -> return to Qt event loop

completion timer / safe-point callback
  -> poll every device nonblockingly
  -> retire cancelled attempts
  -> when the cancelled wave is fully terminal, capture only the newest revision
  -> copy current tile completions
  -> submit the current wave when all replicas are synchronized
  -> if coherent wave complete: publish once
  -> if progress < 1: post lower-priority generation-tagged refinement

LayoutRequest -> render()
  -> ignore a stale/mismatched refinement request
  -> runtime.beginNextProgressiveWave() only for the published generation
  -> submit available tile work
  -> return to Qt event loop
```

The existing tight `render()` loop remains intact for the direct path. The
multi-device branch must not spin until GPU completion because that would block
camera/resize events and prevent the QTimer from firing.

`UpdateRequest` continues to outrank `LayoutRequest`. A new interactive request:

1. increments the desired logical revision immediately;
2. cancels its generation token;
3. stops assigning pending tiles from the previous generation;
4. marks old in-flight attempts superseded;
5. discards the old partial assembly;
6. records only the latest desired logical revision while the old wave retires;
7. waits until every old attempt reaches a terminal safe point;
8. captures the latest logical inputs once, synchronizes every replica, and
   starts the new wave on all workers.

Vulkan submissions cannot generally be physically cancelled. Supersession is a
logical publication decision, not a promise that GPU execution stops. Tile size
therefore affects interactive cancellation latency and is part of the measured
configuration.

There is one active generation plus one coalesced desired revision, not a queue
of frame objects. Repeated invalidations replace the pending revision and do not
build an unbounded backlog. Cancelled attempts own all resources needed for
physical retirement and can never update progress, assembly, ready buffers, or
signals.

The runtime retains a cancellation source for the full physical lifetime of the
active generation; it is not reset merely because CPU recording returned. A
UI-thread call such as `cancelActiveRender()` may only request cancellation
through thread-safe shared state. Tile status, replica state, assembly, and
scheduling remain rendering-thread-only.

Use a private generation-tagged refinement event, or an equivalently exact
runtime identity check, instead of trusting an unqualified queued
`LayoutRequest`. A refinement posted for an old generation must never advance a
new one.

### 5.3 Interactive progressive waves and affinity

Current progressive raycasting/slicing state, per-eye validity, accumulators,
and leases are physical replica state. Moving a partially refined tile to
another device would require an accumulator transfer or duplicate state, which
the early design excludes.

For the first interactive implementation:

- generate one nonempty rectangular spatial tile per participating worker;
- use deterministic near-equal valid-pixel stripes initially;
- include the required overlap guard around every stripe;
- pin each tile to that worker for the entire input generation;
- let every progressive wave advance each incomplete tile once;
- retain a completed tile's last pixels while other tiles refine;
- publish only when every incomplete tile selected for the wave has completed
  that same wave.

For full-height vertical stripes, the participating worker count cannot exceed
the output width in physical pixels because zero-width tiles are invalid. Extra
selected workers remain idle for that extent; if fewer than two nonempty tiles
are possible, route the frame directly. This is a geometric correctness rule,
not a silent work limit.

One tile per worker keeps exactly one progressive state set per replica and
avoids a new per-tile accumulator cache. It is a deliberate early-phase
simplicity trade-off. Widths weighted by measured worker throughput, or several
persistent tiles per worker, are Phase 3 optimizations only if equal stripes
show material imbalance.

At the start of a later wave, the building buffer is initialized from the last
published coherent buffer for tiles already final, then updated by all tiles
advanced in that wave. No mixed-wave buffer is exposed.

Per-tile progress must be finite and within `[0, 1]`. Overall interactive
progress is weighted by valid pixel area rather than equal tile count:

```text
frameProgress = sum(tileValidPixels * tileProgress)
                / sum(tileValidPixels)
```

For stereo, a tile's reported progress is the minimum progress of its requested
eyes, and publication waits for both eyes. `progressChanged` is emitted from the
coherent wave result, not from arbitrary individual tile completion.

### 5.4 Export scheduling through the same runtime

Export uses the same domains, replicas, descriptor type, attempt state,
completion polling, row assembly, OIT continuations, and failure paths. Its
frame policy is concrete and simpler:

- inputs are frozen for one output image;
- `progressiveRendering` is false;
- a grid may contain many tiles per device;
- assign one tile to every free worker, then use next-free scheduling;
- a completed one-shot tile has no cross-wave affinity;
- return only after every tile is assembled or the request fails/cancels.

An export request temporarily owns the runtime exclusively. It supersedes and
drains the interactive generation before synchronizing export inputs. It then
drives the same nonblocking `submitAvailable()`, `pollAllDomains()`, and
`finishAttempt()` core synchronously on the rendering thread. When no domain
progresses, it may use a named bounded round-robin fence wait; it must not enter
a nested Qt event loop or wait indefinitely on one device while another can
complete.

The lack of a nested event loop preserves the current export/threading contract:
queued UI mutation, backend-switch, and render events cannot re-enter rendering
mid-export. External cancellation remains observable through its cancellation
token. Every queued/latest interactive invalidation is coalesced and an
`UpdateRequest` is posted after export releases the runtime.

Scene/animation export keeps the runtime and its caches alive across output
frames. It advances the input generation and synchronizes animation parameters
between frames only after the prior output has no live attempt/callback.

### 5.5 Tile layout and coordinates

Extract the existing screenshot tile calculations into a pure helper. Given the
full extent, layout policy, and combined guard, it returns the complete ordered
descriptor vector.

The helper validates:

- positive full and attachment dimensions;
- checked tile-count and rectangle arithmetic;
- every valid rectangle lies inside the full output;
- valid rectangles neither overlap nor leave gaps;
- expanded regions include the required guard and transform correctly at edges;
- attachment-local pixel centers map to full-frame logical coordinates;
- deterministic ordering;
- a resize always creates descriptors and assembly storage for the new extent.

No hard tile-count cap or silent truncation is introduced. Export may allocate a
complete O(tile-count) descriptor/status vector with checked `uint64_t` IDs and
fail clearly if exact storage cannot be represented or allocated. Interactive
one-tile-per-worker layout contains only participating workers' nonempty tiles.

Tile camera projection alone is insufficient for fullscreen resolve, glow,
text, axes, screen-space width, and picking. Every tile supplies:

```text
fullFrameLogicalPixel =
    attachmentLocalToFullFrame(attachmentLocalPixelCenter)
```

Pass payloads receive both the full output extent and this transform. Newly
enabled effects require parity at nonzero tile origins and after resize; a pass
with an unbounded/full-frame dependency is ineligible until it has a correct
tile implementation.

### 5.6 Host readback, assembly, and publication

Each worker returns final display pixels for its expanded tile. On the owning
device's completion, the runtime copies/converts only valid rows directly into
the current building buffer. It should not allocate the current temporary
`localColorBufferToRGBAImg -> crop -> pasteImg` chain for every tile.

Interactive output uses reusable host buffers compatible with the existing
`Z3DLocalColorBuffer`/canvas contract:

1. require exact equality of runtime incarnation, input generation, wave,
   extent, tile ownership, and non-cancelled state before touching assembly;
2. every current valid rectangle is written exactly once for the wave;
3. all tiles and requested eyes reach the same input generation/wave barrier;
4. under the existing `targetSwitchMutex`, swap building and ready buffers;
5. update `hasNewRendering` and the engine-ready accessors;
6. emit `renderingFinished` exactly once for that coherent publication.

The previously displayed image remains visible while a newer generation is
partial. A resize can never publish an old-extent buffer into the new canvas.
Old extent-specific buffers retire only after their UI/readback lease is safe;
the persistent domains and caches remain alive.

The engine may select the ready source with a small value such as
`Compositor | TileRuntime` plus a retained published-frame owner. This is a
presentation-boundary value, not a render interface. The owner remains alive
through the locked canvas handoff, so assembled memory cannot disappear while
`QImage`/`QPixmap` consumes it.

The initial design does not upload assembled pixels into a Vulkan image on the
primary GPU because the current canvas already consumes host-visible pixels.
Assembly-to-QImage/QPixmap/visible time must be measured explicitly. A future
GPU transport path is justified only by that evidence.

The `ATLAS_USE_OPENGLWIDGET` build path is a separate presentation contract: its
canvas painter samples compositor-owned OpenGL targets rather than the
host-image/QPixmap buffer. Initial interactive multi-device Vulkan eligibility
therefore requires the normal host-image canvas path. OpenGL-widget builds use
direct rendering until a deliberate host-image painter or Vulkan-to-GL bridge is
designed; the tile runtime must not silently publish an undisplayable buffer.

Export assembly uses preallocated per-eye `ZImg` storage and the same valid-row
copy. After exact coverage verification it preserves current formatting order:

- Mono: one `W x H` image;
- Full side-by-side: complete left/right images concatenate to `2W x H`;
- Half side-by-side: concatenate first, then horizontally resample to `W x H`;
- preserve backend-dependent Y orientation and current save semantics.

No partial interactive wave or partial export image is reported as success.

### 5.7 Source capture and topology changes

Interactive capture cannot freeze the document for the runtime lifetime. The
contract is instead:

- capture scalar/camera/view values once per accepted input generation;
- retain canonical immutable pack/data generations through explicit ownership;
- immediately lower CPU spans before yielding, or pin their exact owner in the
  active attempt;
- never read live mutable filter state from a deferred callback;
- on object/filter topology change, supersede current assignment and rebuild
  each replica when its worker reaches a safe point;
- keep retired physical state alive until its last attempt releases it.

If a source has no stable immutable owner or copy-on-write generation, the
worker must finish immediate lowering before returning to the event loop. If
that still cannot make lifetime safe, the active pipeline is not eligible for
multi-device mode until the source contract is fixed. Silent raw-pointer use is
not allowed.

Export adds a short request-scoped freeze/ownership guard for deterministic
inputs. It is not the lifetime owner of the persistent runtime.

### 5.8 Picking

Visual color work remains tile-only. Picking is auxiliary generation state, not
another work-partition policy. Current event listeners query
`Z3DPickingManager` synchronously during mouse handling, so an asynchronous
“render a pick after the click” API would require a wider UI interaction
redesign. The first multi-device implementation instead keeps one exact
primary-device picking target ready for the displayed input generation:

1. color tile replicas skip their ordinary picking publication entirely;
2. as part of worker 0's wave-zero attempt, its isolated primary-device replica
   records one separate full-frame picking pass using the same immutable camera,
   viewport, scene, and topology generation;
3. the picking attachment stays owned by the primary replica and no full picking
   image is read to host;
4. wave zero is publishable only after every color tile and the worker-0 picking
   pass are complete;
5. under the same short presentation critical section, switch the coherent host
   color buffer and generation-matched picking target together;
6. later progressive waves reuse that picking target while input generation is
   unchanged.

Until new color and picking are both ready, leave the prior displayed color and
prior matching picking target active. A cancelled result changes neither. This
keeps existing synchronous picking behavior, avoids duplicating full picking
attachments on every device, and does not create a second scheduler task
family. Export never records picking.

If the full-frame primary picking pass becomes a measured latency bottleneck, a
later design may retain tile-local picking targets and route a query to the
worker owning the displayed tile. That optimization must first account for the
current synchronous listener contract.

### 5.9 Stereo is frame configuration, not scheduling

A spatial descriptor is per-eye geometry, while the private frame request says
which eyes are required. One worker evaluates Mono or Left-then-Right for its
tile. An attempt becomes complete only after all requested eye readbacks and
valid-row copies succeed.

Interactive publication atomically exposes the complete requested view set.
Export performs file concatenation/resampling only after the tile join. There is
no eye task, eye worker, eye load balancer, or cross-device eye pairing.

## 6. Device-Local State and Vulkan Completion

### 6.1 Singular immutable backend affinity

Keep `Z3DRendererBase::m_backend` singular. A renderer inside one physical
pipeline replica has exactly one Vulkan backend tied to one device and scratch
pool. Do not add:

- a backend vector to the live renderer;
- a per-draw worker/device index;
- a thread-local current device;
- temporary backend swapping;
- a virtual device-provider interface.

An explicit device/scratch-bound factory replaces mutable `ensureDevice()`
discovery/rebinding. Changing backend or device drains and destroys the old
backend and creates a new one; it never retargets an in-flight backend.

### 6.2 Native ownership checks

When a native buffer, image, attachment, or scratch lease enters or changes a
backend-local realization:

- required pointers are non-null by contract;
- `ownerDevice()` matches the backend's stored device;
- resource generation matches cached descriptor state;
- scratch-lease owner matches the backend's stored pool.

Use hard `CHECK`s for these engine invariants. Validate on realization change
and cache the result; unchanged draws do not perform multi-device routing or a
registry lookup.

### 6.3 Scratch, caches, and dense resources

Every secondary domain owns a Vulkan scratch pool and uses its own executor.
Worker 0 borrows the engine's primary pool. A release callback captures the
exact pool/device that created the lease and never rediscovers a mutable current
pool through global state.

Static geometry and dense images upload independently on every worker that
needs them. Caches remain typed inside the relevant replica and are keyed by
canonical data generation. They survive frames and invalidate when that source
generation changes. There is no aggregate VRAM pool or cross-device native
handle.

### 6.4 Asynchronous attempt completion

One `TileAttempt` owns one worker's active tile state:

- runtime incarnation, input generation, wave, tile, and worker identities;
- immutable render/performance token and start time;
- cancellation/supersession reason;
- record/submit/readback or PPLL continuation phase;
- device completion token/fence;
- mapped readback metadata for requested eyes;
- source/old-replica lifetime pins;
- structured external failure information.

A tile attempt has one terminal transition:

```text
Pending -> InFlight -> Complete
                   |-> Superseded
                   |-> Cancelled
                   `-> Failed
```

`Superseded` is logical: underlying device work may still need retirement before
the slot becomes free. Duplicate completion, wrong worker/device identity, stale
attempt touching current assembly, or invalid state transition is a hard
`CHECK`.

Allocate one render/performance token per coherent interactive wave or exported
output image, never one per tile. Every callback captures it by value. Close it
on success, cancellation, supersession, and failure. Per-device/tile spans are
children/lanes of that identity rather than unrelated global frames.

### 6.5 Completion ownership

While the runtime is active, the engine's existing Vulkan completion timer
delegates to `ZVulkanTileRuntime::pollCompletionsAndAdvance()`. That method polls
each device nonblockingly and pumps its safe-point callbacks. It then performs
all state transitions and new recording on the rendering thread.

Exactly one owner polls each executor. Interactive polling yields to the Qt
event loop. Synchronous export uses the same poll method plus a bounded wait when
no device progresses, without `processEvents()`.

The initial policy is at most one active tile attempt per worker. This bounds
working state and makes PPLL ring/continuation reuse explicit. It limits
concurrency, not total correct work, and introduces no arbitrary tile-count cap.

### 6.6 Worker compositor behavior

Every tile compositor replica, including worker 0, renders/readbacks pixels only.
It must not directly:

- update the live picking manager;
- swap engine/canvas ready buffers;
- mutate logical validity or progress;
- emit `renderingFinished` or UI signals;
- query mutable current render tokens.

The runtime alone performs current-generation assembly, progress, presentation,
picking publication, export completion, and failure reporting.

## 7. Transparency, Paging, and Stereo

### 7.1 Whole-tile transparency

Every fragment affecting one valid pixel executes on one GPU because the tile
runs the complete pipeline. No cross-device OIT reduction is needed.

| Transparency mode | Phase 2a eligibility | Notes |
|---|---|---|
| Opaque | Yes | Complete pixel state is device-local |
| Blend delayed | After tiled parity | Ordering remains inside one tile |
| Dual depth peeling | After tiled parity | Peel attachments/passes are worker-local |
| Weighted average | After tiled parity | Accumulation and resolve are worker-local |
| Weighted blended | After tiled parity | Accumulation and resolve are worker-local |
| PPLL exact | After continuation parity | Count/scan/store/resolve state is worker-local |

Eligibility and gates apply independently to interactive and export outputs. An
OIT mode remains on the matching direct single-device path until both its
correctness and intended consumer's performance gates pass.

### 7.2 Exact PPLL continuation

PPLL has two submissions separated by CPU-visible prefix information. The
private attempt state advances through concrete PPLL phases rather than assuming
one submission equals one tile.

All PPLL buffers/rings remain in the worker replica. While one worker waits for
prefix completion, the runtime continues polling and submitting other workers.
The same tile, worker, generation, and wave identities survive both submissions.

Supersession is checked before every continuation stage. A stale prefix result
may release resources, but it cannot record further work when cancellation makes
that unnecessary and can never assemble or publish. PPLL follows the simpler
opaque/DDP/weighted vertical slice inside Phase 2a; fallback remains direct for
each consumer until its gates pass.

### 7.3 Paging is Phase 2b

Paged images currently have one mutable uploader, page mappings/tables, cache
managers, and per-channel generations. Their render path may yield across block
discovery, readback, source I/O, upload, and another render round. Sharing that
state across devices would be incorrect.

Phase 2a therefore marks a pipeline using paged volume data as ineligible for
multi-device tiling. Interactive and export both use their current direct paths;
an explicitly forced request reports why before rendering.

Phase 2b adds only tile-worker-local paging state:

- one complete mapping/cache/uploader state per device;
- shared canonical source access and optional immutable decoded-block sharing;
- exact source-block pins;
- resumable per-attempt paging continuation;
- cancellation/supersession at readback, source-I/O, and upload boundaries.

It does not add brick tasks, cross-device page sharing, or dropped requests.

### 7.4 Stereo rule

Stereo remains exactly as defined in Section 5.9: every spatial tile renders all
requested eyes. This rule applies to interactive and export. No later phase in
this design introduces eye-based work decomposition.

## 8. Failure, Supersession, and Teardown

### 8.1 Logical supersession versus physical retirement

An interactive generation becomes stale immediately when newer logical inputs
are accepted. The runtime then:

- stops assigning its pending tiles;
- marks in-flight attempts superseded and requests cancellation;
- destroys its partial assembly once callbacks can no longer reference it;
- lets each attempt release device resources/pins at physical completion or a
  device-loss-safe retirement path;
- never lets stale completion change progress, ready buffers, signals, or the
  current generation.

This distinction prevents both UI staleness and unsafe destruction. It also
avoids keeping a public collection of fully modeled retiring frame objects.

### 8.2 Request failure policy

Export is fail-fast and all-or-nothing:

- first failure stops new assignment;
- remaining attempts cancel or drain;
- no partial image is saved/reported as success;
- the error identifies output frame, device UUID, tile, and failing stage.

Interactive failure preserves the last coherent displayed image. A failed
current generation never publishes. For a non-device external runtime failure,
the first implementation disables the multi-device runtime at a safe point and,
if the primary Vulkan device remains healthy, posts a full invalidation through
the direct one-device path with a visible/logged warning. It must not repeatedly
retry the same failing multi-device configuration.

Internal ownership/state inconsistencies remain hard `CHECK`s rather than being
hidden by fallback.

### 8.3 Device loss

`vk::Result::eErrorDeviceLost` is an external failure. Poll/wait paths report it
without hanging on a fence that can never signal.

On secondary loss:

1. poison the affected runtime/worker against new submission;
2. discard the current generation and stop all assignment;
3. invalidate the lost worker's readback and neutralize its callbacks safely;
4. drain healthy domains without depending on the lost fence;
5. destroy replicas before owned secondary domains;
6. keep the last coherent interactive image visible;
7. if the primary is healthy, restore direct rendering and post the latest
   `UpdateRequest`; export fails the complete request.

On primary loss, the runtime cannot fall back through the poisoned device. It
hands control to the existing backend recreation/switch policy after neutralizing
all runtime publication. No new frame starts until a healthy backend is ready.

Automatic removal of one failed secondary followed by rebuilding a smaller
multi-device runtime is deferred. The first implementation chooses deterministic
teardown and direct fallback.

### 8.4 Per-generation retirement

When a generation completes, fails, cancels, or is superseded:

1. stop its pending assignment;
2. finish immediate CPU lowering operations;
3. retire healthy GPU submissions/readbacks;
4. release mapped data, scratch, source pins, and obsolete replica versions;
5. close its render/performance token;
6. retain only the coherent ready buffer still leased by the canvas, if any.

The runtime, domains, and reusable caches stay alive for the next generation.
A resize retires old extent-specific attachments/buffers only, not the runtime.

### 8.5 Runtime shutdown

Normal deactivation/engine teardown order is:

1. stop accepting frame/export requests and stop assigning tiles;
2. supersede/cancel the current generation;
3. retire healthy attempts and neutralize lost-device callbacks;
4. release all attempt pins and extent-specific host buffers;
5. destroy every pipeline replica, including worker 0's;
6. `CHECK` that runtime-owned scratch leases and callbacks are gone;
7. destroy owned secondary domains;
8. release the borrowed primary slot;
9. restore the normal primary completion route only if the device is healthy;
10. invalidate/post the latest direct frame request when switching back.

Destructors must not wait forever on a lost device. Secondary pool destruction
`CHECK`s that no owned lease remains. Pre-existing engine-owned primary leases
outside the runtime are not misclassified as secondary/runtime resources.

## 9. Device Selection, Memory, and Performance

### 9.1 Selection and reconfiguration

The primary retains current preference sorting and
`--atlas_vk_device_index` semantics. Multi-device mode receives one explicit
ordered selection list shared by interactive and export routing. The exact UI/
CLI spelling is intentionally not fixed yet. Existing `--use_gpu_devices`
multi-process animation behavior must not change silently.

Selection rules:

- reject duplicate request indices and duplicate UUIDs;
- reject out-of-range or incompatible devices with support reasons;
- require every selected device to support the active pipeline's formats,
  features, expanded tile extent, and mandatory working set;
- use the intersection of selected-device limits for layout;
- let the existing primary device participate without recreation;
- construct each secondary from explicit `DeviceSelection`;
- resolve one usable device to the direct path, not a one-worker runtime.

Changing the selected list occurs at the runtime deactivation/activation safe
point. Automatic hardware choice and performance heuristics are deferred; early
benchmarks require explicit reproducible selection.

### 9.2 Memory policy

Every device enforces its own residency budget. Atlas never sums VRAM or assumes
resource sharing.

Initial bounds are structural, not truncation:

- at most one active tile attempt per worker;
- one interactive progressive tile state per worker;
- reusable readback buffers per worker;
- current building and published host buffers, plus only buffers still leased by
  the canvas or stale attempt;
- preallocated export image(s) for the current output;
- persistent device-local caches governed by each device's existing residency
  policy.

If an expanded tile cannot fit a selected device, choose a smaller correct
export layout before submission when possible. Interactive layout may increase
the number of worker-pinned tiles only in a later design that also owns the
additional progressive state. Otherwise the forced request fails clearly or
automatic mode uses the direct path. No pixels or scene content are dropped.

### 9.3 Measurements

Record at least:

- runtime incarnation, input generation, wave, tile, worker, and device UUID;
- cold runtime activation versus warm steady-state time;
- CPU capture/lowering time per tile;
- GPU time and idle time per worker/tile;
- PPLL phase timing;
- final readback bytes/time;
- host row-copy/assembly time;
- assembly-to-`renderingFinished` and visible-canvas time;
- request-to-first coherent preview;
- preview-to-final and last-input-to-final settle time;
- coherent presented FPS/cadence;
- resize-to-first-correct-extent presentation;
- superseded generations, unstarted abandoned tiles, completed discarded tiles,
  and stale GPU time;
- current-generation critical path, separate from summed GPU work;
- peak memory per device and host;
- stale/wrong-extent publication count, which must be zero;
- export end-to-end time and file finalization time.

The feature succeeds only on end-to-end critical-path improvement. Higher summed
GPU utilization alone is not sufficient.

## 10. Delivery Phases

### 10.1 Phase 0: controlled export and interactive baselines

Deliver:

- this reduced tile-only design;
- fresh single-device Vulkan export baselines for DDP, weighted, and PPLL;
- deterministic interactive camera preview/final measurements;
- real visible rotate latency/FPS measurements on macOS;
- fixed-size and resize/cancellation behavior at representative extents;
- direct-path allocation/call-structure measurements.

Gate:

- record exact binary, input, output/canvas dimensions, tile settings, selected
  UUID, platform mode, hashes, performance flags, and raw results before code
  changes.

### 10.2 Phase 1: explicit ownership and generation-ready primitives

Multi-device execution remains disabled. Do not introduce a secondary domain or
runtime yet. Deliver:

- explicit `ZVulkanContext::DeviceSelection`;
- engine-owned renderer shared state and scratch dependencies instead of
  rendering-time singleton discovery;
- constructor-injected scratch and immutable Vulkan device affinity;
- owner-device checks at native realization boundaries;
- final-quality asynchronous tile submission/readback separated from render
  quality;
- side-effect-free tile compositor/readback behavior;
- immutable render/generation values captured by completion callbacks;
- unconditional logical `renderInputChanged()` notification separate from
  edge-triggered physical-output validity;
- clean direct backend switch and device-loss completion;
- no change to ordinary `processFrame()` scheduling.

Gate:

- direct output parity across baseline modes;
- no one-device performance regression;
- no new multi-device hot-path branch/allocation/dispatch;
- repeated logical invalidation remains observable while live physical outputs
  stay invalid, and direct fallback still recomputes them;
- wrong-owner death tests fail at the boundary;
- an asynchronous final-quality test submits two isolated attempts before
  waiting and retires both deterministically;
- completion cannot publish through a destroyed/stale owner.

#### Local implementation checkpoint (2026-08-03)

The Phase 1 code foundation is implemented without adding a runtime, secondary
domain, renderer-fleet interface, or device-dependent container to the ordinary
render path:

- `ZVulkanContext::DeviceSelection` captures both the preference-sorted index
  and physical-device UUID. The explicit constructor rejects an index/UUID
  mismatch without fallback; the existing command-line preference keeps its
  best-effort single-device fallback behavior.
- Each `Z3DRenderingEngine` owns its concrete scratch pool and renderer shared
  state. Renderer bases and backends receive those dependencies directly rather
  than discovering physical state through `Z3DRenderGlobalState`.
- A Vulkan backend has one immutable `ZVulkanDevice` and one scratch pool for
  its lifetime. Scratch leases record their originating pool, and render-target,
  buffer, texture, descriptor, and bindless realization boundaries reject a
  foreign owner with `CHECK`.
- Readback completion policy is separate from render quality. Final Vulkan
  readback state is move-only, captures immutable owner revision/frame/extent
  values, rejects stale publication, and retires mapped resources exactly once.
- Logical render-input notification is unconditional while physical invalidity
  remains edge-triggered, so replica work can observe repeated logical changes
  later without falsely validating the live direct pipeline.
- The existing `processFrame()` and export routing remain single-device. There
  is no new per-frame scheduler branch, virtual dispatch, worker allocation, or
  multi-device container on that path.

Completed validation includes the canonical Release build, CPU selection and
ownership tests, logical-invalidation tests, readback policy/lifetime tests, a
hardware-backed MoltenVK smoke that constructs an independently enumerated
context from an exact index/UUID selection, and output-hash parity for DDP,
weighted average, and PPLL. Section 11 records the concrete evidence.

The following qualification gates intentionally remain open:

- two isolated *GPU submissions* must be live before waiting; the current CPU
  lifetime test proves two completion values can coexist and retire exactly
  once, but it is not a dual-device submission test;
- one-device performance needs repeated, configuration-identical samples before
  a no-regression statement is valid;
- pipeline replicas, tile layout/assembly, secondary domains, and all actual
  multi-device scheduling begin in Phase 2a.

### 10.3 Phase 2a: shared non-paged multi-device tile rendering

Implement with internal milestones, without new public abstraction layers.

1. **One-worker replica parity**
   - Force one tile replica on the borrowed primary device in tests.
   - Match direct export pixels and direct interactive host presentation.
   - Prove it does not mutate live validity, camera, progress, picking, ready
     buffers, or callbacks.
2. **Export concurrency oracle**
   - Add secondary domains, many-tile next-free scheduling, direct row assembly,
     cancellation, and complete failure handling.
   - Enable opaque first, then blend-delayed, DDP, and weighted modes after
     individual gates.
3. **Interactive shared runtime**
   - Keep the same domains, replicas, attempts, polling, and assembly core.
   - Add one worker-pinned tile per device, generation supersession, progressive
     waves, resize, coherent ready-buffer publication, and worker-0 picking.
4. **Exact PPLL**
   - Add its concrete two-submission continuation for both consumers after the
     simpler vertical slice is stable.

Phase 2a deliverables include:

- pure tile requirements/layout values;
- one `ZVulkanTileRuntime`, secondary domain per extra device, and pipeline
  replica per worker;
- one active attempt per worker and one schedulable current generation;
- common nonblocking completion/poll core;
- common host valid-row assembly;
- export next-free grid scheduling;
- interactive pinned progressive tiles and coherent wave publication;
- camera/parameter/scene/resize supersession;
- exact mono/stereo behavior with all eyes inside each tile;
- direct-path fallback for non-Vulkan, one-device, unsupported, and paged
  workloads;
- direct interactive fallback for OpenGL-widget presentation builds;
- explicit selection and per-device/generation metrics.

Gates:

- every valid pixel is assembled exactly once;
- no stale generation, wave, eye, or extent is ever published;
- one-worker parity passes for export and interactive routes;
- opaque/blend-delayed/DDP/weighted match single-device references before PPLL;
- nonzero tile origins preserve text/axes/glow/screen-coordinate behavior;
- rapid camera changes coalesce to the latest generation;
- resize publishes only the latest exact extent;
- preview and final waves remain coherent;
- picking matches the displayed generation;
- cancellation/failure never produces partial success;
- two or more selected devices overlap GPU work;
- target workloads improve end-to-end export time and interactive latency/FPS.

Additional PPLL gates:

- exact output matches single-device tiled references for both consumers;
- prefix wait on one worker does not prevent progress on another;
- supersession at every PPLL phase cannot publish or hang.

### 10.4 Phase 2b: paged-volume tile rendering

After Phase 2a stability and evidence justify the memory cost, deliver:

- complete per-device page mapping/cache/uploader state;
- immutable decoded source sharing and exact block pins;
- resumable paging continuation for interactive and export attempts;
- per-device cache-eviction isolation;
- cancellation/supersession during source I/O;
- progressive interactive affinity with paging state on the same worker.

Gate:

- paged outputs match direct references;
- eviction on one GPU cannot change another GPU's mapping;
- live invalidation and export cancellation do not drop requests or hang;
- devices overlap useful work rather than nesting serial wait loops.

### 10.5 Phase 3: measured optimizations only

Phase 3 does not contain initial interactive support. Candidate work must be
driven by Phase 2 measurements:

- throughput-weighted interactive stripe widths;
- several persistent progressive tiles per worker;
- more than one one-shot export tile in flight per device;
- reduced-copy host assembly;
- primary-device presentation upload if the current canvas path is limiting;
- device-group/external-memory transport experiments;
- automatic reduced-device runtime rebuilding after loss.

No generic fleet or task graph is added merely to host these possibilities.

## 11. Validation and Benchmark Plan

### 11.1 Existing export harness and results

Use `util/benchmark/export_scene_animation_stability.py`. It already records
binary/input identity, command lines, platform selection, stdout/stderr,
per-output hashes, aggregate summaries, Vulkan performance NDJSON, and optional
baseline comparison.

Historical results in `/Users/feng/Documents/test_folder`, including the May
2026 GL/Vulkan native/offscreen stability roots, are useful references but
predate parts of the current harness. Generate a fresh current single-device
baseline rather than treating them as directly consumable baseline roots.

Use existing assets under `/Users/feng/Dropbox/atlas_test`:

| Workload | Mode | Purpose |
|---|---|---|
| `testscene2.scene` | Dual Depth Peeling | 204 sources and five translucent rendered objects |
| `testscene3.scene` | Weighted Average | Same scene content with a weighted control mode |
| `v4_vulkan.animation3d`, frame 180 | PPLL Exact | Existing Vulkan/PPLL translucent animation workload |

Historical native/offscreen Vulkan `testscene2.scene` output produced SHA-256
`40b42aa7eab4ef8267e92bb109faeb181b12b9e1fcf3465e984c50c9e4ec1517`.
It is a reference, not a replacement for a fresh controlled baseline.

There is no need to modify the DDP scene for initial PPLL coverage. The animation
already covers PPLL. If a same-content static comparison becomes useful later,
create a derived `testscene2_ppll.scene`, retain the original, and change only
the transparency mode to `Per-Pixel Fragment List (PPLL Exact)`.

An appropriate fresh export command shape remains:

```bash
python3 util/benchmark/export_scene_animation_stability.py \
  --atlas-path build/Release/src/atlas/Atlas.app/Contents/MacOS/Atlas \
  --output-root "/Users/feng/Documents/test_folder/atlas_vk_tile_baseline_$(date +%Y%m%d_%H%M%S)" \
  --backend vulkan \
  --scene /Users/feng/Dropbox/atlas_test/testscene2.scene \
  --scene /Users/feng/Dropbox/atlas_test/testscene3.scene \
  --animation /Users/feng/Dropbox/atlas_test/v4_vulkan.animation3d \
  --scene-runs 1 \
  --animation-runs 1 \
  --scene-width 8000 \
  --scene-height 8000 \
  --animation-width 3840 \
  --animation-height 2160 \
  --animation-fps 30 \
  --animation-start-frame 180 \
  --animation-end-frame 181 \
  --qt-platform-vulkan offscreen \
  --vulkan-perf-mode full \
  --run-label single_gpu_tile_baseline \
  --extra-arg=--atlas_volume_rendering_analytic_ray_setup=false \
  --extra-arg=--atlas_vk_device_index=0
```

The output root must not already exist. Record the actual selected UUID because
preference-sorted index 0 is host-specific.

#### Phase 0 evidence captured on 2026-08-03

The controlled single-device export root is
`/Users/feng/Documents/test_folder/atlas_vulkan_multi_gpu_phase0_export_20260803_101856`.
Its manifest records binary SHA-256
`821fd61d96426451c27b5cffe21594a6719f2af6ea992618fdeeadb7dec800ee`,
Vulkan device index 0, AMD Radeon Pro 5600M through MoltenVK, full performance
collection, 1024-pixel tiles with a 64-pixel border, and one run per workload.
All three runs completed without a stability failure:

| Workload | Mode and extent | End-to-end time | Output SHA-256 |
|---|---|---:|---|
| `testscene2.scene` | DDP, 8000 x 8000 | 12.230583 s | `40b42aa7eab4ef8267e92bb109faeb181b12b9e1fcf3465e984c50c9e4ec1517` |
| `testscene3.scene` | Weighted Average, 8000 x 8000 | 10.702929 s | `324cad2cba76ab5bad6a36f7004b49521d0b1a815d9833895c30c1aff21adb2c` |
| `v4_vulkan.animation3d`, frame 180 | PPLL Exact, 3840 x 2160 | 31.810213 s | `76b056e94204ca1cb4fc96ff8fa4e8801cbba3b7e553b161f4494123bbe3fd32` |

The valid no-reopen interactive regression/baseline root is
`/Users/feng/Documents/test_folder/atlas_vulkan_empty_scene_regression_20260803_1118`.
It used binary SHA-256
`2b8a71a2b44337339f8ba6861530d995ad875bf5022c867e2a5ee97f06e5de68`,
physical-device UUID `00001002-0000-7360-0000-000000000441`, a 1000 x 750
logical (2000 x 1500 physical) canvas, MIP, full-resolution rendering, hidden
background/axis/bounds, and one continuously open 3D engine/window. Input
SHA-256 values were:

- dataset: `d8ca1d3a06c3582fc4039f433929681abbb7f5144f76344f37fb64203ed190e5`;
- camera: `e8ff2f8fcdbabd2e719cae76448aac073ec6ad6434e503818c03a6ab137a4658`;
- GUI calibration: `b9ec36da69872092df0cc817afa0af0416898ab5d877de172683c12926d39fde`.

The warm-up observed 117 changed samples, a first changed sample at 41.98 ms,
and 23.61 visible FPS. The measured run observed 105 changed samples, a first
changed sample at 33.50 ms, and 21.10 visible FPS. The log proves the empty
scene clear executed before dynamic image insertion, then records image-layer
collection, final Vulkan readback, and successful rendering; it contains none
of the former scratch restore/no-backup error. These single runs establish a
correctness seed, not a performance distribution; use multiple measured runs
for a no-regression or speedup decision.

Earlier GUI roots ending in `102700` and `103100` are diagnostic failures, not
baselines: the empty pre-load scene failed before the image topology change and
the captures consequently contained zero changed samples.

#### Phase 1 foundation evidence captured on 2026-08-03

The Phase 1 single-device correctness root is
`/Users/feng/Documents/test_folder/atlas_vulkan_multi_gpu_phase1_final_20260803_1324`.
Its manifest records binary SHA-256
`2f8c9a64cb503b4dc51909679ace2cfb8e7fc10a50b00af77d28e5405c434a8d`.
All three Atlas processes returned zero, produced their complete expected
outputs and versioned Vulkan performance records, and selected AMD Radeon Pro
5600M UUID `00001002-0000-7360-0000-000000000441`. Every output is byte-for-byte
identical to its Phase 0 baseline:

| Workload | Mode and extent | Informational time | Output SHA-256 |
|---|---|---:|---|
| `testscene2.scene` | DDP, 8000 x 8000 | 16.527483 s | `40b42aa7eab4ef8267e92bb109faeb181b12b9e1fcf3465e984c50c9e4ec1517` |
| `testscene3.scene` | Weighted Average, 8000 x 8000 | 11.940945 s | `324cad2cba76ab5bad6a36f7004b49521d0b1a815d9833895c30c1aff21adb2c` |
| `v4_vulkan.animation3d`, frame 180 | PPLL Exact, 3840 x 2160 | 21.259941 s | `76b056e94204ca1cb4fc96ff8fa4e8801cbba3b7e553b161f4494123bbe3fd32` |

The final root was captured after the compositor/readback lifetime and borrowed
scratch-lease ownership hardening. Its aggregate status is `passed`, with three
of three complete runs and no stability or harness failures. It intentionally
has no comparison baseline configured because the older Phase 0 invocation has
different benchmark metadata; the table compares the complete output hashes
directly. Each duration is a single informational sample and cannot establish
a one-device performance distribution or no-regression result.

Focused validation for this checkpoint also includes:

- the canonical `cmake --build build/Release` completed successfully for the
  final code snapshot;
- `z3dfiltertest`: 1/1 passed;
- `zvulkandevicesupporttest`: 17 CPU policy tests passed, and the separately
  enabled `VulkanIcdStartupSmoke` passed against the packaged MoltenVK ICD;
- `zvulkanpipelinecontexttest`: 4/4 passed;
- the `Z3DGlobalParametersTest.*` and `Z3DRendererOwnershipTest.*` filter from
  `zatlasheavytest`: 5/5 passed, including pool affinity and non-owning borrowed
  lease lifetime coverage.

The hardware smoke enumerated the discrete AMD and integrated Intel adapters,
captured the selected AMD index/UUID, and successfully constructed a second
independently enumerated context from that exact identity. It validates strict
selection and independent context ownership, not simultaneous multi-device
rendering.

### 11.2 Existing interactive harnesses

Atlas already has complementary interactive tools:

| Concern | Existing facility | Current value | Gap to close |
|---|---|---|---|
| Deterministic camera preview/final latency | `atlas_volume_benchmark.py` plus `atlas_deterministic_batch.py` | Applies camera states through Scene RPC and records preview/final markers, raw events, aggregates, and screenshots | It waits for each step to finish, so it does not test supersession |
| Camera generation | `benchmark_camera_from_scene.py` and `volume_benchmark_camera_template.json` | Produces repeatable open/rotate/zoom states | Confirm/extend direct scene and OIT use |
| Real input-to-visible latency and FPS | `atlas_gui_rotate_batch.py` | On macOS, injects input, captures the actual window, verifies backend, and measures first-visible/FPS/settle | Action is currently rotation-focused |
| Live canvas sizing | `atlas_volume_benchmark.py` Scene RPC `_set_canvas_size()` | Cross-platform size primitive already exists | It is setup-only, not a timed resize sequence |
| Visible pixel observation | ScreenCaptureKit/Quartz helpers in `util/benchmark` | Measures real WindowServer-visible change | Fixed capture geometry complicates real window resize |

The deterministic RPC pair is the primary correctness/latency harness. The GUI
pair is the user-visible responsiveness oracle. Both are required: internal
generation markers can prove stale publication behavior, while screen capture
detects real presentation stalls and cadence.

### 11.3 Required benchmark extensions

Add these locally when implementation begins, not during this design-only step:

- `camera_burst`/no-wait mode that submits states faster than final rendering;
- a latest-generation wait instead of waiting for every intermediate frame;
- a timed resize action using alternating odd, increasing, and decreasing
  extents;
- authoritative request/input-generation and progressive-wave IDs in benchmark
  markers;
- request-to-first-current-preview and last-input-to-final metrics;
- resize-to-first-correct-extent and final-settle metrics;
- counts for superseded generations, abandoned pending tiles, completed stale
  work, and stale publication;
- per-device/tile telemetry correlated with generation/wave;
- separate cold activation and warm steady-state runs;
- one-device direct and multi-device comparisons using identical inputs.

Do not add a fixed event or frame cap. The requested workload is complete by
default; explicit benchmark duration/step count remains caller configuration.

### 11.4 Unit and invariant tests

- explicit device index/UUID selection and changed-enumeration rejection;
- duplicate, incompatible, and out-of-range selection;
- odd/arbitrary tile coverage, guard, full-frame transforms, and resize layout;
- unsupported/paged fallback for both consumers;
- one active attempt per worker;
- out-of-order current completion and stale completion retirement;
- generation supersession stops old assignment;
- repeated invalidations retain only the newest request;
- repeated logical changes notify while the idle live pipeline remains invalid;
- old completion cannot change progress, ready buffers, picking, or signals;
- progressive waves never mix tile generation/wave/extent/eye state;
- valid-pixel-weighted progress and completed-tile reuse;
- one-worker replica parity for export and canvas-ready host buffers;
- worker 0 cannot mutate the live direct pipeline;
- native owner mismatch death tests;
- asynchronous final-quality submission across two workers;
- exclusive completion-pump ownership and restoration;
- PPLL continuation and supersession at every phase;
- direct row placement, orientation, mono/full-SBS/half-SBS output;
- exact worker-0 full-frame picking target switches atomically with wave-zero
  color and remains matched through refinement;
- export/interactivity handoff and latest-update resume;
- cancellation at capture, submit, PPLL readback, and final readback;
- secondary/primary device loss with a live attempt;
- topology removal cannot invalidate a stale attempt's source;
- no publication after runtime/engine destruction;
- clean partial-secondary-construction rollback.

### 11.5 Hardware, correctness, and performance gates

Exercise:

- one Vulkan GPU: direct path only;
- two independent discrete GPUs;
- integrated plus discrete when both meet the full contract;
- unequal compatible GPUs;
- incompatible secondary selection;
- constrained per-device residency;
- native/offscreen export modes where supported;
- multiple interactive canvas sizes, including odd extents;
- camera bursts and repeated resize;
- opaque, DDP, weighted, and PPLL workloads as each is enabled.

Compare identical binary/input/configuration using:

- exact export hashes or a documented image tolerance where exactness is not
  promised by the existing reference;
- captured interactive screenshots at stable final state;
- zero stale/wrong-extent publications;
- end-to-end export critical path;
- request-to-preview/final and visible FPS/cadence;
- assembly/presentation overhead;
- device overlap, idle time, and load imbalance;
- peak per-device and host memory.

### 11.6 Phase 2a performance acceptance

Do not enable multi-device interaction by default merely because two GPUs are
busy. For a representative GPU-bound workload, require:

- statistically significant improvement in either request-to-coherent-preview,
  last-input-to-final, or sustained visible cadence without a material regression
  in the others;
- no material increase in cancellation/resize recovery tail latency;
- stable warm-frame improvement after excluding one-time runtime creation;
- export wall-time improvement for large tiled outputs;
- single-device disabled-mode parity and non-regression.

Small, geometry/CPU-bound, or transfer-dominated workloads may correctly remain
on the direct path until a measured automatic policy is designed.

## 12. Expected Code Impact

| Area | Direction |
|---|---|
| `z3drenderingengine.*` | Preserve direct `processFrame`; own nullable persistent runtime; route `renderFast`, `render`, export, completion polling, ready buffers, backend switch, and teardown once at coarse boundaries |
| `z3dcanvas.*` | Preserve current host-buffer consumption; validate coherent current-generation and current-extent publication |
| `zvulkancontext.*` | Accept explicit index plus UUID; expose immutable per-device support without platform/topology classes |
| `zvulkandevice.*` | Preserve independent allocator/executor/residency ownership and concrete domain lifetime |
| `z3drenderervulkanbackend.*` | Immutable device/scratch affinity; separate final quality from synchronous wait; owner checks and side-effect-free tile completion |
| `z3dscratchresourcepool.*` | Physical-pipeline-local Vulkan inventory/release; no mutable current-device rediscovery |
| `z3drenderglobalstate.*` | Remove physical scratch/view ownership from globals where needed; callbacks capture immutable generation values; keep direct-path token semantics |
| `z3drendererbase.*` | Direct state/scratch references and one backend per physical pipeline; never a backend vector |
| `z3dfilter.*` / `z3dcompositor.*` invalidation | Add unconditional logical render-input notification while preserving existing edge-triggered physical validity |
| Filter/renderer implementations | Typed device-local replica state and immutable generation synchronization; live direct path remains unchanged |
| `z3dcompositor.*` rendering | Tile replicas return pixels only; runtime performs coherent ready-buffer and generation-matched picking handoff |
| `z3dimg.*` and paged uploaders | Phase 2a eligibility only; complete per-device paging state in Phase 2b |
| Export runners | Submit output frames to the same runtime; preserve existing one-device and multi-process meanings |
| `z3dperfcollector.*` | Add generation/wave/device/tile lanes and presentation/supersession metrics without a direct hot-path lookup |
| `util/benchmark/*` | Reuse export/deterministic/GUI tools; extend locally for burst camera, timed resize, and generation correlation |

New implementation files follow Atlas naming and one-type-per-file rules for the
three concrete owners. Private runtime structs remain in the runtime's
implementation rather than becoming standalone abstraction files.

## 13. Deferred Alternatives

### 13.1 Generic render fleet, task graph, and sink hierarchy

Deferred because independent final tiles need a flat status set and one coherent
join. Add a graph only if Atlas accepts a workload with actual inter-task GPU
dependencies. Interactive versus export output is a concrete completion switch,
not a reason for a sink interface.

### 13.2 Shared Vulkan platform/topology owner

Deferred because secondary contexts are created only when multi-device mode is
activated and persist thereafter. Share instance/discovery only if measured
activation time or memory materially justifies the additional lifetime owner.

### 13.3 Device groups, peer/external memory, and GPU composition

Deferred because the correctness-first route uses per-device final readback and
Atlas already presents Vulkan output from host-visible color data. These
mechanisms become candidates only if measured host assembly/presentation cost
prevents an interactive speedup. They should not be prebuilt behind a transport
interface.

### 13.4 Multiple interactive tiles per worker

Deferred because every progressive tile needs independent per-eye accumulator,
validity, output, and continuation state. One pinned tile per worker is the
smallest correct model. Add more only if cancellation latency/load imbalance
justifies the memory and state cost.

### 13.5 Stereo, layer, object, geometry, or brick scheduling

Removed from this architecture. These decompositions require different
dependencies, reductions, or transfer. Stereo remains inside each spatial tile;
paging remains device-local state for a spatial tile.

### 13.6 Fleet-wide domain keys and resource registries

Deferred. Attempts cannot outlive runtime/domain retirement, native wrappers
already retain owner-device references, and callbacks carry direct worker plus
runtime/generation identity. A process-wide lookup would add cost without
improving this ownership model.

### 13.7 Full immutable scene snapshots

Deferred. Interactive mutation is handled by a small immutable scalar/camera
capture, canonical data-generation pins, immediate lowering, and topology-safe
replica versioning. Introduce deeper copies only for a concrete source whose
lifetime cannot be expressed safely, or if future CPU worker threads require
them.

### 13.8 Partial interactive publication

Deferred because it causes visible tile tearing and complicates picking,
progress, and stale-generation rules. The first design keeps the previous
coherent frame until the entire current wave is ready.

### 13.9 Automatic failure replanning

Deferred. Phase 2 tears down the failed runtime and falls back to a healthy
direct primary path when possible. Rebuilding with a reduced device set can be
added only after device-loss retirement is proven deterministic.

## 14. Final Architecture Test

The design is intentionally small enough to restate as six invariants:

1. Ordinary one-device filters/passes/draws know nothing about multi-device
   rendering.
2. Every multi-device GPU owns a complete, immutable-affinity physical pipeline
   replica; native resources never cross devices.
3. The only scheduled rendering work is a spatial tile containing the whole
   pipeline and all requested eyes.
4. Interactive and export use the same concrete runtime, worker, attempt,
   completion, and assembly machinery; only generation policy and final output
   action differ.
5. Only a complete current interactive generation/wave or a complete export
   image can publish successfully.
6. Any proposed new layer must solve a measured requirement that these concrete
   owners and values cannot solve.

If an implementation requires per-draw device selection, mutable backend
retargeting, live-filter sharing, partial publication, or a task graph, it has
departed from this design and needs a new architecture review.

## 15. References

Atlas sources motivating this design include:

- `src/atlas/z3drenderingengine.*`
- `src/atlas/z3dcanvas.*`
- `src/atlas/zvulkancontext.*`
- `src/atlas/zvulkandevice.*`
- `src/atlas/zvulkanframeexecutor.*`
- `src/atlas/z3dscratchresourcepool.*`
- `src/atlas/z3drendercommands.h`
- `src/atlas/z3drendererbase.*`
- `src/atlas/z3drenderervulkanbackend.*`
- `src/atlas/z3drenderglobalstate.*`
- `src/atlas/z3dimg.*`
- `src/atlas/zvulkanpagedimageblockuploader.*`
- `src/atlas/z3dimgfilter.*`
- `src/atlas/z3dcompositor.*`
- `src/atlas/z3dperfcollector.*`
- `util/benchmark/export_scene_animation_stability.py`
- `util/benchmark/atlas_volume_benchmark.py`
- `util/benchmark/atlas_deterministic_batch.py`
- `util/benchmark/atlas_gui_rotate_batch.py`
- `util/benchmark/benchmark_camera_from_scene.py`

Relevant Vulkan references:

- [Vulkan object model and object lifetime](https://docs.vulkan.org/spec/latest/chapters/fundamentals.html#fundamentals-objectmodel-overview)
- [Physical-device enumeration](https://registry.khronos.org/vulkan/specs/latest/html/vkspec.html#devsandqueues-physical-device-enumeration)
