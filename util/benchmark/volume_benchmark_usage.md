# Volume Benchmark Scripts

These scripts under `/Users/feng/code/atlas/util/benchmark/` provide a shared benchmark workflow for Atlas and ParaView:

- `atlas_volume_benchmark.py`: drives Atlas through Scene RPC
- `atlas_deterministic_batch.py`: runs 1 warm-up + N measured deterministic Atlas runs and aggregates stats
- `paraview_volume_benchmark.py`: drives ParaView through `pvpython`
- `paraview_deterministic_batch.py`: runs 1 warm-up + N measured deterministic ParaView runs and aggregates stats
- `volume_benchmark_capture.py`: captures a fixed screen region and computes first-visible / final-stable timings
- `volume_benchmark_camera_template.json`: starting point for the shared camera/action spec

## Outputs

Each driver writes a JSONL event log:

- Atlas: `atlas_events.jsonl`
- ParaView: `paraview_events.jsonl`

The capture observer reads one of those files and writes a timing summary JSON.

## Camera Spec

The benchmark spec stores:

- viewport size
- named camera states
- action sequence

Supported action kinds:

- `jump`: apply one camera state immediately
- `interpolate`: linearly interpolate between two named camera states over a fixed duration and step count

The generic camera fields are:

- `projection`
- `eye`
- `center`
- `up`
- `field_of_view_degrees`
- `eye_separation_angle_degrees`

The same file is accepted by both Atlas and ParaView. Atlas-specific typed camera keys are also accepted if you copy them from Scene RPC output.

## Atlas

Start Atlas with the Scene RPC server available, then run:

```bash
source /Users/feng/miniconda3/etc/profile.d/conda.sh
conda activate pt12
python /Users/feng/code/atlas/util/benchmark/atlas_volume_benchmark.py \
  --dataset /Users/feng/Dropbox/atlas_test/slice15_paraview/slice15_ch2_dense.nim \
  --camera-spec /Users/feng/code/atlas/util/benchmark/volume_benchmark_camera_template.json \
  --output-dir /tmp/atlas_benchmark \
  --atlas-dir /Applications/fenglab/Atlas.app \
  --canvas-logical-width 1000 \
  --canvas-logical-height 750
```

Notes:

- The script uses `StartLoadTask -> WaitTask -> WaitForObjectsReady`.
- Atlas RPC tasks persist after completion until `DeleteTask`; the driver explicitly deletes the finished load task so it does not leave stale task metadata behind.
- If the first action in the camera spec is named `open`, the driver measures dataset load plus the first rendered view as the `open` action.
- Both drivers support `--pre-action-delay-seconds` so the capture observer can sample a clean baseline frame before each action starts changing the image.
- For deterministic Atlas timing, launch Atlas with `--atlas_log_benchmark_render_timings`. If you also pass `--atlas-log-path`, the driver follows the live Atlas log and waits for `ATLAS_BENCHMARK_FAST_PREVIEW_DONE` and `ATLAS_BENCHMARK_RENDER_FINISHED` after each requested state. This is the recommended mode for large or out-of-core datasets because it removes the need to guess `--step-hold-seconds`.
- `--step-hold-seconds` remains available as a fallback when no Atlas log path is provided.
- Use `--preview-timeout-seconds` and `--final-timeout-seconds` to control how long the driver waits for each preview/final marker in log-driven mode.
- `--canvas-logical-width/--canvas-logical-height` resizes the live 3D canvas itself. On macOS Retina, `1000x750` logical typically produces `2000x1500` physical rendering.
- By default the Atlas driver enables the object parameter `Full Resolution Rendering` on any loaded object that exposes it. Pass `--disable-full-resolution` to benchmark Atlas's fast/downsampled path instead.
- Pass `--hide-background`, `--hide-axis`, and `--hide-bound-box` to disable Atlas's background gradient, axis pseudo-object, and image bound-box overlay during the benchmark. This is recommended when comparing against ParaView volume renders that do not show equivalent overlays.
- The driver reapplies the requested canvas size after loading the dataset, because object dock/layout changes can otherwise shrink the central 3D canvas after the initial resize.
- For `open`, the Atlas driver now logs an explicit `open_target_view_requested` event immediately before the final camera apply. The batch parser uses that marker, when available, to distinguish the intended target-view render from earlier preview work triggered by load-time parameter changes.

For repeated deterministic Atlas runs with persisted open/step metrics and aggregate summaries:

