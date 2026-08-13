# Vulkan Multi-Device Rendering

Atlas binds each `Z3DRenderingEngine` to one Vulkan logical device. A canonical
engine can own an optional, private
`Z3DRenderingEngine::ZVulkanTileWorkerPool` containing complete single-device
rendering engines for the selected devices. Filters, renderers, compositor
passes, and native Vulkan resources remain local to one engine and do not
perform cross-device routing.

The pool supports two spatial workloads:

- mono interactive rendering assigns one stable screen region to each
  participating engine; and
- fixed-size capture dynamically distributes complete output tiles and
  assembles their CPU pixels into a save-oriented frame.

The canonical engine is the public facade for device selection, rendering,
input, capture, cancellation, and failure reporting. Each application path
installs at most one selected device set on its engine; an empty initial
selection leaves the direct path active.

Related references: [Developer Guide](DEVELOPER_GUIDE.md) and
[Image Paging and Progressive Rendering](Atlas_Image_Paging_and_Progressive_Rendering.md).

## Scope and Activation

`--atlas_vk_multi_device_tile_worker_indices=0,1,...` names Vulkan devices for
the private pool. A Vulkan 3D canvas normalizes a nonempty list before pool
construction: when the independently selected canonical device is absent,
Atlas prepends it; when it is already present, list order is unchanged. The
effective canvas set must contain at least two distinct compatible physical
devices. An empty list stays direct. Generic engine and headless fixed-capture
configuration interpret the supplied selections exactly and do not add the
canonical device. The engine API accepts a one-device set.

When a pixmap-backed 3D canvas is initialized with Vulkan, a valid nonempty
list therefore produces a pool containing the canonical engine and enables
fixed-region mono interactive rendering. The same configured pool services a
fixed-size capture when the requested output contains more than one tile. A
one-participant pool remains on the direct interactive path but can service a
multi-tile capture. A single-tile capture uses the canonical engine's direct
path even when the pool is configured. OpenGL does not construct or execute
Vulkan worker lanes.

A canvas samples the interactive device set when it is initialized with
Vulkan. The participant set is not reconfigured in that canvas; a backend
change or worker failure can remove the pool, but switching back does not create
a replacement. Opening a new Vulkan 3D window applies the current device
configuration.

Interactive regional rendering is mono. Stereo eyes are not separate worker
tasks. A tiled stereo capture assigns one spatial tile to an engine and that
engine renders both eyes before claiming another tile.

Animation export uses a separate process-level contract. It assigns adjacent
frame ranges to single-device Atlas processes; each process owns its document,
renderer, and caches for its range. Optional positive integer weights change
the relative range sizes without changing adjacency or frame ownership.
Animation frames are not distributed through the in-process tile-worker pool.

## Default Direct Path

An empty device list is the default. In that configuration:

- the canonical engine renders interactive frames directly on its one device;
- direct and tiled captures execute through the canonical engine;
- no worker thread, worker engine, additional Vulkan context, logical device,
  or worker snapshot is allocated;
- no regional image or pixmap item is allocated;
- no multi-device state publication, regional query routing, or scheduling runs; and
- filters and per-draw code perform no device-selection lookup.

The empty-list path performs only inexpensive disabled-state checks before
entering regional work; it does not allocate, publish, route, synchronize, or
schedule multi-device work.

## Ownership and Threading

```text
canonical Z3DRenderingEngine (one device, canonical rendering thread)
  `-- optional private Z3DRenderingEngine::ZVulkanTileWorkerPool
        |-- canonical lane, when its device is selected
        |-- QThread -> complete worker Z3DRenderingEngine -> device B
        |-- QThread -> complete worker Z3DRenderingEngine -> device C
        `-- ...
```

The effective selections are the complete pool set, in participant order.
The existing canonical engine serves as its participant; the pool does not
create a duplicate engine for it. The canonical engine also owns orchestration,
state publication, the full-view camera, and input entry. An exact generic or
headless pool may exclude the canonical device; in that case the canonical
engine orchestrates capture but does not claim a tile.

