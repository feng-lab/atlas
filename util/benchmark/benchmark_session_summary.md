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
