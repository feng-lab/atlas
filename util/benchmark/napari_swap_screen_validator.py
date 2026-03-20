#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
import math
import statistics
import subprocess
import threading
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import numpy as np
from PIL import Image


SCRIPT_DIR = Path(__file__).resolve().parent
NAPARI_BENCHMARK = SCRIPT_DIR / "napari_volume_benchmark.py"
BUILD_CAPTURE_SCRIPT = SCRIPT_DIR / "build_macos_window_capture_sckit.sh"
HOME = Path.home()
COMPARE_SIZE = (160, 120)
EARLY_MATCH_TOLERANCE_MS = 5.0
MAX_NONFINAL_MATCH_LAG_MS = 500.0


@dataclass(frozen=True)
class ReferenceStepImage:
    action: str
    step: int
    path: Path
    normalized_gray: np.ndarray


@dataclass(frozen=True)
class CapturedFrameImage:
    action: str
    frame_index: int
    display_monotonic_ns: int
    path: Path
    frame_width: int
    frame_height: int
    normalized_gray: np.ndarray
    changed_fraction: float | None
    significant_change_action_baseline: bool


@dataclass(frozen=True)
class StepTiming:
    action: str
    step: int
    step_start_monotonic_ns: int
    first_frame_swap_sync_ms: float | None
    last_frame_swap_sync_ms: float | None


class DeterministicEventTranslator(threading.Thread):
    def __init__(self, source_path: Path, output_path: Path):
        super().__init__(daemon=True)
        self.source_path = source_path
        self.output_path = output_path
        self.error: Exception | None = None
        self._stop_requested = threading.Event()

    def request_stop(self) -> None:
        self._stop_requested.set()

    def run(self) -> None:
        try:
            self.output_path.parent.mkdir(parents=True, exist_ok=True)
            offset = 0
            with self.output_path.open("w", encoding="utf-8") as stream:
                while not self._stop_requested.is_set():
                    if not self.source_path.exists():
                        time.sleep(0.01)
                        continue
                    with self.source_path.open("r", encoding="utf-8") as input_stream:
                        input_stream.seek(offset)
                        lines = input_stream.readlines()
                        offset = input_stream.tell()
                    if not lines:
                        time.sleep(0.01)
                        continue
                    for line in lines:
                        record = json.loads(line)
                        translated = self._translate_record(record)
                        for out_record in translated:
                            stream.write(json.dumps(out_record, sort_keys=True) + "\n")
                        stream.flush()
                        if record.get("event") == "session_end":
                            return
        except Exception as error:  # noqa: BLE001
            self.error = error

    @staticmethod
    def _translate_record(record: dict[str, Any]) -> list[dict[str, Any]]:
        event = str(record.get("event") or "")
        action = record.get("action")
        base = {
            "wall_time_ns": int(record.get("wall_time_ns", 0) or 0),
            "monotonic_ns": int(record.get("monotonic_ns", 0) or 0),
        }
        if event == "session_start":
            return [
                {
                    **base,
                    "event": "session_start",
                    "app": record.get("app"),
                }
            ]
        if event == "action_start" and action is not None:
            return [
                {
                    **base,
                    "event": "action_start",
                    "action": action,
                },
                {
                    **base,
                    "event": "drag_start",
                    "action": action,
                },
            ]
        if event == "action_complete" and action is not None:
            return [
                {
                    **base,
                    "event": "drag_end",
                    "action": action,
                },
                {
                    **base,
                    "event": "action_end",
                    "action": action,
                },
            ]
        if event == "session_end":
            return [{**base, "event": "session_end"}]
        return []


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Validate napari deterministic swap timings against real on-screen "
            "frames by matching ScreenCaptureKit crops to per-step screenshot "
            "references from the deterministic driver."
        )
    )
    parser.add_argument("--dataset", required=True)
    parser.add_argument("--camera-spec", required=True)
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
    parser.add_argument("--output-root", required=True)
    parser.add_argument(
        "--reference-dir",
        default=None,
        help=(
            "Optional existing screenshot-reference run directory with per-step "
            "saved screenshots. If provided, the validator reuses it instead of "
            "rerunning the reference pass."
        ),
    )
    parser.add_argument(
        "--observed-dir",
        default=None,
        help=(
            "Optional existing observed run directory containing the deterministic "
            "non-screenshot napari output plus ScreenCaptureKit capture files. "
            "If provided, the validator reuses it instead of rerunning the "
            "observed pass."
        ),
    )
    parser.add_argument(
        "--napari-python",
        default=str(HOME / "miniconda3" / "envs" / "napari" / "bin" / "python"),
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
        default="napari swap validator",
        help="Prefix used to make the reference and observed napari windows unique.",
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
        help=(
            "Normalized crop inside the napari canvas used for screen-captured "
            "frame analysis and reference screenshot matching."
        ),
    )
    return parser.parse_args()