Every noncanonical selection owns a dedicated `QThread`, dispatcher, complete
Vulkan-only `Z3DRenderingEngine`, and engine-local `ZQtExecutor`. The engine is
constructed, initialized, invoked, and destroyed on that thread. It owns its
Vulkan context, logical device, allocator, frame executor, scratch pool,
filters, compositor, pipeline contexts, descriptors, textures, readback state,
ready buffers, caches, picking targets, and progressive validity. Native
Vulkan objects never cross engine or device boundaries.

Every participating engine retains the configured Vulkan frame-slot count.
The pool permits at most one outstanding interactive region or capture tile
per participant operation; this does not reduce the engine's internal frame
executor to one slot.

Interactive and capture operations are initiated on the canonical engine
thread. Work for noncanonical engines is queued to their owning threads, while
the canonical participant executes inline. An interactive participant publishes
its own color region when its readback completes. The canonical operation joins
all participant render tasks before reporting aggregate progress, enabling fresh
regional queries, or ending the operation. UI presentation remains queued
independently. Fixed capture returns only after every participant render task
completes.

Configuration is synchronous on the canonical engine thread and does not
replace an active pool. Pool release closes a shared regional-delivery gate
before deleting worker engines. Already queued notifications become no-ops,
and an in-progress buffer conversion finishes under the gate lock before
destruction continues. Each worker engine is then deleted on its owning thread
before that thread is stopped and joined. Canvas detachment disconnects
engine/canvas signals, clears the engine-side canvas pointer, and queues canvas
presentation disablement before the canonical engine tears down its rendering
resources.

## Shared Document and Engine-Local State

All engines observe the same `ZDoc`. Worker views establish their normal
document subscriptions for object, selection, visibility, and data-pack
lifetime changes. Document-owned objects and packs are shared directly; the
pool does not copy scene payloads into worker documents.

Mutable rendering state is engine-local. Workers do not share filter,
parameter, compositor, camera, scratch, descriptor, picking, or ready-buffer
QObjects with the canonical engine or with one another.

At the start of a fixed-capture batch, and at the start of every interactive
render entry that has a noncanonical participant, the canonical engine publishes
one immutable
`Z3DRenderingEngine::VulkanTileRenderState` containing:

- compositor and global-parameter JSON;
- every available per-object view state and selection state;
- device pixel ratio; and
- the complete runtime camera.

All noncanonical participants share that publication. A worker validates the
local object topology, applies per-object state and selection, applies the
device pixel ratio and general state, and installs the complete camera last.
Applying the camera last preserves the canonical clipping range after
worker-local bounds update. A canonical participant already owns these values
and does not serialize them back into itself.

An interactive fast-preview entry performs one regional step. Its queued full
render entry publishes state again before running its progressive loop to
completion. Rendering-thread parameter changes processed between those entries
therefore reach every participant.

The immutable publication synchronizes engine-local render parameters. It does
not freeze, version, or copy the shared document payloads. A synchronous
fixed-capture batch requires the document scene to remain stable for the
batch. Interactive workers continue to receive ordinary queued document
updates between participant callbacks. A document topology mismatch at a
render boundary cancels that interactive attempt and schedules a fresh region
sequence.

Worker-local invalidation, including asynchronous paging or LOD completion,
is queued to the canonical engine. The canonical engine marks regional queries
unavailable and posts the existing render update, so the worker's fixed region
can continue through the ordinary progressive path. Worker rendering-error
signals are also forwarded through the canonical diagnostic path.

Qt input and interaction mutations remain exclusively on the canonical engine,
which owns listener dispatch, gesture state, camera changes, and the
view-to-`ZDoc` mutation path. Normal document subscriptions then propagate
document changes to every engine. A noncanonical engine participates in
interaction only by servicing a synchronous, read-only query against its last
completed regional picking or depth attachment.

`Z3DGpuInfo` is the canonical engine's process-wide planning record. Worker
initialization does not replace it. Every selected device must satisfy the
Vulkan pipeline contract and the canonical record's relevant texture,
array-layer, and memory-planning limits.

