# Deterministic Benchmark Sessions: `slice15_ch2`

This document summarizes the final deterministic benchmark sessions for the
`slice15_ch2` dataset. The raw artifacts under each benchmark root remain the
authoritative source. This file is the user-facing summary for the finished
session set.

## Shared Methodology

| Item | Value |
| --- | --- |
| Camera spec | `~/Dropbox/atlas_test/slice15_paraview/slice15_scene_camera_exact_2000x1500.json` |
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
| `largeimgmergeoutput_large_all_uint8_4 (external drive)` | `Maximum Intensity Projection` | `211452.270 ms` | `212012.365 ms` | `55.416 ms` | `135.392 ms` | `68.001 ms` | `40225.622 ms` | `40.341 GiB` |
| `largeimgmergeoutput_large_all_uint8_4 (external drive)` | `Direct Volume Rendering` | `303831.956 ms` | `305678.759 ms` | `115.072 ms` | `379.193 ms` | `139.241 ms` | `18204.087 ms` | `36.975 GiB` |

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
- Root: `~/Dropbox/atlas_test/slice15_paraview/benchmarks/paraview_deterministic_interactive_plus_final_2000x1500`
- Aggregate summary: `~/Dropbox/atlas_test/slice15_paraview/benchmarks/paraview_deterministic_interactive_plus_final_2000x1500/aggregate/summary.json`

### Setup

| Item | Value |
| --- | --- |
| Software | ParaView 6.1.0-RC1 |
| Representation mode | `GPU Based` |
| Blend mode | `maximum-intensity` |
| Deterministic mode | `interactive-plus-final` |
| Dataset | `~/Dropbox/atlas_test/slice15_paraview/slice15_ch2_grid_atlasscenespace.vtpd` |
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
- Root: `~/Dropbox/atlas_test/slice15_paraview/benchmarks/paraview_gpu_deterministic_interactive_plus_final_2000x1500_composite_v1`
- Aggregate summary: `~/Dropbox/atlas_test/slice15_paraview/benchmarks/paraview_gpu_deterministic_interactive_plus_final_2000x1500_composite_v1/aggregate/summary.json`

### Setup

| Item | Value |
| --- | --- |
| Software | ParaView 6.1.0-RC1 |
| Representation mode | `GPU Based` |
| Blend mode | `composite` |
| Deterministic mode | `interactive-plus-final` |
| Dataset | `~/Dropbox/atlas_test/slice15_paraview/slice15_ch2_grid_atlasscenespace.vtpd` |
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
- Root: `~/Dropbox/atlas_test/slice15_paraview/benchmarks/paraview_ospray_deterministic_interactive_plus_final_2000x1500_composite_v3`
- Aggregate summary: `~/Dropbox/atlas_test/slice15_paraview/benchmarks/paraview_ospray_deterministic_interactive_plus_final_2000x1500_composite_v3/aggregate/summary.json`

### Setup

| Item | Value |
| --- | --- |
| Software | ParaView 6.1.0-RC1 |
| Launch path | `~/code/atlas/util/benchmark/launch_paraview_with_ospray_fix.sh` |
| Representation mode | `OSPRay Based` |
| Blend mode | `composite` |
| View ray tracing | Disabled (`EnableRayTracing = 0`) |
| Deterministic mode | `interactive-plus-final` |
| Dataset | `~/Dropbox/atlas_test/slice15_paraview/slice15_ch2_dense_atlasscenespace.mhd` |
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
- Root: `~/Dropbox/atlas_test/slice15_paraview/benchmarks/atlas_deterministic_interactive_plus_final_2000x1500_mip_v5_parity`
- Aggregate summary: `~/Dropbox/atlas_test/slice15_paraview/benchmarks/atlas_deterministic_interactive_plus_final_2000x1500_mip_v5_parity/aggregate/summary.json`

### Setup

| Item | Value |
| --- | --- |
| Software | Atlas (local build from this repository) |
| Rendering mode | Full-resolution image rendering enabled |
| Compositing mode | `Maximum Intensity Projection` |
| Deterministic mode | `interactive-plus-final` with a fixed `2.0 s` hold between camera commands |
| Dataset | `~/Dropbox/atlas_test/slice15_paraview/slice15_ch2_dense.nim` |
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
- Root: `~/Dropbox/atlas_test/slice15_paraview/benchmarks/atlas_deterministic_interactive_plus_final_2000x1500_dvr_v2_parity`
- Aggregate summary: `~/Dropbox/atlas_test/slice15_paraview/benchmarks/atlas_deterministic_interactive_plus_final_2000x1500_dvr_v2_parity/aggregate/summary.json`

### Setup

| Item | Value |
| --- | --- |
| Software | Atlas (local build from this repository) |
| Rendering mode | Full-resolution image rendering enabled |
| Compositing mode | `Direct Volume Rendering` |
| Deterministic mode | `interactive-plus-final` with a fixed `2.0 s` hold between camera commands |
| Dataset | `~/Dropbox/atlas_test/slice15_paraview/slice15_ch2_dense.nim` |
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
| Source scene | `~/Downloads/test_gpufit.scene` |
| Camera spec | `~/code/atlas/large_test_image/slice15_ch2_gpufit_scene_camera_exact_2000x1500.json` |
| Output render size | `2000 x 1500` physical pixels |
| Atlas live 3D canvas size | `1000 x 750` logical Qt pixels on Retina, yielding about `2000 x 1500` physical pixels |
| Deterministic action sequence | `open`, `rotate`, `zoom` |
| Interpolated actions | `30` requested camera steps per action |
| Warm-up / measured runs | `1` warm-up run, `7` measured runs |
| ParaView dense scene-space header | `~/code/atlas/large_test_image/slice15_ch2_gpufit_1024x1024x980_scenespace.mhd` (`1 x 1 x 1`) |
| Atlas dataset | `~/code/atlas/large_test_image/slice15_ch2_gpufit_1024x1024x980_iso0p1um.nim` (`0.1 x 0.1 x 0.1 um`) |
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
- Root: `~/code/atlas/large_test_image/benchmarks/paraview_gpufit_deterministic_interactive_plus_final_2000x1500_mip_v1`
- Aggregate summary: `~/code/atlas/large_test_image/benchmarks/paraview_gpufit_deterministic_interactive_plus_final_2000x1500_mip_v1/aggregate/summary.json`

### Setup

| Item | Value |
| --- | --- |
| Software | ParaView 6.1.0-RC1 |
| Representation mode | `GPU Based` |
| Blend mode | `maximum-intensity` |
| Deterministic mode | `interactive-plus-final` |
| Dataset | `~/code/atlas/large_test_image/slice15_ch2_gpufit_1024x1024x980_scenespace.mhd` |
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
- Root: `~/code/atlas/large_test_image/benchmarks/paraview_gpufit_deterministic_interactive_plus_final_2000x1500_composite_v1`
- Aggregate summary: `~/code/atlas/large_test_image/benchmarks/paraview_gpufit_deterministic_interactive_plus_final_2000x1500_composite_v1/aggregate/summary.json`