def _wait_for_file(path: Path, timeout_seconds: float) -> None:
    deadline = time.monotonic() + timeout_seconds
    while time.monotonic() < deadline:
        if path.exists():
            return
        time.sleep(0.05)
    raise RuntimeError(f"timed out waiting for file: {path}")


def _tail(path: Path, lines: int = 120) -> str:
    if not path.exists():
        return ""
    content = path.read_text(encoding="utf-8", errors="replace").splitlines()
    return "\n".join(content[-lines:])


def _run_logged(
    *,
    cmd: list[str],
    log_path: Path,
    timeout_seconds: float,
) -> None:
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
        raise RuntimeError(
            f"command failed with exit code {completed.returncode}: {' '.join(cmd)}\n"
            f"{_tail(log_path)}"
        )


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


def _run_reference_pass(args: argparse.Namespace, output_dir: Path) -> None:
    cmd = [
        str(Path(args.napari_python).resolve()),
        str(NAPARI_BENCHMARK),
        "--dataset",
        str(Path(args.dataset).resolve()),
        "--camera-spec",
        str(Path(args.camera_spec).resolve()),
        "--dataset-loader",
        str(args.dataset_loader),
        "--output-dir",
        str(output_dir),
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
            ["--scene-scale-zyx", *(str(float(x)) for x in args.scene_scale_zyx)]
        )
    _run_logged(
        cmd=cmd,
        log_path=output_dir / "reference.log",
        timeout_seconds=float(args.reference_timeout_seconds),
    )


def _build_capture_binary(output_dir: Path) -> str:
    build_log_path = output_dir / "build_capture_helper.log"
    cmd = [str(BUILD_CAPTURE_SCRIPT), str(output_dir / "macos_window_capture_sckit")]
    with build_log_path.open("w", encoding="utf-8") as log_stream:
        completed = subprocess.run(
            cmd,
            stdout=log_stream,
            stderr=subprocess.STDOUT,
            text=True,
            check=False,
        )
    if completed.returncode != 0:
        raise RuntimeError(
            f"failed to build ScreenCaptureKit helper\n{_tail(build_log_path)}"
        )
    helper_path = (output_dir / "macos_window_capture_sckit").resolve()
    if not helper_path.exists():
        raise RuntimeError(f"capture helper missing after build: {helper_path}")
    return str(helper_path)


def _launch_observed_pass(
    args: argparse.Namespace,
    output_dir: Path,
) -> tuple[subprocess.Popen[str], Path, Any]:
    log_path = output_dir / "observed.log"
    log_stream = log_path.open("w", encoding="utf-8")
    cmd = [
        str(Path(args.napari_python).resolve()),
        str(NAPARI_BENCHMARK),
        "--dataset",
        str(Path(args.dataset).resolve()),
        "--camera-spec",
        str(Path(args.camera_spec).resolve()),
        "--dataset-loader",
        str(args.dataset_loader),
        "--output-dir",
        str(output_dir),
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
        "--window-title",
        f"{args.window_title_prefix} observed",
        "--window-x",
        str(args.window_x),
        "--window-y",
        str(args.window_y),
        "--window-width",
        str(args.window_width),
        "--window-height",
        str(args.window_height),
    ]
    if args.open_plugin:
        cmd.extend(["--open-plugin", str(args.open_plugin)])
    if args.scene_scale_zyx is not None:
        cmd.extend(
            ["--scene-scale-zyx", *(str(float(x)) for x in args.scene_scale_zyx)]
        )
    process = subprocess.Popen(
        cmd,
        stdout=log_stream,
        stderr=subprocess.STDOUT,
        text=True,
    )
    return process, log_path, log_stream


