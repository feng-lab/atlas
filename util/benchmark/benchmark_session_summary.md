# Deterministic Benchmark Sessions: `slice15_ch2`

This document summarizes the final deterministic benchmark sessions for the
`slice15_ch2` dataset. The raw artifacts under each benchmark root remain the
authoritative source. This file is the user-facing summary for the finished
session set.

## Shared Methodology

| Item | Value |
| --- | --- |
| Camera spec | `/Users/feng/Dropbox/atlas_test/slice15_paraview/slice15_scene_camera_exact_2000x1500.json` |
| Output render size | `2000 x 1500` physical pixels |
| Atlas live 3D canvas size | `1000 x 750` logical Qt pixels on Retina, yielding about `2000 x 1500` physical pixels |
| Deterministic action sequence | `open`, `rotate`, `zoom` |
| Interpolated actions | `30` requested camera steps per action |
| Warm-up / measured runs | `1` warm-up run, `7` measured runs |
| ParaView preview metric | Per-frame `Interactive Render` time from ParaView timer events |
| ParaView final metric | First `Still Render` after the interactive sequence; preview-to-final settle is `first_still_complete - last_interactive_complete` |
| Atlas preview metric | First `ATLAS_BENCHMARK_FAST_PREVIEW_DONE` after the relevant RPC timestamp |
| Atlas final metric | First `ATLAS_BENCHMARK_RENDER_FINISHED` after the relevant RPC timestamp; preview-to-final settle is `final_marker - preview_marker`, clamped to `0` when `source=renderFast` because preview and final are the same render |
| Atlas parity settings | `--hide-background --hide-axis --hide-bound-box` on the final MIP and DVR runs |
| Notes | Deterministic sessions measure internal service time under scripted camera states. They are not black-box GUI presentation benchmarks. |

Warm-up sections below are single-run results, so `count = 1` and `std = n/a`
for those tables.

## Atlas Cross-Dataset Snapshot

Measured steady-state means for the retained Atlas sessions across all completed
datasets. `Rotate/Zoom release/final` uses the last-step `preview -> final`
settle interval so the values match the detailed Atlas sections later in this
document.

| Dataset | Mode | Open first preview | Open final | Rotate preview | Rotate release/final | Zoom preview | Zoom release/final | Peak RSS |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `slice15_ch2` | `Maximum Intensity Projection` | `1057.494 ms` | `1648.462 ms` | `28.560 ms` | `133.723 ms` | `28.768 ms` | `654.791 ms` | `3.674 GiB` |
| `slice15_ch2` | `Direct Volume Rendering` | `1120.548 ms` | `1787.644 ms` | `30.466 ms` | `136.012 ms` | `31.925 ms` | `676.605 ms` | `5.615 GiB` |
| `slice15_ch2_gpufit_1024x1024x980` | `Maximum Intensity Projection` | `8119.536 ms` | `8119.561 ms` | `46.233 ms` | `0.000 ms` | `67.763 ms` | `0.000 ms` | `3.345 GiB` |
| `slice15_ch2_gpufit_1024x1024x980` | `Direct Volume Rendering` | `8289.513 ms` | `8289.545 ms` | `52.739 ms` | `0.000 ms` | `88.085 ms` | `0.000 ms` | `3.392 GiB` |
| `slice15_ch2_x2z` | `Maximum Intensity Projection` | `3322.114 ms` | `3643.508 ms` | `37.006 ms` | `113.781 ms` | `38.463 ms` | `1127.208 ms` | `8.564 GiB` |
| `slice15_ch2_x2z` | `Direct Volume Rendering` | `3357.746 ms` | `3593.807 ms` | `39.085 ms` | `124.365 ms` | `43.987 ms` | `1171.357 ms` | `9.374 GiB` |
| `high_res_20220219_stitched_all_spacing_0p1_0p1_2_um` | `Maximum Intensity Projection` | `4071.009 ms` | `4420.345 ms` | `35.940 ms` | `112.519 ms` | `41.128 ms` | `1032.147 ms` | `10.290 GiB` |
| `high_res_20220219_stitched_all_spacing_0p1_0p1_2_um` | `Direct Volume Rendering` | `4465.727 ms` | `4832.279 ms` | `37.062 ms` | `122.712 ms` | `45.618 ms` | `1071.328 ms` | `16.238 GiB` |

## Cross-Session Snapshot

Measured steady-state means for the final five `slice15_ch2` sessions. For
ParaView, `Rotate/Zoom release/final` is `release -> first still`. For Atlas,
the same columns use the last-step preview-to-final settle interval so the
snapshot matches the detailed sections below.

| Session | Input | Render mode | Open first preview | Open final | Rotate preview | Rotate release/final | Zoom preview | Zoom release/final | Peak RSS | Full-run wall |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| ParaView GPU MIP | Blocked `.vtpd` | `GPU Based` + `maximum-intensity` | n/a | `14205.041 ms` | `1124.907 ms` | `1111.071 ms` | `930.703 ms` | `718.979 ms` | `10.705 GiB` | `94.302 s` |
| ParaView GPU Composite | Blocked `.vtpd` | `GPU Based` + `composite` | n/a | `14626.693 ms` | `1215.071 ms` | `1187.297 ms` | `1030.389 ms` | `811.907 ms` | `10.702 GiB` | `100.664 s` |
| ParaView OSPRay Composite | Dense `.mhd/.zraw` | `OSPRay Based` + `composite` | n/a | `41352.606 ms` | `5091.170 ms` | `6125.940 ms` | `9473.051 ms` | `11010.973 ms` | `7.294 GiB` | `513.502 s` |
| Atlas MIP | Dense `.nim` | `Maximum Intensity Projection` | `1057.494 ms` | `1648.462 ms` | `28.560 ms` | `133.723 ms` | `28.768 ms` | `654.791 ms` | `3.674 GiB` | `123.584 s` |
| Atlas DVR | Dense `.nim` | `Direct Volume Rendering` | `1120.548 ms` | `1787.644 ms` | `30.466 ms` | `136.012 ms` | `31.925 ms` | `676.605 ms` | `5.616 GiB` | `123.711 s` |

## Session 1: ParaView GPU, Blocked Input, MIP

Artifacts:
- Root: `/Users/feng/Dropbox/atlas_test/slice15_paraview/benchmarks/paraview_deterministic_interactive_plus_final_2000x1500`
- Aggregate summary: `/Users/feng/Dropbox/atlas_test/slice15_paraview/benchmarks/paraview_deterministic_interactive_plus_final_2000x1500/aggregate/summary.json`

### Setup

| Item | Value |
| --- | --- |
| Software | ParaView 6.1.0-RC1 |
| Representation mode | `GPU Based` |
| Blend mode | `maximum-intensity` |
| Deterministic mode | `interactive-plus-final` |
| Dataset | `/Users/feng/Dropbox/atlas_test/slice15_paraview/slice15_ch2_grid_atlasscenespace.vtpd` |
| Dataset format | Blocked `.vtpd` with `.vti` pieces |
| Dataset size | `9216 x 6144 x 98`, single channel |
| Dataset spacing | `1 x 1 x 5.0472259521484375` scene-space units |
| Scalar array | `channels`, component `0` |
| Color ramp | black -> red over `0 .. 255` |

### Warm-up

| Metric | Warm-up value |
| --- | --- |
| Open final complete from action start | `15324.192 ms` |
| Open still-render total | `7035.687 ms` |
| Rotate preview frame mean | `1134.757 ms` |
| Rotate first preview complete | `1541.107 ms` |
| Rotate release -> first still | `1142.908 ms` |
| Rotate final complete from action start | `35629.222 ms` |
| Zoom preview frame mean | `941.496 ms` |
| Zoom first preview complete | `1377.458 ms` |
| Zoom release -> first still | `717.087 ms` |
| Zoom final complete from action start | `29424.362 ms` |
| Peak RSS | `11497373696 bytes` (`10.708 GiB`) |

### Measured Steady State

| Metric | Count | Mean | Median | Std | p95 | Notes |
| --- | --- | --- | --- | --- | --- | --- |
| Open final complete from action start | `7` | `14205.041 ms` | `14182.645 ms` | `177.294 ms` | `14441.481 ms` | First and final render are the same for `open` in this run shape. |
| Open still-render total | `7` | `6951.447 ms` | `6975.392 ms` | `61.916 ms` | `7014.817 ms` | Dense final render cost after the dataset is in place. |
| Rotate preview frame duration | `210` | `1124.907 ms` | `1106.376 ms` | `116.026 ms` | `1366.194 ms` | Pooled `Interactive Render` frames. |
| Rotate frame 1 preview duration | `7` | `1304.958 ms` | `1307.380 ms` | `21.891 ms` | `1330.400 ms` | First preview frame across measured runs. |
| Rotate frame 30 preview duration | `7` | `1152.997 ms` | `1147.062 ms` | `12.719 ms` | `1170.469 ms` | Last preview frame across measured runs. |
| Rotate preview service FPS | `210` | `0.889` | n/a | `n/a (derived)` | n/a | Computed as `1000 / mean_preview_ms`. |
| Rotate release -> first still | `7` | `1111.071 ms` | `1108.071 ms` | `19.279 ms` | `1133.692 ms` | Final-view latency after the last interactive step. |
| Rotate final complete from action start | `7` | `35289.989 ms` | `35225.049 ms` | `433.274 ms` | `35778.500 ms` | End-to-end action duration to the first final still. |
| Zoom preview frame duration | `210` | `930.703 ms` | `928.464 ms` | `144.761 ms` | `1155.128 ms` | Pooled `Interactive Render` frames. |
| Zoom frame 1 preview duration | `7` | `1161.113 ms` | `1160.451 ms` | `12.160 ms` | `1178.020 ms` | First preview frame across measured runs. |
| Zoom frame 30 preview duration | `7` | `757.211 ms` | `762.983 ms` | `16.947 ms` | `773.788 ms` | Last preview frame across measured runs. |
| Zoom preview service FPS | `210` | `1.074` | n/a | `n/a (derived)` | n/a | Computed as `1000 / mean_preview_ms`. |
| Zoom release -> first still | `7` | `718.979 ms` | `720.012 ms` | `13.552 ms` | `736.929 ms` | Final-view latency after the last interactive step. |
| Zoom final complete from action start | `7` | `29051.510 ms` | `28968.634 ms` | `358.891 ms` | `29481.736 ms` | End-to-end action duration to the first final still. |
| Peak RSS | `7` | `11493954121 bytes` (`10.705 GiB`) | `11494887424 bytes` (`10.706 GiB`) | `4536078 bytes` | `11498919117 bytes` (`10.709 GiB`) | Aggregate memory summary across measured runs. |
| Full-run wall time | `7` | `94.302 s` | `94.496 s` | `0.747 s` | `95.134 s` | Entire deterministic script duration per measured run. |

## Session 2: ParaView GPU, Blocked Input, Composite

Artifacts:
- Root: `/Users/feng/Dropbox/atlas_test/slice15_paraview/benchmarks/paraview_gpu_deterministic_interactive_plus_final_2000x1500_composite_v1`
- Aggregate summary: `/Users/feng/Dropbox/atlas_test/slice15_paraview/benchmarks/paraview_gpu_deterministic_interactive_plus_final_2000x1500_composite_v1/aggregate/summary.json`

### Setup

| Item | Value |
| --- | --- |
| Software | ParaView 6.1.0-RC1 |
| Representation mode | `GPU Based` |
| Blend mode | `composite` |
| Deterministic mode | `interactive-plus-final` |
| Dataset | `/Users/feng/Dropbox/atlas_test/slice15_paraview/slice15_ch2_grid_atlasscenespace.vtpd` |
| Dataset format | Blocked `.vtpd` with `.vti` pieces |
| Dataset size | `9216 x 6144 x 98`, single channel |
| Dataset spacing | `1 x 1 x 5.0472259521484375` scene-space units |
| Scalar array | `channels`, component `0` |
| Color ramp | black -> red over `0 .. 255` |

### Warm-up

| Metric | Warm-up value |
| --- | --- |
| Open final complete from action start | `16183.680 ms` |
| Open still-render total | `7567.858 ms` |
| Rotate preview frame mean | `1271.951 ms` |
| Rotate first preview complete | `1674.717 ms` |
| Rotate release -> first still | `1264.413 ms` |
| Rotate final complete from action start | `39828.663 ms` |
| Zoom preview frame mean | `1106.831 ms` |
| Zoom first preview complete | `1506.970 ms` |
| Zoom release -> first still | `971.576 ms` |
| Zoom final complete from action start | `34629.810 ms` |
| Peak RSS | `11484262400 bytes` (`10.696 GiB`) |

### Measured Steady State

| Metric | Count | Mean | Median | Std | p95 | Notes |
| --- | --- | --- | --- | --- | --- | --- |
| Open final complete from action start | `7` | `14626.693 ms` | `14445.083 ms` | `517.873 ms` | `15308.140 ms` | First and final render are the same for `open` in this run shape. |
| Open still-render total | `7` | `7018.694 ms` | `6972.196 ms` | `190.101 ms` | `7279.776 ms` | Dense final render cost after the dataset is in place. |
| Rotate preview frame duration | `210` | `1215.071 ms` | `1207.146 ms` | `109.198 ms` | `1411.862 ms` | Pooled `Interactive Render` frames. |
| Rotate frame 1 preview duration | `7` | `1405.942 ms` | `1367.261 ms` | `55.304 ms` | `1473.005 ms` | First preview frame across measured runs. |
| Rotate frame 30 preview duration | `7` | `1230.734 ms` | `1195.255 ms` | `63.271 ms` | `1299.983 ms` | Last preview frame across measured runs. |
| Rotate preview service FPS | `210` | `0.823` | n/a | `n/a (derived)` | n/a | Computed as `1000 / mean_preview_ms`. |
| Rotate release -> first still | `7` | `1187.297 ms` | `1156.144 ms` | `55.358 ms` | `1255.604 ms` | Final-view latency after the last interactive step. |
| Rotate final complete from action start | `7` | `38063.437 ms` | `37097.870 ms` | `1671.924 ms` | `39954.990 ms` | End-to-end action duration to the first final still. |
| Zoom preview frame duration | `210` | `1030.389 ms` | `1027.851 ms` | `140.824 ms` | `1262.248 ms` | Pooled `Interactive Render` frames. |
| Zoom frame 1 preview duration | `7` | `1228.623 ms` | `1210.048 ms` | `71.643 ms` | `1302.987 ms` | First preview frame across measured runs. |
| Zoom frame 30 preview duration | `7` | `897.340 ms` | `884.567 ms` | `30.688 ms` | `942.364 ms` | Last preview frame across measured runs. |
| Zoom preview service FPS | `210` | `0.971` | n/a | `n/a (derived)` | n/a | Computed as `1000 / mean_preview_ms`. |
| Zoom release -> first still | `7` | `811.907 ms` | `795.514 ms` | `34.946 ms` | `864.742 ms` | Final-view latency after the last interactive step. |
| Zoom final complete from action start | `7` | `32125.037 ms` | `31317.502 ms` | `1458.027 ms` | `34203.718 ms` | End-to-end action duration to the first final still. |
| Peak RSS | `7` | `11491011438 bytes` (`10.702 GiB`) | `11490791424 bytes` (`10.701 GiB`) | `5139628 bytes` | `11498342400 bytes` (`10.709 GiB`) | Aggregate memory summary across measured runs. |
| Full-run wall time | `7` | `100.664 s` | n/a | `3.833 s` | n/a | Stored in `memory_stats.run_elapsed_wall_seconds` for this batch. |