```bash
python /Users/feng/code/atlas/util/benchmark/atlas_deterministic_batch.py \
  --atlas-log-path /Users/feng/Library/Logs/Atlas/ \
  --atlas-pid <atlas_pid> \
  --atlas-dir /Applications/fenglab/Atlas.app \
  --output-root /Users/feng/Dropbox/atlas_test/slice15_paraview/benchmarks/atlas_deterministic_manual \
  --canvas-logical-width 1000 \
  --canvas-logical-height 750 \
  --hide-background \
  --hide-axis \
  --hide-bound-box \
  --preview-timeout-seconds 900 \
  --final-timeout-seconds 3600 \
  --sample-rss
```

That produces:

- `warmup/run01/`: warm-up artifacts
- `measured/run01` ... `measured/run07`: raw event logs, Atlas log slices, parsed preview/final log events, per-step metrics, RSS samples
- `aggregate/summary.json`: top-level aggregate summary
- `aggregate/open_metric_stats.json`: open/load metric statistics across measured runs
- `aggregate/action_step_stats.json`: pooled per-step preview/final timing statistics by action
- `aggregate/step_index_stats.json`: statistics for step 1, step 2, ... across measured runs
- `aggregate/all_measured_steps.jsonl`: every measured Atlas step with parsed preview/final timing fields

If `--atlas-log-path` points to a directory, the batch runner automatically picks the newest
`atlas_info_*_log.txt` file under that directory and passes the resolved file path down to the
driver.

Important Atlas open metrics:

- `open_total_to_first_preview_ms`: action start to the first preview of the intended target view
- `open_total_to_final_ms`: action start to the first final frame of the intended target view
- `open_target_view_to_first_preview_ms`: target-view request to first preview
- `open_target_view_to_final_ms`: target-view request to first final frame
- `open_target_view_preview_to_final_ms`: derived preview-to-final settle interval for the intended target view. If the final marker has `source=renderFast`, Atlas finished in the fast pass and this interval is reported as `0`.
- `open_postload_to_*`: load-and-camera completion to the next render marker; useful, but in historical runs these can miss a preview if the render thread beat the client-side `dataset_load_done` log by a few milliseconds

Important Atlas per-step metrics:

- `preview_client_ms`: step start to the first `ATLAS_BENCHMARK_FAST_PREVIEW_DONE`
- `final_client_ms`: step start to the first `ATLAS_BENCHMARK_RENDER_FINISHED`
- `preview_to_final_client_ms`: `final_marker - preview_marker` for the same step. If the final marker has `source=renderFast`, Atlas finished in the fast pass and this interval is reported as `0`.

## ParaView

Run the ParaView driver with `pvpython`:

```bash
pvpython /Users/feng/code/atlas/util/benchmark/paraview_volume_benchmark.py \
  --dataset /Users/feng/Dropbox/atlas_test/slice15_paraview/slice15_ch2_grid_atlasscenespace.vtpd \
  --camera-spec /Users/feng/code/atlas/util/benchmark/volume_benchmark_camera_template.json \
  --output-dir /tmp/paraview_benchmark \
  --array-name channels \
  --channel-mode component \
  --component 0
```

For Atlas scene-matching benchmarks, use the same anisotropy-corrected coordinate space that the
scene uses for 3D image display. In `/Users/feng/Downloads/test_benchmark.scene`, the image object
applies `Scale Vec3 = [1, 1, 5.0472259521484375]`, so the ParaView benchmark should use the
`*_atlasscenespace.*` export rather than the physical-spacing export:

```bash
/Applications/ParaView-6.1.0-RC1.app/Contents/bin/pvpython \
  /Users/feng/code/atlas/util/benchmark/paraview_volume_benchmark.py \
  --dataset /Users/feng/Dropbox/atlas_test/slice15_paraview/slice15_ch2_grid_atlasscenespace.vtpd \
  --camera-spec /Users/feng/Dropbox/atlas_test/slice15_paraview/slice15_scene_camera_exact_2000x1500.json \
  --output-dir /tmp/paraview_benchmark_ch2_scene \
  --array-name channels \
  --channel-mode component \
  --component 0 \
  --blend-mode maximum-intensity \
  --data-range-min 0 \
  --data-range-max 255 \
  --color-min-rgb 0 0 0 \
  --color-max-rgb 0.99215686 0 0 \
  --capture-screenshots
```

### OSPRay Launch Fix On macOS

The ParaView 6.1.0 RC1 macOS app bundle in `/Applications` is missing the unversioned
`libopenvkl_module_cpu_device.dylib` filename that OpenVKL tries to load for OSPRay volume
rendering. The benchmark utilities include a small launch wrapper that adds a shim directory with
that missing name and prepends it to `DYLD_LIBRARY_PATH`.