def _run_observed_pass(
    args: argparse.Namespace,
    output_dir: Path,
    capture_binary: str,
) -> None:
    output_dir.mkdir(parents=True, exist_ok=True)
    process, log_path, observed_log_stream = _launch_observed_pass(args, output_dir)
    calibration_path = output_dir / "napari_gui_calibration.json"
    events_path = output_dir / "napari_events.jsonl"
    translated_events_path = output_dir / "gui_capture_events.jsonl"
    capture_summary_path = output_dir / "capture_summary.json"
    capture_frames_path = output_dir / "capture_summary_frames.jsonl"
    captured_frame_dir = output_dir / "analysis_frames"
    capture_log_path = output_dir / "capture_helper.log"
    translator = DeterministicEventTranslator(events_path, translated_events_path)
    capture_process: subprocess.Popen[str] | None = None
    capture_log_stream = None
    try:
        _wait_for_file(calibration_path, float(args.calibration_wait_timeout_seconds))
        _wait_for_file(events_path, float(args.calibration_wait_timeout_seconds))
        calibration_payload = json.loads(calibration_path.read_text(encoding="utf-8"))
        calibration_payload["analysis_region_norm"] = {
            "x": float(args.analysis_region_norm[0]),
            "y": float(args.analysis_region_norm[1]),
            "width": float(args.analysis_region_norm[2]),
            "height": float(args.analysis_region_norm[3]),
        }
        calibration_path.write_text(
            json.dumps(calibration_payload, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        translator.start()
        capture_log_stream = capture_log_path.open("w", encoding="utf-8")
        capture_process = subprocess.Popen(
            [
                capture_binary,
                "--calibration",
                str(calibration_path),
                "--events",
                str(translated_events_path),
                "--output",
                str(capture_summary_path),
                "--frames-output",
                str(capture_frames_path),
                "--analysis-frames-dir",
                str(captured_frame_dir),
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
                "--skip-frame-diff-analysis",
                "--timeout-seconds",
                str(args.capture_timeout_seconds),
            ],
            stdout=capture_log_stream,
            stderr=subprocess.STDOUT,
            text=True,
        )
        process.wait(timeout=float(args.observed_timeout_seconds))
        translator.join(timeout=10.0)
        if translator.is_alive():
            translator.request_stop()
            translator.join(timeout=5.0)
        if translator.error is not None:
            raise translator.error
        if capture_process.wait(timeout=float(args.capture_process_wait_seconds)) != 0:
            raise RuntimeError(f"capture helper failed\n{_tail(capture_log_path)}")
        if process.returncode != 0:
            raise RuntimeError(f"observed napari run failed\n{_tail(log_path)}")
    finally:
        if translator.is_alive():
            translator.request_stop()
            translator.join(timeout=1.0)
        if capture_process is not None and capture_process.poll() is None:
            capture_process.terminate()
            try:
                capture_process.wait(timeout=10.0)
            except subprocess.TimeoutExpired:
                capture_process.kill()
                capture_process.wait(timeout=10.0)
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=10.0)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=10.0)
        observed_log_stream.close()
        if capture_log_stream is not None:
            capture_log_stream.close()


def _normalize_gray_array(gray: np.ndarray) -> np.ndarray:
    array = gray.astype(np.float32) / 255.0
    array = array - float(array.mean())
    std = float(array.std())
    if std > 1e-6:
        array = array / std
    return array


def _crop_box(
    width: int,
    height: int,
    region_norm: tuple[float, float, float, float],
) -> tuple[int, int, int, int]:
    x, y, crop_width, crop_height = region_norm
    left = max(0, min(width, int(round(width * x))))
    top = max(0, min(height, int(round(height * y))))
    right = max(left + 1, min(width, int(round(width * (x + crop_width)))))
    bottom = max(top + 1, min(height, int(round(height * (y + crop_height)))))
    return (left, top, right, bottom)


def _load_normalized_gray(
    path: Path,
    *,
    crop_region_norm: tuple[float, float, float, float] | None = None,
) -> np.ndarray:
    with Image.open(path) as image:
        if crop_region_norm is not None:
            image = image.crop(_crop_box(image.width, image.height, crop_region_norm))
        gray = image.convert("L").resize(COMPARE_SIZE, Image.Resampling.BILINEAR)
        gray_array = np.asarray(gray, dtype=np.uint8)
    return _normalize_gray_array(gray_array)


def _normalize_captured_gray_array(
    gray: np.ndarray,
    *,
    width: int,
    height: int,
    capture_target: str = "window",
    point_pixel_scale: float = 1.0,
) -> np.ndarray:
    frame = gray.reshape((height, width))
    if capture_target == "window" and point_pixel_scale > 1.0:
        logical_width = max(1, min(width, int(round(width / point_pixel_scale))))
        logical_height = max(1, min(height, int(round(height / point_pixel_scale))))
        frame = frame[:logical_height, :logical_width]
    resized = Image.fromarray(frame, mode="L").resize(
        COMPARE_SIZE, Image.Resampling.BILINEAR
    )
    return _normalize_gray_array(np.asarray(resized, dtype=np.uint8))


def _load_normalized_gray_from_captured_png(
    path: Path,
    *,
    width: int,
    height: int,
    capture_target: str = "window",
    point_pixel_scale: float = 1.0,
) -> np.ndarray:
    with Image.open(path) as image:
        gray = np.asarray(image.convert("L"), dtype=np.uint8)
    return _normalize_captured_gray_array(
        gray,
        width=width,
        height=height,
        capture_target=capture_target,
        point_pixel_scale=point_pixel_scale,
    )


def _load_normalized_gray_from_raw_bgra32(
    path: Path,
    *,
    width: int,
    height: int,
    capture_target: str = "window",
    point_pixel_scale: float = 1.0,
) -> np.ndarray:
    raw = path.read_bytes()
    expected_size = width * height * 4
    if len(raw) != expected_size:
        raise RuntimeError(
            f"raw frame size mismatch for {path}: expected {expected_size} bytes, got {len(raw)}"
        )
    frame = np.frombuffer(raw, dtype=np.uint8).reshape((height, width, 4))
    gray = (
        0.114 * frame[:, :, 0].astype(np.float32)
        + 0.587 * frame[:, :, 1].astype(np.float32)
        + 0.299 * frame[:, :, 2].astype(np.float32)
    ).astype(np.uint8)
    return _normalize_captured_gray_array(
        gray,
        width=width,
        height=height,
        capture_target=capture_target,
        point_pixel_scale=point_pixel_scale,
    )