## Session 3: ParaView OSPRay, Dense Input, Composite

Artifacts:
- Root: `/Users/feng/Dropbox/atlas_test/slice15_paraview/benchmarks/paraview_ospray_deterministic_interactive_plus_final_2000x1500_composite_v3`
- Aggregate summary: `/Users/feng/Dropbox/atlas_test/slice15_paraview/benchmarks/paraview_ospray_deterministic_interactive_plus_final_2000x1500_composite_v3/aggregate/summary.json`

### Setup

| Item | Value |
| --- | --- |
| Software | ParaView 6.1.0-RC1 |
| Launch path | `/Users/feng/code/atlas/util/benchmark/launch_paraview_with_ospray_fix.sh` |
| Representation mode | `OSPRay Based` |
| Blend mode | `composite` |
| View ray tracing | Disabled (`EnableRayTracing = 0`) |
| Deterministic mode | `interactive-plus-final` |
| Dataset | `/Users/feng/Dropbox/atlas_test/slice15_paraview/slice15_ch2_dense_atlasscenespace.mhd` |
| Dataset format | Dense `.mhd/.zraw` |
| Dataset size | `9216 x 6144 x 98`, single channel |
| Dataset spacing | `1 x 1 x 5.0472259521484375` scene-space units |
| Scalar array | `MetaImage`, single component |
| Color ramp | black -> red over `0 .. 255` |
| Notes | Stock OSPRay volume rendering on this ParaView build did not render the blocked `.vtpd` path correctly, so this benchmark uses dense input only. |

### Warm-up

| Metric | Warm-up value |
| --- | --- |
| Open final complete from action start | `41092.554 ms` |
| Open still-render total | `13145.980 ms` |
| Rotate preview frame mean | `4359.011 ms` |
| Rotate first preview complete | `3317.717 ms` |
| Rotate release -> first still | `5375.746 ms` |
| Rotate final complete from action start | `136636.509 ms` |
| Zoom preview frame mean | `9003.979 ms` |
| Zoom first preview complete | `5203.264 ms` |
| Zoom release -> first still | `10894.365 ms` |
| Zoom final complete from action start | `281624.981 ms` |
| Peak RSS | `7832903680 bytes` (`7.295 GiB`) |

### Measured Steady State

| Metric | Count | Mean | Median | Std | p95 | Notes |
| --- | --- | --- | --- | --- | --- | --- |
| Open final complete from action start | `7` | `41352.606 ms` | `41255.482 ms` | `243.419 ms` | `41641.150 ms` | First and final render are the same for `open` in this run shape. |
| Open still-render total | `7` | `12633.521 ms` | `12576.184 ms` | `100.972 ms` | `12787.783 ms` | Dense final render cost after the dataset is in place. |
| Rotate preview frame duration | `210` | `5091.170 ms` | `5245.222 ms` | `742.544 ms` | `6145.012 ms` | Pooled `Interactive Render` frames. |
| Rotate frame 1 preview duration | `7` | `3277.866 ms` | `3247.267 ms` | `191.843 ms` | `3565.101 ms` | First preview frame across measured runs. |
| Rotate frame 30 preview duration | `7` | `6085.699 ms` | `5982.469 ms` | `150.572 ms` | `6284.891 ms` | Last preview frame across measured runs. |
| Rotate preview service FPS | `210` | `0.196` | n/a | `n/a (derived)` | n/a | Computed as `1000 / mean_preview_ms`. |
| Rotate release -> first still | `7` | `6125.940 ms` | `6211.250 ms` | `157.330 ms` | `6285.723 ms` | Final-view latency after the last interactive step. |
| Rotate final complete from action start | `7` | `159432.798 ms` | `158333.418 ms` | `2671.600 ms` | `163714.674 ms` | End-to-end action duration to the first final still. |
| Zoom preview frame duration | `210` | `9473.051 ms` | `9952.588 ms` | `1701.546 ms` | `11404.568 ms` | Pooled `Interactive Render` frames. |
| Zoom frame 1 preview duration | `7` | `5956.232 ms` | `6035.004 ms` | `227.506 ms` | `6212.268 ms` | First preview frame across measured runs. |
| Zoom frame 30 preview duration | `7` | `11050.017 ms` | `11182.425 ms` | `388.780 ms` | `11499.758 ms` | Last preview frame across measured runs. |
| Zoom preview service FPS | `210` | `0.106` | n/a | `n/a (derived)` | n/a | Computed as `1000 / mean_preview_ms`. |
| Zoom release -> first still | `7` | `11010.973 ms` | `10688.045 ms` | `604.124 ms` | `11919.764 ms` | Final-view latency after the last interactive step. |
| Zoom final complete from action start | `7` | `295782.896 ms` | `293839.432 ms` | `4594.407 ms` | `302295.213 ms` | End-to-end action duration to the first final still. |
| Peak RSS | `7` | `7831657911 bytes` (`7.294 GiB`) | `7832907776 bytes` (`7.295 GiB`) | `2908109 bytes` | `7834938573 bytes` (`7.297 GiB`) | Aggregate memory summary across measured runs. |
| Full-run wall time | `7` | `513.502 s` | `510.532 s` | `6.714 s` | `523.946 s` | Entire deterministic script duration per measured run. |

## Session 4: Atlas Dense Input, MIP

Artifacts:
- Root: `/Users/feng/Dropbox/atlas_test/slice15_paraview/benchmarks/atlas_deterministic_interactive_plus_final_2000x1500_mip_v5_parity`
- Aggregate summary: `/Users/feng/Dropbox/atlas_test/slice15_paraview/benchmarks/atlas_deterministic_interactive_plus_final_2000x1500_mip_v5_parity/aggregate/summary.json`

### Setup

| Item | Value |
| --- | --- |
| Software | Atlas (local build from this repository) |
| Rendering mode | Full-resolution image rendering enabled |
| Compositing mode | `Maximum Intensity Projection` |
| Deterministic mode | `interactive-plus-final` with a fixed `2.0 s` hold between camera commands |
| Dataset | `/Users/feng/Dropbox/atlas_test/slice15_paraview/slice15_ch2_dense.nim` |
| Dataset format | Dense `.nim` |
| Dataset size | `9216 x 6144 x 98`, single channel |
| Dataset spacing | `0.10378322750329971 x 0.10378322750329971 x 0.5238174200057983 um` |
| Benchmark cleanup flags | `--hide-background --hide-axis --hide-bound-box` |
| Effective scene-space Z/X ratio | `5.0472259521484375` |

### Warm-up

| Metric | Warm-up value |
| --- | --- |
| Open total -> first preview | `1072.207 ms` |
| Open total -> final | `2176.444 ms` |
| Open target view -> first preview | `176.380 ms` |
| Open target view -> final | `1280.617 ms` |
| Open target view preview -> final | `1104.237 ms` |
| Open post-load -> first preview | `13.844 ms` |
| Open post-load -> final | `1118.081 ms` |
| Rotate preview step mean | `33.001 ms` |
| Rotate step 1 preview | `23.146 ms` |
| Rotate step 30 preview | `26.881 ms` |
| Rotate step 30 preview -> final | `162.464 ms` |
| Zoom preview step mean | `30.402 ms` |
| Zoom step 1 preview | `24.435 ms` |
| Zoom step 30 preview | `29.624 ms` |
| Zoom step 30 preview -> final | `1907.010 ms` |
| Peak RSS | `3096059904 bytes` (`2.884 GiB`) |

### Measured Steady State

| Metric | Count | Mean | Median | Std | p95 | Notes |
| --- | --- | --- | --- | --- | --- | --- |
| Open total -> first preview | `7` | `1057.494 ms` | `1093.328 ms` | `90.155 ms` | `1152.699 ms` | From the RPC action start to the first fast-preview milestone for the intended target view. |
| Open total -> final | `7` | `1648.462 ms` | `1637.176 ms` | `30.404 ms` | `1693.290 ms` | From the RPC action start to the first final-complete milestone. |
| Open target view -> first preview | `7` | `92.081 ms` | `148.015 ms` | `78.810 ms` | `158.468 ms` | From the explicit target-view request to the first fast-preview milestone. |
| Open target view -> final | `7` | `683.049 ms` | `681.502 ms` | `13.528 ms` | `701.987 ms` | From the explicit target-view request to the first final-complete milestone. |
| Open target view preview -> final | `7` | `590.969 ms` | `548.182 ms` | `79.824 ms` | `683.485 ms` | Derived preview-to-final settle interval for the intended target view. |
| Open post-load -> first preview | `7` | `23.437 ms` | `22.548 ms` | `3.673 ms` | `28.212 ms` | Pure render latency after `dataset_load_done`. |
| Open post-load -> final | `7` | `561.395 ms` | `545.427 ms` | `26.309 ms` | `598.305 ms` | Pure final-render latency after `dataset_load_done`. |
| Rotate preview step duration | `210` | `28.560 ms` | `25.768 ms` | `8.274 ms` | `44.998 ms` | Derived from per-step preview client timing. |
| Rotate step 1 preview duration | `7` | `42.590 ms` | `42.239 ms` | `11.524 ms` | `58.488 ms` | First preview step across measured runs. |
| Rotate step 30 preview duration | `7` | `25.144 ms` | `24.947 ms` | `1.960 ms` | `27.338 ms` | Last preview step across measured runs. |
| Rotate preview service FPS | `210` | `35.014` | n/a | `n/a (derived)` | n/a | Computed as `1000 / mean_preview_ms`. |
| Rotate preview -> final after step 30 | `7` | `133.723 ms` | `132.685 ms` | `3.155 ms` | `138.581 ms` | Step-30 preview-to-final settle interval, aligned to the ParaView release -> first still metric. |
| Zoom preview step duration | `210` | `28.768 ms` | `27.406 ms` | `6.066 ms` | `40.283 ms` | Derived from per-step preview client timing. |
| Zoom step 1 preview duration | `7` | `24.658 ms` | `24.728 ms` | `1.482 ms` | `26.645 ms` | First preview step across measured runs. |
| Zoom step 30 preview duration | `7` | `28.821 ms` | `28.393 ms` | `2.846 ms` | `32.921 ms` | Last preview step across measured runs. |
| Zoom preview service FPS | `210` | `34.760` | n/a | `n/a (derived)` | n/a | Computed as `1000 / mean_preview_ms`. |
| Zoom preview -> final after step 30 | `7` | `654.791 ms` | `657.555 ms` | `23.364 ms` | `680.266 ms` | Step-30 preview-to-final settle interval, aligned to the ParaView release -> first still metric. |
| Peak RSS | `7` | `3944781531 bytes` (`3.674 GiB`) | `3955355648 bytes` (`3.683 GiB`) | `577681606 bytes` | `4680753562 bytes` (`4.360 GiB`) | Aggregate memory summary across measured runs. |
| Full-run wall time | `7` | `123.584 s` | `123.582 s` | `0.043 s` | `123.637 s` | Entire deterministic script duration per measured run. |

## Session 5: Atlas Dense Input, DVR

Artifacts:
- Root: `/Users/feng/Dropbox/atlas_test/slice15_paraview/benchmarks/atlas_deterministic_interactive_plus_final_2000x1500_dvr_v2_parity`
- Aggregate summary: `/Users/feng/Dropbox/atlas_test/slice15_paraview/benchmarks/atlas_deterministic_interactive_plus_final_2000x1500_dvr_v2_parity/aggregate/summary.json`

### Setup

| Item | Value |
| --- | --- |
| Software | Atlas (local build from this repository) |
| Rendering mode | Full-resolution image rendering enabled |
| Compositing mode | `Direct Volume Rendering` |
| Deterministic mode | `interactive-plus-final` with a fixed `2.0 s` hold between camera commands |
| Dataset | `/Users/feng/Dropbox/atlas_test/slice15_paraview/slice15_ch2_dense.nim` |
| Dataset format | Dense `.nim` |
| Dataset size | `9216 x 6144 x 98`, single channel |
| Dataset spacing | `0.10378322750329971 x 0.10378322750329971 x 0.5238174200057983 um` |
| Benchmark cleanup flags | `--hide-background --hide-axis --hide-bound-box` |
| Effective scene-space Z/X ratio | `5.0472259521484375` |

### Warm-up

| Metric | Warm-up value |
| --- | --- |
| Open total -> first preview | `1234.253 ms` |
| Open total -> final | `1736.764 ms` |
| Open target view -> first preview | `167.677 ms` |
| Open target view -> final | `670.188 ms` |
| Open target view preview -> final | `502.511 ms` |
| Open post-load -> first preview | `24.547 ms` |
| Open post-load -> final | `527.058 ms` |
| Rotate preview step mean | `30.372 ms` |
| Rotate step 1 preview | `57.902 ms` |
| Rotate step 30 preview | `28.522 ms` |
| Rotate step 30 preview -> final | `129.477 ms` |
| Zoom preview step mean | `33.495 ms` |
| Zoom step 1 preview | `26.017 ms` |
| Zoom step 30 preview | `29.204 ms` |
| Zoom step 30 preview -> final | `675.849 ms` |
| Peak RSS | `5018193920 bytes` (`4.674 GiB`) |

### Measured Steady State

