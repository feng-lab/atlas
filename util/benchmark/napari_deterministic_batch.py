from __future__ import annotations

import argparse
import csv
import json
import math
import statistics
import subprocess
import sys
import time
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Any

HOME = Path.home()


@dataclass(frozen=True)
class RunRecord:
    label: str
    warmup: bool
    output_dir: Path
    elapsed_wall_seconds: float
    process_returncode: int
    timer_summary: dict[str, Any]
    memory_summary: dict[str, Any] | None


def _quantile(values: list[float], q: float) -> float | None:
    if not values:
        return None
    if len(values) == 1:
        return float(values[0])
    ordered = sorted(float(v) for v in values)
    position = (len(ordered) - 1) * float(q)
    lower = int(math.floor(position))
    upper = int(math.ceil(position))
    if lower == upper:
        return ordered[lower]
    weight = position - lower
    return ordered[lower] * (1.0 - weight) + ordered[upper] * weight


def _stats(values: list[float]) -> dict[str, float | int | None]:
    ordered = [float(v) for v in values]
    if not ordered:
        return {
            "count": 0,
            "min": None,
            "max": None,
            "mean": None,
            "median": None,
            "stdev": None,
            "p05": None,
            "p25": None,
            "p75": None,
            "p95": None,
        }
    stdev_value = statistics.stdev(ordered) if len(ordered) > 1 else 0.0
    return {
        "count": len(ordered),
        "min": float(min(ordered)),
        "max": float(max(ordered)),
        "mean": float(statistics.fmean(ordered)),
        "median": float(statistics.median(ordered)),
        "stdev": float(stdev_value),
        "p05": _quantile(ordered, 0.05),
        "p25": _quantile(ordered, 0.25),
        "p75": _quantile(ordered, 0.75),
        "p95": _quantile(ordered, 0.95),
    }


def _parse_args() -> argparse.Namespace:
    script_dir = Path(__file__).resolve().parent
    default_output_root = (
        HOME
        / "Dropbox"
        / "atlas_test"
        / "slice15_paraview"
        / "benchmarks"
        / f"napari_deterministic_{datetime.now().strftime('%Y%m%d_%H%M%S')}"
    )
    parser = argparse.ArgumentParser(
        description=(
            "Run the napari deterministic benchmark repeatedly, persist each run, "
            "and aggregate open/action timing statistics."
        )
    )
    parser.add_argument(
        "--driver-script",
        default=str(script_dir / "napari_volume_benchmark.py"),
        help="Path to the napari benchmark driver script",
    )
    parser.add_argument(
        "--dataset",
        required=True,
        help="Source dataset path forwarded to the driver",
    )
    parser.add_argument(
        "--camera-spec",
        required=True,
        help="Shared benchmark camera spec JSON",
    )
    parser.add_argument(
        "--output-root",
        default=str(default_output_root),
        help="Root directory for warm-up, measured runs, and aggregate outputs",
    )
    parser.add_argument(
        "--conda-env",
        default="napari",
        help="Conda environment used to run the driver",
    )
    parser.add_argument(
        "--rendering",
        default="mip",
        help="napari rendering mode forwarded to the driver",
    )
    parser.add_argument(
        "--channel-index",
        type=int,
        default=0,
        help="Channel index forwarded to the driver",
    )
    parser.add_argument(
        "--time-index",
        type=int,
        default=0,
        help="Time index forwarded to the driver",
    )
    parser.add_argument(
        "--contrast-limits-min",
        type=float,
        default=0.0,
        help="Lower contrast limit forwarded to the driver",
    )
    parser.add_argument(
        "--contrast-limits-max",
        type=float,
        default=255.0,
        help="Upper contrast limit forwarded to the driver",
    )
    parser.add_argument(
        "--warmup-runs",
        type=int,
        default=1,
        help="Number of warm-up runs",
    )
    parser.add_argument(
        "--measured-runs",
        type=int,
        default=7,
        help="Number of measured runs",
    )
    parser.add_argument(
        "--sample-rss",
        action="store_true",
        help="Enable RSS sampling inside the driver",
    )
    parser.add_argument(
        "--dataset-loader",
        choices=("zimg-array", "tifffile-array", "napari-open"),
        default="zimg-array",
        help="Dataset loader mode forwarded to the driver",
    )
    parser.add_argument(
        "--screenshot-reference-every-step",
        action="store_true",
        help=(
            "Forward napari's screenshot-reference mode so every step records "
            "the synchronous screenshot upper-bound timing"
        ),
    )
    parser.add_argument(
        "--open-plugin",
        default=None,
        help="Explicit napari reader plugin forwarded to the driver",
    )
    parser.add_argument(
        "--scene-scale-zyx",
        type=float,
        nargs=3,
        metavar=("Z", "Y", "X"),
        default=None,
        help="Optional scene scale override forwarded to the driver",
    )
    return parser.parse_args()