Launch the GUI with:

```bash
/Users/feng/code/atlas/util/benchmark/launch_paraview_with_ospray_fix.sh paraview
```

Launch `pvpython` with:

```bash
/Users/feng/code/atlas/util/benchmark/launch_paraview_with_ospray_fix.sh pvpython \
  /Users/feng/code/atlas/util/benchmark/paraview_volume_benchmark.py --help
```

Without that wrapper, `OSPRay Based` volume rendering can fail with OpenVKL device-loader errors
even though ParaView reports OSPRay support.

Supported ParaView channel modes:

- `component`: render one selected component with one transfer function
- `magnitude`: ParaView's stock multi-component magnitude path
- `rgb-direct`: bypass transfer functions and treat three components as direct RGB

`component` is the closest stock ParaView mode to single-channel microscopy rendering. Stock ParaView does not expose Atlas-style independent transfer functions per microscopy channel through the standard volume-representation UI.

For deterministic internal benchmarking without screen capture, use the batch runner. It keeps every
per-run timer artifact, every internal frame timing, and aggregate statistics under one benchmark root:

```bash
python /Users/feng/code/atlas/util/benchmark/paraview_deterministic_batch.py \
  --output-root /Users/feng/Dropbox/atlas_test/slice15_paraview/benchmarks/paraview_deterministic_manual \
  --deterministic-mode interactive-plus-final
```

That produces:

- `warmup/run01/`: warm-up artifacts
- `measured/run01` ... `measured/run07`: raw timer events, timer summaries, per-frame timelines, RSS samples
- `aggregate/summary.json`: top-level aggregate summary
- `aggregate/action_metric_stats.json`: run-level action metric statistics
- `aggregate/pooled_frame_stats.json`: pooled interactive/still frame statistics across measured runs
- `aggregate/frame_index_stats.json`: statistics for frame 1, frame 2, ... across measured runs
- `aggregate/all_measured_frames.jsonl`: every measured internal render frame with run labels

To run the deterministic batch with ParaView's stock `OSPRay Based` volume representation on macOS,
launch through the OpenVKL wrapper and keep view-level ray tracing disabled:

```bash
python /Users/feng/code/atlas/util/benchmark/paraview_deterministic_batch.py \
  --launch-wrapper /Users/feng/code/atlas/util/benchmark/launch_paraview_with_ospray_fix.sh \
  --dataset /Users/feng/Dropbox/atlas_test/slice15_paraview/slice15_ch2_dense_atlasscenespace.mhd \
  --array-name MetaImage \
  --volume-rendering-mode ospray \
  --blend-mode composite \
  --deterministic-mode interactive-plus-final \
  --output-root /Users/feng/Dropbox/atlas_test/slice15_paraview/benchmarks/paraview_ospray_deterministic_interactive_plus_final_2000x1500
```

That batch preserves the same per-run and aggregate artifacts as the default runner, while also
recording the wrapper path and effective OSPRay settings in the benchmark config and summary.

## Windows Deterministic Runs

The deterministic scripted-camera benchmarks are designed to run on Windows as well. The macOS GUI
drag tools are still macOS-only, but the Atlas and ParaView deterministic runners are portable if
you pass explicit Windows paths.

Recommended Windows workflow:

- Copy the prepared benchmark datasets and camera JSON files to the Windows machine.
- Pass explicit `--dataset`, `--camera-spec`, and `--output-root` instead of relying on the macOS
  defaults embedded in the scripts.
- Pass explicit `--pvpython` for ParaView.
- Pass explicit `--atlas-dir` and `--atlas-log-path` for Atlas.
- Use the same Python environment to launch the batch runner and its per-run driver. The Atlas
  batch now relaunches the driver with `sys.executable`, so Windows no longer depends on a
  `python3` command being available on `PATH`.
- RSS sampling now works on Windows too. The shared sampler uses WinAPI
  `GetProcessMemoryInfo(WorkingSetSize)` on Windows and `ps` on POSIX.

Example Atlas deterministic run on Windows:

```powershell
py -3 util/benchmark/atlas_deterministic_batch.py `
  --driver-script util/benchmark/atlas_volume_benchmark.py `
  --dataset D:\atlas_bench\slice15_ch2_dense.nim `
  --camera-spec D:\atlas_bench\slice15_scene_camera_exact_2000x1500.json `
  --output-root D:\atlas_bench\results\atlas_slice15 `
  --atlas-dir "C:\Program Files\fenglab\Atlas" `
  --atlas-log-path "$env:LOCALAPPDATA\Atlas\Logs" `
  --atlas-pid <atlas_pid> `
  --canvas-logical-width 1000 `
  --canvas-logical-height 750 `
  --sample-rss
```

