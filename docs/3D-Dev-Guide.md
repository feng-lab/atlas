3D Rendering Engine: Developer Guide

Overview

- Core classes: `Z3DRenderingEngine`, `Z3DCanvas`, `Z3DCompositor`, `Z3DGlobalParameters`, `Z3DObjView` (+ concrete views like `Z3DImgView`, `Z3DMeshView`, `Z3DAnimationView`).
- UI runs on the main thread. The 3D engine runs on its own rendering thread (`QThread`) in the GUI app; in headless/CLI it runs on the calling thread.
- OpenGL context is created offscreen by the engine and bound to the rendering thread. The canvas is a thin bridge that posts UI events to the engine and displays images when the engine signals readiness.

Threading Model (UI vs Engine)

- `Z3DRenderingEngine` is a `QObject` that lives on the rendering thread in the GUI path (see `Z3DMainWindow`, `m_renderingThread`).
- `Z3DCanvas` lives on the UI thread. It posts input events to the engine thread and updates its viewport when the engine emits `renderingFinished`.
- All rendering parameters (`ZParameter` and subclasses, e.g., `ZBoolParameter`, `ZFloatParameter`, `Z3DCameraParameter`) are QObjects owned by engine-side objects. Mutations must occur on the engine thread.
- When in headless/CLI (see `src/atlas/zrunexport3danimation.cpp`), the engine is used on the current thread and the same rules apply but without cross-thread hops.

Safe Cross-Thread Patterns

- To call engine methods from UI: use `QMetaObject::invokeMethod(engine, lambda, Qt::QueuedConnection)` to schedule work on the engine thread. For synchronous waits, use `Qt::BlockingQueuedConnection`.
- To mutate parameters from UI or other threads: queue to the parameter’s thread. We do this inside `ZParameterAnimation::setCurrentTime`.
- Do not pass UI-owned QObject into engine thread or vice versa. Instead, pass POD data (JSON, ids, tuples) or use engine helper methods that return plain data.

Scene Loading (robust, thread-safe)

- UI parses `.scene` and dispatches 3D state to the engine thread with deferred apply:
  - Engine API:
    - `beginScene3DApply()` starts a new session (resets queue/counter)
    - `applyView3DGeneral(const json::object&)` applies compositor/global parameters
    - `applyView3DForId(size_t id, json::object)` applies per-object 3D state immediately if the view exists; otherwise queues it until `objViewReady(id)`
    - Emits `scene3DApplyFinished()` when everything in the session is applied
  - UI option: `--atlas_block_scene_3d_apply` blocks until `scene3DApplyFinished()` for deterministic load.
- Implementation references:
  - Dispatch: `src/atlas/zmainwindow.cpp:910+`
  - Engine handlers: `src/atlas/z3drenderingengine.cpp:878+`

Animation Loading and Binding

- `Z3DAnimationDoc::bindView` binds engine and triggers initial time set.
- `ZAnimation::rebindView` binds animations to parameters obtained from the engine thread:
  - Use `Z3DRenderingEngine::parametersOfViewSetting(id)` to fetch a raw `std::vector<ZParameter*>` (no cross-thread `ZWidgetsGroup` sharing)
  - Guards cross-thread access with `BlockingQueuedConnection` where needed
- On late linking (when a view becomes ready after load), `tryLinkAnimationWith(id)` binds parameters and immediately applies the current animation time to avoid partial states.
- Parameter updates during animation (`ZParameterAnimation::setCurrentTime`) are queued to the parameter’s owning thread to avoid dropped updates.

Canvas/Engine Lifecycle

- Engine detaches canvas via `Z3DRenderingEngine::detachCanvas()` and clears engine ref on canvas.
- `Z3DCanvas::renderingFinished` must handle in-flight queued signals after detach/destruction. The slot now checks `m_engine` before dereferencing.

Logging & Diagnostics

- We use glog. Useful macros:
  - `LOG(INFO)`, `LOG(WARNING)`, `LOG(ERROR)`
  - `LOG_FIRST_N(INFO, 1)` – log once per callsite
  - `VLOG(n)` for verbose logs (enable via `--v=N`)
- When scene apply completes, engine logs: “3D scene parameters applied”.
- When animation parameters first bind, we log once: “3D animation parameters bound”.

Key APIs and Signals (with files/lines)

- Engine init/attach: `src/atlas/z3drenderingengine.cpp:800` (`initAndAttachToCanvas`)
- Deferred scene apply: `src/atlas/z3drenderingengine.cpp:878, 885, 898`
- Scene load UI flow: `src/atlas/zmainwindow.cpp:910–964`
- Animation rebind/link: `src/atlas/zanimation.cpp:300–327, 560–600`
- Parameter thread-safe updates: `src/atlas/zparameteranimation.cpp:103`
- Canvas update guard: `src/atlas/z3dcanvas.cpp:66–118` (slot hardening in `renderingFinished`)

Adding a New 3D Object View Type

1) Create a `Z3DObjView` subclass (e.g., `Z3DNewTypeView`).
2) Instantiate and wire it in `Z3DRenderingEngine::init()` similarly to existing views:
   - Construct with doc + engine
   - Connect its `objViewReady` signal to engine’s `objViewReady` signal
   - Connect outputs to compositor inputs
3) Implement `viewSettingWidgetsGroupOf(id)` on the view to return a `ZWidgetsGroup` for view settings. Engine’s `parametersOfViewSetting(id)` uses it.
4) Ensure the view properly sets `visible`, `selected`, and updates bound box.

Thread-Safety Checklist

- Do:
  - Post engine changes to the engine thread via `invokeMethod`.
  - Post parameter changes to their owning thread (see `ZParameterAnimation::setCurrentTime`).
  - Use engine helpers that return plain data (`parametersOfViewSetting`) instead of moving QObject UI across threads.
- Don’t:
  - Call engine methods or mutate parameters from the UI thread directly.
  - Share `QWidget`/`ZWidgetsGroup` across threads.

Flags & Options

- `--atlas_block_scene_3d_apply` (bool): if true, scene load blocks until all 3D parameters applied.
- Headless export (Linux): `--__use_EGL` enables EGL path; `--use_gpu_devices` controls GPU selection.