### Setup

| Item | Value |
| --- | --- |
| Software | ParaView 6.1.0-RC1 |
| Representation mode | `GPU Based` |
| Blend mode | `composite` |
| Deterministic mode | `interactive-plus-final` |
| Dataset | `~/code/atlas/large_test_image/slice15_ch2_gpufit_1024x1024x980_scenespace.mhd` |
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
- Root: `~/code/atlas/large_test_image/benchmarks/paraview_gpufit_ospray_deterministic_interactive_plus_final_2000x1500_composite_v1`
- Aggregate summary: `~/code/atlas/large_test_image/benchmarks/paraview_gpufit_ospray_deterministic_interactive_plus_final_2000x1500_composite_v1/aggregate/summary.json`

### Setup

| Item | Value |
| --- | --- |
| Software | ParaView 6.1.0-RC1 |
| Launch path | `~/code/atlas/util/benchmark/launch_paraview_with_ospray_fix.sh` |
| View ray tracing | Disabled (`EnableRayTracing = 0`) |
| Representation mode | `OSPRay Based` |
| Blend mode | `composite` |
| Deterministic mode | `interactive-plus-final` |
| Dataset | `~/code/atlas/large_test_image/slice15_ch2_gpufit_1024x1024x980_scenespace.mhd` |
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
- Root: `~/code/atlas/large_test_image/benchmarks/atlas_gpufit_deterministic_interactive_plus_final_2000x1500_mip_v2_clean`
- Aggregate summary: `~/code/atlas/large_test_image/benchmarks/atlas_gpufit_deterministic_interactive_plus_final_2000x1500_mip_v2_clean/aggregate/summary.json`

### Setup

| Item | Value |
| --- | --- |
| Software | Atlas (local build from this repository) |
| Rendering mode | Default Atlas behavior for this dataset |
| Compositing mode | `Maximum Intensity Projection` |
| Deterministic mode | `interactive-plus-final` with a fixed `2.0 s` hold between camera commands |
| Dataset | `~/code/atlas/large_test_image/slice15_ch2_gpufit_1024x1024x980_iso0p1um.nim` |
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
- Root: `~/code/atlas/large_test_image/benchmarks/atlas_gpufit_deterministic_interactive_plus_final_2000x1500_dvr_v1_clean`
- Aggregate summary: `~/code/atlas/large_test_image/benchmarks/atlas_gpufit_deterministic_interactive_plus_final_2000x1500_dvr_v1_clean/aggregate/summary.json`

### Setup

| Item | Value |
| --- | --- |
| Software | Atlas (local build from this repository) |
| Rendering mode | Default Atlas behavior for this dataset |
| Compositing mode | `Direct Volume Rendering` |
| Deterministic mode | `interactive-plus-final` with a fixed `2.0 s` hold between camera commands |
| Dataset | `~/code/atlas/large_test_image/slice15_ch2_gpufit_1024x1024x980_iso0p1um.nim` |
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
| Source scene | `~/Downloads/test_slice15_ch2_2x.scene` |
| Camera spec | `~/code/atlas/large_test_image/slice15_ch2_x2z_scene_camera_exact_2000x1500_v2.json` |
| Output render size | `2000 x 1500` physical pixels |
| Atlas live 3D canvas size | `1000 x 750` logical Qt pixels on Retina, yielding about `2000 x 1500` physical pixels |
| Deterministic action sequence | `open`, `rotate`, `zoom` |
| Interpolated actions | `30` requested camera steps per action |
| Warm-up / measured runs | `1` warm-up run, `7` measured runs |
| ParaView dense scene-space header | `~/code/atlas/large_test_image/slice15_ch2_x2z_scenespace.mhd` (`1 x 1 x 5.0472259521484375`) |
| ParaView blocked scene-space dataset | `~/code/atlas/large_test_image/slice15_ch2_x2z_grid_atlasscenespace.vtpd` (`1 x 1 x 5.0472259521484375`) |
| Atlas dataset | `~/code/atlas/large_test_image/slice15_ch2_x2z.nim` (`0.10378322750329971 x 0.10378322750329971 x 0.52381742000579834 um`) |
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
- Root: `~/code/atlas/large_test_image/benchmarks/paraview_x2z_deterministic_interactive_plus_final_2000x1500_mip_v1`
- Aggregate summary: `~/code/atlas/large_test_image/benchmarks/paraview_x2z_deterministic_interactive_plus_final_2000x1500_mip_v1/aggregate/summary.json`

### Setup

| Item | Value |
| --- | --- |
| Software | ParaView 6.1.0-RC1 |
| Representation mode | `GPU Based` |
| Blend mode | `maximum-intensity` |
| Deterministic mode | `interactive-plus-final` |
| Dataset | `~/code/atlas/large_test_image/slice15_ch2_x2z_grid_atlasscenespace.vtpd` |
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
- Root: `~/code/atlas/large_test_image/benchmarks/paraview_x2z_deterministic_interactive_plus_final_2000x1500_composite_v1`
- Aggregate summary: `~/code/atlas/large_test_image/benchmarks/paraview_x2z_deterministic_interactive_plus_final_2000x1500_composite_v1/aggregate/summary.json`

### Setup

| Item | Value |
| --- | --- |
| Software | ParaView 6.1.0-RC1 |
| Representation mode | `GPU Based` |
| Blend mode | `composite` |
| Deterministic mode | `interactive-plus-final` |
| Dataset | `~/code/atlas/large_test_image/slice15_ch2_x2z_grid_atlasscenespace.vtpd` |
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
- Root: `~/code/atlas/large_test_image/benchmarks/paraview_x2z_ospray_deterministic_interactive_plus_final_2000x1500_composite_v1`
- Aggregate summary: `~/code/atlas/large_test_image/benchmarks/paraview_x2z_ospray_deterministic_interactive_plus_final_2000x1500_composite_v1/aggregate/summary.json`

### Setup

| Item | Value |
| --- | --- |
| Software | ParaView 6.1.0-RC1 |
| Launch path | `~/code/atlas/util/benchmark/launch_paraview_with_ospray_fix.sh` |
| View ray tracing | Disabled (`EnableRayTracing = 0`) |
| Representation mode | `OSPRay Based` |
| Blend mode | `composite` |
| Deterministic mode | `interactive-plus-final` |
| Dataset | `~/code/atlas/large_test_image/slice15_ch2_x2z_scenespace.mhd` |
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
- Root: `~/code/atlas/large_test_image/benchmarks/atlas_x2z_deterministic_interactive_plus_final_2000x1500_mip_v1`
- Aggregate summary: `~/code/atlas/large_test_image/benchmarks/atlas_x2z_deterministic_interactive_plus_final_2000x1500_mip_v1/aggregate/summary.json`