Example ParaView deterministic run on Windows:

```powershell
py -3 util/benchmark/paraview_deterministic_batch.py `
  --pvpython "C:\Program Files\ParaView 6.1.0-RC1\bin\pvpython.exe" `
  --benchmark-script util/benchmark/paraview_volume_benchmark.py `
  --dataset D:\atlas_bench\slice15_ch2_grid_atlasscenespace.vtpd `
  --camera-spec D:\atlas_bench\slice15_scene_camera_exact_2000x1500.json `
  --output-root D:\atlas_bench\results\paraview_slice15 `
  --array-name channels `
  --channel-mode component `
  --component 0 `
  --blend-mode maximum-intensity `
  --deterministic-mode interactive-plus-final `
  --sample-rss
```

Notes:

- The macOS OSPRay wrapper `launch_paraview_with_ospray_fix.sh` is macOS-specific. Do not use it on
  Windows unless a Windows-specific OSPRay workaround becomes necessary.
- If you want one place to change all Windows paths, the simplest current approach is a small
  PowerShell wrapper script that sets the shared dataset/camera/output variables and then invokes
  the existing Python batch runners.

If you want to benchmark ParaView's separate view-level ray-tracing path explicitly, the driver can
still lock and log those controls:

```bash
/Users/feng/code/atlas/util/benchmark/launch_paraview_with_ospray_fix.sh pvpython \
  /Users/feng/code/atlas/util/benchmark/paraview_volume_benchmark.py \
  --dataset /Users/feng/Dropbox/atlas_test/slice15_paraview/slice15_ch2_dense.mhd \
  --camera-spec /Users/feng/Dropbox/atlas_test/slice15_paraview/slice15_scene_camera_exact_2000x1500.json \
  --output-dir /tmp/paraview_ospray_singlepass \
  --array-name MetaImage \
  --blend-mode composite \
  --enable-ray-tracing \
  --ray-tracing-backend "OSPRay raycaster" \
  --samples-per-pixel 1 \
  --progressive-passes 1 \
  --ambient-samples 0 \
  --denoise 0
```

The resulting `paraview_events.jsonl` and `paraview_timer_summary.json` both record the effective
view-level ray-tracing settings. That is a different benchmark path from the stock
`VolumeRenderingMode = OSPRay Based` representation and should be reported separately.

For the stock deterministic OSPRay volume benchmark that aligns with the existing ParaView/Atlas
deterministic runs, use:

- dense `.mhd/.zraw` input, not blocked `.vtpd`
- `VolumeRenderingMode = OSPRay Based`
- `EnableRayTracing = 0`
- `blend-mode = composite`

If you explicitly benchmark view-level ray tracing instead, `ProgressivePasses > 1` changes the
semantics: `t_to_final_view` becomes the completion of the last progressive pass rather than the
first still render, and ParaView warns that iterative refinement requires the global
`Enable Streaming` preference.

Supported ParaView blend modes:

- `composite`
- `maximum-intensity`
- `minimum-intensity`
- `average-intensity`
- `additive`
- `isosurface`
- `slice`

If the first action in the camera spec is named `open`, the ParaView driver measures dataset load plus the first rendered view as the `open` action.

### Always-On Timer Log

ParaView's `Tools > Timer Log` dialog stores the `Enable` checkbox in settings, but it only reapplies
that state when the dialog is opened again on a later launch. If you want timer logging enabled from
process startup without opening the dialog manually, launch ParaView with the helper script:

```bash
/Applications/ParaView-6.1.0-RC1.app/Contents/MacOS/paraview \
  --script /Users/feng/code/atlas/util/benchmark/paraview_enable_timer_log.py
```

That script uses the same `misc/TimerLog` proxy as the UI and sets:

- `Enable = 1`
- `MaxEntries = 1000000`

If you only need it for the current session, opening `Tools > Timer Log`, checking `Enable`, and
setting a larger buffer length is enough.

## Capture Observer

Run the capture script before or alongside the driver so it can watch the event file:

```bash
source /Users/feng/miniconda3/etc/profile.d/conda.sh
conda activate pt12
python /Users/feng/code/atlas/util/benchmark/volume_benchmark_capture.py \
  --events /tmp/atlas_benchmark/atlas_events.jsonl \
  --output /tmp/atlas_benchmark/summary.json \
  --x 100 --y 100 --width 1920 --height 1080
