#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
import os
import signal
import statistics
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from macos_gui_drag_benchmark import _list_windows


SCRIPT_DIR = Path(__file__).resolve().parent
PREP_SCRIPT = SCRIPT_DIR / "prepare_napari_gui_benchmark.py"
INJECT_SCRIPT = SCRIPT_DIR / "macos_gui_drag_benchmark.py"
SUMMARIZE_SCRIPT = SCRIPT_DIR / "summarize_gui_capture_fps.py"
BUILD_CAPTURE_SCRIPT = SCRIPT_DIR / "build_macos_window_capture_sckit.sh"
HOME = Path.home()


@dataclass(frozen=True)
class RunArtifacts:
    kind: str
    run_index: int
    run_dir: Path
    gui_summary_path: Path
    capture_summary_path: Path


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Run repeated real-GUI napari rotate benchmarks with ScreenCaptureKit "
            "capture and Quartz mouse drag injection."
        )
    )
    parser.add_argument("--dataset", required=True)
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
    parser.add_argument("--camera-spec", required=True)
    parser.add_argument(
        "--calibration",
        default=None,
        help=(
            "Optional GUI drag calibration JSON. If omitted, each prepared napari "
            "run uses the auto-generated prep calibration."
        ),
    )
    parser.add_argument("--output-root", required=True)
    parser.add_argument(
        "--napari-python",
        default=str(HOME / "miniconda3" / "envs" / "napari" / "bin" / "python"),
        help="Python executable inside the dedicated napari environment.",
    )
    parser.add_argument("--warmup-runs", type=int, default=1)
    parser.add_argument("--measured-runs", type=int, default=7)
    parser.add_argument("--window-x", type=int, default=100)
    parser.add_argument("--window-y", type=int, default=95)
    parser.add_argument("--window-width", type=int, default=1400)
    parser.add_argument("--window-height", type=int, default=960)
    parser.add_argument("--window-title", default="napari GUI Benchmark")
    parser.add_argument(
        "--capture-target",
        choices=("window", "display"),
        default="display",
        help=(
            "ScreenCaptureKit target type. 'display' captures the containing "
            "display and crops to the napari canvas via sourceRect."
        ),
    )
    parser.add_argument("--sample-hz", type=float, default=60.0)
    parser.add_argument("--pixel-threshold", type=float, default=0.0)
    parser.add_argument("--changed-fraction-threshold", type=float, default=0.0)
    parser.add_argument("--stable-frames", type=int, default=5)
    parser.add_argument("--capture-timeout-seconds", type=float, default=25.0)
    parser.add_argument(
        "--capture-process-wait-seconds",
        type=float,
        default=300.0,
        help=(
            "Maximum wall time to wait for the ScreenCaptureKit helper process, "
            "including post-capture exact-pixel analysis."
        ),
    )
    parser.add_argument("--prepare-timeout-seconds", type=float, default=120.0)
    parser.add_argument("--activate-delay-seconds", type=float, default=0.5)
    parser.add_argument("--initial-delay-seconds", type=float, default=0.5)
    parser.add_argument("--rendering", default="mip")
    parser.add_argument("--contrast-limits-min", type=float, default=0.0)
    parser.add_argument("--contrast-limits-max", type=float, default=255.0)
    parser.add_argument("--process-events-count", type=int, default=12)
    parser.add_argument(
        "--show-layer-docks",
        action="store_true",
        help="Keep the napari layer list and layer controls visible during capture.",
    )
    parser.add_argument(
        "--show-status-bar",
        action="store_true",
        help="Keep the napari status bar visible during capture.",
    )
    return parser.parse_args()


def _stats(values: list[float]) -> dict[str, Any]:
    if not values:
        return {
            "count": 0,
            "mean": None,
            "median": None,
            "std": None,
            "min": None,
            "max": None,
            "p95": None,
        }
    ordered = sorted(values)
    count = len(ordered)

    def percentile(q: float) -> float:
        if count == 1:
            return ordered[0]
        pos = q * (count - 1)
        lower = int(pos)
        upper = min(lower + 1, count - 1)
        if upper == lower:
            return ordered[lower]
        frac = pos - lower
        return ordered[lower] + (ordered[upper] - ordered[lower]) * frac

    return {
        "count": count,
        "mean": sum(ordered) / count,
        "median": percentile(0.5),
        "std": statistics.pstdev(ordered) if count > 1 else 0.0,
        "min": ordered[0],
        "max": ordered[-1],
        "p95": percentile(0.95),
    }


