#!/usr/bin/env python3

from __future__ import annotations

import argparse
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


SCRIPT_DIR = Path(__file__).resolve().parent
HOME = Path.home()


@dataclass(frozen=True)
class RunRecord:
    label: str
    warmup: bool
    output_dir: Path
    elapsed_wall_seconds: float
    report: dict[str, Any]


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
    default_output_root = (
        HOME
        / "code"
        / "atlas"
        / "large_test_image"
        / "benchmarks"
        / f"napari_deterministic_screenvisible_{datetime.now().strftime('%Y%m%d_%H%M%S')}"
    )
    parser = argparse.ArgumentParser(
        description=(
            "Run the napari deterministic swap-vs-screen validator repeatedly, "
            "reusing one screenshot-reference pass and aggregating screen-visible "
            "first-display statistics."
        )
    )
    parser.add_argument("--dataset", required=True)
    parser.add_argument("--camera-spec", required=True)
    parser.add_argument("--output-root", default=str(default_output_root))
    parser.add_argument(
        "--validator-script",
        default=str(SCRIPT_DIR / "napari_swap_screen_validator.py"),
    )
    parser.add_argument(
        "--driver-script",
        default=str(SCRIPT_DIR / "napari_volume_benchmark.py"),
    )
    parser.add_argument(
        "--reference-dir",
        default=None,
        help="Existing reusable screenshot-reference directory. If omitted, build one once under output-root.",
    )
    parser.add_argument(
        "--napari-python",
        default=str(HOME / "miniconda3" / "envs" / "napari" / "bin" / "python"),
    )
    parser.add_argument(
        "--dataset-loader",
        choices=("zimg-array", "tifffile-array", "napari-open"),
        default="tifffile-array",
    )
    parser.add_argument("--open-plugin", default=None)
    parser.add_argument(
        "--scene-scale-zyx",
        type=float,
        nargs=3,
        metavar=("Z", "Y", "X"),
        default=None,
    )
    parser.add_argument("--rendering", default="mip")
    parser.add_argument("--contrast-limits-min", type=float, default=0.0)
    parser.add_argument("--contrast-limits-max", type=float, default=255.0)
    parser.add_argument("--process-events-count", type=int, default=12)
    parser.add_argument("--frame-settle-quiet-seconds", type=float, default=0.1)
    parser.add_argument("--draw-sync-timeout-seconds", type=float, default=10.0)
    parser.add_argument("--window-x", type=int, default=100)
    parser.add_argument("--window-y", type=int, default=95)
    parser.add_argument("--window-width", type=int, default=1400)
    parser.add_argument("--window-height", type=int, default=960)
    parser.add_argument(
        "--window-title-prefix",
        default="napari screenvisible batch",
    )
    parser.add_argument(
        "--capture-target",
        choices=("window", "display"),
        default="window",
    )
    parser.add_argument("--sample-hz", type=float, default=60.0)
    parser.add_argument("--pixel-threshold", type=float, default=0.0)
    parser.add_argument("--changed-fraction-threshold", type=float, default=0.0)
    parser.add_argument("--stable-frames", type=int, default=5)
    parser.add_argument("--capture-timeout-seconds", type=float, default=25.0)
    parser.add_argument("--capture-process-wait-seconds", type=float, default=300.0)
    parser.add_argument("--reference-timeout-seconds", type=float, default=300.0)
    parser.add_argument("--observed-timeout-seconds", type=float, default=300.0)
    parser.add_argument("--calibration-wait-timeout-seconds", type=float, default=120.0)
    parser.add_argument(
        "--analysis-region-norm",
        type=float,
        nargs=4,
        metavar=("X", "Y", "W", "H"),
        default=(0.3, 0.3, 0.4, 0.4),
    )
    parser.add_argument("--warmup-runs", type=int, default=1)
    parser.add_argument("--measured-runs", type=int, default=7)
    return parser.parse_args()