## Device Selection

`compatibleVulkanTileWorkerSelections()` returns exact, preference-sorted
device selections from the initialized canonical Vulkan context. Each
selection contains the preference index and expected physical-device UUID.

Pool configuration requires every selection to belong to that compatible set,
with unique indices and UUIDs. A noncanonical engine independently enumerates
devices and requires both values to resolve the requested adapter. There is no
substitute-device or OpenGL fallback within pool construction.

`--atlas_vk_device_index` selects the canonical engine independently of the
pool list. Canvas initialization prepends that canonical selection when a
nonempty list omits it and otherwise preserves the supplied order. Generic and
headless fixed-capture configuration keeps its list exact, so the canonical
device renders capture tiles only when explicitly selected there.

## Spatial Descriptor Contract

`Z3DTileDescriptor` is pure spatial geometry. It stores a full physical output
extent, a bottom-left-origin valid output rectangle, and a guard width.
Attachment extent, valid attachment crop, guarded frustum, and top-left
presentation or assembly origin are derived from those checked values.

`makeZ3DFixedRegionDescriptors()` divides the physical canvas into stable,
left-to-right vertical regions. It returns exactly one region per participant;
every region spans the full canvas height. The first regions receive any
remainder columns, so assignment is deterministic for odd widths. The region
list covers every output pixel exactly once without overlap. Its order matches
the configured participant order and does not change during the active layout.

`makeZ3DTileDescriptors()` divides a fixed-size capture into bounded tiles in
bottom-row-first serpentine order. Capture lanes dynamically claim indices from
that descriptor list; the descriptor order does not assign a tile permanently
to a device.

Before either workload starts, the pool verifies a nonempty descriptor list,
one common full output extent, nonoverlapping valid rectangles, and exact full
pixel coverage.

Guard pixels give a region or tile the neighboring screen-space context needed
by the pipeline and are removed from the result. Interactive guard width is the
maximum support required by active 2x2 geometry supersampling and enabled glow
blur. Guards may overlap adjacent assignments or extend outside the output;
only the valid rectangles form the presented or saved image.

## Interactive Fixed-Region Rendering

### Region execution and progress

Interactive regional rendering is active only for a Vulkan canvas with a
configured effective pool containing the canonical device and at least one
additional participant and at least one physical output column per participant.
Each participant retains its effective
left-to-right position; resize derives a new physical region for that same
participant index.

At the beginning of a progressive sequence, each participant installs:

- its guarded attachment extent;
- the common full-canvas camera viewport;
- its guarded regional frustum; and
- the matching compositor rendering region.

Installing this internal geometry retains the engine-local invalidation it
causes but suppresses outward scene-update feedback, so preparation does not
schedule another copy of the same render request.

Interactive preparation starts from an idle process-wide render-cancellation
checkpoint. Worker-state publication and fixed-region preparation finish while
the cancellation source remains absent. A UI or document cancellation request
during that interval invalidates the checkpoint, so the prepared state is not
submitted and the regional attempt is reposted. Successful acquisition installs
an exclusively owned source for participant submission and collection. A
document, paging, LOD, or parameter invalidation after acquisition cancels the
active regional attempt normally.

Each engine executes the complete color pipeline for its region, including
background, geometry, and transparency. Color, depth, picking, progressive
paging, and caches are resolved locally. The depth and picking attachments
remain engine-local and are used only for synchronous read-only queries after
the regional frame has completed. An interactive pool step submits every
unfinished region. As each participant reaches final readback publication for
that step, it queues a completion notification containing its attached
presentation source and fixed-region identity. The pool does not copy or retain
color images. A participant whose progress is already complete is skipped, and
its existing canvas pixmap remains visible.

The pool joins the submitted participant render tasks after their independent
color publication. The minimum participant progress is the interactive sequence's
reported progress, and regional queries become available only after the join.
A slow participant therefore still bounds aggregate progress and final
completion, but it does not delay presentation of another participant's color.

### Color presentation