### Setup

| Item | Value |
| --- | --- |
| Software | Atlas (local build from this repository) |
| Rendering mode | Default Atlas behavior for this dataset |
| Compositing mode | `Maximum Intensity Projection` |
| Deterministic mode | `interactive-plus-final` with log-driven preview/final waits from Atlas benchmark markers |
| Dataset | `~/code/atlas/large_test_image/slice15_ch2_x2z.nim` |
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
- Root: `~/code/atlas/large_test_image/benchmarks/atlas_x2z_deterministic_interactive_plus_final_2000x1500_dvr_v1`
- Aggregate summary: `~/code/atlas/large_test_image/benchmarks/atlas_x2z_deterministic_interactive_plus_final_2000x1500_dvr_v1/aggregate/summary.json`

### Setup

| Item | Value |
| --- | --- |
| Software | Atlas (local build from this repository) |
| Rendering mode | Default Atlas behavior for this dataset |
| Compositing mode | `Direct Volume Rendering` |
| Deterministic mode | `interactive-plus-final` with log-driven preview/final waits from Atlas benchmark markers |
| Dataset | `~/code/atlas/large_test_image/slice15_ch2_x2z.nim` |
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
| Camera spec | `~/code/atlas/large_test_image/high_res_scene_camera_exact_2000x1500.json` |
| Viewport convention | `2000 x 1500` physical pixels (`1000 x 750` logical Retina canvas in Atlas) |
| Action sequence | `open`, then `rotate`, then `zoom` |
| Rotate action | `0.5 s`, `30` interpolated steps, followed by settle |
| Zoom action | `0.5 s`, `30` interpolated steps, followed by settle |
| Deterministic method | Internal Atlas benchmark markers plus live Atlas render-log parsing |
| Benchmark cleanup flags | `--hide-background --hide-axis --hide-bound-box` |
| ParaView retained sessions | None on this machine for this dataset |
| Atlas dataset | `~/code/atlas/large_test_image/high_res_20220219_stitched_all_spacing_0p1_0p1_2_um.nim` |
| Dataset format | Dense `.nim` |
| Dataset size | `25395 x 19459 x 169`, single channel |
| Dataset spacing | `0.1 x 0.1 x 2.0 um` |
| Reference blocked ParaView export | `~/code/atlas/large_test_image/high_res_20220219_stitched_all_spacing_0p1_0p1_2_um.vtpd` (`500` blocks) |

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
- Root: `~/code/atlas/large_test_image/benchmarks/atlas_high_res_deterministic_interactive_plus_final_2000x1500_mip_v1`
- Aggregate summary: `~/code/atlas/large_test_image/benchmarks/atlas_high_res_deterministic_interactive_plus_final_2000x1500_mip_v1/aggregate/summary.json`

### Setup

| Item | Value |
| --- | --- |
| Software | Atlas (local build from this repository) |
| Rendering mode | Default Atlas behavior for this dataset |
| Compositing mode | `Maximum Intensity Projection` |
| Deterministic mode | `interactive-plus-final` with log-driven preview/final waits from Atlas benchmark markers |
| Dataset | `~/code/atlas/large_test_image/high_res_20220219_stitched_all_spacing_0p1_0p1_2_um.nim` |
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
- Root: `~/code/atlas/large_test_image/benchmarks/atlas_high_res_deterministic_interactive_plus_final_2000x1500_dvr_v1`
- Aggregate summary: `~/code/atlas/large_test_image/benchmarks/atlas_high_res_deterministic_interactive_plus_final_2000x1500_dvr_v1/aggregate/summary.json`

### Setup

| Item | Value |
| --- | --- |
| Software | Atlas (local build from this repository) |
| Rendering mode | Default Atlas behavior for this dataset |
| Compositing mode | `Direct Volume Rendering` |
| Deterministic mode | `interactive-plus-final` with log-driven preview/final waits from Atlas benchmark markers |
| Dataset | `~/code/atlas/large_test_image/high_res_20220219_stitched_all_spacing_0p1_0p1_2_um.nim` |
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

# Deterministic Benchmark Sessions: `largeimgmergeoutput_large_all_uint8_4`

This section summarizes the retained Atlas deterministic benchmark sessions for
`largeimgmergeoutput_large_all_uint8_4`. The dataset file lives on an external
drive, so these results include external-drive I/O behavior and should not be
treated as directly comparable to the earlier internal-disk Atlas datasets.

## Shared Setup

| Item | Value |
| --- | --- |
| Camera spec | `~/code/atlas/large_test_image/largeimgmerge_scene_camera_exact_2000x1500.json` |
| Viewport convention | `2000 x 1500` physical pixels (`1000 x 750` logical Retina canvas in Atlas) |
| Action sequence | `open`, then `rotate`, then `zoom` |
| Rotate action | `0.5 s`, `30` interpolated steps, followed by settle |
| Zoom action | `0.5 s`, `30` interpolated steps, followed by settle |
| Deterministic method | Internal Atlas benchmark markers plus live Atlas render-log parsing |
| Benchmark cleanup flags | `--hide-background --hide-axis --hide-bound-box` |
| ParaView retained sessions | None for this dataset in this summary |
| Atlas dataset | `/Volumes/T7 Shield/largeimgmergeoutput_large_all_uint8_4.nim` |
| Dataset format | Dense `.nim` on external SSD |
| Dataset size | `16866 x 10127 x 7436`, single channel |
| Dataset spacing | `1 x 1 x 1` (file metadata; `VoxelSizeUnit.none`) |
| File size on disk | About `328 GiB` |
| Raw voxel payload | `1,270,083,538,152` voxels, about `1.27 TB` at `uint8` (`~1.16 TiB`) |
| Storage note | The benchmark file is on `/Volumes/T7 Shield`, so open and paging-heavy final renders include external-drive latency and bandwidth effects. |

For the Atlas rows below:
- `Open total -> first preview` and `Open total -> final` are measured from action start.
- `Rotate/Zoom release/final` uses the last-step `preview -> final` settle interval.

## Cross-Session Snapshot

| Session | Input | Compositing | Open first preview | Open final | Rotate preview | Rotate release/final | Zoom preview | Zoom release/final | Peak RSS | Full-run wall |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Atlas MIP | Dense `.nim` (external drive) | `Maximum Intensity Projection` | `211452.270 ms` | `212012.365 ms` | `55.416 ms` | `135.392 ms` | `68.001 ms` | `40225.622 ms` | `40.341 GiB` | `597.663 s` |
| Atlas DVR | Dense `.nim` (external drive) | `Direct Volume Rendering` | `303831.956 ms` | `305678.759 ms` | `115.072 ms` | `379.193 ms` | `139.241 ms` | `18204.087 ms` | `36.975 GiB` | `811.201 s` |

## Session 18: Atlas Dense Input, MIP