| Metric | Count | Mean | Median | Std | p95 | Notes |
| --- | --- | --- | --- | --- | --- | --- |
| Open total -> first preview | `7` | `1120.548 ms` | `1061.567 ms` | `114.698 ms` | `1288.056 ms` | From the RPC action start to the first fast-preview milestone for the intended target view. |
| Open total -> final | `7` | `1787.644 ms` | `1815.910 ms` | `56.277 ms` | `1843.592 ms` | From the RPC action start to the first final-complete milestone. |
| Open target view -> first preview | `7` | `56.895 ms` | `8.414 ms` | `86.207 ms` | `188.486 ms` | From the explicit target-view request to the first fast-preview milestone. |
| Open target view -> final | `7` | `723.991 ms` | `713.939 ms` | `34.153 ms` | `770.370 ms` | From the explicit target-view request to the first final-complete milestone. |
| Open target view preview -> final | `7` | `667.095 ms` | `703.044 ms` | `86.693 ms` | `755.173 ms` | Derived preview-to-final settle interval for the intended target view. |
| Open post-load -> first preview | `5` | `30.165 ms` | `24.080 ms` | `11.173 ms` | `45.610 ms` | Two runs rendered the preview a few milliseconds before `dataset_load_done`, so use the target-view row above for the stable cross-run metric. |
| Open post-load -> final | `7` | `578.395 ms` | `596.993 ms` | `54.318 ms` | `630.090 ms` | Pure final-render latency after `dataset_load_done`. |
| Rotate preview step duration | `210` | `30.466 ms` | `28.318 ms` | `6.824 ms` | `46.511 ms` | Derived from per-step preview client timing. |
| Rotate step 1 preview duration | `7` | `30.384 ms` | `28.222 ms` | `6.296 ms` | `40.182 ms` | First preview step across measured runs. |
| Rotate step 30 preview duration | `7` | `28.593 ms` | `26.624 ms` | `3.857 ms` | `34.644 ms` | Last preview step across measured runs. |
| Rotate preview service FPS | `210` | `32.823` | n/a | `n/a (derived)` | n/a | Computed as `1000 / mean_preview_ms`. |
| Rotate preview -> final after step 30 | `7` | `136.012 ms` | `134.910 ms` | `2.949 ms` | `139.886 ms` | Step-30 preview-to-final settle interval, aligned to the ParaView release -> first still metric. |
| Zoom preview step duration | `210` | `31.925 ms` | `29.960 ms` | `7.598 ms` | `48.521 ms` | Derived from per-step preview client timing. |
| Zoom step 1 preview duration | `7` | `36.837 ms` | `29.154 ms` | `14.705 ms` | `59.539 ms` | First preview step across measured runs. |
| Zoom step 30 preview duration | `7` | `31.211 ms` | `30.214 ms` | `2.509 ms` | `35.123 ms` | Last preview step across measured runs. |
| Zoom preview service FPS | `210` | `31.323` | n/a | `n/a (derived)` | n/a | Computed as `1000 / mean_preview_ms`. |
| Zoom preview -> final after step 30 | `7` | `676.605 ms` | `676.460 ms` | `7.786 ms` | `686.108 ms` | Step-30 preview-to-final settle interval, aligned to the ParaView release -> first still metric. |
| Peak RSS | `7` | `6029028791 bytes` (`5.616 GiB`) | `6059520000 bytes` (`5.643 GiB`) | `580044944 bytes` | `6759393280 bytes` (`6.295 GiB`) | Aggregate memory summary across measured runs. |
| Full-run wall time | `7` | `123.711 s` | `123.667 s` | `0.344 s` | `124.228 s` | Entire deterministic script duration per measured run. |

# Deterministic Benchmark Sessions: `slice15_ch2_gpufit_1024x1024x980`

This section summarizes the final deterministic benchmark sessions for the GPU-fit
`slice15_ch2_gpufit_1024x1024x980` dataset. The raw artifacts under each benchmark root
remain the authoritative source.

## Shared Methodology

| Item | Value |
| --- | --- |
| Source scene | `/Users/feng/Downloads/test_gpufit.scene` |
| Camera spec | `/Users/feng/code/atlas/large_test_image/slice15_ch2_gpufit_scene_camera_exact_2000x1500.json` |
| Output render size | `2000 x 1500` physical pixels |
| Atlas live 3D canvas size | `1000 x 750` logical Qt pixels on Retina, yielding about `2000 x 1500` physical pixels |
| Deterministic action sequence | `open`, `rotate`, `zoom` |
| Interpolated actions | `30` requested camera steps per action |
| Warm-up / measured runs | `1` warm-up run, `7` measured runs |
| ParaView dense scene-space header | `/Users/feng/code/atlas/large_test_image/slice15_ch2_gpufit_1024x1024x980_scenespace.mhd` (`1 x 1 x 1`) |
| Atlas dataset | `/Users/feng/code/atlas/large_test_image/slice15_ch2_gpufit_1024x1024x980_iso0p1um.nim` (`0.1 x 0.1 x 0.1 um`) |
| Notes | Atlas uses its default current behavior for this GPU-fit dataset. Preview and final are the same render pass here, so the Atlas preview-to-final settle metric is `0`. |

## Cross-Session Snapshot

| Session | Input | Render mode | Open first preview | Open final | Rotate preview | Rotate release/final | Zoom preview | Zoom release/final | Peak RSS | Full-run wall |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| ParaView GPU MIP | Dense `.mhd/.zraw` | `GPU Based` + `maximum-intensity` | n/a | `7397.294 ms` | `23.329 ms` | `49.768 ms` | `34.770 ms` | `111.964 ms` | `2.264 GiB` | `24.014 s` |
| ParaView GPU Composite | Dense `.mhd/.zraw` | `GPU Based` + `composite` | n/a | `7525.040 ms` | `23.646 ms` | `48.560 ms` | `36.542 ms` | `111.142 ms` | `2.265 GiB` | `24.297 s` |
| ParaView OSPRay Composite | Dense `.mhd/.zraw` | `OSPRay Based` + `composite` | n/a | `9112.088 ms` | `355.945 ms` | `333.210 ms` | `799.372 ms` | `1184.178 ms` | `1.975 GiB` | `60.586 s` |
| Atlas MIP | Dense `.nim` | `Maximum Intensity Projection` | `8119.536 ms` | `8119.561 ms` | `46.233 ms` | `0.000 ms` | `67.763 ms` | `0.000 ms` | `3.345 GiB` | `130.595 s` |
| Atlas DVR | Dense `.nim` | `Direct Volume Rendering` | `8289.513 ms` | `8289.545 ms` | `52.739 ms` | `0.000 ms` | `88.085 ms` | `0.000 ms` | `3.392 GiB` | `130.750 s` |

## Session 6: ParaView GPU, Dense Input, MIP

Artifacts:
- Root: `/Users/feng/code/atlas/large_test_image/benchmarks/paraview_gpufit_deterministic_interactive_plus_final_2000x1500_mip_v1`
- Aggregate summary: `/Users/feng/code/atlas/large_test_image/benchmarks/paraview_gpufit_deterministic_interactive_plus_final_2000x1500_mip_v1/aggregate/summary.json`

### Setup

| Item | Value |
| --- | --- |
| Software | ParaView 6.1.0-RC1 |
| Representation mode | `GPU Based` |
| Blend mode | `maximum-intensity` |
| Deterministic mode | `interactive-plus-final` |
| Dataset | `/Users/feng/code/atlas/large_test_image/slice15_ch2_gpufit_1024x1024x980_scenespace.mhd` |
| Dataset format | Dense `.mhd/.zraw` |
| Dataset size | `1024 x 1024 x 980`, single channel |
| Dataset spacing | `1 x 1 x 1` scene-space units |
| Scalar array | `MetaImage`, component `0` |
| Color ramp | black -> red over `0 .. 255` |

### Warm-up

| Metric | Warm-up value |
| --- | --- |
| Open final complete from action start | `7494.275 ms` |
| Open still-render total | `1127.115 ms` |
| Rotate preview frame mean | `23.662 ms` |
| Rotate first preview complete | `219.881 ms` |
| Rotate release -> first still | `49.088 ms` |
| Rotate final complete from action start | `1082.868 ms` |
| Zoom preview frame mean | `34.089 ms` |
| Zoom first preview complete | `230.760 ms` |
| Zoom release -> first still | `105.689 ms` |
| Zoom final complete from action start | `1467.739 ms` |
| Peak RSS | `2422341632 bytes` (`2.256 GiB`) |

### Measured Steady State

| Metric | Count | Mean | Median | Std | p95 | Notes |
| --- | --- | --- | --- | --- | --- | --- |
| Open final complete from action start | `7` | `7397.294` | `7318.632` | `167.345` | `7654.004` | First and final render are the same for `open` in this run shape. |
| Open still-render total | `7` | `1150.068` | `1137.274` | `45.929` | `1215.809` | Dense final render cost after the dataset is in place. |
| Rotate preview frame duration | `210` | `23.329` | `23.576` | `3.258` | `27.807` | Pooled `Interactive Render` frames. |
| Rotate frame 1 preview duration | `7` | `15.630` | `15.947` | `1.764` | `17.875` | First preview frame across measured runs. |
| Rotate frame 30 preview duration | `7` | `26.476` | `26.025` | `1.235` | `28.226` | Last preview frame across measured runs. |
| Rotate preview service FPS | `210` | `42.864` | n/a | `n/a (derived)` | n/a | Computed as `1000 / mean_preview_ms`. |
| Rotate release -> first still | `7` | `49.768` | `49.338` | `1.841` | `52.330` | Final-view latency after the last interactive step. |
| Rotate final complete from action start | `7` | `1061.450` | `1061.210` | `19.685` | `1085.680` | End-to-end action duration to the first final still. |
| Zoom preview frame duration | `210` | `34.770` | `31.345` | `7.639` | `50.680` | Pooled `Interactive Render` frames. |
| Zoom frame 1 preview duration | `7` | `27.628` | `27.783` | `1.058` | `29.022` | First preview frame across measured runs. |
| Zoom frame 30 preview duration | `7` | `30.563` | `30.487` | `1.121` | `32.170` | Last preview frame across measured runs. |
| Zoom preview service FPS | `210` | `28.760` | n/a | `n/a (derived)` | n/a | Computed as `1000 / mean_preview_ms`. |
| Zoom release -> first still | `7` | `111.964` | `110.546` | `5.537` | `120.457` | Final-view latency after the last interactive step. |
| Zoom final complete from action start | `7` | `1480.766` | `1490.678` | `18.696` | `1498.492` | End-to-end action duration to the first final still. |
| Peak RSS | `7` | `2431461083 bytes` (`2.264 GiB`) | `2429849600 bytes` (`2.263 GiB`) | `3796972 bytes` | `2436527718 bytes` (`2.269 GiB`) | Aggregate memory summary across measured runs. |
| Full-run wall time | `7` | `24.014 s` | `23.876 s` | `0.324 s` | `24.528 s` | Entire deterministic script duration per measured run. |

## Session 7: ParaView GPU, Dense Input, Composite

Artifacts:
- Root: `/Users/feng/code/atlas/large_test_image/benchmarks/paraview_gpufit_deterministic_interactive_plus_final_2000x1500_composite_v1`
- Aggregate summary: `/Users/feng/code/atlas/large_test_image/benchmarks/paraview_gpufit_deterministic_interactive_plus_final_2000x1500_composite_v1/aggregate/summary.json`

### Setup

| Item | Value |
| --- | --- |
| Software | ParaView 6.1.0-RC1 |
| Representation mode | `GPU Based` |
| Blend mode | `composite` |
| Deterministic mode | `interactive-plus-final` |
| Dataset | `/Users/feng/code/atlas/large_test_image/slice15_ch2_gpufit_1024x1024x980_scenespace.mhd` |
| Dataset format | Dense `.mhd/.zraw` |
| Dataset size | `1024 x 1024 x 980`, single channel |
| Dataset spacing | `1 x 1 x 1` scene-space units |
| Scalar array | `MetaImage`, component `0` |
| Color ramp | black -> red over `0 .. 255` |

### Warm-up

| Metric | Warm-up value |
| --- | --- |
| Open final complete from action start | `7656.411 ms` |
| Open still-render total | `1155.082 ms` |
| Rotate preview frame mean | `23.985 ms` |
| Rotate first preview complete | `221.165 ms` |
| Rotate release -> first still | `44.799 ms` |
| Rotate final complete from action start | `1077.061 ms` |
| Zoom preview frame mean | `36.710 ms` |
| Zoom first preview complete | `229.174 ms` |
| Zoom release -> first still | `107.711 ms` |
| Zoom final complete from action start | `1521.454 ms` |
| Peak RSS | `2428215296 bytes` (`2.261 GiB`) |

### Measured Steady State

| Metric | Count | Mean | Median | Std | p95 | Notes |
| --- | --- | --- | --- | --- | --- | --- |
| Open final complete from action start | `7` | `7525.040` | `7455.747` | `213.321` | `7827.829` | First and final render are the same for `open` in this run shape. |
| Open still-render total | `7` | `1161.084` | `1140.819` | `55.536` | `1243.881` | Dense final render cost after the dataset is in place. |
| Rotate preview frame duration | `210` | `23.646` | `23.915` | `2.039` | `26.657` | Pooled `Interactive Render` frames. |
| Rotate frame 1 preview duration | `7` | `16.855` | `16.027` | `1.448` | `18.762` | First preview frame across measured runs. |
| Rotate frame 30 preview duration | `7` | `24.417` | `24.287` | `0.810` | `25.650` | Last preview frame across measured runs. |
| Rotate preview service FPS | `210` | `42.290` | n/a | `n/a (derived)` | n/a | Computed as `1000 / mean_preview_ms`. |
| Rotate release -> first still | `7` | `48.560` | `45.414` | `8.790` | `61.719` | Final-view latency after the last interactive step. |
| Rotate final complete from action start | `7` | `1063.845` | `1066.775` | `10.290` | `1074.245` | End-to-end action duration to the first final still. |
| Zoom preview frame duration | `210` | `36.542` | `34.544` | `7.351` | `51.790` | Pooled `Interactive Render` frames. |
| Zoom frame 1 preview duration | `7` | `26.396` | `26.659` | `1.151` | `27.697` | First preview frame across measured runs. |
| Zoom frame 30 preview duration | `7` | `35.677` | `35.643` | `0.727` | `36.571` | Last preview frame across measured runs. |
| Zoom preview service FPS | `210` | `27.366` | n/a | `n/a (derived)` | n/a | Computed as `1000 / mean_preview_ms`. |
| Zoom release -> first still | `7` | `111.142` | `106.961` | `10.052` | `125.781` | Final-view latency after the last interactive step. |
| Zoom final complete from action start | `7` | `1543.943` | `1550.509` | `15.819` | `1561.381` | End-to-end action duration to the first final still. |
| Peak RSS | `7` | `2432157989 bytes` (`2.265 GiB`) | `2430984192 bytes` (`2.264 GiB`) | `3231434 bytes` | `2436914381 bytes` (`2.270 GiB`) | Aggregate memory summary across measured runs. |
| Full-run wall time | `7` | `24.297 s` | `24.284 s` | `0.437 s` | `24.879 s` | Entire deterministic script duration per measured run. |