```

Important details:

- The capture rectangle must match the actual render window region on screen.
- Prefer capturing just the rendered image area rather than large static toolbars or side panels.
- The summary reports:
  - `first_visible_ms_from_start`
  - `stable_ms_from_end`
  - `stable_ms_from_start`
- The capture tool also writes a per-frame timeline JSONL next to the summary by default:
  - if the summary is `summary.json`, the frame timeline is `summary_frames.jsonl`
  - each frame sample includes wall time, monotonic time, diff-to-previous-frame, diff-to-action-baseline, and the currently active action
- For ParaView runs, the driver also writes:
  - `paraview_timer_log.txt`
  - `paraview_timer_log_standalone.txt`
  - `paraview_timer_events.json`
  - `paraview_timer_summary.json`
  - `paraview_timing_calibration.json` if the capture summary lands in the same output directory
- `paraview_timer_log.txt` is the scope-preserving indented timer log reconstructed from raw timer events, which is the right artifact for `Interactive Render` / `Still Render` analysis. `paraview_timer_log_standalone.txt` is the legacy `vtkTimerLog.DumpLog()` output for comparison.
- The ParaView event log records `runtime_mode`, requested `ViewSize`, and actual `RenderWindow` size so you can verify whether the run used `pvpython` or a live GUI macro and whether the output size matched the benchmark spec.

## Recommended Workflow

1. Open the target application and position the render window at a known screen location.
2. Tune the camera once manually.
3. Save the final numeric camera states into a JSON spec.
4. Start the capture observer with the matching window rectangle.
5. Run the application driver.
6. Read the observer summary JSON for the published benchmark numbers.

## Summary CSV Export

After the retained benchmark sessions are complete, export the compact cross-session CSV snapshots with:

```bash
python /Users/feng/code/atlas/util/benchmark/export_benchmark_snapshot_csv.py
```

This writes:

- `/Users/feng/code/atlas/util/benchmark/benchmark_cross_session_snapshot.csv`
- `/Users/feng/code/atlas/util/benchmark/benchmark_atlas_cross_dataset_snapshot.csv`
- `/Users/feng/code/atlas/util/benchmark/benchmark_gui_rotate_snapshot.csv`

The exporter reads the retained session aggregate `summary.json` files directly, so the CSV values stay
aligned with the authoritative benchmark artifacts rather than depending on manual copy/paste from the
markdown summary.

## Real GUI Drag Benchmark (macOS)

For black-box GUI interaction benchmarking on macOS, use real Quartz mouse input plus window
capture. This is separate from the deterministic scripted-camera benchmarks above.

Required Python packages:

```bash
source /Users/feng/miniconda3/etc/profile.d/conda.sh
conda activate pt12
python -m pip install pyobjc
```

Required macOS permissions for the terminal you use to launch the scripts:

- `Accessibility`
- `Screen Recording`

The basic components are:

- `/Users/feng/code/atlas/util/benchmark/macos_gui_drag_benchmark.py`
- `/Users/feng/code/atlas/util/benchmark/macos_window_capture_sckit.swift`
- `/Users/feng/code/atlas/util/benchmark/build_macos_window_capture_sckit.sh`
- `/Users/feng/code/atlas/util/benchmark/volume_benchmark_capture.py`
- `/Users/feng/code/atlas/util/benchmark/summarize_gui_capture_fps.py`
- `/Users/feng/code/atlas/util/benchmark/gui_drag_benchmark_calibration_template.json`
- `/Users/feng/code/atlas/util/benchmark/paraview_gui_rotate_batch.py`
- `/Users/feng/code/atlas/util/benchmark/atlas_gui_rotate_batch.py`

Preferred capture backend on macOS:

- `ScreenCaptureKit` via `macos_window_capture_sckit.swift`

Fallback capture backend:

- `volume_benchmark_capture.py` using `mss`

Use the ScreenCaptureKit helper when you need accurate GUI FPS on Atlas. The Python `mss` path is
still useful as a fallback, but it was too slow to resolve Atlas's real visible frame cadence
cleanly.

### 1. List Windows For Calibration

```bash
python /Users/feng/code/atlas/util/benchmark/macos_gui_drag_benchmark.py --list-windows
```

Use that to identify the target application window. Then create a calibration JSON derived from
`gui_drag_benchmark_calibration_template.json`.

Important calibration fields:

- `capture_region`: the render-area rectangle used by the ScreenCaptureKit observer
- `input_region`: the Quartz input rectangle used for mouse injection
- `analysis_region_norm`: optional normalized subregion inside `capture_region`; use this when a
  centered ROI is enough to represent visible render changes
- `region_coordinate_space`: `absolute` or `window-relative`
- `actions`: the drag path(s), expressed as normalized coordinates inside `input_region`

On Retina displays, `capture_region` and `input_region` may intentionally differ. A common pattern is:

- `capture_region`: `2000 x 1500` physical pixels
- `input_region`: `1000 x 750` logical Quartz points

If you want the calibration to survive app relaunches or minor window-position drift, prefer:

- `region_coordinate_space: "window-relative"`
- `capture_region` and `input_region` expressed relative to the matched top-level window origin

For the retained GUI benchmark, `capture_region` should cover only the real render area. Do not
include toolbars, overlays, or unrelated parts of the window. If the visible response is dominated
by the center of the volume, you can set `analysis_region_norm` to a centered ROI and keep
`input_region` larger for the actual drag path.

### 2. Build The ScreenCaptureKit Helper

```bash
CAPTURE_BIN=$(/Users/feng/code/atlas/util/benchmark/build_macos_window_capture_sckit.sh)
```

### 3. Start Capture

```bash
"${CAPTURE_BIN}" \
  --calibration /path/to/gui_calibration.json \
  --events /tmp/gui_benchmark/gui_events.jsonl \
  --output /tmp/gui_benchmark/capture_summary.json \
  --sample-hz 60 \
  --pixel-threshold 0 \
  --changed-fraction-threshold 0 \
  --stable-frames 5 \
  --timeout-seconds 20