Artifacts:
- Root: `~/code/atlas/large_test_image/benchmarks/atlas_largeimgmerge_externaldrive_deterministic_interactive_plus_final_2000x1500_mip_v1`
- Aggregate summary: `~/code/atlas/large_test_image/benchmarks/atlas_largeimgmerge_externaldrive_deterministic_interactive_plus_final_2000x1500_mip_v1/aggregate/summary.json`

### Setup

| Item | Value |
| --- | --- |
| Software | Atlas (local build from this repository) |
| Rendering mode | Default Atlas behavior for this dataset |
| Compositing mode | `Maximum Intensity Projection` |
| Deterministic mode | `interactive-plus-final` with log-driven preview/final waits from Atlas benchmark markers |
| Dataset | `/Volumes/T7 Shield/largeimgmergeoutput_large_all_uint8_4.nim` |
| Dataset format | Dense `.nim` on external SSD |
| Dataset size | `16866 x 10127 x 7436`, single channel |
| Dataset spacing | `1 x 1 x 1` (file metadata; `VoxelSizeUnit.none`) |
| Storage note | The dataset file is on `/Volumes/T7 Shield`, so open and deep zoom final times include external-drive I/O effects. |
| Benchmark cleanup flags | `--hide-background --hide-axis --hide-bound-box` |

### Warm-up

| Metric | Warm-up value |
| --- | --- |
| Open total -> first preview | `33459.331 ms` |
| Open total -> final | `34306.152 ms` |
| Open target view -> first preview | `32.567 ms` |
| Open target view -> final | `879.388 ms` |
| Open target view preview -> final | `846.821 ms` |
| Rotate preview step mean | `54.332 ms` |
| Rotate step 1 preview | `59.467 ms` |
| Rotate step 30 preview | `55.547 ms` |
| Rotate step 30 preview -> final | `134.856 ms` |
| Zoom preview step mean | `68.196 ms` |
| Zoom step 1 preview | `55.921 ms` |
| Zoom step 30 preview | `68.736 ms` |
| Zoom step 30 preview -> final | `100153.530 ms` |
| Peak RSS | `10194698240 bytes` (`9.495 GiB`) |

### Measured Steady State

| Metric | Count | Mean | Median | Std | p95 | Notes |
| --- | --- | --- | --- | --- | --- | --- |
| Open total -> first preview | `7` | `211452.270` | `209482.515` | `5046.332` | `219099.086` | Client-observed open latency to the first preview marker. |
| Open total -> final | `7` | `212012.365` | `210042.998` | `5032.962` | `219624.854` | Client-observed open latency to the first final marker. |
| Open target view -> first preview | `7` | `26.477` | `25.970` | `2.418` | `29.473` | Latency after the intended benchmark camera/state was requested. |
| Open target view -> final | `7` | `586.572` | `588.862` | `32.372` | `622.631` | Final-view latency after the intended benchmark camera/state was requested. |
| Open target view preview -> final | `7` | `560.096` | `560.483` | `31.500` | `597.256` | Settle interval from the intended target-view preview to the first final marker. |
| Rotate preview step duration | `210` | `55.416` | `54.806` | `7.194` | `57.711` | Per-step preview completion over all measured rotate steps. |
| Rotate step 1 preview duration | `7` | `51.913` | `51.827` | `1.775` | `54.553` | First requested rotate step across measured runs. |
| Rotate step 30 preview duration | `7` | `56.536` | `55.958` | `1.450` | `58.434` | Last requested rotate step across measured runs. |
| Rotate preview service FPS | `210` | `18.046` | n/a | `n/a (derived)` | n/a | Computed as `1000 / mean_preview_ms`. |
| Rotate step 30 preview -> final duration | `7` | `135.392` | `135.723` | `2.348` | `137.854` | Last-step final-view latency after the last rotate preview step. |
| Zoom preview step duration | `210` | `68.001` | `68.540` | `10.014` | `76.180` | Per-step preview completion over all measured zoom steps. |
| Zoom step 1 preview duration | `7` | `56.712` | `56.266` | `1.035` | `58.032` | First requested zoom step across measured runs. |
| Zoom step 30 preview duration | `7` | `84.806` | `75.687` | `18.460` | `113.941` | Last requested zoom step across measured runs. |
| Zoom preview service FPS | `210` | `14.706` | n/a | `n/a (derived)` | n/a | Computed as `1000 / mean_preview_ms`. |
| Zoom step 30 preview -> final duration | `7` | `40225.622` | `36256.655` | `9221.169` | `54521.353` | Last-step final-view latency after the last zoom preview step. |
| Peak RSS | `7` | `43315508955 bytes` (`40.341 GiB`) | `43894411264 bytes` (`40.880 GiB`) | `1074783265 bytes` | `44161131315 bytes` (`41.128 GiB`) | Aggregate memory summary across measured runs. |
| Full-run wall time | `7` | `597.663 s` | `574.125 s` | `69.671 s` | `702.500 s` | Entire deterministic script duration per measured run. |

## Session 19: Atlas Dense Input, DVR

Artifacts:
- Root: `~/code/atlas/large_test_image/benchmarks/atlas_largeimgmerge_externaldrive_deterministic_interactive_plus_final_2000x1500_dvr_v1`
- Aggregate summary: `~/code/atlas/large_test_image/benchmarks/atlas_largeimgmerge_externaldrive_deterministic_interactive_plus_final_2000x1500_dvr_v1/aggregate/summary.json`

### Setup

| Item | Value |
| --- | --- |
| Software | Atlas (local build from this repository) |
| Rendering mode | Default Atlas behavior for this dataset |
| Compositing mode | `Direct Volume Rendering` |
| Deterministic mode | `interactive-plus-final` with log-driven preview/final waits from Atlas benchmark markers |
| Dataset | `/Volumes/T7 Shield/largeimgmergeoutput_large_all_uint8_4.nim` |
| Dataset format | Dense `.nim` on external SSD |
| Dataset size | `16866 x 10127 x 7436`, single channel |
| Dataset spacing | `1 x 1 x 1` (file metadata; `VoxelSizeUnit.none`) |
| Storage note | The dataset file is on `/Volumes/T7 Shield`, so open and deep zoom final times include external-drive I/O effects. |
| Benchmark cleanup flags | `--hide-background --hide-axis --hide-bound-box` |

### Warm-up

| Metric | Warm-up value |
| --- | --- |
| Open total -> first preview | `249011.905 ms` |
| Open total -> final | `250053.771 ms` |
| Open target view -> first preview | `35.080 ms` |
| Open target view -> final | `1076.946 ms` |
| Open target view preview -> final | `1041.866 ms` |
| Rotate preview step mean | `79.400 ms` |
| Rotate step 1 preview | `70.474 ms` |
| Rotate step 30 preview | `83.903 ms` |
| Rotate step 30 preview -> final | `255.304 ms` |
| Zoom preview step mean | `122.846 ms` |
| Zoom step 1 preview | `80.877 ms` |
| Zoom step 30 preview | `322.960 ms` |
| Zoom step 30 preview -> final | `32389.343 ms` |
| Peak RSS | `35023290368 bytes` (`32.618 GiB`) |