## Session 8: ParaView OSPRay, Dense Input, Composite

Artifacts:
- Root: `/Users/feng/code/atlas/large_test_image/benchmarks/paraview_gpufit_ospray_deterministic_interactive_plus_final_2000x1500_composite_v1`
- Aggregate summary: `/Users/feng/code/atlas/large_test_image/benchmarks/paraview_gpufit_ospray_deterministic_interactive_plus_final_2000x1500_composite_v1/aggregate/summary.json`

### Setup

| Item | Value |
| --- | --- |
| Software | ParaView 6.1.0-RC1 |
| Launch path | `/Users/feng/code/atlas/util/benchmark/launch_paraview_with_ospray_fix.sh` |
| View ray tracing | Disabled (`EnableRayTracing = 0`) |
| Representation mode | `OSPRay Based` |
| Blend mode | `composite` |
| Deterministic mode | `interactive-plus-final` |
| Dataset | `/Users/feng/code/atlas/large_test_image/slice15_ch2_gpufit_1024x1024x980_scenespace.mhd` |
| Dataset format | Dense `.mhd/.zraw` |
| Dataset size | `1024 x 1024 x 980`, single channel |
| Dataset spacing | `1 x 1 x 1` scene-space units |
| Scalar array | `MetaImage`, component `0` |
| Color ramp | black -> red over `0 .. 255` |

### Warm-up

| Metric | Warm-up value |
| --- | --- |
| Open final complete from action start | `8882.783 ms` |
| Open still-render total | `2697.679 ms` |
| Rotate preview frame mean | `351.210 ms` |
| Rotate first preview complete | `584.034 ms` |
| Rotate release -> first still | `318.325 ms` |
| Rotate final complete from action start | `11269.458 ms` |
| Zoom preview frame mean | `737.100 ms` |
| Zoom first preview complete | `600.902 ms` |
| Zoom release -> first still | `1273.644 ms` |
| Zoom final complete from action start | `23796.913 ms` |
| Peak RSS | `2120073216 bytes` (`1.974 GiB`) |

### Measured Steady State

| Metric | Count | Mean | Median | Std | p95 | Notes |
| --- | --- | --- | --- | --- | --- | --- |
| Open final complete from action start | `7` | `9112.088` | `9155.612` | `85.372` | `9190.666` | First and final render are the same for `open` in this run shape. |
| Open still-render total | `7` | `2757.072` | `2753.656` | `62.497` | `2843.420` | Dense final render cost after the dataset is in place. |
| Rotate preview frame duration | `210` | `355.945` | `348.057` | `26.856` | `401.779` | Pooled `Interactive Render` frames. |
| Rotate frame 1 preview duration | `7` | `388.810` | `386.326` | `9.743` | `403.731` | First preview frame across measured runs. |
| Rotate frame 30 preview duration | `7` | `352.588` | `337.158` | `35.430` | `399.859` | Last preview frame across measured runs. |
| Rotate preview service FPS | `210` | `2.809` | n/a | `n/a (derived)` | n/a | Computed as `1000 / mean_preview_ms`. |
| Rotate release -> first still | `7` | `333.210` | `337.055` | `15.845` | `351.524` | Final-view latency after the last interactive step. |
| Rotate final complete from action start | `7` | `11456.414` | `11484.756` | `290.636` | `11814.256` | End-to-end action duration to the first final still. |
| Zoom preview frame duration | `210` | `799.372` | `762.076` | `319.217` | `1226.099` | Pooled `Interactive Render` frames. |
| Zoom frame 1 preview duration | `7` | `397.410` | `397.921` | `20.127` | `423.550` | First preview frame across measured runs. |
| Zoom frame 30 preview duration | `7` | `1204.961` | `1204.944` | `35.680` | `1247.785` | Last preview frame across measured runs. |
| Zoom preview service FPS | `210` | `1.251` | n/a | `n/a (derived)` | n/a | Computed as `1000 / mean_preview_ms`. |
| Zoom release -> first still | `7` | `1184.178` | `1171.457` | `37.258` | `1229.461` | Final-view latency after the last interactive step. |
| Zoom final complete from action start | `7` | `25609.516` | `25418.818` | `911.620` | `26837.079` | End-to-end action duration to the first final still. |
| Peak RSS | `7` | `2120881883 bytes` (`1.975 GiB`) | `2119872512 bytes` (`1.974 GiB`) | `3501327 bytes` | `2125121536 bytes` (`1.979 GiB`) | Aggregate memory summary across measured runs. |
| Full-run wall time | `7` | `60.586 s` | `60.670 s` | `1.214 s` | `62.221 s` | Entire deterministic script duration per measured run. |

## Session 9: Atlas Dense Input, MIP

Artifacts:
- Root: `/Users/feng/code/atlas/large_test_image/benchmarks/atlas_gpufit_deterministic_interactive_plus_final_2000x1500_mip_v2_clean`
- Aggregate summary: `/Users/feng/code/atlas/large_test_image/benchmarks/atlas_gpufit_deterministic_interactive_plus_final_2000x1500_mip_v2_clean/aggregate/summary.json`

### Setup

| Item | Value |
| --- | --- |
| Software | Atlas (local build from this repository) |
| Rendering mode | Default Atlas behavior for this dataset |
| Compositing mode | `Maximum Intensity Projection` |
| Deterministic mode | `interactive-plus-final` with a fixed `2.0 s` hold between camera commands |
| Dataset | `/Users/feng/code/atlas/large_test_image/slice15_ch2_gpufit_1024x1024x980_iso0p1um.nim` |
| Dataset format | Dense `.nim` |
| Dataset size | `1024 x 1024 x 980`, single channel |
| Dataset spacing | `0.1 x 0.1 x 0.1 um` |
| Benchmark cleanup flags | `--hide-background --hide-axis --hide-bound-box` |
| Effective scene-space Z/X ratio | `1.0` |
| Notes | Preview and final are the same render pass on this dataset, so preview-to-final settle metrics are `0`. |

### Warm-up

| Metric | Warm-up value |
| --- | --- |
| Open total -> first preview | `8068.996 ms` |
| Open total -> final | `8069.013 ms` |
| Open target view -> first preview | `30.206 ms` |
| Open target view -> final | `30.223 ms` |
| Open target view preview -> final | `0.000 ms` |
| Open post-load -> first preview | `47.272 ms` |
| Open post-load -> final | `47.296 ms` |
| Rotate preview step mean | `45.688 ms` |
| Rotate step 1 preview | `41.984 ms` |
| Rotate step 30 preview | `48.148 ms` |
| Rotate step 30 preview -> final | `0.000 ms` |
| Zoom preview step mean | `69.253 ms` |
| Zoom step 1 preview | `52.846 ms` |
| Zoom step 30 preview | `76.034 ms` |
| Zoom step 30 preview -> final | `0.000 ms` |
| Peak RSS | `3519929344 bytes` (`3.278 GiB`) |

### Measured Steady State

| Metric | Count | Mean | Median | Std | p95 | Notes |
| --- | --- | --- | --- | --- | --- | --- |
| Open total -> first preview | `7` | `8119.536` | `8127.462` | `31.668` | `8152.646` | Client-observed open latency to the first preview marker. |
| Open total -> final | `7` | `8119.561` | `8127.500` | `31.673` | `8152.671` | Client-observed open latency to the first final marker. |
| Open target view -> first preview | `7` | `30.445` | `30.639` | `1.507` | `32.287` | Latency after the intended benchmark camera/state was requested. |
| Open target view -> final | `7` | `30.469` | `30.666` | `1.510` | `32.313` | Final-view latency after the intended benchmark camera/state was requested. |
| Open target view preview -> final | `7` | `0.000` | `0.000` | `0.000` | `0.000` | Preview and final are the same render pass for this dataset. |
| Rotate preview step duration | `210` | `46.233` | `46.107` | `3.504` | `51.595` | Per-step preview completion over all measured rotate steps. |
| Rotate step 1 preview duration | `7` | `42.047` | `42.599` | `2.411` | `44.219` | First requested rotate step across measured runs. |
| Rotate step 30 preview duration | `7` | `50.982` | `51.313` | `1.630` | `52.501` | Last requested rotate step across measured runs. |
| Rotate preview service FPS | `210` | `21.630` | n/a | `n/a (derived)` | n/a | Computed as `1000 / mean_preview_ms`. |
| Rotate step 30 preview -> final duration | `7` | `0.000` | `0.000` | `0.000` | `0.000` | Preview and final are the same render pass for this dataset. |
| Zoom preview step duration | `210` | `67.763` | `74.389` | `18.758` | `83.248` | Per-step preview completion over all measured zoom steps. |
| Zoom step 1 preview duration | `7` | `51.546` | `51.755` | `2.221` | `54.756` | First requested zoom step across measured runs. |
| Zoom step 30 preview duration | `7` | `78.655` | `78.743` | `1.321` | `80.640` | Last requested zoom step across measured runs. |
| Zoom preview service FPS | `210` | `14.757` | n/a | `n/a (derived)` | n/a | Computed as `1000 / mean_preview_ms`. |
| Zoom step 30 preview -> final duration | `7` | `0.000` | `0.000` | `0.000` | `0.000` | Preview and final are the same render pass for this dataset. |
| Peak RSS | `7` | `3591521426 bytes` (`3.345 GiB`) | `3597942784 bytes` (`3.351 GiB`) | `30341648 bytes` | `3635706675 bytes` (`3.386 GiB`) | Aggregate memory summary across measured runs. |
| Full-run wall time | `7` | `130.595 s` | `130.462 s` | `0.274 s` | `131.056 s` | Entire deterministic script duration per measured run. |

## Session 10: Atlas Dense Input, DVR

Artifacts:
- Root: `/Users/feng/code/atlas/large_test_image/benchmarks/atlas_gpufit_deterministic_interactive_plus_final_2000x1500_dvr_v1_clean`
- Aggregate summary: `/Users/feng/code/atlas/large_test_image/benchmarks/atlas_gpufit_deterministic_interactive_plus_final_2000x1500_dvr_v1_clean/aggregate/summary.json`

### Setup

| Item | Value |
| --- | --- |
| Software | Atlas (local build from this repository) |
| Rendering mode | Default Atlas behavior for this dataset |
| Compositing mode | `Direct Volume Rendering` |
| Deterministic mode | `interactive-plus-final` with a fixed `2.0 s` hold between camera commands |
| Dataset | `/Users/feng/code/atlas/large_test_image/slice15_ch2_gpufit_1024x1024x980_iso0p1um.nim` |
| Dataset format | Dense `.nim` |
| Dataset size | `1024 x 1024 x 980`, single channel |
| Dataset spacing | `0.1 x 0.1 x 0.1 um` |
| Benchmark cleanup flags | `--hide-background --hide-axis --hide-bound-box` |
| Effective scene-space Z/X ratio | `1.0` |
| Notes | Preview and final are the same render pass on this dataset, so preview-to-final settle metrics are `0`. |

### Warm-up

| Metric | Warm-up value |
| --- | --- |
| Open total -> first preview | `8207.686 ms` |
| Open total -> final | `8207.711 ms` |
| Open target view -> first preview | `36.225 ms` |
| Open target view -> final | `36.250 ms` |
| Open target view preview -> final | `0.000 ms` |
| Open post-load -> first preview | `47.550 ms` |
| Open post-load -> final | `47.585 ms` |
| Rotate preview step mean | `52.556 ms` |
| Rotate step 1 preview | `47.494 ms` |
| Rotate step 30 preview | `55.087 ms` |
| Rotate step 30 preview -> final | `0.000 ms` |
| Zoom preview step mean | `86.487 ms` |
| Zoom step 1 preview | `52.009 ms` |
| Zoom step 30 preview | `117.398 ms` |
| Zoom step 30 preview -> final | `0.000 ms` |
| Peak RSS | `3553859584 bytes` (`3.310 GiB`) |

### Measured Steady State

| Metric | Count | Mean | Median | Std | p95 | Notes |
| --- | --- | --- | --- | --- | --- | --- |
| Open total -> first preview | `7` | `8289.513` | `8295.485` | `28.286` | `8319.588` | Client-observed open latency to the first preview marker. |
| Open total -> final | `7` | `8289.545` | `8295.520` | `28.287` | `8319.619` | Client-observed open latency to the first final marker. |
| Open target view -> first preview | `7` | `46.260` | `45.665` | `1.305` | `48.468` | Latency after the intended benchmark camera/state was requested. |
| Open target view -> final | `7` | `46.291` | `45.697` | `1.311` | `48.496` | Final-view latency after the intended benchmark camera/state was requested. |
| Open target view preview -> final | `7` | `0.000` | `0.000` | `0.000` | `0.000` | Preview and final are the same render pass for this dataset. |
| Rotate preview step duration | `210` | `52.739` | `52.452` | `3.503` | `56.955` | Per-step preview completion over all measured rotate steps. |
| Rotate step 1 preview duration | `7` | `48.473` | `48.010` | `2.577` | `50.444` | First requested rotate step across measured runs. |
| Rotate step 30 preview duration | `7` | `53.501` | `53.434` | `1.560` | `55.624` | Last requested rotate step across measured runs. |
| Rotate preview service FPS | `210` | `18.961` | n/a | `n/a (derived)` | n/a | Computed as `1000 / mean_preview_ms`. |
| Rotate step 30 preview -> final duration | `7` | `0.000` | `0.000` | `0.000` | `0.000` | Preview and final are the same render pass for this dataset. |
| Zoom preview step duration | `210` | `88.085` | `111.370` | `37.641` | `121.002` | Per-step preview completion over all measured zoom steps. |
| Zoom step 1 preview duration | `7` | `54.845` | `54.769` | `1.578` | `56.877` | First requested zoom step across measured runs. |
| Zoom step 30 preview duration | `7` | `118.520` | `118.028` | `2.780` | `122.189` | Last requested zoom step across measured runs. |
| Zoom preview service FPS | `210` | `11.353` | n/a | `n/a (derived)` | n/a | Computed as `1000 / mean_preview_ms`. |
| Zoom step 30 preview -> final duration | `7` | `0.000` | `0.000` | `0.000` | `0.000` | Preview and final are the same render pass for this dataset. |
| Peak RSS | `7` | `3642284910 bytes` (`3.392 GiB`) | `3650013184 bytes` (`3.399 GiB`) | `24131928 bytes` | `3672610483 bytes` (`3.421 GiB`) | Aggregate memory summary across measured runs. |
| Full-run wall time | `7` | `130.750 s` | `130.795 s` | `0.355 s` | `131.145 s` | Entire deterministic script duration per measured run. |

