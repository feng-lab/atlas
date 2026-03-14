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
  --canvas-logical-width 1000 \
  --canvas-logical-height 750
```

Notes:

- The script uses `StartLoadTask -> WaitTask -> WaitForObjectsReady`.
- Atlas RPC tasks persist after completion until `DeleteTask`; the driver explicitly deletes the finished load task so it does not leave stale task metadata behind.
- If the first action in the camera spec is named `open`, the driver measures dataset load plus the first rendered view as the `open` action.
- Both drivers support `--pre-action-delay-seconds` so the capture observer can sample a clean baseline frame before each action starts changing the image.
- For simple deterministic Atlas timing, launch Atlas with `--atlas_log_benchmark_render_timings` and use `--step-hold-seconds` in the driver so each camera state has time to finish before the next command is sent. The log then emits `ATLAS_BENCHMARK_FAST_PREVIEW_DONE` and, when applicable, `ATLAS_BENCHMARK_RENDER_FINISHED`.
- `--canvas-logical-width/--canvas-logical-height` resizes the live 3D canvas itself. On macOS Retina, `1000x750` logical typically produces `2000x1500` physical rendering.
- By default the Atlas driver enables the object parameter `Full Resolution Rendering` on any loaded object that exposes it. Pass `--disable-full-resolution` to benchmark Atlas's fast/downsampled path instead.
- Pass `--hide-background`, `--hide-axis`, and `--hide-bound-box` to disable Atlas's background gradient, axis pseudo-object, and image bound-box overlay during the benchmark. This is recommended when comparing against ParaView volume renders that do not show equivalent overlays.
- The driver reapplies the requested canvas size after loading the dataset, because object dock/layout changes can otherwise shrink the central 3D canvas after the initial resize.
- For `open`, the Atlas driver now logs an explicit `open_target_view_requested` event immediately before the final camera apply. The batch parser uses that marker, when available, to distinguish the intended target-view render from earlier preview work triggered by load-time parameter changes.

For repeated deterministic Atlas runs with persisted open/step metrics and aggregate summaries:

```bash
python /Users/feng/code/atlas/util/benchmark/atlas_deterministic_batch.py \
  --atlas-log-path /path/to/atlas.log \
  --atlas-pid <atlas_pid> \
  --output-root /Users/feng/Dropbox/atlas_test/slice15_paraview/benchmarks/atlas_deterministic_manual \
  --canvas-logical-width 1000 \
  --canvas-logical-height 750 \
  --hide-background \
  --hide-axis \
  --hide-bound-box \
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

## Retina Viewports

For the current Atlas vs ParaView comparison on macOS Retina, the shared benchmark viewport is
`2000x1500`. That corresponds to a `1000x750` logical-point window rendered at device-pixel ratio 2.
Use the same `2000x1500` benchmark camera spec for Atlas and ParaView so both applications render to
the same effective pixel size. For Atlas live-window benchmarking, set the canvas to `1000x750`
logical with `Set3DCanvasSize` or the benchmark driver flags above.