def _tail(path: Path, lines: int = 80) -> str:
    if not path.exists():
        return ""
    content = path.read_text(encoding="utf-8", errors="replace").splitlines()
    return "\n".join(content[-lines:])


def _wait_for_window(
    owner_name: str,
    title_substring: str,
    timeout_seconds: float,
) -> dict[str, Any]:
    deadline = time.monotonic() + timeout_seconds
    owner_match = owner_name.lower()
    title_match = title_substring.lower()
    while time.monotonic() < deadline:
        for window in _list_windows():
            if owner_match and owner_match not in window.owner_name.lower():
                continue
            if title_match and title_match not in window.title.lower():
                continue
            return window.to_json()
        time.sleep(0.2)
    raise RuntimeError(
        f"timed out waiting for window owner={owner_name!r} title~={title_substring!r}"
    )


def _wait_for_window_bounds(
    *,
    owner_name: str,
    title_substring: str,
    x: int,
    y: int,
    width: int,
    height: int,
    timeout_seconds: float,
    position_tolerance: float = 200.0,
    minimum_width: float = 1000.0,
    minimum_height: float = 750.0,
) -> dict[str, Any]:
    deadline = time.monotonic() + timeout_seconds
    while time.monotonic() < deadline:
        window = _wait_for_window(owner_name, title_substring, 1.0)
        bounds = window.get("bounds") or {}
        actual_x = float(bounds.get("x", 0.0))
        actual_y = float(bounds.get("y", 0.0))
        actual_width = float(bounds.get("width", 0.0))
        actual_height = float(bounds.get("height", 0.0))
        if (
            abs(actual_x - x) <= position_tolerance
            and abs(actual_y - y) <= position_tolerance
            and actual_width >= minimum_width
            and actual_height >= minimum_height
        ):
            return window
        time.sleep(0.2)
    raise RuntimeError(
        f"timed out waiting for napari window near ({x}, {y}) with size at least "
        f"({minimum_width}, {minimum_height})"
    )


def _terminate_process(
    process: subprocess.Popen[str], timeout_seconds: float = 10.0
) -> None:
    if process.poll() is not None:
        return
    process.terminate()
    try:
        process.wait(timeout=timeout_seconds)
        return
    except subprocess.TimeoutExpired:
        pass
    process.kill()
    try:
        process.wait(timeout=timeout_seconds)
    except subprocess.TimeoutExpired:
        os.kill(process.pid, signal.SIGKILL)


def _launch_napari(
    *,
    args: argparse.Namespace,
    run_dir: Path,
) -> tuple[subprocess.Popen[str], Path, Path, Path]:
    prep_dir = run_dir / "prep"
    prep_dir.mkdir(parents=True, exist_ok=True)
    launch_log_path = run_dir / "napari_launch.log"
    launch_log = launch_log_path.open("w", encoding="utf-8")
    cmd = [
        str(Path(args.napari_python).resolve()),
        str(PREP_SCRIPT),
        "--dataset",
        str(Path(args.dataset).resolve()),
        "--dataset-loader",
        str(args.dataset_loader),
        "--camera-spec",
        str(Path(args.camera_spec).resolve()),
        "--output-dir",
        str(prep_dir),
        "--window-x",
        str(args.window_x),
        "--window-y",
        str(args.window_y),
        "--window-width",
        str(args.window_width),
        "--window-height",
        str(args.window_height),
        "--window-title",
        str(args.window_title),
        "--rendering",
        str(args.rendering),
        "--contrast-limits-min",
        str(args.contrast_limits_min),
        "--contrast-limits-max",
        str(args.contrast_limits_max),
        "--process-events-count",
        str(args.process_events_count),
    ]
    if args.open_plugin:
        cmd.extend(["--open-plugin", str(args.open_plugin)])
    if args.scene_scale_zyx is not None:
        cmd.extend(
            ["--scene-scale-zyx", *(str(float(x)) for x in args.scene_scale_zyx)]
        )
    if args.show_layer_docks:
        cmd.append("--show-layer-docks")
    if args.show_status_bar:
        cmd.append("--show-status-bar")
    process = subprocess.Popen(
        cmd,
        stdout=launch_log,
        stderr=subprocess.STDOUT,
        text=True,
    )
    return (
        process,
        prep_dir / "napari_gui_prepare_status.json",
        prep_dir / "napari_gui_prepare_error.json",
        launch_log_path,
    )