def _load_reference_steps(
    reference_dir: Path,
    *,
    crop_region_norm: tuple[float, float, float, float],
) -> dict[str, list[ReferenceStepImage]]:
    summary = json.loads(
        (reference_dir / "napari_timer_summary.json").read_text(encoding="utf-8")
    )
    screenshots_dir = reference_dir / "screenshots"
    actions: dict[str, list[ReferenceStepImage]] = {}
    for action in summary.get("actions", []):
        action_name = str(action["name"])
        step_metrics = action.get("step_metrics", [])
        step_images: list[ReferenceStepImage] = []
        for step_index in range(1, len(step_metrics) + 1):
            if step_index == len(step_metrics):
                image_path = screenshots_dir / f"{action_name}.png"
            else:
                image_path = screenshots_dir / f"{action_name}_step{step_index:02d}.png"
            if not image_path.exists():
                raise RuntimeError(f"missing reference step image: {image_path}")
            step_images.append(
                ReferenceStepImage(
                    action=action_name,
                    step=step_index,
                    path=image_path,
                    normalized_gray=_load_normalized_gray(
                        image_path,
                        crop_region_norm=crop_region_norm,
                    ),
                )
            )
        actions[action_name] = step_images
    return actions


def _effective_reference_crop_region_norm(
    observed_dir: Path,
    *,
    requested_crop_region_norm: tuple[float, float, float, float],
) -> tuple[float, float, float, float]:
    capture_summary = json.loads(
        (observed_dir / "capture_summary.json").read_text(encoding="utf-8")
    )
    window_payload = capture_summary.get("window", {})
    capture_target = str(window_payload.get("capture_target") or "window")
    point_pixel_scale = float(window_payload.get("point_pixel_scale") or 1.0)
    capture_region_pixels = capture_summary.get("capture_region_pixels")
    analysis_region_pixels = capture_summary.get("analysis_region_pixels")
    if (
        capture_target != "window"
        or point_pixel_scale <= 1.0
        or not isinstance(capture_region_pixels, dict)
        or not isinstance(analysis_region_pixels, dict)
    ):
        return requested_crop_region_norm

    capture_width = float(capture_region_pixels.get("width") or 0.0)
    capture_height = float(capture_region_pixels.get("height") or 0.0)
    if capture_width <= 0.0 or capture_height <= 0.0:
        return requested_crop_region_norm
    logical_capture_width = capture_width / point_pixel_scale
    logical_capture_height = capture_height / point_pixel_scale
    analysis_left = max(0.0, float(analysis_region_pixels.get("x") or 0.0))
    analysis_top = max(0.0, float(analysis_region_pixels.get("y") or 0.0))
    analysis_right = min(
        logical_capture_width,
        analysis_left + float(analysis_region_pixels.get("width") or 0.0),
    )
    analysis_bottom = min(
        logical_capture_height,
        analysis_top + float(analysis_region_pixels.get("height") or 0.0),
    )
    if analysis_right <= analysis_left or analysis_bottom <= analysis_top:
        return requested_crop_region_norm

    return (
        (analysis_left * point_pixel_scale) / capture_width,
        (analysis_top * point_pixel_scale) / capture_height,
        ((analysis_right - analysis_left) * point_pixel_scale) / capture_width,
        ((analysis_bottom - analysis_top) * point_pixel_scale) / capture_height,
    )