def _driver_command(args: argparse.Namespace, run_dir: Path) -> list[str]:
    cmd = [
        "conda",
        "run",
        "--no-capture-output",
        "-n",
        args.conda_env,
        "python",
        args.driver_script,
        "--dataset",
        args.dataset,
        "--camera-spec",
        args.camera_spec,
        "--output-dir",
        str(run_dir),
        "--rendering",
        args.rendering,
        "--channel-index",
        str(args.channel_index),
        "--time-index",
        str(args.time_index),
        "--contrast-limits-min",
        str(args.contrast_limits_min),
        "--contrast-limits-max",
        str(args.contrast_limits_max),
        "--dataset-loader",
        str(args.dataset_loader),
    ]
    if args.open_plugin:
        cmd.extend(["--open-plugin", str(args.open_plugin)])
    if args.scene_scale_zyx is not None:
        cmd.extend(
            [
                "--scene-scale-zyx",
                *(str(float(v)) for v in args.scene_scale_zyx),
            ]
        )
    if args.sample_rss:
        cmd.append("--sample-rss")
    if args.screenshot_reference_every_step:
        cmd.append("--screenshot-reference-every-step")
    return cmd


def _run_once(
    *,
    args: argparse.Namespace,
    label: str,
    warmup: bool,
    run_dir: Path,
) -> RunRecord:
    run_dir.mkdir(parents=True, exist_ok=True)
    command = _driver_command(args, run_dir)
    (run_dir / "command.json").write_text(
        json.dumps(command, indent=2) + "\n",
        encoding="utf-8",
    )
    started = time.monotonic()
    completed = subprocess.run(command, check=False)
    elapsed_wall_seconds = time.monotonic() - started

    timer_summary_path = run_dir / "napari_timer_summary.json"
    if not timer_summary_path.exists():
        raise RuntimeError(
            f"napari driver did not write {timer_summary_path} (returncode={completed.returncode})"
        )
    timer_summary = json.loads(timer_summary_path.read_text(encoding="utf-8"))
    memory_summary_path = run_dir / "napari_memory_summary.json"
    memory_summary = (
        json.loads(memory_summary_path.read_text(encoding="utf-8"))
        if memory_summary_path.exists()
        else None
    )
    return RunRecord(
        label=label,
        warmup=warmup,
        output_dir=run_dir,
        elapsed_wall_seconds=elapsed_wall_seconds,
        process_returncode=int(completed.returncode),
        timer_summary=timer_summary,
        memory_summary=memory_summary,
    )