# Deterministic Benchmark Sessions: `slice15_ch2_x2z`

This section summarizes the final deterministic benchmark sessions for the doubled-depth
`slice15_ch2_x2z` dataset. The raw artifacts under each benchmark root remain the
authoritative source.

## Shared Methodology

| Item | Value |
| --- | --- |
| Source scene | `/Users/feng/Downloads/test_slice15_ch2_2x.scene` |
| Camera spec | `/Users/feng/code/atlas/large_test_image/slice15_ch2_x2z_scene_camera_exact_2000x1500_v2.json` |
| Output render size | `2000 x 1500` physical pixels |
| Atlas live 3D canvas size | `1000 x 750` logical Qt pixels on Retina, yielding about `2000 x 1500` physical pixels |
| Deterministic action sequence | `open`, `rotate`, `zoom` |
| Interpolated actions | `30` requested camera steps per action |
| Warm-up / measured runs | `1` warm-up run, `7` measured runs |
| ParaView dense scene-space header | `/Users/feng/code/atlas/large_test_image/slice15_ch2_x2z_scenespace.mhd` (`1 x 1 x 5.0472259521484375`) |
| ParaView blocked scene-space dataset | `/Users/feng/code/atlas/large_test_image/slice15_ch2_x2z_grid_atlasscenespace.vtpd` (`1 x 1 x 5.0472259521484375`) |
| Atlas dataset | `/Users/feng/code/atlas/large_test_image/slice15_ch2_x2z.nim` (`0.10378322750329971 x 0.10378322750329971 x 0.52381742000579834 um`) |
| Notes | ParaView GPU sessions use the blocked scene-space `.vtpd` export because the dense GPU smoke run kept allocating toward system memory and was not practical. ParaView `OSPRay Based` uses the dense scene-space `.mhd/.zraw` path because blocked OSPRay input renders blank in this ParaView build. |

## Cross-Session Snapshot

For this snapshot, ParaView uses `release -> first still` for the settle metric, while
Atlas uses the last-step `preview -> final` settle interval.

| Session | Input | Render mode | Open first preview | Open final | Rotate preview | Rotate release/final | Zoom preview | Zoom release/final | Peak RSS | Full-run wall |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| ParaView GPU MIP | Blocked `.vtpd` | `GPU Based` + `maximum-intensity` | n/a | `26649.038 ms` | `2358.539 ms` | `2565.902 ms` | `2079.501 ms` | `1811.688 ms` | `21.035 GiB` | `181.118 s` |
| ParaView GPU Composite | Blocked `.vtpd` | `GPU Based` + `composite` | n/a | `26693.039 ms` | `2362.393 ms` | `2553.504 ms` | `2108.934 ms` | `1937.154 ms` | `21.035 GiB` | `182.395 s` |
| ParaView OSPRay Composite | Dense `.mhd/.zraw` | `OSPRay Based` + `composite` | n/a | `82665.091 ms` | `3196.924 ms` | `3581.389 ms` | `4782.539 ms` | `5029.514 ms` | `13.730 GiB` | `348.358 s` |
| Atlas MIP | Dense `.nim` | `Maximum Intensity Projection` | `3322.114 ms` | `3643.508 ms` | `37.006 ms` | `113.781 ms` | `38.463 ms` | `1127.208 ms` | `8.564 GiB` | `121.948 s` |
| Atlas DVR | Dense `.nim` | `Direct Volume Rendering` | `3357.746 ms` | `3593.807 ms` | `39.085 ms` | `124.365 ms` | `43.987 ms` | `1171.357 ms` | `9.374 GiB` | `121.901 s` |

## Session 11: ParaView GPU, Blocked Input, MIP

Artifacts:
- Root: `/Users/feng/code/atlas/large_test_image/benchmarks/paraview_x2z_deterministic_interactive_plus_final_2000x1500_mip_v1`
- Aggregate summary: `/Users/feng/code/atlas/large_test_image/benchmarks/paraview_x2z_deterministic_interactive_plus_final_2000x1500_mip_v1/aggregate/summary.json`

### Setup

| Item | Value |
| --- | --- |
| Software | ParaView 6.1.0-RC1 |
| Representation mode | `GPU Based` |
| Blend mode | `maximum-intensity` |
| Deterministic mode | `interactive-plus-final` |
| Dataset | `/Users/feng/code/atlas/large_test_image/slice15_ch2_x2z_grid_atlasscenespace.vtpd` |
| Dataset format | Blocked `.vtpd` |
| Dataset size | `9216 x 6144 x 196`, single channel |
| Dataset spacing | `1 x 1 x 5.0472259521484375` scene-space units |
| Scalar array | `channels`, component `0` |
| Color ramp | black -> red over `0 .. 255` |

### Warm-up

| Metric | Warm-up value |
| --- | --- |
| Open final complete from action start | `26719.774 ms` |
| Open still-render total | `12664.219 ms` |
| Rotate preview frame mean | `2351.840 ms` |
| Rotate first preview complete | `2901.991 ms` |
| Rotate release -> first still | `2542.947 ms` |
| Rotate final complete from action start | `73522.154 ms` |
| Zoom preview frame mean | `2076.030 ms` |
| Zoom first preview complete | `2818.430 ms` |
| Zoom release -> first still | `1834.261 ms` |
| Zoom final complete from action start | `64590.836 ms` |
| Peak RSS | `22582480896 bytes` (`21.032 GiB`) |

### Measured Steady State

| Metric | Count | Mean | Median | Std | p95 | Notes |
| --- | --- | --- | --- | --- | --- | --- |
| Open final complete from action start | `7` | `26649.038` | `26618.090` | `169.793` | `26859.649` | First and final render are the same for `open` in this run shape. |
| Open still-render total | `7` | `12938.515` | `12919.084` | `153.493` | `13159.819` | Blocked final render cost after the dataset is in place. |
| Rotate preview frame duration | `210` | `2358.539` | `2356.757` | `112.487` | `2478.312` | Pooled `Interactive Render` frames. |
| Rotate frame 1 preview duration | `7` | `2721.626` | `2729.996` | `35.047` | `2760.107` | First preview frame across measured runs. |
| Rotate frame 30 preview duration | `7` | `2401.024` | `2401.250` | `7.291` | `2410.166` | Last preview frame across measured runs. |
| Rotate preview service FPS | `210` | `0.424` | n/a | `n/a (derived)` | n/a | Computed as `1000 / mean_preview_ms`. |
| Rotate release -> first still | `7` | `2565.902` | `2560.593` | `15.396` | `2588.249` | Final-view latency after the last interactive step. |
| Rotate final complete from action start | `7` | `73758.524` | `73766.688` | `101.695` | `73889.993` | End-to-end action duration to the first final still. |
| Zoom preview frame duration | `210` | `2079.501` | `2087.081` | `214.351` | `2426.899` | Pooled `Interactive Render` frames. |
| Zoom frame 1 preview duration | `7` | `2625.627` | `2620.847` | `28.013` | `2667.603` | First preview frame across measured runs. |
| Zoom frame 30 preview duration | `7` | `1773.365` | `1769.835` | `6.675` | `1784.055` | Last preview frame across measured runs. |
| Zoom preview service FPS | `210` | `0.481` | n/a | `n/a (derived)` | n/a | Computed as `1000 / mean_preview_ms`. |
| Zoom release -> first still | `7` | `1811.688` | `1809.565` | `16.262` | `1836.645` | Final-view latency after the last interactive step. |
| Zoom final complete from action start | `7` | `64632.985` | `64606.988` | `85.156` | `64737.080` | End-to-end action duration to the first final still. |
| Peak RSS | `7` | `22585819721 bytes` (`21.035 GiB`) | `22584086528 bytes` (`21.033 GiB`) | `3993845 bytes` | `22591339315 bytes` (`21.040 GiB`) | Aggregate memory summary across measured runs. |
| Full-run wall time | `7` | `181.118 s` | `181.120 s` | `0.264 s` | `181.466 s` | Entire deterministic script duration per measured run. |

## Session 12: ParaView GPU, Blocked Input, Composite

Artifacts:
- Root: `/Users/feng/code/atlas/large_test_image/benchmarks/paraview_x2z_deterministic_interactive_plus_final_2000x1500_composite_v1`
- Aggregate summary: `/Users/feng/code/atlas/large_test_image/benchmarks/paraview_x2z_deterministic_interactive_plus_final_2000x1500_composite_v1/aggregate/summary.json`

### Setup

| Item | Value |
| --- | --- |
| Software | ParaView 6.1.0-RC1 |
| Representation mode | `GPU Based` |
| Blend mode | `composite` |
| Deterministic mode | `interactive-plus-final` |
| Dataset | `/Users/feng/code/atlas/large_test_image/slice15_ch2_x2z_grid_atlasscenespace.vtpd` |
| Dataset format | Blocked `.vtpd` |
| Dataset size | `9216 x 6144 x 196`, single channel |
| Dataset spacing | `1 x 1 x 5.0472259521484375` scene-space units |
| Scalar array | `channels`, component `0` |
| Color ramp | black -> red over `0 .. 255` |

### Warm-up

| Metric | Warm-up value |
| --- | --- |
| Open final complete from action start | `26521.045 ms` |
| Open still-render total | `12741.269 ms` |
| Rotate preview frame mean | `2368.523 ms` |
| Rotate first preview complete | `2955.198 ms` |
| Rotate release -> first still | `2566.749 ms` |
| Rotate final complete from action start | `74054.162 ms` |
| Zoom preview frame mean | `2108.154 ms` |
| Zoom first preview complete | `2833.235 ms` |
| Zoom release -> first still | `1938.867 ms` |
| Zoom final complete from action start | `65613.427 ms` |
| Peak RSS | `22590259200 bytes` (`21.039 GiB`) |

### Measured Steady State

| Metric | Count | Mean | Median | Std | p95 | Notes |
| --- | --- | --- | --- | --- | --- | --- |
| Open final complete from action start | `7` | `26693.039` | `26663.732` | `69.973` | `26794.542` | First and final render are the same for `open` in this run shape. |
| Open still-render total | `7` | `12802.965` | `12786.549` | `146.031` | `13004.357` | Blocked final render cost after the dataset is in place. |
| Rotate preview frame duration | `210` | `2362.393` | `2364.618` | `117.137` | `2472.596` | Pooled `Interactive Render` frames. |
| Rotate frame 1 preview duration | `7` | `2766.145` | `2760.020` | `24.856` | `2795.526` | First preview frame across measured runs. |
| Rotate frame 30 preview duration | `7` | `2399.230` | `2404.419` | `10.563` | `2410.081` | Last preview frame across measured runs. |
| Rotate preview service FPS | `210` | `0.423` | n/a | `n/a (derived)` | n/a | Computed as `1000 / mean_preview_ms`. |
| Rotate release -> first still | `7` | `2553.504` | `2553.452` | `22.626` | `2582.849` | Final-view latency after the last interactive step. |
| Rotate final complete from action start | `7` | `73867.320` | `73895.437` | `84.238` | `73969.788` | End-to-end action duration to the first final still. |
| Zoom preview frame duration | `210` | `2108.934` | `2108.293` | `209.364` | `2456.557` | Pooled `Interactive Render` frames. |
| Zoom frame 1 preview duration | `7` | `2647.693` | `2647.319` | `12.925` | `2666.868` | First preview frame across measured runs. |
| Zoom frame 30 preview duration | `7` | `1788.724` | `1790.753` | `5.467` | `1793.375` | Last preview frame across measured runs. |
| Zoom preview service FPS | `210` | `0.474` | n/a | `n/a (derived)` | n/a | Computed as `1000 / mean_preview_ms`. |
| Zoom release -> first still | `7` | `1937.154` | `1933.340` | `16.641` | `1959.532` | Final-view latency after the last interactive step. |
| Zoom final complete from action start | `7` | `65634.206` | `65650.952` | `82.384` | `65729.928` | End-to-end action duration to the first final still. |
| Peak RSS | `7` | `22586001701 bytes` (`21.035 GiB`) | `22586335232 bytes` (`21.036 GiB`) | `2616124 bytes` | `22589043917 bytes` (`21.038 GiB`) | Aggregate memory summary across measured runs. |
| Full-run wall time | `7` | `182.395 s` | `182.432 s` | `0.147 s` | `182.535 s` | Entire deterministic script duration per measured run. |

## Session 13: ParaView OSPRay, Dense Input, Composite

Artifacts:
- Root: `/Users/feng/code/atlas/large_test_image/benchmarks/paraview_x2z_ospray_deterministic_interactive_plus_final_2000x1500_composite_v1`
- Aggregate summary: `/Users/feng/code/atlas/large_test_image/benchmarks/paraview_x2z_ospray_deterministic_interactive_plus_final_2000x1500_composite_v1/aggregate/summary.json`

### Setup

| Item | Value |
| --- | --- |
| Software | ParaView 6.1.0-RC1 |
| Launch path | `/Users/feng/code/atlas/util/benchmark/launch_paraview_with_ospray_fix.sh` |
| View ray tracing | Disabled (`EnableRayTracing = 0`) |
| Representation mode | `OSPRay Based` |
| Blend mode | `composite` |
| Deterministic mode | `interactive-plus-final` |
| Dataset | `/Users/feng/code/atlas/large_test_image/slice15_ch2_x2z_scenespace.mhd` |
| Dataset format | Dense `.mhd/.zraw` |
| Dataset size | `9216 x 6144 x 196`, single channel |
| Dataset spacing | `1 x 1 x 5.0472259521484375` scene-space units |
| Scalar array | `MetaImage`, component `0` |
| Color ramp | black -> red over `0 .. 255` |
| Notes | These macOS OSPRay runs exited with `SIGSEGV` after writing complete artifacts; the batch accepted them because `session_end` was `ok` and all expected outputs were present. |

