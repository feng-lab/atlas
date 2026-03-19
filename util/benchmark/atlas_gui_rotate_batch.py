#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
import os
import signal
import socket
import statistics
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any

REPO_ROOT = Path(__file__).resolve().parents[2]
ATLAS_AGENT_SRC = REPO_ROOT / "python" / "atlas_agent" / "src"
if str(ATLAS_AGENT_SRC) not in sys.path:
    sys.path.insert(0, str(ATLAS_AGENT_SRC))

from atlas_agent.scene_rpc import SceneClient

from atlas_volume_benchmark import (
    AXIS_SCOPE_ID,
    BACKGROUND_SCOPE_ID,
    NO_BOUND_BOX_VALUE,
    AtlasRenderLogFollower,
    _apply_camera,
    _enable_full_resolution,
    _loaded_ids,
    _set_bound_box_mode,
    _set_canvas_size,
    _set_compositing_mode,
    _set_scope_bool_param,
    _wait_ready,
    SHOW_AXIS_JSON_KEY,
    SHOW_BACKGROUND_JSON_KEY,
)
from macos_gui_drag_benchmark import _list_windows
from volume_benchmark_common import load_benchmark_spec


SCRIPT_DIR = Path(__file__).resolve().parent
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
            "Run repeated real-GUI Atlas rotate benchmarks with ScreenCaptureKit "
            "capture and Quartz mouse drag injection."
        )
    )
    parser.add_argument(
        "--dataset",
        default=str(
            HOME
            / "Dropbox"
            / "atlas_test"
            / "slice15_paraview"
            / "slice15_ch2_dense.nim"
        ),
    )
    parser.add_argument(
        "--camera-spec",
        default=str(
            HOME
            / "Dropbox"
            / "atlas_test"
            / "slice15_paraview"
            / "slice15_scene_camera_exact_2000x1500.json"
        ),
    )
    parser.add_argument("--calibration", required=True)
    parser.add_argument("--output-root", required=True)
    parser.add_argument(
        "--atlas-binary",
        default=str(
            REPO_ROOT
            / "build"
            / "Release"
            / "src"
            / "atlas"
            / "Atlas.app"
            / "Contents"
            / "MacOS"
            / "Atlas"
        ),
    )
    parser.add_argument("--address", default="localhost:50051")
    parser.add_argument(
        "--atlas-log-path", default=str(HOME / "Library" / "Logs" / "Atlas")
    )
    parser.add_argument("--warmup-runs", type=int, default=1)
    parser.add_argument("--measured-runs", type=int, default=7)
    parser.add_argument("--window-x", type=int, default=100)
    parser.add_argument("--window-y", type=int, default=95)
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
    parser.add_argument("--launch-timeout-seconds", type=float, default=120.0)
    parser.add_argument("--preview-timeout-seconds", type=float, default=30.0)
    parser.add_argument("--final-timeout-seconds", type=float, default=30.0)
    parser.add_argument("--activate-delay-seconds", type=float, default=0.5)
    parser.add_argument("--initial-delay-seconds", type=float, default=0.5)
    parser.add_argument("--canvas-logical-width", type=int, default=1000)
    parser.add_argument("--canvas-logical-height", type=int, default=750)
    parser.add_argument(
        "--compositing-mode",
        default="Maximum Intensity Projection",
        help="Atlas image compositing mode, for example 'Maximum Intensity Projection'.",
    )
    parser.add_argument(
        "--disable-full-resolution",
        action="store_true",
        help="Do not enable Atlas full-resolution rendering after load.",
    )
    parser.add_argument(
        "--hide-background",
        action="store_true",
        default=True,
        help="Hide the Atlas background scope for parity with ParaView captures.",
    )
    parser.add_argument(
        "--hide-axis",
        action="store_true",
        default=True,
        help="Hide the Atlas axis scope for parity with ParaView captures.",
    )
    parser.add_argument(
        "--hide-bound-box",
        action="store_true",
        default=True,
        help="Set loaded Atlas image objects to 'No Bound Box' during the GUI benchmark.",
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
    owner_name: str, title_substring: str, timeout_seconds: float
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


def _move_atlas_window_with_system_events(*, x: int, y: int) -> None:
    script = f"""
tell application "System Events"
  tell application process "Atlas"
    set frontmost to true
    repeat with w in windows
      try
        set windowName to name of w
      on error
        set windowName to ""
      end try
      if windowName contains "3D View" then
        set position of w to {{{x}, {y}}}
        exit repeat
      end if
    end repeat
  end tell
end tell
"""
    subprocess.run(["osascript", "-e", script], check=True, text=True)


def _wait_for_window_bounds(
    owner_name: str,
    title_substring: str,
    *,
    x: int,
    y: int,
    timeout_seconds: float,
    position_tolerance: float = 200.0,
    minimum_width: float = 1200.0,
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
        f"timed out waiting for {owner_name!r} window on the target screen near "
        f"({x}, {y}) with size at least ({minimum_width}, {minimum_height})"
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


def _wait_for_rpc_ready(client: SceneClient, timeout_seconds: float) -> None:
    deadline = time.monotonic() + timeout_seconds
    last_error = ""
    while time.monotonic() < deadline:
        try:
            if client.ensure_view(require=False):
                return
        except Exception as exc:  # pragma: no cover - best effort polling
            last_error = str(exc)
        time.sleep(0.25)
    raise RuntimeError(
        f"timed out waiting for Atlas RPC/3D view readiness after {timeout_seconds:.1f}s"
        + (f": {last_error}" if last_error else "")
    )


def _wait_for_rpc_port(address: str, timeout_seconds: float) -> None:
    host, port_text = address.rsplit(":", 1)
    port = int(port_text)
    deadline = time.monotonic() + timeout_seconds
    last_error = ""
    while time.monotonic() < deadline:
        try:
            with socket.create_connection((host, port), timeout=0.5):
                return
        except OSError as exc:
            last_error = str(exc)
            time.sleep(0.25)
    raise RuntimeError(
        f"timed out waiting for Atlas RPC port {address} after {timeout_seconds:.1f}s"
        + (f": {last_error}" if last_error else "")
    )


def _rpc_port_is_open(address: str) -> bool:
    host, port_text = address.rsplit(":", 1)
    port = int(port_text)
    try:
        with socket.create_connection((host, port), timeout=0.25):
            return True
    except OSError:
        return False


def _wait_for_rpc_port_closed(address: str, timeout_seconds: float) -> None:
    deadline = time.monotonic() + timeout_seconds
    while time.monotonic() < deadline:
        if not _rpc_port_is_open(address):
            return
        time.sleep(0.25)
    raise RuntimeError(
        f"timed out waiting for Atlas RPC port {address} to become free after {timeout_seconds:.1f}s"
    )


def _launch_atlas(
    args: argparse.Namespace, run_dir: Path
) -> tuple[subprocess.Popen[str], Path]:
    launch_log_path = run_dir / "atlas_launch.log"
    launch_log = launch_log_path.open("w", encoding="utf-8")
    process = subprocess.Popen(
        [
            str(Path(args.atlas_binary).resolve()),
            "--atlas_log_benchmark_render_timings",
        ],
        stdout=launch_log,
        stderr=subprocess.STDOUT,
        text=True,
    )
    return process, launch_log_path


def _object_ids(list_objects_response: Any) -> list[int]:
    ids: list[int] = []
    for obj in getattr(list_objects_response, "objects", []) or []:
        obj_id = getattr(obj, "id", None)
        if obj_id is None:
            continue
        try:
            ids.append(int(obj_id))
        except Exception:
            continue
    return ids


def _prepare_scene(
    *,
    client: SceneClient,
    dataset_path: Path,
    camera_payload: dict[str, Any],
    args: argparse.Namespace,
    log_follower: AtlasRenderLogFollower,
    run_dir: Path,
) -> dict[str, Any]:
    prep: dict[str, Any] = {
        "dataset": str(dataset_path),
        "camera_spec": str(Path(args.camera_spec).resolve()),
        "compositing_mode": args.compositing_mode,
        "full_resolution_enabled": not args.disable_full_resolution,
        "hide_background": bool(args.hide_background),
        "hide_axis": bool(args.hide_axis),
        "hide_bound_box": bool(args.hide_bound_box),
    }

    client.ensure_view()
    existing_ids = _object_ids(client.list_objects())
    if existing_ids:
        removed = client.remove_objects(existing_ids, allow_unsaved=True)
        if not removed:
            raise RuntimeError(
                f"failed to remove pre-existing Atlas objects: {existing_ids}"
            )
        prep["removed_existing_ids"] = existing_ids

    if args.hide_background:
        _set_scope_bool_param(
            client,
            BACKGROUND_SCOPE_ID,
            SHOW_BACKGROUND_JSON_KEY,
            False,
            logger=_NullLogger(),
            event_name="background_hidden",
            unsupported_event_name="background_hide_not_supported",
        )
    if args.hide_axis:
        _set_scope_bool_param(
            client,
            AXIS_SCOPE_ID,
            SHOW_AXIS_JSON_KEY,
            False,
            logger=_NullLogger(),
            event_name="axis_hidden",
            unsupported_event_name="axis_hide_not_supported",
        )
    _set_canvas_size(
        client,
        _NullLogger(),
        args.canvas_logical_width,
        args.canvas_logical_height,
        stage="pre_load",
    )

    task_id = client.start_load_task([str(dataset_path)], set_visible=True)
    prep["task_id"] = int(task_id)
    task_status = client.wait_task(task_id, timeout_sec=600.0, poll_interval_sec=0.2)
    prep["task_status"] = task_status
    ids = _loaded_ids(task_status)
    if not ids:
        raise RuntimeError(
            f"Atlas load task produced no objects: {json.dumps(task_status, sort_keys=True)}"
        )
    ready = _wait_ready(client, ids, timeout_sec=180.0)
    prep["ready_status"] = ready
    if not ready.get("ok", False):
        raise RuntimeError(
            f"Atlas objects never became ready: {json.dumps(ready, sort_keys=True)}"
        )
    client.delete_task(task_id)
    prep["loaded_ids"] = ids

    if not args.disable_full_resolution:
        prep["full_resolution_ids"] = _enable_full_resolution(
            client, ids, _NullLogger()
        )
    if args.compositing_mode:
        prep["compositing_ids"] = _set_compositing_mode(
            client, ids, _NullLogger(), args.compositing_mode
        )
    if args.hide_bound_box:
        prep["bound_box_ids"] = _set_bound_box_mode(
            client, ids, _NullLogger(), NO_BOUND_BOX_VALUE
        )
    _set_canvas_size(
        client,
        _NullLogger(),
        args.canvas_logical_width,
        args.canvas_logical_height,
        stage="post_load",
    )

    _wait_for_camera_settle(
        client=client,
        camera_payload=camera_payload,
        log_follower=log_follower,
        preview_timeout_seconds=args.preview_timeout_seconds,
        final_timeout_seconds=args.final_timeout_seconds,
    )
    prep["prepared_camera"] = camera_payload
    status_path = run_dir / "atlas_gui_prepare_status.json"
    status_path.write_text(
        json.dumps(prep, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    return prep


class _NullLogger:
    def log(self, *_args: Any, **_kwargs: Any) -> dict[str, Any]:
        return {}


def _wait_for_camera_settle(
    *,
    client: SceneClient,
    camera_payload: dict[str, Any],
    log_follower: AtlasRenderLogFollower,
    preview_timeout_seconds: float,
    final_timeout_seconds: float,
) -> None:
    start_ns = time.time_ns()
    _apply_camera(client, camera_payload)
    log_follower.wait_for_next_marker(
        kind="preview",
        start_ns=start_ns,
        timeout_seconds=preview_timeout_seconds,
    )
    log_follower.wait_for_next_marker(
        kind="final",
        start_ns=start_ns,
        timeout_seconds=final_timeout_seconds,
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
        json.dumps(aggregate, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )

    lines = [
        "# Atlas GUI Rotate Benchmark Summary",
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
    calibration_path = Path(args.calibration).resolve()
    capture_binary = subprocess.check_output(
        [str(BUILD_CAPTURE_SCRIPT)], text=True
    ).strip()

    spec = load_benchmark_spec(args.camera_spec)
    if "open" not in spec.states:
        raise RuntimeError(
            f"camera spec {args.camera_spec} does not define an 'open' state"
        )
    open_camera = spec.states["open"].to_atlas_typed_value()

    _wait_for_rpc_port_closed(args.address, 30.0)

    preexisting_logs = {
        str(path.resolve())
        for path in Path(args.atlas_log_path).resolve().glob("**/atlas_info_*_log.txt")
    }

    bootstrap_dir = output_root / "bootstrap"
    bootstrap_dir.mkdir(parents=True, exist_ok=True)
    process, launch_log_path = _launch_atlas(args, bootstrap_dir)
    artifacts: list[RunArtifacts] = []
    client: SceneClient | None = None
    try:
        deadline = time.monotonic() + args.launch_timeout_seconds
        while not _rpc_port_is_open(args.address):
            if process.poll() is not None:
                raise RuntimeError(
                    "Atlas exited before opening the RPC port.\n"
                    f"{_tail(launch_log_path)}"
                )
            if time.monotonic() >= deadline:
                raise RuntimeError(
                    f"timed out waiting for Atlas RPC port {args.address} after "
                    f"{args.launch_timeout_seconds:.1f}s.\n{_tail(launch_log_path)}"
                )
            time.sleep(0.25)
        client = SceneClient(address=args.address)
        _wait_for_rpc_ready(client, args.launch_timeout_seconds)
        atlas_log_path = _wait_for_new_atlas_log(
            args.atlas_log_path,
            preexisting_logs=preexisting_logs,
            timeout_seconds=args.launch_timeout_seconds,
        )
        log_follower = AtlasRenderLogFollower(
            atlas_log_path, fallback_year=time.gmtime().tm_year
        )

        _wait_for_window("Atlas", "3D View", args.launch_timeout_seconds)
        _move_atlas_window_with_system_events(x=args.window_x, y=args.window_y)
        _wait_for_window_bounds(
            "Atlas",
            "3D View",
            x=args.window_x,
            y=args.window_y,
            timeout_seconds=10.0,
        )

        _prepare_scene(
            client=client,
            dataset_path=Path(args.dataset).resolve(),
            camera_payload=open_camera,
            args=args,
            log_follower=log_follower,
            run_dir=bootstrap_dir,
        )

        run_plan: list[tuple[str, int]] = []
        run_plan.extend(("warmup", index) for index in range(1, args.warmup_runs + 1))
        run_plan.extend(
            ("measured", index) for index in range(1, args.measured_runs + 1)
        )

        for kind, run_index in run_plan:
            run_dir = output_root / kind / f"run{run_index:02d}"
            run_dir.mkdir(parents=True, exist_ok=True)

            _wait_for_camera_settle(
                client=client,
                camera_payload=open_camera,
                log_follower=log_follower,
                preview_timeout_seconds=args.preview_timeout_seconds,
                final_timeout_seconds=args.final_timeout_seconds,
            )
            _move_atlas_window_with_system_events(x=args.window_x, y=args.window_y)
            _wait_for_window_bounds(
                "Atlas",
                "3D View",
                x=args.window_x,
                y=args.window_y,
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

        _aggregate_runs(artifacts, output_root / "aggregate")
        return 0
    finally:
        _terminate_process(process)
        try:
            _wait_for_rpc_port_closed(args.address, 30.0)
        except RuntimeError:
            pass
        time.sleep(1.0)


def _wait_for_new_atlas_log(
    log_root: str | Path,
    *,
    preexisting_logs: set[str],
    timeout_seconds: float,
) -> Path:
    deadline = time.monotonic() + timeout_seconds
    root = Path(log_root).resolve()
    last_candidate: Path | None = None
    while time.monotonic() < deadline:
        candidates = sorted(root.glob("**/atlas_info_*_log.txt"))
        for candidate in reversed(candidates):
            resolved = str(candidate.resolve())
            if resolved not in preexisting_logs:
                return candidate.resolve()
            last_candidate = candidate.resolve()
        time.sleep(0.25)
    if last_candidate is not None:
        return last_candidate
    raise RuntimeError(f"timed out waiting for a new Atlas log under {root}")


if __name__ == "__main__":
    raise SystemExit(main())
