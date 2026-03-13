from __future__ import annotations

import argparse
import json
import time
from dataclasses import dataclass
from pathlib import Path

import mss
import numpy as np


def _load_events(path: Path, offset: int) -> tuple[list[dict], int]:
    if not path.exists():
        return [], offset
    with path.open("r", encoding="utf-8") as stream:
        stream.seek(offset)
        lines = stream.readlines()
        new_offset = stream.tell()
    events = [json.loads(line) for line in lines if line.strip()]
    return events, new_offset


def _to_gray(frame_bgra: np.ndarray) -> np.ndarray:
    rgb = frame_bgra[:, :, :3].astype(np.float32)
    return (0.114 * rgb[:, :, 0] + 0.587 * rgb[:, :, 1] + 0.299 * rgb[:, :, 2]).astype(
        np.uint8
    )


def _mad(a: np.ndarray, b: np.ndarray) -> float:
    return float(np.mean(np.abs(a.astype(np.int16) - b.astype(np.int16))))


@dataclass
class ActionTracker:
    name: str
    start_wall_ns: int
    baseline: np.ndarray
    end_wall_ns: int | None = None
    first_visible_wall_ns: int | None = None
    stable_wall_ns: int | None = None
    stable_streak: int = 0
    stable_start_wall_ns: int | None = None