### Warm-up

| Metric | Warm-up value |
| --- | --- |
| Open final complete from action start | `80998.738 ms` |
| Open still-render total | `25370.720 ms` |
| Rotate preview frame mean | `2787.157 ms` |
| Rotate first preview complete | `2660.361 ms` |
| Rotate release -> first still | `3193.102 ms` |
| Rotate final complete from action start | `87496.225 ms` |
| Zoom preview frame mean | `4131.503 ms` |
| Zoom first preview complete | `3640.816 ms` |
| Zoom release -> first still | `4708.675 ms` |
| Zoom final complete from action start | `129368.041 ms` |
| Peak RSS | `14730981376 bytes` (`13.719 GiB`) |

### Measured Steady State

| Metric | Count | Mean | Median | Std | p95 | Notes |
| --- | --- | --- | --- | --- | --- | --- |
| Open final complete from action start | `7` | `82665.091` | `82796.452` | `1295.953` | `84309.358` | First and final render are the same for `open` in this run shape. |
| Open still-render total | `7` | `27017.632` | `27107.226` | `692.710` | `27569.995` | Dense final render cost after the dataset is in place. |
| Rotate preview frame duration | `210` | `3196.924` | `3242.572` | `370.880` | `3726.070` | Pooled `Interactive Render` frames. |
| Rotate frame 1 preview duration | `7` | `2559.952` | `2523.300` | `149.654` | `2773.388` | First preview frame across measured runs. |
| Rotate frame 30 preview duration | `7` | `3591.772` | `3576.740` | `142.815` | `3733.278` | Last preview frame across measured runs. |
| Rotate preview service FPS | `210` | `0.313` | n/a | `n/a (derived)` | n/a | Computed as `1000 / mean_preview_ms`. |
| Rotate release -> first still | `7` | `3581.389` | `3608.436` | `209.256` | `3768.365` | Final-view latency after the last interactive step. |
| Rotate final complete from action start | `7` | `100153.472` | `101823.384` | `4748.610` | `102965.784` | End-to-end action duration to the first final still. |
| Zoom preview frame duration | `210` | `4782.539` | `4999.179` | `565.683` | `5377.263` | Pooled `Interactive Render` frames. |
| Zoom frame 1 preview duration | `7` | `3510.437` | `3510.325` | `111.356` | `3663.055` | First preview frame across measured runs. |
| Zoom frame 30 preview duration | `7` | `5035.696` | `5056.314` | `148.026` | `5179.414` | Last preview frame across measured runs. |
| Zoom preview service FPS | `210` | `0.209` | n/a | `n/a (derived)` | n/a | Computed as `1000 / mean_preview_ms`. |
| Zoom release -> first still | `7` | `5029.514` | `5060.279` | `154.306` | `5175.664` | Final-view latency after the last interactive step. |
| Zoom final complete from action start | `7` | `149189.963` | `151331.732` | `4260.233` | `152211.241` | End-to-end action duration to the first final still. |
| Peak RSS | `7` | `14742307401 bytes` (`13.730 GiB`) | `14743097344 bytes` (`13.730 GiB`) | `2433188 bytes` | `14744906957 bytes` (`13.732 GiB`) | Aggregate memory summary across measured runs. |
| Full-run wall time | `7` | `348.358 s` | `351.317 s` | `8.359 s` | `354.015 s` | Entire deterministic script duration per measured run. |

## Session 14: Atlas Dense Input, MIP

Artifacts:
- Root: `/Users/feng/code/atlas/large_test_image/benchmarks/atlas_x2z_deterministic_interactive_plus_final_2000x1500_mip_v1`
- Aggregate summary: `/Users/feng/code/atlas/large_test_image/benchmarks/atlas_x2z_deterministic_interactive_plus_final_2000x1500_mip_v1/aggregate/summary.json`

### Setup

| Item | Value |
| --- | --- |
| Software | Atlas (local build from this repository) |
| Rendering mode | Default Atlas behavior for this dataset |
| Compositing mode | `Maximum Intensity Projection` |
| Deterministic mode | `interactive-plus-final` with log-driven preview/final waits from Atlas benchmark markers |
| Dataset | `/Users/feng/code/atlas/large_test_image/slice15_ch2_x2z.nim` |
| Dataset format | Dense `.nim` |
| Dataset size | `9216 x 6144 x 196`, single channel |
| Dataset spacing | `0.10378322750329971 x 0.10378322750329971 x 0.52381742000579834 um` |
| Benchmark cleanup flags | `--hide-background --hide-axis --hide-bound-box` |
| Effective scene-space Z/X ratio | `5.0472259521484375` |

### Warm-up

| Metric | Warm-up value |
| --- | --- |
| Open total -> first preview | `3417.543 ms` |
| Open total -> final | `3819.736 ms` |
| Open target view -> first preview | `16.498 ms` |
| Open target view -> final | `418.691 ms` |
| Open target view preview -> final | `402.193 ms` |
| Rotate preview step mean | `35.312 ms` |
| Rotate step 1 preview | `36.235 ms` |
| Rotate step 30 preview | `35.035 ms` |
| Rotate step 30 preview -> final | `102.563 ms` |
| Zoom preview step mean | `37.684 ms` |
| Zoom step 1 preview | `30.702 ms` |
| Zoom step 30 preview | `38.701 ms` |
| Zoom step 30 preview -> final | `5395.752 ms` |
| Peak RSS | `8643321856 bytes` (`8.050 GiB`) |

### Measured Steady State

| Metric | Count | Mean | Median | Std | p95 | Notes |
| --- | --- | --- | --- | --- | --- | --- |
| Open total -> first preview | `7` | `3322.114` | `3355.671` | `191.528` | `3543.089` | Client-observed open latency to the first preview marker. |
| Open total -> final | `7` | `3643.508` | `3633.893` | `195.671` | `3875.067` | Client-observed open latency to the first final marker. |
| Open target view -> first preview | `7` | `13.075` | `13.125` | `7.660` | `23.879` | Latency after the intended benchmark camera/state was requested. |
| Open target view -> final | `7` | `334.469` | `333.277` | `15.870` | `350.519` | Final-view latency after the intended benchmark camera/state was requested. |
| Open target view preview -> final | `7` | `321.394` | `317.332` | `23.272` | `344.353` | Settle interval from the intended target-view preview to the first final marker. |
| Rotate preview step duration | `210` | `37.006` | `36.729` | `1.804` | `39.129` | Per-step preview completion over all measured rotate steps. |
| Rotate step 1 preview duration | `7` | `36.276` | `36.448` | `1.515` | `37.910` | First requested rotate step across measured runs. |
| Rotate step 30 preview duration | `7` | `36.888` | `37.330` | `2.122` | `39.277` | Last requested rotate step across measured runs. |
| Rotate preview service FPS | `210` | `27.022` | n/a | `n/a (derived)` | n/a | Computed as `1000 / mean_preview_ms`. |
| Rotate step 30 preview -> final duration | `7` | `113.781` | `114.374` | `3.729` | `117.973` | Last-step final-view latency after the last rotate preview step. |
| Zoom preview step duration | `210` | `38.463` | `38.170` | `4.031` | `40.952` | Per-step preview completion over all measured zoom steps. |
| Zoom step 1 preview duration | `7` | `37.339` | `37.936` | `2.142` | `39.598` | First requested zoom step across measured runs. |
| Zoom step 30 preview duration | `7` | `38.118` | `38.327` | `2.430` | `40.990` | Last requested zoom step across measured runs. |
| Zoom preview service FPS | `210` | `26.000` | n/a | `n/a (derived)` | n/a | Computed as `1000 / mean_preview_ms`. |
| Zoom step 30 preview -> final duration | `7` | `1127.208` | `1124.854` | `10.731` | `1143.559` | Last-step final-view latency after the last zoom preview step. |
| Peak RSS | `7` | `9195952421 bytes` (`8.564 GiB`) | `9235755008 bytes` (`8.601 GiB`) | `231517789 bytes` | `9471777997 bytes` (`8.821 GiB`) | Aggregate memory summary across measured runs. |
| Full-run wall time | `7` | `121.948 s` | `121.896 s` | `0.253 s` | `122.261 s` | Entire deterministic script duration per measured run. |

## Session 15: Atlas Dense Input, DVR

Artifacts:
- Root: `/Users/feng/code/atlas/large_test_image/benchmarks/atlas_x2z_deterministic_interactive_plus_final_2000x1500_dvr_v1`
- Aggregate summary: `/Users/feng/code/atlas/large_test_image/benchmarks/atlas_x2z_deterministic_interactive_plus_final_2000x1500_dvr_v1/aggregate/summary.json`

### Setup

| Item | Value |
| --- | --- |
| Software | Atlas (local build from this repository) |
| Rendering mode | Default Atlas behavior for this dataset |
| Compositing mode | `Direct Volume Rendering` |
| Deterministic mode | `interactive-plus-final` with log-driven preview/final waits from Atlas benchmark markers |
| Dataset | `/Users/feng/code/atlas/large_test_image/slice15_ch2_x2z.nim` |
| Dataset format | Dense `.nim` |
| Dataset size | `9216 x 6144 x 196`, single channel |
| Dataset spacing | `0.10378322750329971 x 0.10378322750329971 x 0.52381742000579834 um` |
| Benchmark cleanup flags | `--hide-background --hide-axis --hide-bound-box` |
| Effective scene-space Z/X ratio | `5.0472259521484375` |

### Warm-up

| Metric | Warm-up value |
| --- | --- |
| Open total -> first preview | `3164.535 ms` |
| Open total -> final | `3516.079 ms` |
| Open target view -> first preview | `8.666 ms` |
| Open target view -> final | `360.210 ms` |
| Open target view preview -> final | `351.544 ms` |
| Rotate preview step mean | `39.500 ms` |
| Rotate step 1 preview | `36.863 ms` |
| Rotate step 30 preview | `36.859 ms` |
| Rotate step 30 preview -> final | `123.319 ms` |
| Zoom preview step mean | `47.011 ms` |
| Zoom step 1 preview | `39.972 ms` |
| Zoom step 30 preview | `45.951 ms` |
| Zoom step 30 preview -> final | `1181.550 ms` |
| Peak RSS | `9643327488 bytes` (`8.981 GiB`) |

### Measured Steady State

| Metric | Count | Mean | Median | Std | p95 | Notes |
| --- | --- | --- | --- | --- | --- | --- |
| Open total -> first preview | `7` | `3357.746` | `3405.316` | `122.993` | `3446.181` | Client-observed open latency to the first preview marker. |
| Open total -> final | `7` | `3593.807` | `3607.924` | `70.558` | `3652.342` | Client-observed open latency to the first final marker. |
| Open target view -> first preview | `7` | `120.825` | `162.711` | `76.636` | `172.825` | Latency after the intended benchmark camera/state was requested. |
| Open target view -> final | `7` | `356.886` | `354.720` | `11.123` | `372.902` | Final-view latency after the intended benchmark camera/state was requested. |
| Open target view preview -> final | `7` | `236.061` | `191.977` | `74.417` | `347.831` | Settle interval from the intended target-view preview to the first final marker. |
| Rotate preview step duration | `210` | `39.085` | `39.258` | `1.649` | `41.846` | Per-step preview completion over all measured rotate steps. |
| Rotate step 1 preview duration | `7` | `39.057` | `39.169` | `0.791` | `39.726` | First requested rotate step across measured runs. |
| Rotate step 30 preview duration | `7` | `38.522` | `38.911` | `1.079` | `39.671` | Last requested rotate step across measured runs. |
| Rotate preview service FPS | `210` | `25.586` | n/a | `n/a (derived)` | n/a | Computed as `1000 / mean_preview_ms`. |
| Rotate step 30 preview -> final duration | `7` | `124.365` | `123.881` | `1.193` | `126.237` | Last-step final-view latency after the last rotate preview step. |
| Zoom preview step duration | `210` | `43.987` | `43.769` | `4.952` | `48.453` | Per-step preview completion over all measured zoom steps. |
| Zoom step 1 preview duration | `7` | `39.356` | `39.686` | `1.394` | `40.618` | First requested zoom step across measured runs. |
| Zoom step 30 preview duration | `7` | `46.206` | `45.999` | `1.708` | `48.201` | Last requested zoom step across measured runs. |
| Zoom preview service FPS | `210` | `22.734` | n/a | `n/a (derived)` | n/a | Computed as `1000 / mean_preview_ms`. |
| Zoom step 30 preview -> final duration | `7` | `1171.357` | `1168.754` | `8.792` | `1183.155` | Last-step final-view latency after the last zoom preview step. |
| Peak RSS | `7` | `10064986697 bytes` (`9.374 GiB`) | `10030759936 bytes` (`9.342 GiB`) | `216531612 bytes` | `10348531712 bytes` (`9.638 GiB`) | Aggregate memory summary across measured runs. |
| Full-run wall time | `7` | `121.901 s` | `121.904 s` | `0.089 s` | `122.002 s` | Entire deterministic script duration per measured run. |

# Deterministic Benchmark Sessions: `high_res_20220219_stitched_all_spacing_0p1_0p1_2_um`

This section summarizes the final retained deterministic benchmark sessions for the
`high_res_20220219_stitched_all_spacing_0p1_0p1_2_um` dataset. On this machine,
the retained benchmark set is Atlas-only. ParaView GPU and ParaView OSPRay do not
have retained sessions for this dataset here; the reasons are documented in
`Additional Experiment Notes`.

## Shared Setup

| Item | Value |
| --- | --- |
| Camera spec | `/Users/feng/code/atlas/large_test_image/high_res_scene_camera_exact_2000x1500.json` |
| Viewport convention | `2000 x 1500` physical pixels (`1000 x 750` logical Retina canvas in Atlas) |
| Action sequence | `open`, then `rotate`, then `zoom` |
| Rotate action | `0.5 s`, `30` interpolated steps, followed by settle |
| Zoom action | `0.5 s`, `30` interpolated steps, followed by settle |
| Deterministic method | Internal Atlas benchmark markers plus live Atlas render-log parsing |
| Benchmark cleanup flags | `--hide-background --hide-axis --hide-bound-box` |
| ParaView retained sessions | None on this machine for this dataset |
| Atlas dataset | `/Users/feng/code/atlas/large_test_image/high_res_20220219_stitched_all_spacing_0p1_0p1_2_um.nim` |
| Dataset format | Dense `.nim` |
| Dataset size | `25395 x 19459 x 169`, single channel |
| Dataset spacing | `0.1 x 0.1 x 2.0 um` |
| Reference blocked ParaView export | `/Users/feng/code/atlas/large_test_image/high_res_20220219_stitched_all_spacing_0p1_0p1_2_um.vtpd` (`500` blocks) |