def _load_observed_frames(
    observed_dir: Path,
) -> dict[str, list[CapturedFrameImage]]:
    frames_path = observed_dir / "capture_summary_frames.jsonl"
    capture_summary = json.loads(
        (observed_dir / "capture_summary.json").read_text(encoding="utf-8")
    )
    window_payload = capture_summary.get("window", {})
    capture_target = str(window_payload.get("capture_target") or "window")
    point_pixel_scale = float(window_payload.get("point_pixel_scale") or 1.0)
    actions: dict[str, list[CapturedFrameImage]] = {}
    with frames_path.open("r", encoding="utf-8") as stream:
        for line in stream:
            if not line.strip():
                continue
            record = json.loads(line)
            action_name = record.get("active_action")
            image_path = record.get("analysis_frame_png")
            image_loader = "png"
            if image_path is None:
                image_path = record.get("analysis_frame_raw_bgra32")
                image_loader = "raw_bgra32"
            if action_name is None or image_path is None:
                continue
            significant_change = bool(record.get("significant_change_action_baseline"))
            changed_fraction = record.get("diff_action_baseline")
            if (
                changed_fraction is not None
                and not significant_change
                and float(changed_fraction or 0.0) <= 0.0
            ):
                continue
            path = Path(str(image_path))
            if not path.exists():
                continue
            width = int(record["analysis_width"])
            height = int(record["analysis_height"])
            actions.setdefault(str(action_name), []).append(
                CapturedFrameImage(
                    action=str(action_name),
                    frame_index=int(record["frame_index"]),
                    display_monotonic_ns=int(record["display_monotonic_ns"]),
                    path=path,
                    frame_width=width,
                    frame_height=height,
                    normalized_gray=(
                        _load_normalized_gray_from_captured_png(
                            path,
                            width=width,
                            height=height,
                            capture_target=capture_target,
                            point_pixel_scale=point_pixel_scale,
                        )
                        if image_loader == "png"
                        else _load_normalized_gray_from_raw_bgra32(
                            path,
                            width=width,
                            height=height,
                            capture_target=capture_target,
                            point_pixel_scale=point_pixel_scale,
                        )
                    ),
                    changed_fraction=(
                        float(changed_fraction)
                        if changed_fraction is not None
                        else None
                    ),
                    significant_change_action_baseline=significant_change,
                )
            )
    for frame_list in actions.values():
        frame_list.sort(key=lambda frame: frame.display_monotonic_ns)
    return actions


def _load_step_timings(observed_dir: Path) -> dict[tuple[str, int], StepTiming]:
    summary = json.loads(
        (observed_dir / "napari_timer_summary.json").read_text(encoding="utf-8")
    )
    step_start_by_key: dict[tuple[str, int], int] = {}
    with (observed_dir / "napari_events.jsonl").open("r", encoding="utf-8") as stream:
        for line in stream:
            if not line.strip():
                continue
            record = json.loads(line)
            if record.get("event") != "step_start":
                continue
            key = (str(record["action"]), int(record["step"]))
            step_start_by_key[key] = int(record["monotonic_ns"])

    timings: dict[tuple[str, int], StepTiming] = {}
    for action in summary.get("actions", []):
        action_name = str(action["name"])
        for step_metric in action.get("step_metrics", []):
            key = (action_name, int(step_metric["step"]))
            step_start_monotonic_ns = step_start_by_key.get(key)
            if step_start_monotonic_ns is None:
                raise RuntimeError(f"missing step_start event for {key}")
            timings[key] = StepTiming(
                action=action_name,
                step=int(step_metric["step"]),
                step_start_monotonic_ns=step_start_monotonic_ns,
                first_frame_swap_sync_ms=(
                    float(step_metric["first_frame_swap_sync_ms"])
                    if step_metric.get("first_frame_swap_sync_ms") is not None
                    else None
                ),
                last_frame_swap_sync_ms=(
                    float(step_metric["last_frame_swap_sync_ms"])
                    if step_metric.get("last_frame_swap_sync_ms") is not None
                    else None
                ),
            )
    return timings


def _correlation(a: np.ndarray, b: np.ndarray) -> float:
    return float(np.mean(a * b))


def _timing_feasible_mask(
    frames: list[CapturedFrameImage],
    steps: list[ReferenceStepImage],
    step_timings: dict[tuple[str, int], StepTiming],
) -> np.ndarray:
    mask = np.ones((len(frames), len(steps)), dtype=bool)
    early_tolerance_ns = int(EARLY_MATCH_TOLERANCE_MS * 1_000_000.0)
    max_nonfinal_lag_ns = int(MAX_NONFINAL_MATCH_LAG_MS * 1_000_000.0)
    last_step = steps[-1].step if steps else 0
    for frame_index, frame in enumerate(frames):
        for step_index, step in enumerate(steps):
            timing = step_timings[(step.action, step.step)]
            delta_ns = frame.display_monotonic_ns - timing.step_start_monotonic_ns
            if delta_ns < -early_tolerance_ns:
                mask[frame_index, step_index] = False
                continue
            if step.step != last_step and delta_ns > max_nonfinal_lag_ns:
                mask[frame_index, step_index] = False
    return mask


def _monotonic_assign(cost: np.ndarray) -> list[int]:
    if cost.ndim != 2 or cost.shape[0] == 0 or cost.shape[1] == 0:
        return []
    frame_count, step_count = cost.shape
    dp = np.full((frame_count, step_count), np.inf, dtype=np.float64)
    back = np.full((frame_count, step_count), -1, dtype=np.int32)
    dp[0, :] = cost[0, :]
    for frame_index in range(1, frame_count):
        prefix_best_value = math.inf
        prefix_best_index = -1
        for step_index in range(step_count):
            candidate = float(dp[frame_index - 1, step_index])
            if candidate < prefix_best_value:
                prefix_best_value = candidate
                prefix_best_index = step_index
            dp[frame_index, step_index] = (
                cost[frame_index, step_index] + prefix_best_value
            )
            back[frame_index, step_index] = prefix_best_index
    assignment = [0] * frame_count
    step_index = int(np.argmin(dp[-1, :]))
    assignment[-1] = step_index
    for frame_index in range(frame_count - 1, 0, -1):
        step_index = int(back[frame_index, step_index])
        if step_index < 0:
            step_index = 0
        assignment[frame_index - 1] = step_index
    return assignment