### Measured Steady State

| Metric | Count | Mean | Median | Std | p95 | Notes |
| --- | --- | --- | --- | --- | --- | --- |
| Open total -> first preview | `7` | `303831.956` | `374285.045` | `107856.393` | `406181.903` | Client-observed open latency to the first preview marker. |
| Open total -> final | `7` | `305678.759` | `377386.774` | `109059.769` | `408949.123` | Client-observed open latency to the first final marker. |
| Open target view -> first preview | `7` | `76.004` | `46.722` | `122.328` | `259.866` | Latency after the intended benchmark camera/state was requested. |
| Open target view -> final | `7` | `1922.807` | `2252.004` | `1206.494` | `3115.811` | Final-view latency after the intended benchmark camera/state was requested. |
| Open target view preview -> final | `7` | `1846.803` | `2246.784` | `1257.619` | `3068.289` | Settle interval from the intended target-view preview to the first final marker. |
| Rotate preview step duration | `210` | `115.072` | `109.055` | `51.511` | `200.155` | Per-step preview completion over all measured rotate steps. |
| Rotate step 1 preview duration | `7` | `128.612` | `112.512` | `73.237` | `237.146` | First requested rotate step across measured runs. |
| Rotate step 30 preview duration | `7` | `113.756` | `108.340` | `50.904` | `183.679` | Last requested rotate step across measured runs. |
| Rotate preview service FPS | `210` | `8.690` | n/a | `n/a (derived)` | n/a | Computed as `1000 / mean_preview_ms`. |
| Rotate step 30 preview -> final duration | `7` | `379.193` | `457.344` | `206.023` | `597.523` | Last-step final-view latency after the last rotate preview step. |
| Zoom preview step duration | `210` | `139.241` | `118.345` | `61.667` | `267.861` | Per-step preview completion over all measured zoom steps. |
| Zoom step 1 preview duration | `7` | `107.625` | `114.057` | `41.454` | `159.848` | First requested zoom step across measured runs. |
| Zoom step 30 preview duration | `7` | `127.703` | `104.245` | `35.250` | `179.178` | Last requested zoom step across measured runs. |
| Zoom preview service FPS | `210` | `7.182` | n/a | `n/a (derived)` | n/a | Computed as `1000 / mean_preview_ms`. |
| Zoom step 30 preview -> final duration | `7` | `18204.087` | `9832.811` | `12223.510` | `35811.488` | Last-step final-view latency after the last zoom preview step. |
| Peak RSS | `7` | `39701996105 bytes` (`36.975 GiB`) | `38061592576 bytes` (`35.448 GiB`) | `4323526308 bytes` | `45962110157 bytes` (`42.806 GiB`) | Aggregate memory summary across measured runs. |
| Full-run wall time | `7` | `811.201 s` | `853.333 s` | `374.563 s` | `1202.466 s` | Entire deterministic script duration per measured run. |

# Additional Experiment Notes

These exploratory runs were useful for planning and interpreting the retained benchmark
sessions, but they were not kept as final benchmark results.

| Dataset | Attempted path | Input | Observed behavior | Retained in final benchmark? | Notes |
| --- | --- | --- | --- | --- | --- |
| `slice15_ch2` | ParaView GPU, single-file dense input | Dense `.mhd/.zraw` (`slice15_ch2_dense_atlasscenespace.mhd`) | The rendered image was visibly wrong: the volume content was recognizable, but the image arrangement was corrupted. It was also much slower than the retained blocked `.vtpd` path. | No | Possible large-single-texture or driver issue on this system, but not confirmed. Final ParaView GPU results for `slice15_ch2` use the blocked scene-space dataset. |
| `slice15_ch2_x2z` | ParaView GPU, single-file dense input | Dense `.mhd/.zraw` (`slice15_ch2_x2z_scenespace.mhd`) | ParaView kept allocating toward and past system memory during load/render, so the run was killed instead of being allowed to continue. | No | Final ParaView GPU results for `slice15_ch2_x2z` use the blocked scene-space dataset [slice15_ch2_x2z_grid_atlasscenespace.vtpd](~/code/atlas/large_test_image/slice15_ch2_x2z_grid_atlasscenespace.vtpd). |
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
| Detector | Exact pixel comparison inside the captured render-area ROI; any pixel change counts |
| Capture crop | Centered `400 x 300` window-relative region inside the render pane |
| Drag path | Horizontal left-button drag from `45%` to `75%` of the calibrated input region width |
| Retained drags | Short rotate `0.5 s` (`60` injected samples, `120 Hz`) and sustained rotate `5.0 s` (`600` injected samples, `120 Hz`) |
| Warm-up / measured runs | `1` warm-up run, `7` measured runs |
| ParaView GUI prep | Applies the benchmark camera, then recenters `CameraFocalPoint` and `CenterOfRotation` to the data center before each run so GUI rotation stays centered on the object |
| Atlas GUI prep | Loads the dataset once, applies the benchmark camera, then resets back to the `open` camera and waits for Atlas preview/final markers before each run |
| Retained render modes | ParaView GPU MIP and Atlas MIP |
| Notes | Published GUI metrics are strictly render-area-only. The retained exact-pixel runs do not use thresholding, and this section intentionally focuses on drag-window cadence metrics rather than session-wide helper cadence or final-stable timing. |

## GUI Snapshot: Short Rotate (`0.5 s`)

Measured steady-state means for the retained centered `0.5 s` rotate sessions.

| Session | Input | Render mode | First visible from drag start | Changed samples during drag | Changed samples / second | Visible FPS |
| --- | --- | --- | --- | --- | --- | --- |
| ParaView GPU MIP | Blocked `.vtpd` | `GPU Based` + `maximum-intensity` | `2007.321 ms` | `0.000` | `0.000` | n/a |
| Atlas MIP | Dense `.nim` | `Maximum Intensity Projection` | `54.144 ms` | `10.143` | `20.253` | `22.181 fps` |

## GUI Snapshot: Sustained Rotate (`5.0 s`)

Measured steady-state means for the retained centered `5.0 s` rotate sessions.

| Session | Input | Render mode | First visible from drag start | Changed samples during drag | Changed samples / second | Visible FPS |
| --- | --- | --- | --- | --- | --- | --- |
| ParaView GPU MIP | Blocked `.vtpd` | `GPU Based` + `maximum-intensity` | `1969.282 ms` | `2.000` | `0.400` | `0.554 fps` |
| Atlas MIP | Dense `.nim` | `Maximum Intensity Projection` | `44.721 ms` | `113.286` | `22.652` | `22.794 fps` |