For the Atlas rows below:
- `Open total -> first preview` and `Open total -> final` are measured from action start.
- `Rotate/Zoom release/final` uses the last-step `preview -> final` settle interval.

## Cross-Session Snapshot

| Session | Input | Compositing | Open first preview | Open final | Rotate preview | Rotate release/final | Zoom preview | Zoom release/final | Peak RSS |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Atlas MIP | Dense `.nim` | `Maximum Intensity Projection` | `4071.009 ms` | `4420.345 ms` | `35.940 ms` | `112.519 ms` | `41.128 ms` | `1032.147 ms` | `10.290 GiB` |
| Atlas DVR | Dense `.nim` | `Direct Volume Rendering` | `4465.727 ms` | `4832.279 ms` | `37.062 ms` | `122.712 ms` | `45.618 ms` | `1071.328 ms` | `16.238 GiB` |

## Session 16: Atlas Dense Input, MIP

Artifacts:
- Root: `/Users/feng/code/atlas/large_test_image/benchmarks/atlas_high_res_deterministic_interactive_plus_final_2000x1500_mip_v1`
- Aggregate summary: `/Users/feng/code/atlas/large_test_image/benchmarks/atlas_high_res_deterministic_interactive_plus_final_2000x1500_mip_v1/aggregate/summary.json`

### Setup

| Item | Value |
| --- | --- |
| Software | Atlas (local build from this repository) |
| Rendering mode | Default Atlas behavior for this dataset |
| Compositing mode | `Maximum Intensity Projection` |
| Deterministic mode | `interactive-plus-final` with log-driven preview/final waits from Atlas benchmark markers |
| Dataset | `/Users/feng/code/atlas/large_test_image/high_res_20220219_stitched_all_spacing_0p1_0p1_2_um.nim` |
| Dataset format | Dense `.nim` |
| Dataset size | `25395 x 19459 x 169`, single channel |
| Dataset spacing | `0.1 x 0.1 x 2.0 um` |
| Benchmark cleanup flags | `--hide-background --hide-axis --hide-bound-box` |

### Warm-up

| Metric | Warm-up value |
| --- | --- |
| Open total -> first preview | `3986.488 ms` |
| Open total -> final | `4477.006 ms` |
| Open target view -> first preview | `5.939 ms` |
| Open target view -> final | `496.457 ms` |
| Open target view preview -> final | `490.518 ms` |
| Rotate preview step mean | `35.681 ms` |
| Rotate step 1 preview | `33.144 ms` |
| Rotate step 30 preview | `37.752 ms` |
| Rotate step 30 preview -> final | `112.865 ms` |
| Zoom preview step mean | `38.380 ms` |
| Zoom step 1 preview | `37.104 ms` |
| Zoom step 30 preview | `38.324 ms` |
| Zoom step 30 preview -> final | `1361.982 ms` |
| Peak RSS | `7754129408 bytes` (`7.222 GiB`) |

### Measured Steady State

| Metric | Count | Mean | Median | Std | p95 | Notes |
| --- | --- | --- | --- | --- | --- | --- |
| Open total -> first preview | `7` | `4071.009` | `4078.505` | `77.033` | `4149.301` | Client-observed open latency to the first preview marker. |
| Open total -> final | `7` | `4420.345` | `4424.883` | `77.043` | `4496.342` | Client-observed open latency to the first final marker. |
| Open target view -> first preview | `7` | `12.997` | `15.038` | `3.878` | `15.889` | Latency after the intended benchmark camera/state was requested. |
| Open target view -> final | `7` | `362.333` | `360.188` | `5.440` | `369.123` | Final-view latency after the intended benchmark camera/state was requested. |
| Open target view preview -> final | `7` | `349.336` | `349.397` | `3.301` | `353.288` | Settle interval from the intended target-view preview to the first final marker. |
| Rotate preview step duration | `210` | `35.940` | `35.921` | `2.055` | `39.295` | Per-step preview completion over all measured rotate steps. |
| Rotate step 1 preview duration | `7` | `34.327` | `34.606` | `1.365` | `35.672` | First requested rotate step across measured runs. |
| Rotate step 30 preview duration | `7` | `39.272` | `39.632` | `1.207` | `40.311` | Last requested rotate step across measured runs. |
| Rotate preview service FPS | `210` | `27.824` | n/a | `n/a (derived)` | n/a | Computed as `1000 / mean_preview_ms`. |
| Rotate step 30 preview -> final duration | `7` | `112.519` | `111.238` | `2.431` | `115.413` | Last-step final-view latency after the last rotate preview step. |
| Zoom preview step duration | `210` | `41.128` | `40.424` | `5.804` | `43.945` | Per-step preview completion over all measured zoom steps. |
| Zoom step 1 preview duration | `7` | `38.878` | `39.129` | `1.737` | `40.722` | First requested zoom step across measured runs. |
| Zoom step 30 preview duration | `7` | `39.538` | `39.723` | `1.269` | `40.878` | Last requested zoom step across measured runs. |
| Zoom preview service FPS | `210` | `24.314` | n/a | `n/a (derived)` | n/a | Computed as `1000 / mean_preview_ms`. |
| Zoom step 30 preview -> final duration | `7` | `1032.147` | `1031.822` | `2.462` | `1034.731` | Last-step final-view latency after the last zoom preview step. |
| Peak RSS | `7` | `11048315173 bytes` (`10.290 GiB`) | `11055112192 bytes` (`10.296 GiB`) | `1734590189 bytes` | `13210313114 bytes` (`12.303 GiB`) | Aggregate memory summary across measured runs. |
| Full-run wall time | `7` | `122.787 s` | `122.788 s` | `0.101 s` | `122.900 s` | Entire deterministic script duration per measured run. |

## Session 17: Atlas Dense Input, DVR

Artifacts:
- Root: `/Users/feng/code/atlas/large_test_image/benchmarks/atlas_high_res_deterministic_interactive_plus_final_2000x1500_dvr_v1`
- Aggregate summary: `/Users/feng/code/atlas/large_test_image/benchmarks/atlas_high_res_deterministic_interactive_plus_final_2000x1500_dvr_v1/aggregate/summary.json`

### Setup

| Item | Value |
| --- | --- |
| Software | Atlas (local build from this repository) |
| Rendering mode | Default Atlas behavior for this dataset |
| Compositing mode | `Direct Volume Rendering` |
| Deterministic mode | `interactive-plus-final` with log-driven preview/final waits from Atlas benchmark markers |
| Dataset | `/Users/feng/code/atlas/large_test_image/high_res_20220219_stitched_all_spacing_0p1_0p1_2_um.nim` |
| Dataset format | Dense `.nim` |
| Dataset size | `25395 x 19459 x 169`, single channel |
| Dataset spacing | `0.1 x 0.1 x 2.0 um` |
| Benchmark cleanup flags | `--hide-background --hide-axis --hide-bound-box` |

### Warm-up

| Metric | Warm-up value |
| --- | --- |
| Open total -> first preview | `4200.538 ms` |
| Open total -> final | `4568.694 ms` |
| Open target view -> first preview | `17.725 ms` |
| Open target view -> final | `385.881 ms` |
| Open target view preview -> final | `368.156 ms` |
| Rotate preview step mean | `37.175 ms` |
| Rotate step 1 preview | `35.500 ms` |
| Rotate step 30 preview | `40.505 ms` |
| Rotate step 30 preview -> final | `123.036 ms` |
| Zoom preview step mean | `46.757 ms` |
| Zoom step 1 preview | `39.200 ms` |
| Zoom step 30 preview | `50.209 ms` |
| Zoom step 30 preview -> final | `1073.860 ms` |
| Peak RSS | `14240514048 bytes` (`13.263 GiB`) |

### Measured Steady State

| Metric | Count | Mean | Median | Std | p95 | Notes |
| --- | --- | --- | --- | --- | --- | --- |
| Open total -> first preview | `7` | `4465.727` | `4404.405` | `182.277` | `4684.670` | Client-observed open latency to the first preview marker. |
| Open total -> final | `7` | `4832.279` | `4774.965` | `180.578` | `5049.306` | Client-observed open latency to the first final marker. |
| Open target view -> first preview | `7` | `13.609` | `15.969` | `4.488` | `17.671` | Latency after the intended benchmark camera/state was requested. |
| Open target view -> final | `7` | `380.162` | `382.987` | `8.599` | `390.299` | Final-view latency after the intended benchmark camera/state was requested. |
| Open target view preview -> final | `7` | `366.552` | `365.209` | `5.367` | `373.675` | Settle interval from the intended target-view preview to the first final marker. |
| Rotate preview step duration | `210` | `37.062` | `36.511` | `4.937` | `40.778` | Per-step preview completion over all measured rotate steps. |
| Rotate step 1 preview duration | `7` | `34.996` | `35.425` | `1.083` | `36.206` | First requested rotate step across measured runs. |
| Rotate step 30 preview duration | `7` | `41.078` | `42.090` | `2.301` | `43.095` | Last requested rotate step across measured runs. |
| Rotate preview service FPS | `210` | `26.982` | n/a | `n/a (derived)` | n/a | Computed as `1000 / mean_preview_ms`. |
| Rotate step 30 preview -> final duration | `7` | `122.712` | `122.302` | `1.714` | `124.857` | Last-step final-view latency after the last rotate preview step. |
| Zoom preview step duration | `210` | `45.618` | `44.986` | `6.469` | `50.727` | Per-step preview completion over all measured zoom steps. |
| Zoom step 1 preview duration | `7` | `40.203` | `40.419` | `1.104` | `41.698` | First requested zoom step across measured runs. |
| Zoom step 30 preview duration | `7` | `49.849` | `49.020` | `2.847` | `53.808` | Last requested zoom step across measured runs. |
| Zoom preview service FPS | `210` | `21.921` | n/a | `n/a (derived)` | n/a | Computed as `1000 / mean_preview_ms`. |
| Zoom step 30 preview -> final duration | `7` | `1071.328` | `1069.903` | `6.132` | `1078.914` | Last-step final-view latency after the last zoom preview step. |
| Peak RSS | `7` | `17435769710 bytes` (`16.238 GiB`) | `17427472384 bytes` (`16.231 GiB`) | `1720481274 bytes` | `19588741939 bytes` (`18.243 GiB`) | Aggregate memory summary across measured runs. |
| Full-run wall time | `7` | `123.291 s` | `123.288 s` | `0.187 s` | `123.502 s` | Entire deterministic script duration per measured run. |

# Additional Experiment Notes

These exploratory runs were useful for planning and interpreting the retained benchmark
sessions, but they were not kept as final benchmark results.

| Dataset | Attempted path | Input | Observed behavior | Retained in final benchmark? | Notes |
| --- | --- | --- | --- | --- | --- |
| `slice15_ch2` | ParaView GPU, single-file dense input | Dense `.mhd/.zraw` (`slice15_ch2_dense_atlasscenespace.mhd`) | The rendered image was visibly wrong: the volume content was recognizable, but the image arrangement was corrupted. It was also much slower than the retained blocked `.vtpd` path. | No | Possible large-single-texture or driver issue on this system, but not confirmed. Final ParaView GPU results for `slice15_ch2` use the blocked scene-space dataset. |
| `slice15_ch2_x2z` | ParaView GPU, single-file dense input | Dense `.mhd/.zraw` (`slice15_ch2_x2z_scenespace.mhd`) | ParaView kept allocating toward and past system memory during load/render, so the run was killed instead of being allowed to continue. | No | Final ParaView GPU results for `slice15_ch2_x2z` use the blocked scene-space dataset [slice15_ch2_x2z_grid_atlasscenespace.vtpd](/Users/feng/code/atlas/large_test_image/slice15_ch2_x2z_grid_atlasscenespace.vtpd). |
| `high_res_20220219_stitched_all_spacing_0p1_0p1_2_um` | ParaView GPU | Prepared ParaView dataset | ParaView attempted to allocate well beyond host memory; the observed footprint exceeded about `100 GiB` on a machine with `64 GiB` RAM, so the run was killed. | No | This is currently treated as a non-viable ParaView GPU benchmark path for this dataset. |
| `high_res_20220219_stitched_all_spacing_0p1_0p1_2_um` | ParaView OSPRay | Skipped dense-input path | Not run. The dense MetaImage path would first allocate a full resident `vtkImageData` and then hand that resident scalar buffer to OSPRay, so it has the same fundamental host-memory requirement as the failed dense GPU path. For this dataset shape (`25395 x 19459 x 169`), that is about `77.78 GiB` even at `uint8`, before reader and renderer overhead. | No | Blocked `.vtpd` input is not a valid OSPRay volume path in this ParaView build. ParaView `vtkMetaImageReader::ExecuteDataWithInformation()` allocates the full output image and reads the full file into `data->GetScalarPointer()`, and `vtkOSPRayVolumeMapperNode` only accepts direct `vtkImageData` input and builds a `structuredRegular` OSPRay volume from that resident scalar array. |

# Real GUI Rotate Benchmarks: `slice15_ch2`

These sessions measure real on-screen interaction rather than internal timer events.
They use WindowServer capture plus injected mouse drag input and should be interpreted
as user-visible interaction benchmarks, not engine-only service-time benchmarks.

## Shared GUI Methodology

| Item | Value |
| --- | --- |
| Dataset focus | `slice15_ch2` |
| Capture backend | `ScreenCaptureKit` via `macos_window_capture_sckit.swift` |
| Input path | Quartz `CGEvent` drag injection via `macos_gui_drag_benchmark.py` |
| Capture cadence target | `60 Hz` |
| Capture crop | `600 x 600` window-relative region inside the render pane |
| Drag path | horizontal left-button drag from `45%` to `75%` of the calibrated input region width |
| Warm-up / measured runs | `1` warm-up run, `7` measured runs |
| ParaView GUI prep | Applies the benchmark camera, then recenters `CameraFocalPoint` and `CenterOfRotation` to the data center before each run so GUI rotation stays centered on the object |
| Atlas GUI prep | Loads the dataset once, applies the benchmark camera, then resets back to the `open` camera and waits for Atlas preview/final markers before each run |
| Retained render modes | ParaView GPU MIP and Atlas MIP |
| Notes | GUI metrics are capture-derived. `changed_samples_per_second` and `visible_fps_from_mean_interval` are based on observed changed frames in the capture stream, not app-internal render timers. |