Each engine exposes its guarded, engine-owned local color buffer after
readback. A participant posts an attached presentation source, region index,
and fixed-region geometry to the UI thread. The source identifies the engine
only while that participant remains attached; it does not own pixels. The pool
does not assemble a full-frame image, create a regional presentation image, or
transfer depth or picking data to the canonical engine.

The UI thread accepts a notification only while regional presentation and its
source remain attached. It locks the source engine's target-switch mutex,
crops the configured guard from the actual mapped ready-buffer extent, wraps
the remaining pixels in a temporary `QImage` view, and creates the region
`QPixmap` before releasing the lock. Qt image and pixmap objects belong to the
canvas presentation path; `Z3DRenderingEngine` stores neither. The pixmap owns
the presented pixels, so the engine can subsequently reuse its ready buffer.
Per-participant execution is sequential, and detach/disable drops late
notifications. Presentation does not require the ready-buffer size to match
the canvas's current size; a resize can therefore leave a temporarily smaller
or larger result visible until the next regional render replaces it.

The canvas changes only the notified region's persistent pixmap item. Other
regions remain unchanged, so progressive levels or successive render states
may be temporarily mixed until every participant catches up. The most recent
direct full-frame pixmap remains visible underneath the regional items, so an
area without a regional result keeps the direct image as its underlay.
Disabling regional presentation hides and clears the regional items while
leaving that direct pixmap visible. Ordinary full-frame `renderingFinished`
delivery does not replace the display while regional presentation is enabled.

### Input ownership

The canonical engine receives every canvas event and is the only engine that
dispatches Qt input listeners. It owns all gesture state, camera and navigation
state, object-editing state, and document mutations. Listener dispatch always
uses the complete camera frustum, including while the canonical device renders
only one complete color region.

During canonical listener dispatch, a scoped picking-query override resolves
ordinary object, nearby-object, and scene-depth queries from logical canvas
coordinates to the participant whose valid region owns the corresponding
physical pixel. The query is translated to that participant's guarded
attachment coordinates and executed synchronously on the participant's engine
thread against its last completed regional attachments. Volume-depth queries
retain the image filter's physical-input contract: the Qt position is multiplied
by the device-pixel ratio before integer conversion, the pool applies the same
edge and attachment-row mapping as direct rendering, and the participant owning
the actual sampled pixel returns its depth. The result also carries the exact
full-frame physical pixel and extent used by canonical unprojection. All of
these queries are read-only. The canonical engine then continues normal
listener dispatch and performs any resulting mutations itself.

Engine-local picking identities are normalized before a result enters canonical
listeners. A transform-handle hit carries its stable 1-based handle index and
resolves to that handle in the canonical filter. Document-owned mesh pointers
are shared across engines and retain their exact identity. An engine-local
runtime mesh hit resolves through its object ID to the canonical mesh filter's
stable regional token. The token represents object-level selection and is not
substituted with a canonical mesh pointer; exact per-mesh selection remains
limited to document-owned/current canonical mesh pointers. Picking colors and
query results identify a document object by the pair `(object ID, payload
pointer)`, so aliases that share an SWC node, punctum, or mesh payload remain
distinct. The canonical mesh, SWC, and puncta filters consume only their own
identities. Shared mesh,
SWC, and puncta pointers are accepted only while they still belong to that
object's current topology. A shared-payload topology change during a query
cancels the interaction attempt instead of exposing a stale identity.

Filters deregister their picking identities at destruction. Mesh, SWC, and
puncta filters retain the registration tokens returned for their published
colors so a rebuild or destruction removes exactly those registrations.

Regional queries become available only after a regional frame completes with
valid picking and depth attachments. Invalidation immediately makes them
unavailable; positional queries then report no hit or no usable depth until a
new regional frame completes. Camera and other non-picking interaction still
uses the canonical state normally. Atlas does not render or maintain a hidden
full-canvas picking frame on the canonical device.

### Layout changes, nonregional work, and failure