## GUI Session 1: ParaView GPU MIP, Centered Rotate, `0.5 s`

Artifacts:
- Root: `~/Dropbox/atlas_test/slice15_paraview/benchmarks/paraview_gui_rotate_slice15_ch2_gpu_mip_2000x1500_v4_centercrop_120hzinput`
- Aggregate summary: `~/Dropbox/atlas_test/slice15_paraview/benchmarks/paraview_gui_rotate_slice15_ch2_gpu_mip_2000x1500_v4_centercrop_120hzinput/aggregate/summary.json`

### Setup

| Item | Value |
| --- | --- |
| Software | ParaView 6.1.0-RC1 |
| Dataset | `~/Dropbox/atlas_test/slice15_paraview/slice15_ch2_grid_atlasscenespace.vtpd` |
| Dataset format | Blocked `.vtpd` with `.vti` pieces |
| Dataset size | `9216 x 6144 x 98`, single channel |
| Dataset spacing | `1 x 1 x 5.0472259521484375` scene-space units |
| Render mode | `GPU Based` + `maximum-intensity` |
| Camera / rotation center | Benchmark camera from `slice15_scene_camera_exact_2000x1500.json`, then recentered to the data bounds center for GUI rotation parity |
| Capture ROI | Centered `400 x 300` window-relative crop inside the render pane |
| Drag duration / steps | `0.5 s`, `60` injected drag samples (`120 Hz`) |

### Warm-up

| Metric | Warm-up value |
| --- | --- |
| Changed samples during drag | `0` |
| Changed samples / second | `0.000` |
| Visible FPS from mean interval | n/a |
| First visible from drag start | `1957.010 ms` |
| Drag duration | `502.204 ms` |

### Measured Steady State

| Metric | Count | Mean | Median | Std | p95 | Notes |
| --- | --- | --- | --- | --- | --- | --- |
| Changed samples during drag | `7` | `0.000` | `0.000` | `0.000` | `0.000` | No visible render-area change was observed during any retained `0.5 s` drag. |
| Changed samples / second | `7` | `0.000` | `0.000` | `0.000` | `0.000` | Primary short-drag cadence metric for ParaView in this exact-pixel suite. |
| Visible FPS from mean interval | `0` | n/a | n/a | n/a | n/a | Undefined because there were no visible changed frames during the drag window. |
| First visible from drag start | `7` | `2007.321 ms` | `2022.408 ms` | `31.044 ms` | `2036.915 ms` | The first visible render-area change consistently arrived well after the short drag had already ended. |
| Drag duration | `7` | `501.032 ms` | `501.023 ms` | `0.513 ms` | `501.695 ms` | Measured from injected `drag_start` to `drag_end`. |

## GUI Session 2: Atlas MIP, Centered Rotate, `0.5 s`

Artifacts:
- Root: `~/code/atlas/large_test_image/benchmarks/atlas_gui_rotate_slice15_ch2_mip_2000x1500_v7_centercrop_120hzinput_cancelcheckv2`
- Aggregate summary: `~/code/atlas/large_test_image/benchmarks/atlas_gui_rotate_slice15_ch2_mip_2000x1500_v7_centercrop_120hzinput_cancelcheckv2/aggregate/summary.json`

### Setup

| Item | Value |
| --- | --- |
| Software | Atlas build `v1.0.7-29-gb1cf2f5f` |
| Dataset | `~/code/atlas/large_test_image/slice15_ch2_dense.nim` |
| Dataset format | Dense `.nim` |
| Live 3D canvas | `1000 x 750` logical Qt pixels on Retina, yielding about `2000 x 1500` physical pixels |
| Render mode | `Maximum Intensity Projection` |
| View parity settings | Background hidden, axis hidden, bound box set to `No Bound Box`, full-resolution rendering enabled |
| Camera reset | Benchmark `open` camera from `slice15_scene_camera_exact_2000x1500.json`, reapplied before every run |
| Capture ROI | Centered `400 x 300` window-relative crop inside the 3D canvas |
| Drag duration / steps | `0.5 s`, `60` injected drag samples (`120 Hz`) |

### Warm-up

| Metric | Warm-up value |
| --- | --- |
| Changed samples during drag | `8` |
| Changed samples / second | `15.892` |
| Visible FPS from mean interval | `21.326 fps` |
| First visible from drag start | `157.573 ms` |
| Drag duration | `503.397 ms` |

### Measured Steady State

| Metric | Count | Mean | Median | Std | p95 | Notes |
| --- | --- | --- | --- | --- | --- | --- |
| Changed samples during drag | `7` | `10.143` | `10.000` | `0.833` | `11.000` | Atlas still shows multiple visible updates inside the short drag window with the centered `120 Hz` capture setup. |
| Changed samples / second | `7` | `20.253` | `19.978` | `1.670` | `21.987` | Primary short-drag cadence metric for Atlas in this exact-pixel suite. |
| Visible FPS from mean interval | `7` | `22.181 fps` | `22.453 fps` | `2.578 fps` | `25.987 fps` | Interval-derived visible cadence from changed-frame timestamps. |
| First visible from drag start | `7` | `54.144 ms` | `54.168 ms` | `4.908 ms` | `58.761 ms` | Time to the first visible render-area change after drag motion begins. |
| Drag duration | `7` | `500.824 ms` | `500.553 ms` | `0.572 ms` | `501.744 ms` | Measured from injected `drag_start` to `drag_end`. |

## GUI Session 3: ParaView GPU MIP, Centered Rotate, `5.0 s`

Artifacts:
- Root: `~/Dropbox/atlas_test/slice15_paraview/benchmarks/paraview_gui_rotate_slice15_ch2_gpu_mip_2000x1500_rotate5s_v3_centercrop_120hzinput`
- Aggregate summary: `~/Dropbox/atlas_test/slice15_paraview/benchmarks/paraview_gui_rotate_slice15_ch2_gpu_mip_2000x1500_rotate5s_v3_centercrop_120hzinput/aggregate/summary.json`

### Setup

| Item | Value |
| --- | --- |
| Software | ParaView 6.1.0-RC1 |
| Dataset | `~/Dropbox/atlas_test/slice15_paraview/slice15_ch2_grid_atlasscenespace.vtpd` |
| Dataset format | Blocked `.vtpd` with `.vti` pieces |
| Dataset size | `9216 x 6144 x 98`, single channel |
| Dataset spacing | `1 x 1 x 5.0472259521484375` scene-space units |
| Render mode | `GPU Based` + `maximum-intensity` |
| Camera / rotation center | Benchmark camera from `slice15_scene_camera_exact_2000x1500.json`, then recentered to the data bounds center for GUI rotation parity |
| Capture ROI | Centered `400 x 300` window-relative crop inside the render pane |
| Drag duration / steps | `5.0 s`, `600` injected drag samples (`120 Hz`) |

### Warm-up