def _run_logged(*, cmd: list[str], log_path: Path, timeout_seconds: float) -> None:
    log_path.parent.mkdir(parents=True, exist_ok=True)
    with log_path.open("w", encoding="utf-8") as log_stream:
        completed = subprocess.run(
            cmd,
            stdout=log_stream,
            stderr=subprocess.STDOUT,
            text=True,
            timeout=timeout_seconds,
            check=False,
        )
    if completed.returncode != 0:
        tail = "\n".join(
            log_path.read_text(encoding="utf-8", errors="replace").splitlines()[-120:]
        )
        raise RuntimeError(
            f"command failed with exit code {completed.returncode}: {' '.join(cmd)}\n{tail}"
        )


def _reference_command(args: argparse.Namespace, reference_dir: Path) -> list[str]:
    cmd = [
        str(Path(args.napari_python).resolve()),
        str(Path(args.driver_script).resolve()),
        "--dataset",
        str(Path(args.dataset).resolve()),
        "--camera-spec",
        str(Path(args.camera_spec).resolve()),
        "--dataset-loader",
        str(args.dataset_loader),
        "--output-dir",
        str(reference_dir),
        "--rendering",
        str(args.rendering),
        "--contrast-limits-min",
        str(args.contrast_limits_min),
        "--contrast-limits-max",
        str(args.contrast_limits_max),
        "--process-events-count",
        str(args.process_events_count),
        "--window-title",
        f"{args.window_title_prefix} reference",
        "--window-x",
        str(args.window_x),
        "--window-y",
        str(args.window_y),
        "--window-width",
        str(args.window_width),
        "--window-height",
        str(args.window_height),
        "--screenshot-reference-every-step",
        "--save-screenshot-reference-images",
    ]
    if args.open_plugin:
        cmd.extend(["--open-plugin", str(args.open_plugin)])
    if args.scene_scale_zyx is not None:
        cmd.extend(
            ["--scene-scale-zyx", *(str(float(v)) for v in args.scene_scale_zyx)]
        )
    return cmd


def _validator_command(
    args: argparse.Namespace,
    *,
    reference_dir: Path,
    output_dir: Path,
    run_title_prefix: str,
) -> list[str]:
    cmd = [
        sys.executable,
        str(Path(args.validator_script).resolve()),
        "--dataset",
        str(Path(args.dataset).resolve()),
        "--camera-spec",
        str(Path(args.camera_spec).resolve()),
        "--output-root",
        str(output_dir),
        "--reference-dir",
        str(reference_dir),
        "--napari-python",
        str(Path(args.napari_python).resolve()),
        "--dataset-loader",
        str(args.dataset_loader),
        "--rendering",
        str(args.rendering),
        "--contrast-limits-min",
        str(args.contrast_limits_min),
        "--contrast-limits-max",
        str(args.contrast_limits_max),
        "--process-events-count",
        str(args.process_events_count),
        "--frame-settle-quiet-seconds",
        str(args.frame_settle_quiet_seconds),
        "--draw-sync-timeout-seconds",
        str(args.draw_sync_timeout_seconds),
        "--window-x",
        str(args.window_x),
        "--window-y",
        str(args.window_y),
        "--window-width",
        str(args.window_width),
        "--window-height",
        str(args.window_height),
        "--window-title-prefix",
        run_title_prefix,
        "--capture-target",
        str(args.capture_target),
        "--sample-hz",
        str(args.sample_hz),
        "--pixel-threshold",
        str(args.pixel_threshold),
        "--changed-fraction-threshold",
        str(args.changed_fraction_threshold),
        "--stable-frames",
        str(args.stable_frames),
        "--capture-timeout-seconds",
        str(args.capture_timeout_seconds),
        "--capture-process-wait-seconds",
        str(args.capture_process_wait_seconds),
        "--reference-timeout-seconds",
        str(args.reference_timeout_seconds),
        "--observed-timeout-seconds",
        str(args.observed_timeout_seconds),
        "--calibration-wait-timeout-seconds",
        str(args.calibration_wait_timeout_seconds),
        "--analysis-region-norm",
        *(str(float(v)) for v in args.analysis_region_norm),
    ]
    if args.open_plugin:
        cmd.extend(["--open-plugin", str(args.open_plugin)])
    if args.scene_scale_zyx is not None:
        cmd.extend(
            ["--scene-scale-zyx", *(str(float(v)) for v in args.scene_scale_zyx)]
        )
    return cmd