def _run_capture_and_injection(
    *,
    capture_binary: str,
    calibration_path: Path,
    run_dir: Path,
    args: argparse.Namespace,
) -> None:
    capture_summary_path = run_dir / "capture_summary.json"
    frames_path = run_dir / "capture_summary_frames.jsonl"
    events_path = run_dir / "gui_events.jsonl"
    capture_log_path = run_dir / "capture_helper.log"
    capture_log = capture_log_path.open("w", encoding="utf-8")
    capture_process = subprocess.Popen(
        [
            capture_binary,
            "--calibration",
            str(calibration_path),
            "--events",
            str(events_path),
            "--output",
            str(capture_summary_path),
            "--frames-output",
            str(frames_path),
            "--sample-hz",
            str(args.sample_hz),
            "--capture-target",
            str(args.capture_target),
            "--pixel-threshold",
            str(args.pixel_threshold),
            "--changed-fraction-threshold",
            str(args.changed_fraction_threshold),
            "--stable-frames",
            str(args.stable_frames),
            "--timeout-seconds",
            str(args.capture_timeout_seconds),
        ],
        stdout=capture_log,
        stderr=subprocess.STDOUT,
        text=True,
    )
    try:
        subprocess.run(
            [
                sys.executable,
                str(INJECT_SCRIPT),
                "--calibration",
                str(calibration_path),
                "--output-dir",
                str(run_dir),
                "--action",
                "rotate",
                "--activate-delay-seconds",
                str(args.activate_delay_seconds),
                "--initial-delay-seconds",
                str(args.initial_delay_seconds),
            ],
            check=True,
            text=True,
        )
        capture_process.wait(timeout=max(30.0, args.capture_process_wait_seconds))
    finally:
        if capture_process.poll() is None:
            capture_process.terminate()
            try:
                capture_process.wait(timeout=5.0)
            except subprocess.TimeoutExpired:
                capture_process.kill()
                capture_process.wait(timeout=5.0)
        capture_log.close()
    if capture_process.returncode != 0:
        raise RuntimeError(
            f"capture helper failed with exit code {capture_process.returncode}.\n"
            f"{_tail(capture_log_path)}"
        )


def _summarize_run(run_dir: Path) -> Path:
    output_path = run_dir / "gui_fps_summary.json"
    subprocess.run(
        [
            sys.executable,
            str(SUMMARIZE_SCRIPT),
            "--events",
            str(run_dir / "gui_events.jsonl"),
            "--frames",
            str(run_dir / "capture_summary_frames.jsonl"),
            "--capture-summary",
            str(run_dir / "capture_summary.json"),
            "--output",
            str(output_path),
        ],
        check=True,
        text=True,
    )
    return output_path


def _load_rotate_metrics(summary_path: Path) -> dict[str, Any]:
    summary = json.loads(summary_path.read_text(encoding="utf-8"))
    actions = summary.get("actions")
    if not isinstance(actions, list):
        raise RuntimeError(f"invalid GUI summary at {summary_path}")
    for action in actions:
        if action.get("action") == "rotate":
            return action
    raise RuntimeError(f"rotate action not found in {summary_path}")