def _write_aggregate(
    *,
    output_root: Path,
    measured_runs: list[RunRecord],
) -> None:
    aggregate_dir = output_root / "aggregate"
    aggregate_dir.mkdir(parents=True, exist_ok=True)

    open_dataset_load = [
        float(run.timer_summary["open"]["dataset_load_ms"])
        for run in measured_runs
        if run.timer_summary.get("open", {}).get("dataset_load_ms") is not None
    ]
    open_layer_add = [
        float(run.timer_summary["open"]["layer_add_ms"])
        for run in measured_runs
        if run.timer_summary.get("open", {}).get("layer_add_ms") is not None
    ]
    total_wall_ms = [run.elapsed_wall_seconds * 1000.0 for run in measured_runs]
    open_session_to_first_frame_swap = [
        float(run.timer_summary["open"]["session_to_open_first_frame_swap_ms"])
        for run in measured_runs
        if run.timer_summary.get("open", {}).get("session_to_open_first_frame_swap_ms")
        is not None
    ]
    open_session_to_render_complete = [
        float(run.timer_summary["open"]["session_to_open_render_complete_ms"])
        for run in measured_runs
        if run.timer_summary.get("open", {}).get("session_to_open_render_complete_ms")
        is not None
    ]
    open_loadstart_to_first_frame_swap = [
        float(
            run.timer_summary["open"]["dataset_load_start_to_open_first_frame_swap_ms"]
        )
        for run in measured_runs
        if run.timer_summary.get("open", {}).get(
            "dataset_load_start_to_open_first_frame_swap_ms"
        )
        is not None
    ]
    open_loadstart_to_render_complete = [
        float(
            run.timer_summary["open"]["dataset_load_start_to_open_render_complete_ms"]
        )
        for run in measured_runs
        if run.timer_summary.get("open", {}).get(
            "dataset_load_start_to_open_render_complete_ms"
        )
        is not None
    ]
    open_session_to_last_frame_swap = [
        float(run.timer_summary["open"]["session_to_open_last_frame_swap_ms"])
        for run in measured_runs
        if run.timer_summary.get("open", {}).get("session_to_open_last_frame_swap_ms")
        is not None
    ]
    open_session_to_screenshot_capture_upper_bound = [
        float(
            run.timer_summary["open"][
                "session_to_open_screenshot_capture_upper_bound_ms"
            ]
        )
        for run in measured_runs
        if run.timer_summary.get("open", {}).get(
            "session_to_open_screenshot_capture_upper_bound_ms"
        )
        is not None
    ]
    open_loadstart_to_screenshot_capture_upper_bound = [
        float(
            run.timer_summary["open"][
                "dataset_load_start_to_open_screenshot_capture_upper_bound_ms"
            ]
        )
        for run in measured_runs
        if run.timer_summary.get("open", {}).get(
            "dataset_load_start_to_open_screenshot_capture_upper_bound_ms"
        )
        is not None
    ]
    open_session_to_frame_settle = [
        float(run.timer_summary["open"]["session_to_open_frame_settle_ms"])
        for run in measured_runs
        if run.timer_summary.get("open", {}).get("session_to_open_frame_settle_ms")
        is not None
    ]
    open_action_render_wall = [
        float(
            run.timer_summary["open"][
                "open_action_render_wall_ms_excluding_quiet_and_capture"
            ]
        )
        for run in measured_runs
        if run.timer_summary.get("open", {}).get(
            "open_action_render_wall_ms_excluding_quiet_and_capture"
        )
        is not None
    ]

    actions_by_name: dict[str, list[dict[str, Any]]] = {}
    for run in measured_runs:
        for action in run.timer_summary.get("actions", []):
            actions_by_name.setdefault(str(action["name"]), []).append(action)

    aggregate = {
        "run_count": len(measured_runs),
        "open": {
            "dataset_load_ms": _stats(open_dataset_load),
            "layer_add_ms": _stats(open_layer_add),
            "open_action_render_wall_ms_excluding_quiet_and_capture": _stats(
                open_action_render_wall
            ),
            "session_to_open_render_complete_ms": _stats(
                open_session_to_render_complete
            ),
            "dataset_load_start_to_open_render_complete_ms": _stats(
                open_loadstart_to_render_complete
            ),
            "session_to_open_first_frame_swap_ms": _stats(
                open_session_to_first_frame_swap
            ),
            "dataset_load_start_to_open_first_frame_swap_ms": _stats(
                open_loadstart_to_first_frame_swap
            ),
            "session_to_open_last_frame_swap_ms": _stats(
                open_session_to_last_frame_swap
            ),
            "session_to_open_screenshot_capture_upper_bound_ms": _stats(
                open_session_to_screenshot_capture_upper_bound
            ),
            "dataset_load_start_to_open_screenshot_capture_upper_bound_ms": _stats(
                open_loadstart_to_screenshot_capture_upper_bound
            ),
            "session_to_open_frame_settle_ms": _stats(open_session_to_frame_settle),
            "full_run_wall_ms": _stats(total_wall_ms),
        },
        "actions": {},
        "runs": [
            {
                "label": run.label,
                "output_dir": str(run.output_dir),
                "elapsed_wall_seconds": run.elapsed_wall_seconds,
                "process_returncode": run.process_returncode,
            }
            for run in measured_runs
        ],
    }

    csv_rows: list[dict[str, Any]] = [
        {
            "metric_group": "open",
            "metric": "dataset_load_ms",
            **_stats(open_dataset_load),
        },
        {
            "metric_group": "open",
            "metric": "layer_add_ms",
            **_stats(open_layer_add),
        },
        {
            "metric_group": "open",
            "metric": "session_to_open_first_frame_swap_ms",
            **_stats(open_session_to_first_frame_swap),
        },
        {
            "metric_group": "open",
            "metric": "open_action_render_wall_ms_excluding_quiet_and_capture",
            **_stats(open_action_render_wall),
        },
        {
            "metric_group": "open",
            "metric": "session_to_open_render_complete_ms",
            **_stats(open_session_to_render_complete),
        },
        {
            "metric_group": "open",
            "metric": "dataset_load_start_to_open_render_complete_ms",
            **_stats(open_loadstart_to_render_complete),
        },
        {
            "metric_group": "open",
            "metric": "dataset_load_start_to_open_first_frame_swap_ms",
            **_stats(open_loadstart_to_first_frame_swap),
        },
        {
            "metric_group": "open",
            "metric": "session_to_open_last_frame_swap_ms",
            **_stats(open_session_to_last_frame_swap),
        },
        {
            "metric_group": "open",
            "metric": "session_to_open_screenshot_capture_upper_bound_ms",
            **_stats(open_session_to_screenshot_capture_upper_bound),
        },
        {
            "metric_group": "open",
            "metric": "dataset_load_start_to_open_screenshot_capture_upper_bound_ms",
            **_stats(open_loadstart_to_screenshot_capture_upper_bound),
        },
        {
            "metric_group": "open",
            "metric": "session_to_open_frame_settle_ms",
            **_stats(open_session_to_frame_settle),
        },
        {
            "metric_group": "open",
            "metric": "full_run_wall_ms",
            **_stats(total_wall_ms),
        },
    ]

    for action_name, records in sorted(actions_by_name.items()):
        totals = [float(record["action_total_ms"]) for record in records]
        render_totals = [
            float(record["action_render_wall_ms_excluding_quiet_and_capture"])
            for record in records
            if record.get("action_render_wall_ms_excluding_quiet_and_capture")
            is not None
        ]
        first_frame_swap_step_values = [
            float(step["first_frame_swap_sync_ms"])
            for record in records
            for step in record.get("step_metrics", [])
            if step.get("first_frame_swap_sync_ms") is not None
        ]
        last_frame_swap_step_values = [
            float(step["last_frame_swap_sync_ms"])
            for record in records
            for step in record.get("step_metrics", [])
            if step.get("last_frame_swap_sync_ms") is not None
        ]
        frame_settle_step_values = [
            float(step["frame_settle_sync_ms"])
            for record in records
            for step in record.get("step_metrics", [])
            if step.get("frame_settle_sync_ms") is not None
        ]
        screenshot_step_values = [
            float(step["screenshot_sync_ms"])
            for record in records
            for step in record.get("step_metrics", [])
            if step.get("screenshot_sync_ms") is not None
        ]
        screenshot_post_quiet_step_values = [
            float(step["screenshot_post_quiet_sync_ms"])
            for record in records
            for step in record.get("step_metrics", [])
            if step.get("screenshot_post_quiet_sync_ms") is not None
        ]
        screenshot_capture_upper_bound_step_values = [
            float(step["screenshot_capture_upper_bound_ms"])
            for record in records
            for step in record.get("step_metrics", [])
            if step.get("screenshot_capture_upper_bound_ms") is not None
        ]
        camera_apply_to_screenshot_capture_upper_bound_step_values = [
            float(step["camera_apply_to_screenshot_capture_upper_bound_ms"])
            for record in records
            for step in record.get("step_metrics", [])
            if step.get("camera_apply_to_screenshot_capture_upper_bound_ms") is not None
        ]
        client_wall_step_values = [
            float(step["client_wall_ms_from_step_start"])
            for record in records
            for step in record.get("step_metrics", [])
            if step.get("client_wall_ms_from_step_start") is not None
        ]
        render_wall_step_values = [
            float(step["render_wall_ms_excluding_quiet_and_capture"])
            for record in records
            for step in record.get("step_metrics", [])
            if step.get("render_wall_ms_excluding_quiet_and_capture") is not None
        ]
        aggregate["actions"][action_name] = {
            "action_total_ms": _stats(totals),
            "action_render_wall_ms_excluding_quiet_and_capture": _stats(render_totals),
            "step_first_frame_swap_sync_ms": _stats(first_frame_swap_step_values),
            "step_last_frame_swap_sync_ms": _stats(last_frame_swap_step_values),
            "step_frame_settle_sync_ms": _stats(frame_settle_step_values),
            "step_client_wall_ms_from_step_start": _stats(client_wall_step_values),
            "step_render_wall_ms_excluding_quiet_and_capture": _stats(
                render_wall_step_values
            ),
            "step_screenshot_capture_upper_bound_ms": _stats(
                screenshot_capture_upper_bound_step_values
            ),
            "step_camera_apply_to_screenshot_capture_upper_bound_ms": _stats(
                camera_apply_to_screenshot_capture_upper_bound_step_values
            ),
            "step_screenshot_post_quiet_sync_ms": _stats(
                screenshot_post_quiet_step_values
            ),
            "step_screenshot_sync_ms": _stats(screenshot_step_values),
        }
        csv_rows.append(
            {
                "metric_group": f"action:{action_name}",
                "metric": "action_total_ms",
                **_stats(totals),
            }
        )
        csv_rows.append(
            {
                "metric_group": f"action:{action_name}",
                "metric": "action_render_wall_ms_excluding_quiet_and_capture",
                **_stats(render_totals),
            }
        )
        csv_rows.append(
            {
                "metric_group": f"action:{action_name}",
                "metric": "step_first_frame_swap_sync_ms",
                **_stats(first_frame_swap_step_values),
            }
        )
        csv_rows.append(
            {
                "metric_group": f"action:{action_name}",
                "metric": "step_frame_settle_sync_ms",
                **_stats(frame_settle_step_values),
            }
        )
        csv_rows.append(
            {
                "metric_group": f"action:{action_name}",
                "metric": "step_last_frame_swap_sync_ms",
                **_stats(last_frame_swap_step_values),
            }
        )
        csv_rows.append(
            {
                "metric_group": f"action:{action_name}",
                "metric": "step_client_wall_ms_from_step_start",
                **_stats(client_wall_step_values),
            }
        )
        csv_rows.append(
            {
                "metric_group": f"action:{action_name}",
                "metric": "step_render_wall_ms_excluding_quiet_and_capture",
                **_stats(render_wall_step_values),
            }
        )
        csv_rows.append(
            {
                "metric_group": f"action:{action_name}",
                "metric": "step_screenshot_capture_upper_bound_ms",
                **_stats(screenshot_capture_upper_bound_step_values),
            }
        )
        csv_rows.append(
            {
                "metric_group": f"action:{action_name}",
                "metric": "step_camera_apply_to_screenshot_capture_upper_bound_ms",
                **_stats(camera_apply_to_screenshot_capture_upper_bound_step_values),
            }
        )
        csv_rows.append(
            {
                "metric_group": f"action:{action_name}",
                "metric": "step_screenshot_post_quiet_sync_ms",
                **_stats(screenshot_post_quiet_step_values),
            }
        )
        csv_rows.append(
            {
                "metric_group": f"action:{action_name}",
                "metric": "step_screenshot_sync_ms",
                **_stats(screenshot_step_values),
            }
        )

    (aggregate_dir / "summary.json").write_text(
        json.dumps(aggregate, indent=2) + "\n",
        encoding="utf-8",
    )

    csv_path = aggregate_dir / "summary.csv"
    with csv_path.open("w", encoding="utf-8", newline="") as stream:
        fieldnames = [
            "metric_group",
            "metric",
            "count",
            "min",
            "max",
            "mean",
            "median",
            "stdev",
            "p05",
            "p25",
            "p75",
            "p95",
        ]
        writer = csv.DictWriter(stream, fieldnames=fieldnames)
        writer.writeheader()
        for row in csv_rows:
            writer.writerow(row)

    lines = [
        "# napari deterministic benchmark",
        "",
        f"- Measured runs: `{len(measured_runs)}`",
        "",
        "- Screenshot-reference metrics labeled `*_screenshot_capture_upper_bound_ms` are intentionally conservative upper bounds.",
        "- They include the forced napari screenshot draw plus the `grabFramebuffer()` / `glReadPixels` readback barrier, which cannot be cleanly removed from the capture-complete timing.",
        "",
        "## Open",
        "",
        f"- dataset_load_ms mean: `{aggregate['open']['dataset_load_ms']['mean']}`",
        f"- layer_add_ms mean: `{aggregate['open']['layer_add_ms']['mean']}`",
        f"- open_action_render_wall_ms_excluding_quiet_and_capture mean: "
        f"`{aggregate['open']['open_action_render_wall_ms_excluding_quiet_and_capture']['mean']}`",
        f"- session_to_open_render_complete_ms mean: "
        f"`{aggregate['open']['session_to_open_render_complete_ms']['mean']}`",
        f"- dataset_load_start_to_open_render_complete_ms mean: "
        f"`{aggregate['open']['dataset_load_start_to_open_render_complete_ms']['mean']}`",
        f"- session_to_open_first_frame_swap_ms mean: "
        f"`{aggregate['open']['session_to_open_first_frame_swap_ms']['mean']}`",
        f"- dataset_load_start_to_open_first_frame_swap_ms mean: "
        f"`{aggregate['open']['dataset_load_start_to_open_first_frame_swap_ms']['mean']}`",
        f"- session_to_open_last_frame_swap_ms mean: "
        f"`{aggregate['open']['session_to_open_last_frame_swap_ms']['mean']}`",
        f"- session_to_open_screenshot_capture_upper_bound_ms mean: "
        f"`{aggregate['open']['session_to_open_screenshot_capture_upper_bound_ms']['mean']}`",
        f"- dataset_load_start_to_open_screenshot_capture_upper_bound_ms mean: "
        f"`{aggregate['open']['dataset_load_start_to_open_screenshot_capture_upper_bound_ms']['mean']}`",
        f"- session_to_open_frame_settle_ms mean: "
        f"`{aggregate['open']['session_to_open_frame_settle_ms']['mean']}`",
        f"- full_run_wall_ms mean: `{aggregate['open']['full_run_wall_ms']['mean']}`",
        "",
        "## Actions",
        "",
    ]
    for action_name, action_summary in sorted(aggregate["actions"].items()):
        lines.append(
            f"- `{action_name}` action_total_ms mean: `{action_summary['action_total_ms']['mean']}`"
        )
        lines.append(
            f"- `{action_name}` action_render_wall_ms_excluding_quiet_and_capture mean: "
            f"`{action_summary['action_render_wall_ms_excluding_quiet_and_capture']['mean']}`"
        )
        lines.append(
            f"- `{action_name}` step_first_frame_swap_sync_ms mean: "
            f"`{action_summary['step_first_frame_swap_sync_ms']['mean']}`"
        )
        lines.append(
            f"- `{action_name}` step_last_frame_swap_sync_ms mean: "
            f"`{action_summary['step_last_frame_swap_sync_ms']['mean']}`"
        )
        lines.append(
            f"- `{action_name}` step_frame_settle_sync_ms mean: "
            f"`{action_summary['step_frame_settle_sync_ms']['mean']}`"
        )
        lines.append(
            f"- `{action_name}` step_client_wall_ms_from_step_start mean: "
            f"`{action_summary['step_client_wall_ms_from_step_start']['mean']}`"
        )
        lines.append(
            f"- `{action_name}` step_render_wall_ms_excluding_quiet_and_capture mean: "
            f"`{action_summary['step_render_wall_ms_excluding_quiet_and_capture']['mean']}`"
        )
        lines.append(
            f"- `{action_name}` step_screenshot_capture_upper_bound_ms mean: "
            f"`{action_summary['step_screenshot_capture_upper_bound_ms']['mean']}`"
        )
        lines.append(
            f"- `{action_name}` step_camera_apply_to_screenshot_capture_upper_bound_ms mean: "
            f"`{action_summary['step_camera_apply_to_screenshot_capture_upper_bound_ms']['mean']}` "
            f"(upper bound: includes forced screenshot draw + readback barrier)"
        )
        lines.append(
            f"- `{action_name}` step_screenshot_post_quiet_sync_ms mean: "
            f"`{action_summary['step_screenshot_post_quiet_sync_ms']['mean']}`"
        )
        lines.append(
            f"- `{action_name}` step_screenshot_sync_ms mean: "
            f"`{action_summary['step_screenshot_sync_ms']['mean']}`"
        )
    lines.append("")
    (aggregate_dir / "summary.md").write_text(
        "\n".join(lines),
        encoding="utf-8",
    )