def _ensure_reference(args: argparse.Namespace, output_root: Path) -> Path:
    reference_dir = (
        Path(args.reference_dir).resolve()
        if args.reference_dir is not None
        else output_root / "reference"
    )
    if (reference_dir / "napari_timer_summary.json").exists():
        return reference_dir
    reference_dir.mkdir(parents=True, exist_ok=True)
    command = _reference_command(args, reference_dir)
    (reference_dir / "command.json").write_text(
        json.dumps(command, indent=2) + "\n",
        encoding="utf-8",
    )
    _run_logged(
        cmd=command,
        log_path=reference_dir / "reference.log",
        timeout_seconds=float(args.reference_timeout_seconds),
    )
    return reference_dir


def _run_once(
    *,
    args: argparse.Namespace,
    reference_dir: Path,
    label: str,
    warmup: bool,
    output_dir: Path,
) -> RunRecord:
    output_dir.mkdir(parents=True, exist_ok=True)
    command = _validator_command(
        args,
        reference_dir=reference_dir,
        output_dir=output_dir,
        run_title_prefix=f"{args.window_title_prefix} {label}",
    )
    (output_dir / "command.json").write_text(
        json.dumps(command, indent=2) + "\n",
        encoding="utf-8",
    )
    started = time.monotonic()
    _run_logged(
        cmd=command,
        log_path=output_dir / "batch_driver.log",
        timeout_seconds=max(
            float(args.capture_process_wait_seconds),
            float(args.observed_timeout_seconds),
        )
        + 60.0,
    )
    elapsed = time.monotonic() - started
    report_path = output_dir / "analysis" / "swap_screen_match_report.json"
    if not report_path.exists():
        raise RuntimeError(f"missing validator report: {report_path}")
    report = json.loads(report_path.read_text(encoding="utf-8"))
    return RunRecord(
        label=label,
        warmup=warmup,
        output_dir=output_dir,
        elapsed_wall_seconds=elapsed,
        report=report,
    )