def _aggregate_runs(artifacts: list[RunArtifacts], output_dir: Path) -> None:
    measured = [artifact for artifact in artifacts if artifact.kind == "measured"]
    metrics_by_name: dict[str, list[float]] = {
        "changed_samples_per_second": [],
        "changed_sample_count": [],
        "visible_fps_from_mean_interval": [],
        "capture_first_visible_ms_from_start": [],
        "capture_stable_ms_from_start": [],
        "capture_stable_ms_from_end": [],
        "duration_ms": [],
        "observed_sample_hz": [],
    }
    per_run: list[dict[str, Any]] = []
    for artifact in measured:
        gui_summary = json.loads(artifact.gui_summary_path.read_text(encoding="utf-8"))
        rotate = _load_rotate_metrics(artifact.gui_summary_path)
        per_run.append(
            {
                "run_index": artifact.run_index,
                "run_dir": str(artifact.run_dir),
                "metrics": rotate,
            }
        )
        for name in metrics_by_name:
            if name == "observed_sample_hz":
                value = gui_summary.get("observed_sample_hz")
            else:
                value = rotate.get(name)
            if value is not None:
                metrics_by_name[name].append(float(value))

    aggregate = {
        "measured_run_count": len(measured),
        "metrics": {name: _stats(values) for name, values in metrics_by_name.items()},
        "per_run": per_run,
    }
    output_dir.mkdir(parents=True, exist_ok=True)
    summary_json_path = output_dir / "summary.json"
    summary_json_path.write_text(
        json.dumps(aggregate, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )

    lines = [
        "# Napari GUI Rotate Benchmark Summary",
        "",
        f"Measured runs: {len(measured)}",
        "",
        "| Metric | Count | Mean | Median | Std | Min | Max | P95 |",
        "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |",
    ]
    for name, stats in aggregate["metrics"].items():
        lines.append(
            "| {name} | {count} | {mean} | {median} | {std} | {min} | {max} | {p95} |".format(
                name=name,
                count=stats["count"],
                mean="null" if stats["mean"] is None else f"{stats['mean']:.3f}",
                median="null" if stats["median"] is None else f"{stats['median']:.3f}",
                std="null" if stats["std"] is None else f"{stats['std']:.3f}",
                min="null" if stats["min"] is None else f"{stats['min']:.3f}",
                max="null" if stats["max"] is None else f"{stats['max']:.3f}",
                p95="null" if stats["p95"] is None else f"{stats['p95']:.3f}",
            )
        )
    (output_dir / "summary.md").write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> int:
    args = _parse_args()
    output_root = Path(args.output_root).resolve()
    output_root.mkdir(parents=True, exist_ok=True)
    static_calibration_path = (
        Path(args.calibration).resolve() if args.calibration is not None else None
    )
    capture_binary = subprocess.check_output(
        [str(BUILD_CAPTURE_SCRIPT)], text=True
    ).strip()

    run_plan: list[tuple[str, int]] = []
    run_plan.extend(("warmup", index) for index in range(1, args.warmup_runs + 1))
    run_plan.extend(("measured", index) for index in range(1, args.measured_runs + 1))
    artifacts: list[RunArtifacts] = []

    for kind, run_index in run_plan:
        run_dir = output_root / kind / f"run{run_index:02d}"
        run_dir.mkdir(parents=True, exist_ok=True)
        process = None
        try:
            process, status_path, error_path, log_path = _launch_napari(
                args=args,
                run_dir=run_dir,
            )
            deadline = time.monotonic() + args.prepare_timeout_seconds
            ready = False
            while time.monotonic() < deadline:
                if error_path.exists():
                    raise RuntimeError(
                        f"napari prep failed for {kind} run {run_index}: "
                        f"{error_path.read_text(encoding='utf-8', errors='replace')}"
                    )
                if status_path.exists():
                    ready = True
                    break
                if process.poll() is not None:
                    raise RuntimeError(
                        f"napari exited before prep completed for {kind} run {run_index}.\n"
                        f"{_tail(log_path)}"
                    )
                time.sleep(0.2)
            if not ready:
                raise RuntimeError(
                    f"timed out waiting for napari prep on {kind} run {run_index}.\n"
                    f"{_tail(log_path)}"
                )
            prep_status = json.loads(status_path.read_text(encoding="utf-8"))
            calibration_path = static_calibration_path
            if calibration_path is None:
                calibration_raw = prep_status.get("calibration_path")
                if not isinstance(calibration_raw, str) or not calibration_raw.strip():
                    raise RuntimeError(
                        f"napari prep did not report a calibration path for {kind} run "
                        f"{run_index}.\n{json.dumps(prep_status, indent=2, sort_keys=True)}"
                    )
                calibration_path = Path(calibration_raw).resolve()
            if not calibration_path.exists():
                raise RuntimeError(
                    f"missing napari calibration file for {kind} run {run_index}: "
                    f"{calibration_path}"
                )

            window = _wait_for_window("", args.window_title, 20.0)
            owner_name = str(window.get("owner_name") or "")
            _wait_for_window_bounds(
                owner_name=owner_name,
                title_substring=args.window_title,
                x=args.window_x,
                y=args.window_y,
                width=args.window_width,
                height=args.window_height,
                timeout_seconds=10.0,
            )
            _run_capture_and_injection(
                capture_binary=capture_binary,
                calibration_path=calibration_path,
                run_dir=run_dir,
                args=args,
            )
            gui_summary_path = _summarize_run(run_dir)
            artifacts.append(
                RunArtifacts(
                    kind=kind,
                    run_index=run_index,
                    run_dir=run_dir,
                    gui_summary_path=gui_summary_path,
                    capture_summary_path=run_dir / "capture_summary.json",
                )
            )
        finally:
            if process is not None:
                _terminate_process(process)
            time.sleep(1.0)

    _aggregate_runs(artifacts, output_root / "aggregate")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