def _write_paraview_calibration(output_path: Path, summary: dict) -> None:
    timer_summary_path = output_path.parent / "paraview_timer_summary.json"
    if not timer_summary_path.exists():
        return

    timer_summary = json.loads(timer_summary_path.read_text(encoding="utf-8"))
    timer_actions = {
        str(entry["action"]): entry
        for entry in timer_summary.get("actions", [])
        if isinstance(entry, dict) and "action" in entry
    }

    calibration = {
        "app": summary.get("app"),
        "capture_summary_path": str(output_path),
        "timer_summary_path": str(timer_summary_path),
        "runtime_mode": timer_summary.get("runtime_mode"),
        "requested_view_size": timer_summary.get("requested_view_size"),
        "actual_render_window_size": timer_summary.get("actual_render_window_size"),
        "actions": [],
    }
    for capture_entry in summary.get("actions", []):
        action_name = str(capture_entry.get("action"))
        timer_entry = timer_actions.get(action_name, {})
        calibration["actions"].append(
            {
                "action": action_name,
                "capture_first_visible_ms_from_start": capture_entry.get(
                    "first_visible_ms_from_start"
                ),
                "capture_final_view_ms_from_start": capture_entry.get(
                    "stable_ms_from_start"
                ),
                "capture_stable_ms_from_start": capture_entry.get(
                    "stable_ms_from_start"
                ),
                "capture_stable_ms_from_end": capture_entry.get("stable_ms_from_end"),
                "timer_first_render_complete_ms_from_action_start": timer_entry.get(
                    "first_render_complete_ms_from_action_start"
                ),
                "timer_final_render_complete_ms_from_action_start": timer_entry.get(
                    "final_render_complete_ms_from_action_start"
                ),
                "timer_first_interactive_render_complete_ms_from_action_start": timer_entry.get(
                    "first_interactive_render_complete_ms_from_action_start"
                ),
                "timer_first_still_render_complete_ms_from_action_start": timer_entry.get(
                    "first_still_render_complete_ms_from_action_start"
                ),
                "timer_scope_duration_ms": timer_entry.get("scope_duration_ms"),
                "timer_render_total_ms": timer_entry.get("render_total_ms"),
                "timer_interactive_render_total_ms": timer_entry.get(
                    "interactive_render_total_ms"
                ),
                "timer_still_render_total_ms": timer_entry.get("still_render_total_ms"),
                "timer_render_event_count": timer_entry.get("render_event_count"),
            }
        )

    calibration_path = output_path.parent / "paraview_timing_calibration.json"
    calibration_path.write_text(
        json.dumps(calibration, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Capture a fixed screen region and compute first-visible/final-stable timings "
            "from a benchmark action marker file."
        )
    )
    parser.add_argument(
        "--events", required=True, help="JSONL marker file written by the driver"
    )
    parser.add_argument("--output", required=True, help="Summary JSON output path")
    parser.add_argument(
        "--frames-output",
        default=None,
        help=(
            "Optional JSONL output for every captured frame sample. "
            "Defaults to <output stem>_frames.jsonl next to --output."
        ),
    )
    parser.add_argument("--x", type=int, required=True, help="Capture region left")
    parser.add_argument("--y", type=int, required=True, help="Capture region top")
    parser.add_argument("--width", type=int, required=True, help="Capture region width")
    parser.add_argument(
        "--height", type=int, required=True, help="Capture region height"
    )
    parser.add_argument("--sample-hz", type=float, default=30.0, help="Capture cadence")
    parser.add_argument(
        "--pixel-threshold",
        type=float,
        default=1.0,
        help="Mean absolute grayscale difference threshold",
    )
    parser.add_argument(
        "--stable-frames",
        type=int,
        default=5,
        help="Number of consecutive low-diff frames required for final stability",
    )
    parser.add_argument(
        "--timeout-seconds",
        type=float,
        default=300.0,
        help="Stop if no session_end arrives before this timeout",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    events_path = Path(args.events)
    output_path = Path(args.output)
    frames_output_path = (
        Path(args.frames_output)
        if args.frames_output is not None
        else output_path.with_name(f"{output_path.stem}_frames.jsonl")
    )
    output_path.parent.mkdir(parents=True, exist_ok=True)
    frames_output_path.parent.mkdir(parents=True, exist_ok=True)
    region = {
        "left": int(args.x),
        "top": int(args.y),
        "width": int(args.width),
        "height": int(args.height),
    }

    start_monotonic_ns = time.monotonic_ns()
    timeout_ns = int(args.timeout_seconds * 1e9)
    sample_interval_ns = int(1e9 / max(args.sample_hz, 1.0))
    event_offset = 0
    app_name = None
    session_ended = False
    current_action: ActionTracker | None = None
    completed_actions: list[ActionTracker] = []
    previous_frame: np.ndarray | None = None
    frame_count = 0

    with frames_output_path.open("w", encoding="utf-8") as frame_stream:
        with mss.mss() as capturer:
            while True:
                events, event_offset = _load_events(events_path, event_offset)
                for event in events:
                    name = event.get("event")
                    if name == "session_start":
                        app_name = event.get("app")
                    elif name == "action_start":
                        if previous_frame is None:
                            continue
                        current_action = ActionTracker(
                            name=str(event["action"]),
                            start_wall_ns=int(event["wall_time_ns"]),
                            baseline=previous_frame.copy(),
                        )
                    elif name == "action_end" and current_action is not None:
                        if event.get("action") == current_action.name:
                            current_action.end_wall_ns = int(event["wall_time_ns"])
                    elif name == "session_end":
                        session_ended = True

                frame_monotonic_ns = time.monotonic_ns()
                shot = np.asarray(capturer.grab(region))
                frame_wall_ns = time.time_ns()
                frame = _to_gray(shot)

                diff_prev = (
                    _mad(frame, previous_frame) if previous_frame is not None else None
                )
                diff_baseline = None
                first_visible_triggered = False
                stable_triggered = False
                active_action_name = (
                    current_action.name if current_action is not None else None
                )
                active_action_end_wall_ns = (
                    current_action.end_wall_ns if current_action is not None else None
                )

                if current_action is not None:
                    diff_baseline = _mad(frame, current_action.baseline)
                    if (
                        current_action.first_visible_wall_ns is None
                        and frame_wall_ns >= current_action.start_wall_ns
                        and diff_baseline > args.pixel_threshold
                    ):
                        current_action.first_visible_wall_ns = frame_wall_ns
                        first_visible_triggered = True

                    if (
                        current_action.end_wall_ns is not None
                        and previous_frame is not None
                    ):
                        assert diff_prev is not None
                        if diff_prev <= args.pixel_threshold:
                            if current_action.stable_streak == 0:
                                current_action.stable_start_wall_ns = frame_wall_ns
                            current_action.stable_streak += 1
                            if current_action.stable_streak >= args.stable_frames:
                                current_action.stable_wall_ns = (
                                    current_action.stable_start_wall_ns
                                )
                                stable_triggered = True
                        else:
                            current_action.stable_streak = 0
                            current_action.stable_start_wall_ns = None

                    if (
                        current_action.end_wall_ns is not None
                        and current_action.stable_wall_ns is not None
                    ):
                        completed_actions.append(current_action)
                        current_action = None

                frame_record = {
                    "frame_index": frame_count,
                    "wall_time_ns": frame_wall_ns,
                    "monotonic_ns": frame_monotonic_ns,
                    "active_action": active_action_name,
                    "active_action_end_wall_ns": active_action_end_wall_ns,
                    "diff_prev": diff_prev,
                    "diff_action_baseline": diff_baseline,
                    "first_visible_triggered": first_visible_triggered,
                    "stable_triggered": stable_triggered,
                    "stable_frames_required": int(args.stable_frames),
                    "pixel_threshold": float(args.pixel_threshold),
                }
                frame_stream.write(json.dumps(frame_record, sort_keys=True) + "\n")
                frame_stream.flush()
                frame_count += 1

                previous_frame = frame

                if session_ended and current_action is None:
                    break
                if time.monotonic_ns() - start_monotonic_ns > timeout_ns:
                    break

                next_tick_ns = time.monotonic_ns() + sample_interval_ns
                while True:
                    remaining_ns = next_tick_ns - time.monotonic_ns()
                    if remaining_ns <= 0:
                        break
                    time.sleep(min(remaining_ns / 1e9, 0.005))

    summary = {
        "app": app_name,
        "region": region,
        "frames_output": str(frames_output_path),
        "frame_count": frame_count,
        "sample_hz": float(args.sample_hz),
        "pixel_threshold": float(args.pixel_threshold),
        "stable_frames": int(args.stable_frames),
        "actions": [],
    }
    for action in completed_actions:
        entry = {
            "action": action.name,
            "start_wall_ns": action.start_wall_ns,
            "end_wall_ns": action.end_wall_ns,
            "first_visible_wall_ns": action.first_visible_wall_ns,
            "stable_wall_ns": action.stable_wall_ns,
        }
        if action.first_visible_wall_ns is not None:
            entry["first_visible_ms_from_start"] = (
                action.first_visible_wall_ns - action.start_wall_ns
            ) / 1e6
        if action.end_wall_ns is not None and action.stable_wall_ns is not None:
            entry["stable_ms_from_end"] = (
                action.stable_wall_ns - action.end_wall_ns
            ) / 1e6
            entry["stable_ms_from_start"] = (
                action.stable_wall_ns - action.start_wall_ns
            ) / 1e6
        summary["actions"].append(entry)

    output_path.write_text(
        json.dumps(summary, indent=2, sort_keys=True), encoding="utf-8"
    )
    _write_paraview_calibration(output_path, summary)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