def _aggregate_runs(
    output_root: Path,
    args: argparse.Namespace,
    runs: list[RunRecord],
    reference_dir: Path,
) -> None:
    aggregate_dir = output_root / "aggregate"
    aggregate_dir.mkdir(parents=True, exist_ok=True)
    measured_runs = [run for run in runs if not run.warmup]

    actions_summary: dict[str, Any] = {}
    action_names = sorted(
        {
            str(action["action"])
            for run in measured_runs
            for action in run.report.get("actions", [])
        }
    )

    for action_name in action_names:
        per_run_entries = [
            action
            for run in measured_runs
            for action in run.report.get("actions", [])
            if str(action["action"]) == action_name
        ]
        matched_steps = [
            step
            for action in per_run_entries
            for step in action.get("matched_steps", [])
        ]
        actions_summary[action_name] = {
            "displayed_unique_step_count_per_run": _stats(
                [
                    float(action.get("displayed_unique_step_count") or 0.0)
                    for action in per_run_entries
                ]
            ),
            "observed_changed_frame_count_per_run": _stats(
                [
                    float(action.get("observed_changed_frame_count") or 0.0)
                    for action in per_run_entries
                ]
            ),
            "first_display_ms_from_step_start": _stats(
                [
                    float(step["first_display_ms_from_step_start"])
                    for step in matched_steps
                ]
            ),
            "last_display_ms_from_step_start": _stats(
                [
                    float(step["last_display_ms_from_step_start"])
                    for step in matched_steps
                ]
            ),
            "matched_frame_count": _stats(
                [float(step["matched_frame_count"]) for step in matched_steps]
            ),
            "first_similarity": _stats(
                [float(step["first_similarity"]) for step in matched_steps]
            ),
            "last_similarity": _stats(
                [float(step["last_similarity"]) for step in matched_steps]
            ),
            "delta_from_last_frame_swap_ms": _stats(
                [
                    float(step["delta_from_last_frame_swap_ms"])
                    for step in matched_steps
                    if step.get("delta_from_last_frame_swap_ms") is not None
                ]
            ),
            "per_run_mean_first_display_ms_from_step_start": _stats(
                [
                    float(action["display_ms_from_step_start_stats"]["mean"])
                    for action in per_run_entries
                    if action.get("display_ms_from_step_start_stats", {}).get("mean")
                    is not None
                ]
            ),
            "per_run_mean_similarity": _stats(
                [
                    float(action["similarity_stats"]["mean"])
                    for action in per_run_entries
                    if action.get("similarity_stats", {}).get("mean") is not None
                ]
            ),
        }

    summary = {
        "reference_dir": str(reference_dir),
        "output_root": str(output_root),
        "warmup_runs": args.warmup_runs,
        "measured_runs": args.measured_runs,
        "analysis_region_norm": {
            "x": float(args.analysis_region_norm[0]),
            "y": float(args.analysis_region_norm[1]),
            "width": float(args.analysis_region_norm[2]),
            "height": float(args.analysis_region_norm[3]),
        },
        "runs": [
            {
                "label": run.label,
                "warmup": run.warmup,
                "output_dir": str(run.output_dir),
                "elapsed_wall_seconds": run.elapsed_wall_seconds,
            }
            for run in runs
        ],
        "actions": actions_summary,
    }
    (aggregate_dir / "summary.json").write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )

    lines = [
        "# Napari Deterministic Screen-Visible Batch",
        "",
        f"- Reference dir: `{reference_dir}`",
        f"- Warm-up runs: `{args.warmup_runs}`",
        f"- Measured runs: `{args.measured_runs}`",
        "",
    ]
    for action_name in action_names:
        action = actions_summary[action_name]
        first_mean = action["first_display_ms_from_step_start"]["mean"]
        sim_mean = action["first_similarity"]["mean"]
        delta_mean = action["delta_from_last_frame_swap_ms"]["mean"]
        lines.extend(
            [
                f"## {action_name}",
                "",
                (
                    f"- First display from step start mean: `{first_mean:.3f} ms`"
                    if first_mean is not None
                    else "- First display from step start mean: `n/a`"
                ),
                (
                    f"- First similarity mean: `{sim_mean:.6f}`"
                    if sim_mean is not None
                    else "- First similarity mean: `n/a`"
                ),
                (
                    f"- Delta from last frame swap mean: `{delta_mean:.3f} ms`"
                    if delta_mean is not None
                    else "- Delta from last frame swap mean: `n/a`"
                ),
                "",
            ]
        )
    (aggregate_dir / "summary.md").write_text("\n".join(lines), encoding="utf-8")


def main() -> int:
    args = _parse_args()
    output_root = Path(args.output_root).resolve()
    output_root.mkdir(parents=True, exist_ok=True)
    reference_dir = _ensure_reference(args, output_root)

    records: list[RunRecord] = []
    for run_index in range(1, int(args.warmup_runs) + 1):
        label = f"warmup/run{run_index:02d}"
        records.append(
            _run_once(
                args=args,
                reference_dir=reference_dir,
                label=label,
                warmup=True,
                output_dir=output_root / "warmup" / f"run{run_index:02d}",
            )
        )
    for run_index in range(1, int(args.measured_runs) + 1):
        label = f"measured/run{run_index:02d}"
        records.append(
            _run_once(
                args=args,
                reference_dir=reference_dir,
                label=label,
                warmup=False,
                output_dir=output_root / "measured" / f"run{run_index:02d}",
            )
        )

    _aggregate_runs(output_root, args, records, reference_dir)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