## GUI Snapshot: Short Rotate (`0.5 s`)

Measured steady-state means for the retained centered `0.5 s` rotate sessions.

| Session | Input | Render mode | First visible | Changed samples during drag | Changed samples / second | Visible FPS | Stable after drag end | Capture Hz |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| ParaView GPU MIP | Blocked `.vtpd` | `GPU Based` + `maximum-intensity` | `56.684 ms` | `1.000` | `1.997` | n/a | `14.569 ms` | `55.292 Hz` |
| Atlas MIP | Dense `.nim` | `Maximum Intensity Projection` | `60.538 ms` | `22.571` | `45.069` | `48.873 fps` | `244.129 ms` | `55.024 Hz` |

## GUI Snapshot: Sustained Rotate (`2.0 s`)

Measured steady-state means for the retained centered `2.0 s` rotate sessions.

| Session | Input | Render mode | First visible | Changed samples during drag | Changed samples / second | Visible FPS | Stable after drag end | Capture Hz |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| ParaView GPU MIP | Blocked `.vtpd` | `GPU Based` + `maximum-intensity` | `52.816 ms` | `4.429` | `2.206` | `1.902 fps` | `36.192 ms` | `53.264 Hz` |
| Atlas MIP | Dense `.nim` | `Maximum Intensity Projection` | `63.039 ms` | `90.286` | `44.936` | `45.735 fps` | `230.298 ms` | `53.382 Hz` |

## GUI Session 1: ParaView GPU MIP, Centered Rotate, `0.5 s`

Artifacts:
- Root: `/Users/feng/Dropbox/atlas_test/slice15_paraview/benchmarks/paraview_gui_rotate_slice15_ch2_gpu_mip_2000x1500_v2_centered`
- Aggregate summary: `/Users/feng/Dropbox/atlas_test/slice15_paraview/benchmarks/paraview_gui_rotate_slice15_ch2_gpu_mip_2000x1500_v2_centered/aggregate/summary.json`

### Setup

| Item | Value |
| --- | --- |
| Software | ParaView 6.1.0-RC1 |
| Dataset | `/Users/feng/Dropbox/atlas_test/slice15_paraview/slice15_ch2_grid_atlasscenespace.vtpd` |
| Dataset format | Blocked `.vtpd` with `.vti` pieces |
| Dataset size | `9216 x 6144 x 98`, single channel |
| Dataset spacing | `1 x 1 x 5.0472259521484375` scene-space units |
| Render mode | `GPU Based` + `maximum-intensity` |
| Camera / rotation center | Benchmark camera from `slice15_scene_camera_exact_2000x1500.json`, then recentered to the data bounds center for GUI rotation parity |
| Drag duration / steps | `0.5 s`, `30` injected drag samples |

### Warm-up

| Metric | Warm-up value |
| --- | --- |
| Changed samples during drag | `1` |
| Changed samples / second | `1.995` |
| Visible FPS from mean interval | n/a |
| First visible from drag start | `59.997 ms` |
| Stable from drag start | `522.999 ms` |
| Stable from drag end | `12.272 ms` |
| Drag duration | `501.371 ms` |
| Observed capture Hz | `56.763 Hz` |

### Measured Steady State

| Metric | Count | Mean | Median | Std | p95 | Notes |
| --- | --- | --- | --- | --- | --- | --- |
| Changed samples during drag | `7` | `1.000` | `1.000` | `0.000` | `1.000` | Exactly one visible changed frame was observed during each `0.5 s` drag. |
| Changed samples / second | `7` | `1.997` | `1.997` | `0.001` | `1.998` | Primary short-drag cadence metric for ParaView in this suite. |
| Visible FPS from mean interval | `0` | n/a | n/a | n/a | n/a | Undefined here because each run produced only one visible changed frame during the drag, so there is no interval series to average. |
| First visible from drag start | `7` | `56.684 ms` | `56.084 ms` | `14.681 ms` | `78.766 ms` | Time to the first visible captured change after drag motion begins. |
| Stable from drag start | `7` | `529.579 ms` | `528.353 ms` | `9.237 ms` | `544.008 ms` | Total time until the captured region reaches the stable state. |
| Stable from drag end | `7` | `14.569 ms` | `15.302 ms` | `5.362 ms` | `22.692 ms` | Post-release settle time in the capture stream. |
| Drag duration | `7` | `500.875 ms` | `500.755 ms` | `0.365 ms` | `501.461 ms` | Measured from injected `drag_start` to `drag_end`. |
| Observed capture Hz | `7` | `55.292 Hz` | `55.833 Hz` | `2.050 Hz` | `57.206 Hz` | Actual ScreenCaptureKit sample cadence during the runs. |

## GUI Session 2: Atlas MIP, Rotate, `0.5 s`

Artifacts:
- Root: `/Users/feng/Dropbox/atlas_test/slice15_paraview/benchmarks/atlas_gui_rotate_slice15_ch2_mip_2000x1500_v3_centered`
- Aggregate summary: `/Users/feng/Dropbox/atlas_test/slice15_paraview/benchmarks/atlas_gui_rotate_slice15_ch2_mip_2000x1500_v3_centered/aggregate/summary.json`

### Setup

| Item | Value |
| --- | --- |
| Software | Atlas build `v1.0.7-23-g43291dd4` |
| Dataset | `/Users/feng/Dropbox/atlas_test/slice15_paraview/slice15_ch2_dense.nim` |
| Dataset format | Dense `.nim` |
| Live 3D canvas | `1000 x 750` logical Qt pixels on Retina, yielding about `2000 x 1500` physical pixels |
| Render mode | `Maximum Intensity Projection` |
| View parity settings | Background hidden, axis hidden, bound box set to `No Bound Box`, full-resolution rendering enabled |
| Camera reset | Benchmark `open` camera from `slice15_scene_camera_exact_2000x1500.json`, reapplied before every run |
| Drag duration / steps | `0.5 s`, `30` injected drag samples |

### Warm-up

| Metric | Warm-up value |
| --- | --- |
| Changed samples during drag | `22` |
| Changed samples / second | `43.122` |
| Visible FPS from mean interval | `45.241 fps` |
| First visible from drag start | `53.928 ms` |
| Stable from drag start | `773.043 ms` |
| Stable from drag end | `253.015 ms` |
| Drag duration | `510.180 ms` |
| Observed capture Hz | `53.403 Hz` |

### Measured Steady State

| Metric | Count | Mean | Median | Std | p95 | Notes |
| --- | --- | --- | --- | --- | --- | --- |
| Changed samples during drag | `7` | `22.571` | `23.000` | `0.904` | `23.700` | Atlas shows many visible updates during the short drag, unlike ParaView's single-update behavior in the same suite. |
| Changed samples / second | `7` | `45.069` | `45.846` | `1.822` | `47.370` | Primary short-drag cadence metric for Atlas in this suite. |
| Visible FPS from mean interval | `7` | `48.873 fps` | `49.377 fps` | `1.633 fps` | `50.532 fps` | Interval-derived visible cadence from changed-frame timestamps. |
| First visible from drag start | `7` | `60.538 ms` | `60.910 ms` | `4.748 ms` | `67.307 ms` | Time to the first visible captured change after drag motion begins. |
| Stable from drag start | `7` | `757.202 ms` | `755.734 ms` | `137.289 ms` | `969.960 ms` | Total time until the captured region reaches the stable state. |
| Stable from drag end | `7` | `244.129 ms` | `245.021 ms` | `139.155 ms` | `458.794 ms` | Post-release settle time in the capture stream. |
| Drag duration | `7` | `500.830 ms` | `500.822 ms` | `0.475 ms` | `501.551 ms` | Measured from injected `drag_start` to `drag_end`. |
| Observed capture Hz | `7` | `55.024 Hz` | `55.795 Hz` | `2.064 Hz` | `56.856 Hz` | Actual ScreenCaptureKit sample cadence during the runs. |

## GUI Session 3: ParaView GPU MIP, Centered Rotate, `2.0 s`

Artifacts:
- Root: `/Users/feng/Dropbox/atlas_test/slice15_paraview/benchmarks/paraview_gui_rotate_slice15_ch2_gpu_mip_2000x1500_rotate2s_v5_centered`
- Aggregate summary: `/Users/feng/Dropbox/atlas_test/slice15_paraview/benchmarks/paraview_gui_rotate_slice15_ch2_gpu_mip_2000x1500_rotate2s_v5_centered/aggregate/summary.json`

### Setup

| Item | Value |
| --- | --- |
| Software | ParaView 6.1.0-RC1 |
| Dataset | `/Users/feng/Dropbox/atlas_test/slice15_paraview/slice15_ch2_grid_atlasscenespace.vtpd` |
| Dataset format | Blocked `.vtpd` with `.vti` pieces |
| Dataset size | `9216 x 6144 x 98`, single channel |
| Dataset spacing | `1 x 1 x 5.0472259521484375` scene-space units |
| Render mode | `GPU Based` + `maximum-intensity` |
| Camera / rotation center | Benchmark camera from `slice15_scene_camera_exact_2000x1500.json`, then recentered to the data bounds center for GUI rotation parity |
| Drag duration / steps | `2.0 s`, `120` injected drag samples |

### Warm-up

| Metric | Warm-up value |
| --- | --- |
| Changed samples during drag | `4` |
| Changed samples / second | `1.992` |
| Visible FPS from mean interval | `1.661 fps` |
| First visible from drag start | `55.696 ms` |
| Stable from drag start | `2035.817 ms` |
| Stable from drag end | `17.037 ms` |
| Drag duration | `2007.639 ms` |
| Observed capture Hz | `46.995 Hz` |

### Measured Steady State

| Metric | Count | Mean | Median | Std | p95 | Notes |
| --- | --- | --- | --- | --- | --- | --- |
| Changed samples during drag | `7` | `4.429` | `4.000` | `0.495` | `5.000` | Only about four to five visible changed frames were observed over the full `2 s` rotate drag. |
| Changed samples / second | `7` | `2.206` | `1.999` | `0.247` | `2.499` | Primary sustained-drag cadence metric for ParaView in this suite. |
| Visible FPS from mean interval | `7` | `1.902 fps` | `1.667 fps` | `0.280 fps` | `2.234 fps` | Interval-derived visible cadence from changed-frame timestamps. |
| First visible from drag start | `7` | `52.816 ms` | `55.192 ms` | `5.782 ms` | `58.877 ms` | Time to the first visible captured change after drag motion begins. |
| Stable from drag start | `7` | `2054.805 ms` | `2036.604 ms` | `49.332 ms` | `2137.079 ms` | Total time until the captured region reaches the stable state. |
| Stable from drag end | `7` | `36.192 ms` | `16.966 ms` | `51.739 ms` | `122.667 ms` | Post-release settle time in the capture stream. |
| Drag duration | `7` | `2007.286 ms` | `2005.756 ms` | `6.914 ms` | `2017.721 ms` | Measured from injected `drag_start` to `drag_end`. |
| Observed capture Hz | `7` | `53.264 Hz` | `53.501 Hz` | `2.657 Hz` | `56.831 Hz` | Actual ScreenCaptureKit sample cadence during the runs. |

## GUI Session 4: Atlas MIP, Rotate, `2.0 s`

Artifacts:
- Root: `/Users/feng/Dropbox/atlas_test/slice15_paraview/benchmarks/atlas_gui_rotate_slice15_ch2_mip_2000x1500_rotate2s_v1_centered`
- Aggregate summary: `/Users/feng/Dropbox/atlas_test/slice15_paraview/benchmarks/atlas_gui_rotate_slice15_ch2_mip_2000x1500_rotate2s_v1_centered/aggregate/summary.json`

### Setup

| Item | Value |
| --- | --- |
| Software | Atlas build `v1.0.7-23-g43291dd4` |
| Dataset | `/Users/feng/Dropbox/atlas_test/slice15_paraview/slice15_ch2_dense.nim` |
| Dataset format | Dense `.nim` |
| Live 3D canvas | `1000 x 750` logical Qt pixels on Retina, yielding about `2000 x 1500` physical pixels |
| Render mode | `Maximum Intensity Projection` |
| View parity settings | Background hidden, axis hidden, bound box set to `No Bound Box`, full-resolution rendering enabled |
| Camera reset | Benchmark `open` camera from `slice15_scene_camera_exact_2000x1500.json`, reapplied before every run |
| Drag duration / steps | `2.0 s`, `120` injected drag samples |

### Warm-up

| Metric | Warm-up value |
| --- | --- |
| Changed samples during drag | `87` |
| Changed samples / second | `43.491` |
| Visible FPS from mean interval | `43.998 fps` |
| First visible from drag start | `57.035 ms` |
| Stable from drag start | `2130.240 ms` |
| Stable from drag end | `117.814 ms` |
| Drag duration | `2000.407 ms` |
| Observed capture Hz | `50.668 Hz` |

### Measured Steady State

| Metric | Count | Mean | Median | Std | p95 | Notes |
| --- | --- | --- | --- | --- | --- | --- |
| Changed samples during drag | `7` | `90.286` | `91.000` | `1.979` | `92.700` | Atlas maintains visible updates through nearly the entire sustained drag window. |
| Changed samples / second | `7` | `44.936` | `45.144` | `0.874` | `46.016` | Primary sustained-drag cadence metric for Atlas in this suite. |
| Visible FPS from mean interval | `7` | `45.735 fps` | `45.691 fps` | `0.965 fps` | `46.752 fps` | Interval-derived visible cadence from changed-frame timestamps. |
| First visible from drag start | `7` | `63.039 ms` | `59.068 ms` | `11.824 ms` | `82.853 ms` | Time to the first visible captured change after drag motion begins. |
| Stable from drag start | `7` | `2259.730 ms` | `2249.729 ms` | `152.683 ms` | `2506.513 ms` | Total time until the captured region reaches the stable state. |
| Stable from drag end | `7` | `230.298 ms` | `230.778 ms` | `150.376 ms` | `470.055 ms` | Post-release settle time in the capture stream. |
| Drag duration | `7` | `2009.105 ms` | `2009.710 ms` | `5.663 ms` | `2016.304 ms` | Measured from injected `drag_start` to `drag_end`. |
| Observed capture Hz | `7` | `53.382 Hz` | `54.347 Hz` | `2.272 Hz` | `55.890 Hz` | Actual ScreenCaptureKit sample cadence during the runs. |