```

The helper captures the matched window with ScreenCaptureKit and uses WindowServer frame status plus
the calibrated `capture_region` as the actual ScreenCaptureKit `sourceRect`. It then derives
visible changes from exact pixel differences inside the capture ROI, with timing anchored to
`drag_start`/`drag_end` when those markers are present. The helper keeps the captured render-area
frames in memory during the session and performs the pixel-difference analysis afterward so the
capture callback stays lightweight. It writes the same `capture_summary.json` and
`capture_summary_frames.jsonl` artifacts expected by the existing summarizer.

Use a short still period before motion so the capture helper records a clean pre-drag baseline. The
injector supports this with `pre_drag_still_seconds` in the calibration action.

Use a timeout that comfortably covers:

- helper startup
- the injector's `--initial-delay-seconds`
- drag duration
- settle time

### 4. Inject Real Mouse Drag Input

```bash
python /Users/feng/code/atlas/util/benchmark/macos_gui_drag_benchmark.py \
  --calibration /path/to/gui_calibration.json \
  --output-dir /tmp/gui_benchmark \
  --action rotate \
  --initial-delay-seconds 1.0
```

That writes:

- `/tmp/gui_benchmark/gui_events.jsonl`
- `/tmp/gui_benchmark/injected_mouse_events.jsonl`

The injector logs the same `session_start` / `action_start` / `action_end` / `session_end` markers
used by the capture observer, plus additional `drag_start` / `drag_end` markers and per-event mouse
records for debugging.

### 5. Summarize Visible FPS

```bash
python /Users/feng/code/atlas/util/benchmark/summarize_gui_capture_fps.py \
  --events /tmp/gui_benchmark/gui_events.jsonl \
  --frames /tmp/gui_benchmark/capture_summary_frames.jsonl \
  --capture-summary /tmp/gui_benchmark/capture_summary.json \
  --output /tmp/gui_benchmark/gui_fps_summary.json
```

The output summary reports:

- action duration anchored to `drag_start` / `drag_end` when available
- raw capture sample count and samples-per-second during the drag window
- changed-sample count during the drag window
- visible changed-samples-per-second
- changed-frame interval statistics
- derived visible FPS from the mean changed-frame interval
- first substantial render-area change from the captured pre-drag baseline
- final-stable timings copied from the capture summary when present

The summarizer automatically calibrates ScreenCaptureKit's monotonic frame timestamps back into the
event wall-clock domain, so it works with both:

- `capture_summary_frames.jsonl` from `macos_window_capture_sckit.swift`
- `capture_summary_frames.jsonl` from `volume_benchmark_capture.py`

Recommended first benchmark shape:

- use `slice15_ch2`
- start with `rotate` only
- capture at `60 Hz`
- run one ParaView trial and one Atlas trial before expanding to repeated measurements

### ParaView GUI Batch Runner

Use the batch runner when you want repeated real-GUI ParaView rotate measurements with a fresh
prepared GUI instance on every run:

```bash
python /Users/feng/code/atlas/util/benchmark/paraview_gui_rotate_batch.py \
  --dataset /Users/feng/Dropbox/atlas_test/slice15_paraview/slice15_ch2_grid_atlasscenespace.vtpd \
  --camera-spec /Users/feng/Dropbox/atlas_test/slice15_paraview/slice15_scene_camera_exact_2000x1500.json \
  --calibration /path/to/paraview_gui_calibration.json \
  --output-root /tmp/paraview_gui_rotate_slice15 \
  --warmup-runs 1 \
  --measured-runs 7