A canvas resize or device-pixel-ratio change invalidates regional queries,
cancels active canonical filter gestures, and restores the canonical engine's
full-output configuration before preparing the resized fixed regions. Existing
regional pixmaps remain visible and are replaced independently as results for
the new extent arrive. A completion produced before the resize may therefore
be shown temporarily at its old size. A canvas narrower than the participant
count temporarily uses direct rendering while preserving the configured pool.

Fixed-size screenshot and export entry points invalidate the active
interactive region state and restore the canonical full-output camera and
compositor region before capture. Before participant buffers are reused with
different output or guard geometry, the pool retires the current presentation
attachment. Queued notifications using that attachment become no-ops, while an
in-progress canvas conversion finishes before the source buffer is reused.
Subsequent regional notifications use a new attachment. No attachment
retirement occurs between regional participants or progressive steps. If
regional presentation is enabled, the engine schedules a new regional sequence
after the nonregional operation.
Fixed-size multi-tile capture can use the configured pool through its separate
dynamic tile contract.

An interactive stereo render request disables regional presentation and uses
the canonical direct rendering configuration. A later mono render prepares the
current fixed regions and reenables regional presentation. The pool remains
available for fixed-size tiled capture until the canvas detaches, the backend
switches away from Vulkan, or a worker failure releases it.

Ordinary interactive cancellation stops unfinished regional work and schedules
another render. A participant that has already collected its final readback
queues that completed region. After all participants return, cancellation is
observed before the batch accepts progressive progress or query readiness. Any
previously published region remains visible until a replacement or direct frame
arrives. An input-query topology cancellation clears the regional layout,
progress, and query readiness and schedules a fresh regional sequence. A
render-boundary topology mismatch cancels the current attempt, makes regional
queries unavailable, and schedules a fresh sequence. A non-cancellation
regional render failure restores direct canonical rendering, releases the pool,
reports the error, and schedules a direct update. Regions already presented
before the failure are hidden when regional presentation is disabled. Pool
teardown still completes if restoring the canonical full-output targets also
fails; that cleanup failure is logged without replacing the original operation
error. Interactive rendering does not continue with a reduced device set.

Switching away from Vulkan releases the pool. Switching back within the same
open 3D window uses direct rendering. A newly opened Vulkan 3D window reads the
explicit device-list flag and configures a pool when the list is nonempty and
valid.

## Fixed-Size Tiled Capture

### Dynamic scheduling and assembly

A configured pool is used for fixed-size capture only when the output contains
more than one effective tile. The capture batch owns:

- an atomic next-tile index for dynamic claims;
- an atomic stop flag;
- the first observed failure and first non-cancellation failure;
- one full-size `Z3DRenderedFrame`;
- completion state for every descriptor; and
- a mutex protecting frame assembly.

Each lane claims one tile index at a time. It collects that tile before
claiming another, so faster lanes receive more work while unclaimed tiles
remain. A stereo assignment renders and returns both eyes as one spatial task.
Each tile runs the complete active filter and compositor pipeline, including
local transparency resolution.

At batch entry, every participating engine installs the common full-output
viewport and prepares mesh export LOD for that extent. The mesh working set
remains active for the lane's complete batch. The engine saves its readback
completion policy, selects `ReturnAfterSubmit`, and restores the policy and
mesh-export state when the lane completes or abandons the batch.

For each claimed tile, the engine:

1. validates role, thread, device, batch extent, and submission health;
2. observes cancellation before recording;
3. installs the guarded attachment size, projection, and compositor region;
4. records and submits the complete mono or stereo frame;
5. retains the descriptor, eye mode, and immutable render-frame token;
6. waits at device-local completion safe points for final readback publication;
7. accepts ready pixels only when the published eye token or tokens match the
   outstanding frame token; and
8. materializes the valid rectangle as owned RGBA pixels with the required
   channel, premultiplication, and row-orientation conversion.

Completed tiles are pasted immediately into the batch frame under the assembly
mutex and then released. Per-tile dimensions, type, eye mode, placement, and
one-time completion are checked. The returned frame is save-oriented and
requires no additional vertical flip.