def main() -> int:
    args = _parse_args()
    output_root = Path(args.output_root).resolve()
    output_root.mkdir(parents=True, exist_ok=True)

    warmup_records: list[RunRecord] = []
    measured_records: list[RunRecord] = []

    for run_index in range(1, int(args.warmup_runs) + 1):
        label = f"warmup{run_index:02d}"
        record = _run_once(
            args=args,
            label=label,
            warmup=True,
            run_dir=output_root / "warmup" / label,
        )
        warmup_records.append(record)

    for run_index in range(1, int(args.measured_runs) + 1):
        label = f"run{run_index:02d}"
        record = _run_once(
            args=args,
            label=label,
            warmup=False,
            run_dir=output_root / "measured" / label,
        )
        measured_records.append(record)

    (output_root / "runs.json").write_text(
        json.dumps(
            {
                "warmup": [
                    {
                        "label": run.label,
                        "output_dir": str(run.output_dir),
                        "elapsed_wall_seconds": run.elapsed_wall_seconds,
                        "process_returncode": run.process_returncode,
                    }
                    for run in warmup_records
                ],
                "measured": [
                    {
                        "label": run.label,
                        "output_dir": str(run.output_dir),
                        "elapsed_wall_seconds": run.elapsed_wall_seconds,
                        "process_returncode": run.process_returncode,
                    }
                    for run in measured_records
                ],
            },
            indent=2,
        )
        + "\n",
        encoding="utf-8",
    )

    if measured_records:
        _write_aggregate(output_root=output_root, measured_runs=measured_records)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