```

The runner:

- launches a fresh ParaView GUI process for each run with `prepare_paraview_gui_benchmark.py`
- waits for the startup script to report that the benchmark scene is ready
- starts the ScreenCaptureKit helper
- injects the real rotate drag
- writes `gui_fps_summary.json` for each run
- writes aggregate `summary.json` and `summary.md` under `aggregate/`

For long drags or high-FPS exact-pixel captures, the ScreenCaptureKit helper can spend much longer
in post-capture analysis than in the live capture itself. Use `--capture-process-wait-seconds` to
raise the batch runner's wait budget when needed.

### Atlas GUI Batch Runner

Use the Atlas batch runner when you want repeated real-GUI rotate measurements while keeping the
same prepared Atlas scene alive across warm-up and measured runs:

```bash
python /Users/feng/code/atlas/util/benchmark/atlas_gui_rotate_batch.py \
  --dataset /Users/feng/Dropbox/atlas_test/slice15_paraview/slice15_ch2_dense.nim \
  --camera-spec /Users/feng/Dropbox/atlas_test/slice15_paraview/slice15_scene_camera_exact_2000x1500.json \
  --calibration /path/to/atlas_gui_calibration.json \
  --output-root /tmp/atlas_gui_rotate_slice15 \
  --warmup-runs 1 \
  --measured-runs 7 \
  --compositing-mode "Maximum Intensity Projection"
```

The runner:

- launches the requested Atlas build once for the batch
- waits for the scene-server RPC port to become free before launch so it does not collide with a
  previous Atlas instance on `localhost:50051`
- prepares the 3D scene through RPC:
  - hides background / axis
  - sets `No Bound Box`
  - resizes the live 3D canvas
  - loads the dataset
  - enables full-resolution rendering when supported
  - applies the requested compositing mode
  - applies the benchmark camera
- uses Atlas benchmark render markers only to wait for the reset camera to settle between runs
- starts the ScreenCaptureKit helper
- injects the real rotate drag
- writes `gui_fps_summary.json` for each run
- writes aggregate `summary.json` and `summary.md` under `aggregate/`

For long drags or high-FPS exact-pixel captures, the ScreenCaptureKit helper can spend much longer
in post-capture analysis than in the live capture itself. Use `--capture-process-wait-seconds` to
raise the batch runner's wait budget when needed.

This Atlas GUI runner is intended for steady-state interaction benchmarking. It does not relaunch
Atlas between measured runs. Instead it resets the camera back to the `open` benchmark state and
waits for Atlas preview/final markers before starting the next capture.

### Optional Fallback: Python Region Capture

If ScreenCaptureKit is unavailable, you can still use the older Python observer:

```bash
python /Users/feng/code/atlas/util/benchmark/volume_benchmark_capture.py \
  --events /tmp/gui_benchmark/gui_events.jsonl \
  --output /tmp/gui_benchmark/capture_summary.json \
  --x 100 --y 100 --width 2000 --height 1500 \
  --sample-hz 60 \
  --pixel-threshold 0 \
  --stable-frames 5