| Metric | Warm-up value |
| --- | --- |
| Changed samples during drag | `2` |
| Changed samples / second | `0.400` |
| Visible FPS from mean interval | `0.557 fps` |
| First visible from drag start | `1967.760 ms` |
| Drag duration | `5000.627 ms` |

### Measured Steady State

| Metric | Count | Mean | Median | Std | p95 | Notes |
| --- | --- | --- | --- | --- | --- | --- |
| Changed samples during drag | `7` | `2.000` | `2.000` | `0.000` | `2.000` | Exactly two visible render-area updates were observed in every retained `5 s` drag. |
| Changed samples / second | `7` | `0.400` | `0.400` | `0.001` | `0.400` | Primary sustained-drag cadence metric for ParaView in this exact-pixel suite. |
| Visible FPS from mean interval | `7` | `0.554 fps` | `0.550 fps` | `0.008 fps` | `0.566 fps` | Interval-derived visible cadence from changed-frame timestamps. |
| First visible from drag start | `7` | `1969.282 ms` | `1978.934 ms` | `20.235 ms` | `1990.410 ms` | The first visible render-area change arrives around `2.0 s` into the sustained drag. |
| Drag duration | `7` | `5001.161 ms` | `5000.867 ms` | `0.736 ms` | `5002.235 ms` | Measured from injected `drag_start` to `drag_end`. |

## GUI Session 4: Atlas MIP, Centered Rotate, `5.0 s`

Artifacts:
- Root: `~/code/atlas/large_test_image/benchmarks/atlas_gui_rotate_slice15_ch2_mip_2000x1500_rotate5s_v6_centercrop_120hzinput_cancelcheckv3_rerun2`
- Aggregate summary: `~/code/atlas/large_test_image/benchmarks/atlas_gui_rotate_slice15_ch2_mip_2000x1500_rotate5s_v6_centercrop_120hzinput_cancelcheckv3_rerun2/aggregate/summary.json`

### Setup

| Item | Value |
| --- | --- |
| Software | Atlas build `v1.0.7-29-gb1cf2f5f` |
| Dataset | `~/code/atlas/large_test_image/slice15_ch2_dense.nim` |
| Dataset format | Dense `.nim` |
| Live 3D canvas | `1000 x 750` logical Qt pixels on Retina, yielding about `2000 x 1500` physical pixels |
| Render mode | `Maximum Intensity Projection` |
| View parity settings | Background hidden, axis hidden, bound box set to `No Bound Box`, full-resolution rendering enabled |
| Camera reset | Benchmark `open` camera from `slice15_scene_camera_exact_2000x1500.json`, reapplied before every run |
| Capture ROI | Centered `400 x 300` window-relative crop inside the 3D canvas |
| Drag duration / steps | `5.0 s`, `600` injected drag samples (`120 Hz`) |

### Warm-up

| Metric | Warm-up value |
| --- | --- |
| Changed samples during drag | `112` |
| Changed samples / second | `22.396` |
| Visible FPS from mean interval | `22.539 fps` |
| First visible from drag start | `56.722 ms` |
| Drag duration | `5000.862 ms` |

### Measured Steady State

| Metric | Count | Mean | Median | Std | p95 | Notes |
| --- | --- | --- | --- | --- | --- | --- |
| Changed samples during drag | `7` | `113.286` | `113.000` | `4.199` | `118.700` | Atlas keeps visible updates flowing through nearly the entire `5 s` drag window. |
| Changed samples / second | `7` | `22.652` | `22.589` | `0.839` | `23.736` | Primary sustained-drag cadence metric for Atlas in this exact-pixel suite. |
| Visible FPS from mean interval | `7` | `22.794 fps` | `22.712 fps` | `0.764 fps` | `23.803 fps` | Interval-derived visible cadence from changed-frame timestamps. |
| First visible from drag start | `7` | `44.721 ms` | `44.852 ms` | `8.928 ms` | `56.716 ms` | Time to the first visible render-area change after drag motion begins. |
| Drag duration | `7` | `5001.170 ms` | `5000.870 ms` | `0.734 ms` | `5002.314 ms` | Measured from injected `drag_start` to `drag_end`. |

# Fidelity Validation: `high_res_20220219_stitched_all_spacing_0p1_0p1_2_um`

This section records the retained fidelity validation for Atlas on memory-fit
ROIs cut from the retained
`high_res_20220219_stitched_all_spacing_0p1_0p1_2_um` dataset. This is a
supplementary quality audit, not a throughput benchmark.

Artifacts:
- ROI family: `~/code/atlas/large_test_image/fidelity_validation/high_res_20220219_roi_validation_v2`
- Retained DVR render suite: `~/code/atlas/large_test_image/fidelity_validation/high_res_20220219_fidelity_render_dvr_zoom06_v2_coarse2_audit_v1`
- Retained DVR analysis summary: `~/code/atlas/large_test_image/fidelity_validation/high_res_20220219_fidelity_render_dvr_zoom06_v2_coarse2_audit_v1/analysis/summary.json`
- Retained MIP analysis summary: `~/code/atlas/large_test_image/fidelity_validation/high_res_20220219_fidelity_render_mip_zoom06_v2_screenshot_summary_v1/summary.json`

## Fidelity Protocol

| Item | Value |
| --- | --- |
| Source dataset | `~/code/atlas/large_test_image/high_res_20220219_stitched_all_spacing_0p1_0p1_2_um.nim` |
| Source shape | `25395 x 19459 x 169`, single channel |
| Source voxel size | `0.1 x 0.1 x 2.0 um` |
| ROI export script | `~/code/atlas/util/benchmark/export_high_res_fidelity_rois.py` |
| ROI family | `high_res_20220219_roi_validation_v2` |
| ROI crop shape | `2048 x 2048 x 169` |
| Retained ROI centers | `(16800, 4300)`, `(13600, 7100)`, `(10500, 10000)`, `(4100, 10000)` |
| ROI variants | `fullres.nim`, `level1.nim`, `level2.nim` |
| Camera seed | `~/code/atlas/large_test_image/high_res_scene_camera_exact_2000x1500.json` |
| Camera policy | Fit each ROI box, then apply `camera-distance-scale = 0.6` to zoom in past the `L1` comfort zone |
| Output render size | `2000 x 1500` physical pixels |
| Atlas live 3D canvas size | `1000 x 750` logical Qt pixels on Retina, yielding about `2000 x 1500` physical pixels |
| View cleanup | Background hidden, axis hidden, bound box set to `No Bound Box` |
| Transfer function | Default Atlas transfer function captured from the bootstrap mode preset |
| Display range | Fixed `0 .. 255` |
| Capture path | Atlas fixed-size screenshot export for display-space captures; `ExportScreenSpaceSufficiencyAudit3D` for screen-space audit counters |
| Conditions | `reference`, `adaptive`, `coarse_l1`, `coarse_l2` |
| `reference` | Resident `fullres.nim`, local ROI camera, sampling rate `8.0` |
| `adaptive` | Original large dataset with full-resolution rendering enabled, ROI `X/Y/Z Cut` applied, sampling rate `2.0` |
| `coarse_l1` | Resident `level1.nim`, scaled back to native ROI footprint with `Coord Transform`, sampling rate `2.0` |
| `coarse_l2` | Resident `level2.nim`, scaled back to native ROI footprint with `Coord Transform`, sampling rate `2.0` |
| Analysis script | `~/code/atlas/util/benchmark/analyze_fidelity_validation.py` |
| DVR metric basis | Final screenshot RGB, converted to grayscale SSIM plus masked absolute-difference metrics |
| MIP metric basis | Final screenshot RGB, converted to grayscale SSIM plus masked absolute-difference metrics, consistent with DVR |
| Screen-space audit basis | Contributing sample/pixel counts, sufficient sample/pixel counts, `level 0` sample/pixel counts, and `level-0-limited` sample/pixel counts exported from the Atlas raycaster |
| Mask policy | Reference-derived foreground mask from the retained reference screenshot artifact |
| Current limitation | The current audit distinguishes `level 0` usage and `level-0-limited` insufficiency, but it does not yet export a full per-level histogram beyond that split. |