def _analyze_matches(
    reference_dir: Path,
    observed_dir: Path,
    analysis_dir: Path,
    *,
    crop_region_norm: tuple[float, float, float, float],
) -> tuple[Path, Path]:
    effective_reference_crop = _effective_reference_crop_region_norm(
        observed_dir,
        requested_crop_region_norm=crop_region_norm,
    )
    reference_steps = _load_reference_steps(
        reference_dir,
        crop_region_norm=effective_reference_crop,
    )
    observed_frames = _load_observed_frames(observed_dir)
    step_timings = _load_step_timings(observed_dir)

    report: dict[str, Any] = {
        "reference_dir": str(reference_dir),
        "observed_dir": str(observed_dir),
        "compare_size": {"width": COMPARE_SIZE[0], "height": COMPARE_SIZE[1]},
        "requested_analysis_region_norm": {
            "x": crop_region_norm[0],
            "y": crop_region_norm[1],
            "width": crop_region_norm[2],
            "height": crop_region_norm[3],
        },
        "effective_reference_crop_region_norm": {
            "x": effective_reference_crop[0],
            "y": effective_reference_crop[1],
            "width": effective_reference_crop[2],
            "height": effective_reference_crop[3],
        },
        "actions": [],
    }
    markdown_lines = [
        "# Napari Swap vs Screen Validation",
        "",
        "This report compares the non-screenshot deterministic napari run against",
        "real on-screen captured frames by matching those frames to the saved",
        "per-step screenshot-reference images.",
        "",
        "Requested analysis crop:",
        f"`x={crop_region_norm[0]:.3f}, y={crop_region_norm[1]:.3f}, "
        f"w={crop_region_norm[2]:.3f}, h={crop_region_norm[3]:.3f}`",
        "",
        "Effective reference crop used for matching:",
        f"`x={effective_reference_crop[0]:.3f}, y={effective_reference_crop[1]:.3f}, "
        f"w={effective_reference_crop[2]:.3f}, h={effective_reference_crop[3]:.3f}`",
        "",
    ]

    for action_name, steps in reference_steps.items():
        frames = observed_frames.get(action_name, [])
        action_report: dict[str, Any] = {
            "action": action_name,
            "reference_step_count": len(steps),
            "observed_changed_frame_count": len(frames),
            "matched_steps": [],
        }
        if not frames:
            action_report["note"] = (
                "no changed captured frames available for this action"
            )
            report["actions"].append(action_report)
            markdown_lines.extend(
                [
                    f"## {action_name}",
                    "",
                    "No changed captured frames were available for this action.",
                    "",
                ]
            )
            continue

        similarity = np.empty((len(frames), len(steps)), dtype=np.float64)
        for frame_index, frame in enumerate(frames):
            for step_index, step in enumerate(steps):
                similarity[frame_index, step_index] = _correlation(
                    frame.normalized_gray, step.normalized_gray
                )
        feasible_mask = _timing_feasible_mask(frames, steps, step_timings)
        if not feasible_mask.any():
            raise RuntimeError(
                f"no timing-feasible frame/step matches for action {action_name}"
            )
        cost = -similarity.copy()
        cost[~feasible_mask] = np.inf
        assignment = _monotonic_assign(cost)

        matches_by_step: dict[int, list[dict[str, Any]]] = {}
        for frame_list_index, (frame, assigned_step_index) in enumerate(
            zip(frames, assignment, strict=True)
        ):
            step = steps[assigned_step_index]
            key = (action_name, step.step)
            timing = step_timings[key]
            display_ms_from_step_start = (
                frame.display_monotonic_ns - timing.step_start_monotonic_ns
            ) / 1_000_000.0
            first_swap_absolute_ms = (
                timing.first_frame_swap_sync_ms
                if timing.first_frame_swap_sync_ms is not None
                else None
            )
            last_swap_absolute_ms = (
                timing.last_frame_swap_sync_ms
                if timing.last_frame_swap_sync_ms is not None
                else None
            )
            match_entry = {
                "step": step.step,
                "reference_image": str(step.path),
                "capture_frame_index": frame.frame_index,
                "capture_frame_path": str(frame.path),
                "display_monotonic_ns": frame.display_monotonic_ns,
                "display_ms_from_step_start": display_ms_from_step_start,
                "first_frame_swap_sync_ms": timing.first_frame_swap_sync_ms,
                "last_frame_swap_sync_ms": timing.last_frame_swap_sync_ms,
                "delta_from_first_frame_swap_ms": (
                    display_ms_from_step_start - first_swap_absolute_ms
                    if first_swap_absolute_ms is not None
                    else None
                ),
                "delta_from_last_frame_swap_ms": (
                    display_ms_from_step_start - last_swap_absolute_ms
                    if last_swap_absolute_ms is not None
                    else None
                ),
                "similarity": float(similarity[frame_list_index, assigned_step_index]),
                "changed_fraction": frame.changed_fraction,
            }
            matches_by_step.setdefault(step.step, []).append(match_entry)

        matched_steps: list[dict[str, Any]] = []
        for step_index in sorted(matches_by_step):
            step_matches = sorted(
                matches_by_step[step_index],
                key=lambda entry: int(entry["display_monotonic_ns"]),
            )
            first_match = step_matches[0]
            last_match = step_matches[-1]
            matched_steps.append(
                {
                    "step": step_index,
                    "reference_image": first_match["reference_image"],
                    "matched_frame_count": len(step_matches),
                    "display_ms_from_step_start": first_match[
                        "display_ms_from_step_start"
                    ],
                    "first_display_ms_from_step_start": first_match[
                        "display_ms_from_step_start"
                    ],
                    "last_display_ms_from_step_start": last_match[
                        "display_ms_from_step_start"
                    ],
                    "first_frame_swap_sync_ms": first_match["first_frame_swap_sync_ms"],
                    "last_frame_swap_sync_ms": first_match["last_frame_swap_sync_ms"],
                    "delta_from_first_frame_swap_ms": first_match[
                        "delta_from_first_frame_swap_ms"
                    ],
                    "delta_from_last_frame_swap_ms": first_match[
                        "delta_from_last_frame_swap_ms"
                    ],
                    "last_delta_from_first_frame_swap_ms": last_match[
                        "delta_from_first_frame_swap_ms"
                    ],
                    "last_delta_from_last_frame_swap_ms": last_match[
                        "delta_from_last_frame_swap_ms"
                    ],
                    "similarity": first_match["similarity"],
                    "first_similarity": first_match["similarity"],
                    "last_similarity": last_match["similarity"],
                    "capture_frame_index": first_match["capture_frame_index"],
                    "capture_frame_path": first_match["capture_frame_path"],
                    "display_monotonic_ns": first_match["display_monotonic_ns"],
                    "last_capture_frame_index": last_match["capture_frame_index"],
                    "last_capture_frame_path": last_match["capture_frame_path"],
                    "last_display_monotonic_ns": last_match["display_monotonic_ns"],
                    "changed_fraction": first_match["changed_fraction"],
                    "last_changed_fraction": last_match["changed_fraction"],
                }
            )
        action_report["matched_steps"] = matched_steps
        action_report["displayed_unique_step_count"] = len(matched_steps)
        action_report["display_ms_from_step_start_stats"] = _stats(
            [float(entry["display_ms_from_step_start"]) for entry in matched_steps]
        )
        action_report["last_display_ms_from_step_start_stats"] = _stats(
            [float(entry["last_display_ms_from_step_start"]) for entry in matched_steps]
        )
        action_report["matched_frame_count_stats"] = _stats(
            [float(entry["matched_frame_count"]) for entry in matched_steps]
        )
        action_report["delta_from_first_frame_swap_ms_stats"] = _stats(
            [
                float(entry["delta_from_first_frame_swap_ms"])
                for entry in matched_steps
                if entry["delta_from_first_frame_swap_ms"] is not None
            ]
        )
        action_report["delta_from_last_frame_swap_ms_stats"] = _stats(
            [
                float(entry["delta_from_last_frame_swap_ms"])
                for entry in matched_steps
                if entry["delta_from_last_frame_swap_ms"] is not None
            ]
        )
        action_report["similarity_stats"] = _stats(
            [float(entry["similarity"]) for entry in matched_steps]
        )
        report["actions"].append(action_report)

        markdown_lines.extend(
            [
                f"## {action_name}",
                "",
                f"- Reference steps: `{len(steps)}`",
                f"- Changed captured frames: `{len(frames)}`",
                f"- Unique matched displayed steps: `{len(matched_steps)}`",
                (
                    "- Mean displayed time from step start: "
                    f"`{action_report['display_ms_from_step_start_stats']['mean']:.2f} ms`"
                    if action_report["display_ms_from_step_start_stats"]["mean"]
                    is not None
                    else "- Mean displayed time from step start: `n/a`"
                ),
                (
                    "- Mean last displayed time from step start: "
                    f"`{action_report['last_display_ms_from_step_start_stats']['mean']:.2f} ms`"
                    if action_report["last_display_ms_from_step_start_stats"]["mean"]
                    is not None
                    else "- Mean last displayed time from step start: `n/a`"
                ),
                (
                    "- Mean matched frames per step: "
                    f"`{action_report['matched_frame_count_stats']['mean']:.2f}`"
                    if action_report["matched_frame_count_stats"]["mean"] is not None
                    else "- Mean matched frames per step: `n/a`"
                ),
                (
                    "- Mean delta vs first-frame-swap: "
                    f"`{action_report['delta_from_first_frame_swap_ms_stats']['mean']:.2f} ms`"
                    if action_report["delta_from_first_frame_swap_ms_stats"]["mean"]
                    is not None
                    else "- Mean delta vs first-frame-swap: `n/a`"
                ),
                (
                    "- Mean delta vs last-frame-swap: "
                    f"`{action_report['delta_from_last_frame_swap_ms_stats']['mean']:.2f} ms`"
                    if action_report["delta_from_last_frame_swap_ms_stats"]["mean"]
                    is not None
                    else "- Mean delta vs last-frame-swap: `n/a`"
                ),
                "",
                "| Step | Matched frames | First display from step start | Last display from step start | First swap | Last swap | First delta vs last swap | Last delta vs last swap | First similarity | Last similarity |",
                "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |",
            ]
        )
        for entry in matched_steps:
            first_swap_text = (
                f"{entry['first_frame_swap_sync_ms']:.2f} ms"
                if entry["first_frame_swap_sync_ms"] is not None
                else "n/a"
            )
            last_swap_text = (
                f"{entry['last_frame_swap_sync_ms']:.2f} ms"
                if entry["last_frame_swap_sync_ms"] is not None
                else "n/a"
            )
            last_delta_text = (
                f"{entry['delta_from_last_frame_swap_ms']:.2f} ms"
                if entry["delta_from_last_frame_swap_ms"] is not None
                else "n/a"
            )
            last_last_delta_text = (
                f"{entry['last_delta_from_last_frame_swap_ms']:.2f} ms"
                if entry["last_delta_from_last_frame_swap_ms"] is not None
                else "n/a"
            )
            markdown_lines.append(
                f"| {entry['step']} | "
                f"{entry['matched_frame_count']} | "
                f"{entry['first_display_ms_from_step_start']:.2f} ms | "
                f"{entry['last_display_ms_from_step_start']:.2f} ms | "
                f"{first_swap_text} | "
                f"{last_swap_text} | "
                f"{last_delta_text} | "
                f"{last_last_delta_text} | "
                f"{entry['first_similarity']:.4f} | "
                f"{entry['last_similarity']:.4f} |"
            )
        markdown_lines.append("")

    json_path = analysis_dir / "swap_screen_match_report.json"
    markdown_path = analysis_dir / "swap_screen_match_report.md"
    json_path.write_text(
        json.dumps(report, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    markdown_path.write_text("\n".join(markdown_lines) + "\n", encoding="utf-8")
    return json_path, markdown_path


def main() -> int:
    args = _parse_args()
    output_root = Path(args.output_root).resolve()
    reference_dir = (
        Path(args.reference_dir).resolve()
        if args.reference_dir is not None
        else output_root / "reference"
    )
    observed_dir = (
        Path(args.observed_dir).resolve()
        if args.observed_dir is not None
        else output_root / "observed"
    )
    analysis_dir = output_root / "analysis"
    analysis_dir.mkdir(parents=True, exist_ok=True)
    if args.observed_dir is None:
        observed_dir.mkdir(parents=True, exist_ok=True)
    if args.reference_dir is None:
        reference_dir.mkdir(parents=True, exist_ok=True)
    crop_region_norm = (
        float(args.analysis_region_norm[0]),
        float(args.analysis_region_norm[1]),
        float(args.analysis_region_norm[2]),
        float(args.analysis_region_norm[3]),
    )

    if args.reference_dir is None:
        _run_reference_pass(args, reference_dir)
    elif not (reference_dir / "napari_timer_summary.json").exists():
        raise RuntimeError(
            f"reference directory missing napari_timer_summary.json: {reference_dir}"
        )
    if args.observed_dir is None:
        capture_binary = _build_capture_binary(output_root)
        _run_observed_pass(args, observed_dir, capture_binary)
    else:
        required_observed_files = [
            observed_dir / "capture_summary.json",
            observed_dir / "capture_summary_frames.jsonl",
            observed_dir / "napari_timer_summary.json",
            observed_dir / "napari_events.jsonl",
        ]
        missing_files = [path for path in required_observed_files if not path.exists()]
        if missing_files:
            missing_text = "\n".join(str(path) for path in missing_files)
            raise RuntimeError(
                f"observed directory is missing required files:\n{missing_text}"
            )
    report_json_path, report_md_path = _analyze_matches(
        reference_dir=reference_dir,
        observed_dir=observed_dir,
        analysis_dir=analysis_dir,
        crop_region_norm=crop_region_norm,
    )

    summary = {
        "reference_dir": str(reference_dir),
        "observed_dir": str(observed_dir),
        "analysis_report_json": str(report_json_path),
        "analysis_report_markdown": str(report_md_path),
    }
    (output_root / "validator_summary.json").write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    print(json.dumps(summary, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