```

That path is simpler, but on this machine it was too slow for reliable Atlas GUI FPS measurement.

## Retina Viewports

For the current Atlas vs ParaView comparison on macOS Retina, the shared benchmark viewport is
`2000x1500`. That corresponds to a `1000x750` logical-point window rendered at device-pixel ratio 2.
Use the same `2000x1500` benchmark camera spec for Atlas and ParaView so both applications render to
the same effective pixel size. For Atlas live-window benchmarking, set the canvas to `1000x750`
logical with `Set3DCanvasSize` or the benchmark driver flags above.

## Fidelity Validation ROI Export

For the full-resolution fidelity-validation workflow, the retained ROI datasets are exported as
standalone `.nim` files rather than blocked ParaView datasets. The current retained setup uses the
existing high-resolution Atlas dataset and writes, for each ROI:

- `fullres.nim`: native-resolution resident-GPU reference candidate
- `level1.nim`: XY downsampled by `2x`, physical extent preserved
- `level2.nim`: XY downsampled by `4x`, physical extent preserved
- `metadata.json`: exact ROI bounds plus saved output metadata

The exporter is:

```bash
python /Users/feng/code/atlas/util/benchmark/export_high_res_fidelity_rois.py
```

By default it uses:

- source dataset:
  `/Users/feng/code/atlas/large_test_image/high_res_20220219_stitched_all_spacing_0p1_0p1_2_um.nim`
- output root:
  `/Users/feng/code/atlas/large_test_image/fidelity_validation/high_res_20220219_roi_validation_v1`
- ROI size:
  `2048 x 2048 x 169`
- retained ROI centers:
  - `(16800, 4300)`
  - `(13600, 7100)`
  - `(23300, 8700)`
  - `(4100, 10000)`

The exporter writes a top-level `manifest.json` and `README.md` under the output root so the later
render / analysis step can consume the retained ROI set without recomputing the crop bounds.

## Fidelity Validation Render + Analysis

Use the render driver to compare, for each ROI and mode:

- `reference`: resident native-resolution ROI render
- `adaptive`: the original large dataset rendered with Atlas full-resolution enabled and clipped to
  the same ROI bounds
- `coarse_l1`: resident ROI downsampled by `2x` in XY, upscaled back to the native scene footprint
- `coarse_l2`: resident ROI downsampled by `4x` in XY, upscaled back to the native scene footprint

Launch Atlas with benchmark render markers enabled first:

```bash
/Users/feng/code/atlas/build/Release/src/atlas/Atlas.app/Contents/MacOS/Atlas \
  --atlas_log_benchmark_render_timings
```

Then run the retained render suite:

```bash
python /Users/feng/code/atlas/util/benchmark/atlas_fidelity_render.py \
  --roi-manifest /Users/feng/code/atlas/large_test_image/fidelity_validation/high_res_20220219_roi_validation_v1/manifest.json \
  --adaptive-dataset /Users/feng/code/atlas/large_test_image/high_res_20220219_stitched_all_spacing_0p1_0p1_2_um.nim \
  --base-camera-spec /Users/feng/code/atlas/large_test_image/high_res_scene_camera_exact_2000x1500.json \
  --output-root /Users/feng/code/atlas/large_test_image/fidelity_validation/high_res_20220219_fidelity_render_v1 \
  --atlas-log-path /Users/feng/Library/Logs/Atlas \
  --overwrite
```

Important details:

- The driver derives one local ROI camera for the resident reference/coarse controls and one global
  scene-space camera for the large adaptive dataset.
- `--camera-distance-scale < 1.0` zooms in beyond the fit view while preserving the fitted camera
  direction. This is useful when the fit view still leaves `coarse_l1` screen-space sufficient.
- The adaptive case applies local `X/Y/Z` cuts matching the ROI bounds so MIP/DVR comparisons do
  not include out-of-ROI content from the surrounding large volume.
- The coarse resident controls apply `Coord Transform 3DTransform` so their rendered world footprint
  matches the native-resolution ROI footprint even though the coarse `.nim` files are smaller.
- `--coarse-sampling-rate` can decouple the coarse-control sampling rate from the resident reference
  sampling rate. For LOD-choice comparisons, prefer matching coarse sampling to the adaptive
  sampling rate and keeping the reference sampling rate higher.
- `--transfer-function-overrides <json>` applies per-ROI/per-mode transfer-function overrides. The
  retained example for roi03 background suppression is:
  [high_res_20220219_transfer_function_overrides_roi03_threshold20_v1.json](/Users/feng/code/atlas/large_test_image/fidelity_validation/high_res_20220219_transfer_function_overrides_roi03_threshold20_v1.json)
- The current retained compositing modes are:
  - `MIP Opaque`
  - `Direct Volume Rendering`

After rendering, run the analysis pass:

```bash
python /Users/feng/code/atlas/util/benchmark/analyze_fidelity_validation.py \
  --render-manifest /Users/feng/code/atlas/large_test_image/fidelity_validation/high_res_20220219_fidelity_render_v1/manifest.json
```

That writes:

- `analysis/summary.csv`
- `analysis/summary.json`
- `analysis/details.json`
- `analysis/summary.md`
- per-comparison `analysis/<roi>/<mode>/<condition>/difference_heatmap.png`

Current fidelity metrics:

- grayscale SSIM against the resident native-resolution reference
- masked mean / median / p95 / max absolute difference
- reference-derived foreground mask with largest-connected-component filtering

The current retained analysis is image-based only. The planned screen-space sufficiency audit
(`selected voxel size <= desired pixel-footprint voxel size`) still requires engine-side audit
plumbing and is not part of the current retained output yet.
