Atlas Developer Guide

Build, Run, and Layout

- Build instructions: see `readme.md` (macOS/Linux/Windows, Qt 6.9.x, Intel oneAPI, Vulkan SDK, Ninja, Conda recipe for `zimg`).
- Source layout (selected):
  - `src/atlas/` — application code (UI, engine, docs, filters, views)
  - `src/img/` — image I/O/processing utilities
  - `docs/` — documentation
  - `util/` — build scripts

Architecture Overview

- Main window (`ZMainWindow`) — 2D UI; hosts object manager, docks, menus.
- 3D window (`Z3DMainWindow`) — spawns a rendering thread and owns a `Z3DRenderingEngine` (moved to the rendering thread), and a `Z3DCanvas` on the UI thread.
- Rendering engine (`Z3DRenderingEngine`) — owns the offscreen GL context, global parameters, compositor, network evaluator, and per-object 3D views.
- Parameter system (`ZParameter` + subclasses) — typed, QObject-based, with signals/slots and JSON (de)serialization.

Threading Model

- UI thread: widgets (`Z3DCanvas`, main window), menu actions, docks, drag-and-drop.
- Rendering thread: all engine code, rendering parameters, compositor, and object views.
- Cross-thread rules:
  - Do not manipulate engine or parameter QObjects directly from UI.
  - Use `QMetaObject::invokeMethod` to post to engine thread; use `Qt::BlockingQueuedConnection` if you must wait.
  - For parameter changes, queue to the parameter’s owning thread (see `ZParameterAnimation::setCurrentTime`).

Scene Load/Save (JSON)

- Load: `ZMainWindow::loadJsonSceneImpl`
  - Reads the `Doc` section via `ZDoc::read` which creates objects.
  - For 3D:
    - UI ensures 3D window is ready
    - Engine session starts via `beginScene3DApply()`
    - Applies `View3DGeneral` and per-object `View3D` via `applyView3D*` methods on the engine thread
    - Per-object JSON is queued if its 3D view isn’t ready; engine applies after `objViewReady`.
    - Optionally block until `scene3DApplyFinished()` using flag `--atlas_block_scene_3d_apply`.
- Save: UI collects 2D and 3D view JSON via direct 2D calls and engine-thread `write` calls (`BlockingQueuedConnection`).

Animation System

- A `Z3DAnimation` binds to the engine (view) and maintains timelines (`ZParameterAnimation` keys) for parameters (global: camera, etc.; object-specific).
- Binding and updates:
  - `ZAnimation::rebindView()` obtains parameter lists via `Z3DRenderingEngine::parametersOfViewSetting(id)` on the engine thread and binds animations without crossing QObject ownership.
  - `ZAnimation::tryLinkAnimationWith(id)` binds late objects and applies `m_currentTime` to their parameters immediately.
  - `ZParameterAnimation::setCurrentTime()` posts updates to the parameter’s thread.

Compositor and Rendering

- `Z3DCompositor` orchestrates geometry/image filters and render targets; supports transparency methods and axis/background.
- `Z3DNetworkEvaluator` executes the filter graph and drives progressive updates.
- `Z3DGlobalParameters` holds camera, lights, fog, global cuts, device pixel ratio, and scratch resource pool.

Canvas and Lifecycle

- `Z3DCanvas` posts UI events to engine. It updates its view on `renderingFinished`.
- Teardown (ordering and guards):
  - Queued signals can arrive after detaching/destroying engine.
  - `Z3DCanvas::renderingFinished` guards its engine pointer before access.
  - Engine destructor sets a shutdown flag (`m_shuttingDown`) so `event()` ignores late posts.
  - `detachCanvas()` first disconnects and clears the canvas, then adjusts `devicePixelRatio` to avoid signaling during teardown.
  - Watcher lifetime: engine tracks observed `ZWidgetsGroup*` in `m_observedWGs` and erases on `destroyed`. The set is declared before the compositor so it outlives the groups during destruction.

Logging

- Uses glog: `LOG`, `VLOG`, `LOG_FIRST_N`, `LOG_EVERY_N`.
- Notable info logs:
  - “3D scene parameters applied” — deferred scene apply queue drained.
  - “3D animation parameters bound” — first animation binding completed.

Adding a New 3D Object View

1) Create a `Z3DObjView` subclass (e.g., `Z3DNewTypeView`) for your document type; implement `hasObj`, `boundBoxOfObj`, `read/write(view JSON)`, and `viewSettingWidgetsGroupOf`.
2) Instantiate it in `Z3DRenderingEngine::init()` (or similar factory) and connect its signals to engine (`objViewReady`).
3) Connect view outputs to compositor input ports.
4) Ensure the view respects visibility/selection and updates the engine bound box.
5) Extend scene (de)serialization in the view for per-object 3D JSON.

Coding Guidelines

- Follow existing style (Qt, modern C++17/20).
- Keep UI and engine responsibilities separated; prefer posted calls over shared state.
- Prefer plain data transfer (ids, JSON, POD) across threads.
- When in doubt about a cross-thread call, add a `thread()` equality check and use `invokeMethod` accordingly.

Testing and Diagnostics

- Prefer small, focused tests for views/components (where available).
- Use verbose logging (`--v=1`) to trace rendering progress and apply order.
- For headless animation export, try small sizes first, then scale up with `--output_tile_size` and `--output_tile_border`.