## Retained DVR Result

Aggregate across the retained ROI family:

| Condition | Count | Mean SSIM | Mean abs diff | Mean P95 abs diff | Mean max abs diff |
| --- | --- | --- | --- | --- | --- |
| `adaptive` | `4` | `0.996834` | `0.252` | `1.250` | `36.750` |
| `coarse_l1` | `4` | `0.961353` | `2.450` | `10.250` | `78.250` |
| `coarse_l2` | `4` | `0.901613` | `4.463` | `18.250` | `123.500` |

Retained DVR screen-space sufficiency audit, aggregate across the same ROI family:

| Condition | Mean sample sufficiency | Mean pixel sufficiency | Mean `level 0` sample frac | Mean `level-0-limited` sample frac |
| --- | --- | --- | --- | --- |
| `adaptive` | `1.000000` | `1.000000` | `0.995913` | `0.000000` |
| `coarse_l1` | `0.838051` | `0.038210` | `1.000000` | `0.161949` |
| `coarse_l2` | `0.077649` | `0.000000` | `1.000000` | `0.922351` |

Per-ROI retained DVR result:

| ROI | `adaptive` SSIM | `adaptive` mean abs diff | `adaptive` P95 abs diff | `coarse_l1` SSIM | `coarse_l1` mean abs diff | `coarse_l1` P95 abs diff | `coarse_l2` SSIM | `coarse_l2` mean abs diff | `coarse_l2` P95 abs diff |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `roi01_cx16800_cy4300` | `0.993273` | `0.395` | `1.000` | `0.978200` | `1.159` | `4.000` | `0.948754` | `2.146` | `7.000` |
| `roi02_cx13600_cy7100` | `0.997153` | `0.339` | `2.000` | `0.950269` | `4.093` | `19.000` | `0.862628` | `7.383` | `34.000` |
| `roi03_cx10500_cy10000` | `0.997588` | `0.162` | `1.000` | `0.955193` | `1.991` | `7.000` | `0.897722` | `3.344` | `12.000` |
| `roi04_cx4100_cy10000` | `0.999322` | `0.111` | `1.000` | `0.961751` | `2.558` | `11.000` | `0.897348` | `4.977` | `20.000` |

Interpretation:

- Under this zoomed-in DVR view, Atlas adaptive output is consistently much closer
  to the resident native-resolution reference than either forced coarse control.
- This result supports the claim that Atlas adaptive full-resolution DVR is
  selecting detail levels that are substantially closer to the native resident
  render than globally forced coarse levels, at least for this retained ROI set
  and this zoomed screen-space demand.
- The audit numbers are consistent with that image-based result:
  - `adaptive` is screen-space sufficient across the retained ROIs and is almost
    entirely using `level 0` at this zoom (`0.995913` mean `level 0` sample fraction).
  - `coarse_l1` and especially `coarse_l2` remain locked to their own source
    `level 0`, and large fractions of those contributing samples are
    `level-0-limited` under the same screen-space demand.

## Retained MIP Result

This retained MIP result is a secondary zoomed-in native-data check. It is not
the planned primary MIP validation. The next primary MIP experiment should be a
phantom-based minification / phase-stability test, because screenshot-space
similarity against a forced `level 0` reference still does not fully capture
MIP aliasing and continuity behavior under minification.

Aggregate across the retained ROI family, using final screenshot pixels as the
metric basis:

| Condition | Count | Mean SSIM | Mean abs diff | Mean P95 abs diff | Mean max abs diff |
| --- | --- | --- | --- | --- | --- |
| `adaptive` | `4` | `0.797618` | `17.609` | `60.750` | `201.500` |
| `coarse_l1` | `4` | `0.779350` | `21.307` | `61.750` | `165.250` |
| `coarse_l2` | `4` | `0.501413` | `40.917` | `111.250` | `207.500` |

Per-ROI retained MIP result:

| ROI | `adaptive` SSIM | `adaptive` mean abs diff | `adaptive` P95 abs diff | `coarse_l1` SSIM | `coarse_l1` mean abs diff | `coarse_l1` P95 abs diff | `coarse_l2` SSIM | `coarse_l2` mean abs diff | `coarse_l2` P95 abs diff |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `roi01_cx16800_cy4300` | `0.781703` | `16.464` | `58.000` | `0.792878` | `16.525` | `54.000` | `0.521412` | `30.249` | `106.000` |
| `roi02_cx13600_cy7100` | `0.799548` | `13.372` | `46.000` | `0.781035` | `16.340` | `46.000` | `0.510550` | `31.353` | `82.000` |
| `roi03_cx10500_cy10000` | `0.770531` | `22.190` | `71.000` | `0.730745` | `28.841` | `75.000` | `0.396985` | `52.982` | `127.000` |
| `roi04_cx4100_cy10000` | `0.838690` | `18.409` | `68.000` | `0.812740` | `23.523` | `72.000` | `0.576705` | `49.085` | `130.000` |

Interpretation:

- This screenshot-space MIP check is user-facing and consistent with the DVR
  presentation. `adaptive` is better than both forced coarse controls on the
  aggregate, and much better than `coarse_l2`.
- `roi01_cx16800_cy4300` remains a near-tie case: `coarse_l1` is slightly ahead
  on SSIM and P95 abs diff there, while mean abs diff is essentially equal.
- `roi02_cx13600_cy7100` and `roi04_cx4100_cy10000` favor `adaptive`.
- `roi03_cx10500_cy10000` also favors `adaptive` clearly under the screenshot
  metric.
- This is why the retained MIP result should be treated as a secondary check,
  while the primary MIP validation should move to a phantom-based minification /
  phase-stability experiment.