Direct tiled Vulkan capture uses the same valid-region conversion. Guard pixels
are not copied through an intermediate cropped `ZImg`, a full-attachment
double-precision alpha image, or a second cropped image.

### Capture cancellation and failure

Capture cancellation is checked before state publication, before tile
submission, and between claims. Once a lane observes cancellation or the
shared stop flag, it makes no further claims. A tile that has passed that
observation point can still submit; the lane waits for publication and collects
the result before it exits. Submitted Vulkan and readback resources retain
their ordinary device-local fence and completion ownership.

Every lane records its local exception, sets the shared stop flag, and restores
or abandons its engine-side export state. A non-cancellation failure takes
precedence over ordinary cancellation. Other lanes finish any outstanding tile
and stop, and the caller joins all participant render tasks before rethrowing
the selected failure.

Normal cancellation preserves the configured pool after every lane has
cleanly restored its state. A non-cancellation batch failure releases the pool
at the fixed-capture boundary. Abandonment advances the compositor's readback
owner revision, so a late completion retires its resources without publishing
pixels from the failed attempt. The batch does not return or save a partially
assembled frame and does not continue with a reduced device set.

## Heterogeneous Output and Performance

Atlas does not normalize floating-point, depth, fragment-order, transparency,
or color results across devices or drivers. Interactive region ownership is
fixed in configured participant order, so each boundary remains between the
same pair of devices. Dynamic capture claims can assign a tile to a different
adapter on another run. Complete output hashes are not guaranteed to match
across devices or repeated heterogeneous captures. Guard pixels preserve
required spatial context but do not reconcile device-specific numerical
output.

Compatibility is a correctness gate, not a speed policy. Interactive frames
wait for a result from every unfinished region, so a slower participant can
control frame latency. Fixed capture dynamically distributes unclaimed work,
but a slow outstanding tile can control the batch tail. Additional engines
also duplicate device-local resources and caches and can contend for host CPU,
readback bandwidth, driver serialization, and shared-memory bandwidth. Guard
work increases relative cost as regions or tiles become narrower. Atlas does
not apply automatic throughput admission, region weighting, or slow-lane
retraction; representative scenes, canvas/output sizes, and device sets must be
measured directly.

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

CPU-only `Z3DTileDescriptor` tests cover fixed-size tile geometry, stable
left-to-right fixed regions, odd and remainder extents, exact coverage,
nonoverlap, guard behavior, projection, orientation, mono/stereo frame
allocation and assembly, invalid extents/counts, and attachment overflow.

CPU-only `Z3DPickingManager` tests cover composite alias registration and
deregistration, scoped regional query overrides, exact argument and result
forwarding, direct-query restoration, complete override requirements,
nested-scope rejection, and exact physical volume-depth sample mapping at
integer and fractional device-pixel ratios and at image boundaries.

Complete-engine worker smoke tests are opt-in through
`ATLAS_ENABLE_VULKAN_SMOKE_TEST=1`. They cover canonical-device participation,
same-adapter PPLL capture parity, a two-physical-device capture batch,
immutable state transfer, assembled-frame contracts, worker teardown, and
continued canonical rendering. The smoke executable is built but is not
registered with CTest. Without the environment variable, an explicit run skips
before constructing a Vulkan engine and requires no Vulkan ICD or GPU.

Interactive regional changes require a device-backed manual matrix using a
Vulkan pixmap canvas with the canonical device and at least one noncanonical
participant. Validate progressive regional presentation, the direct-pixmap
underlay, cross-region seams, resize and device-pixel-ratio changes, full-view
camera navigation, canonical gesture continuity across a region boundary,
object and transform-handle identity normalization, owning-region volume-depth
interaction, benign misses while regional queries are invalidated,
worker-local invalidation, cancellation, regional resumption after nonregional
capture, and direct-path recovery after a worker failure. Validate device
selection with both an explicitly listed canonical device and a nonempty canvas
list for which Atlas prepends the omitted canonical device. Generic/headless
fixed-capture validation must keep its supplied participant set exact.
