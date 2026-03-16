#!/bin/zsh

set -euo pipefail

script_dir="$(cd "$(dirname "$0")" && pwd)"
repo_root="$(cd "${script_dir}/../.." && pwd)"

python_bin="${PYTHON_BIN:-python3}"
atlas_binary="${ATLAS_BINARY:-${repo_root}/build/Release/src/atlas/Atlas.app/Contents/MacOS/Atlas}"
dataset_path="${DATASET_PATH:-/Volumes/T7 Shield/largeimgmergeoutput_large_all_uint8_4.nim}"
scene_path="${SCENE_PATH:-/Users/feng/Downloads/test_largeimgmerge.scene}"
camera_spec_path="${CAMERA_SPEC_PATH:-${repo_root}/large_test_image/largeimgmerge_scene_camera_exact_2000x1500.json}"
output_root="${OUTPUT_ROOT:-${repo_root}/large_test_image/benchmarks/atlas_largeimgmerge_externaldrive_deterministic_interactive_plus_final_2000x1500_mip_opaque_v1}"
atlas_log_path="${ATLAS_LOG_PATH:-/Users/feng/Library/Logs/Atlas}"
atlas_address="${ATLAS_ADDRESS:-localhost:50051}"
canvas_logical_width="${CANVAS_LOGICAL_WIDTH:-1000}"
canvas_logical_height="${CANVAS_LOGICAL_HEIGHT:-750}"
warmup_runs="${WARMUP_RUNS:-1}"
measured_runs="${MEASURED_RUNS:-7}"
task_timeout_seconds="${TASK_TIMEOUT_SECONDS:-7200}"
ready_timeout_seconds="${READY_TIMEOUT_SECONDS:-7200}"
preview_timeout_seconds="${PREVIEW_TIMEOUT_SECONDS:-1800}"
final_timeout_seconds="${FINAL_TIMEOUT_SECONDS:-7200}"

if [[ ! -x "${atlas_binary}" ]]; then
  echo "Atlas binary not found or not executable: ${atlas_binary}" >&2
  exit 1
fi

if [[ ! -f "${dataset_path}" ]]; then
  echo "Dataset not found: ${dataset_path}" >&2
  exit 1
fi

if [[ ! -f "${scene_path}" ]]; then
  echo "Scene file not found: ${scene_path}" >&2
  exit 1
fi

"${python_bin}" "${repo_root}/util/benchmark/benchmark_camera_from_scene.py" \
  --scene "${scene_path}" \
  --output "${camera_spec_path}" >/dev/null

atlas_pid="${ATLAS_PID:-}"
if [[ -z "${atlas_pid}" ]]; then
  atlas_pid="$(pgrep -n -f "${atlas_binary}" || true)"
fi

cmd=(
  "${python_bin}"
  "${repo_root}/util/benchmark/atlas_deterministic_batch.py"
  --dataset "${dataset_path}"
  --camera-spec "${camera_spec_path}"
  --output-root "${output_root}"
  --atlas-log-path "${atlas_log_path}"
  --address "${atlas_address}"
  --canvas-logical-width "${canvas_logical_width}"
  --canvas-logical-height "${canvas_logical_height}"
  --warmup-runs "${warmup_runs}"
  --measured-runs "${measured_runs}"
  --task-timeout-seconds "${task_timeout_seconds}"
  --ready-timeout-seconds "${ready_timeout_seconds}"
  --preview-timeout-seconds "${preview_timeout_seconds}"
  --final-timeout-seconds "${final_timeout_seconds}"
  --compositing-mode "MIP Opaque"
  --hide-background
  --hide-axis
  --hide-bound-box
)

if [[ -n "${atlas_pid}" ]]; then
  cmd+=(--sample-rss --atlas-pid "${atlas_pid}")
else
  echo "warning: could not auto-detect an Atlas PID; RSS sampling will be disabled" >&2
  echo "set ATLAS_PID=<pid> if you want RSS sampling" >&2
fi

echo "Camera spec: ${camera_spec_path}"
echo "Output root: ${output_root}"
echo "Atlas log path: ${atlas_log_path}"
printf 'Running:'
for arg in "${cmd[@]}"; do
  printf ' %q' "${arg}"
done
printf '\n'

exec "${cmd[@]}"
