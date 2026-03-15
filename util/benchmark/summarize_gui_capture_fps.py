#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any


def _load_jsonl(path: Path) -> list[dict[str, Any]]:
    if not path.exists():
        raise FileNotFoundError(path)
    rows: list[dict[str, Any]] = []
    with path.open("r", encoding="utf-8") as stream:
        for line in stream:
            line = line.strip()
            if not line:
                continue
            rows.append(json.loads(line))
    return rows


def _stats(values: list[float]) -> dict[str, Any]:
    if not values:
        return {
            "count": 0,
            "mean": None,
            "median": None,
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
        "min": ordered[0],
        "max": ordered[-1],
        "p95": percentile(0.95),
    }


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Summarize visible GUI frame cadence from a capture summary_frames.jsonl file "
            "and matching event markers."
        )
    )
    parser.add_argument("--events", required=True, help="JSONL event marker file")
    parser.add_argument(
        "--frames",
        required=True,
        help="Capture frames JSONL from volume_benchmark_capture.py",
    )
    parser.add_argument(
        "--capture-summary",
        default=None,
        help="Optional capture summary JSON for first-visible / stable timings",
    )
    parser.add_argument("--output", required=True, help="Output summary JSON path")
    parser.add_argument(
        "--pixel-threshold",
        type=float,
        default=None,
        help="Override visible-change threshold. Defaults to the frame log threshold.",
    )
    return parser.parse_args()


def _action_windows(events: list[dict[str, Any]]) -> dict[str, dict[str, int]]:
    by_name: dict[str, dict[str, int]] = {}
    for event in events:
        name = event.get("action")
        event_name = event.get("event")
        if not isinstance(name, str) or not isinstance(event_name, str):
            continue
        window = by_name.setdefault(name, {})
        wall_time_ns = int(event["wall_time_ns"])
        monotonic_ns = int(event["monotonic_ns"])
        if event_name == "action_start":
            window["action_start_wall_ns"] = wall_time_ns
            window["action_start_monotonic_ns"] = monotonic_ns
        elif event_name == "action_end":
            window["action_end_wall_ns"] = wall_time_ns
            window["action_end_monotonic_ns"] = monotonic_ns
        elif event_name == "drag_start":
            window["drag_start_wall_ns"] = wall_time_ns
            window["drag_start_monotonic_ns"] = monotonic_ns
        elif event_name == "drag_end":
            window["drag_end_wall_ns"] = wall_time_ns
            window["drag_end_monotonic_ns"] = monotonic_ns
    return by_name


def _frame_timestamp_key(frames: list[dict[str, Any]]) -> tuple[str, str]:
    for candidate in ("display_monotonic_ns", "monotonic_ns", "wall_time_ns"):
        for frame in frames:
            if frame.get(candidate) is not None:
                return candidate, (
                    "monotonic" if candidate != "wall_time_ns" else "wall"
                )
    return "wall_time_ns", "wall"


def _median_int(values: list[int]) -> int | None:
    if not values:
        return None
    ordered = sorted(values)
    count = len(ordered)
    mid = count // 2
    if count % 2 == 1:
        return ordered[mid]
    return (ordered[mid - 1] + ordered[mid]) // 2


def main() -> int:
    args = _parse_args()
    events = _load_jsonl(Path(args.events))
    frames = _load_jsonl(Path(args.frames))
    capture_summary = (
        json.loads(Path(args.capture_summary).read_text(encoding="utf-8"))
        if args.capture_summary is not None
        else None
    )
    capture_actions = {
        str(entry.get("action")): entry
        for entry in (capture_summary or {}).get("actions", [])
        if isinstance(entry, dict)
    }

    action_windows = _action_windows(events)
    frame_timestamp_key, timestamp_domain = _frame_timestamp_key(frames)
    frame_timestamp_offset_ns = None
    if timestamp_domain == "monotonic":
        offsets = [
            int(frame["wall_time_ns"]) - int(frame[frame_timestamp_key])
            for frame in frames
            if frame.get("wall_time_ns") is not None
            and frame.get(frame_timestamp_key) is not None
        ]
        frame_timestamp_offset_ns = _median_int(offsets) if offsets else 0

    def frame_time_ns(frame: dict[str, Any]) -> int:
        raw = int(frame[frame_timestamp_key])
        if timestamp_domain == "monotonic":
            return raw + int(frame_timestamp_offset_ns or 0)
        return raw

    sample_intervals_ms: list[float] = []
    for prev, cur in zip(frames, frames[1:]):
        sample_intervals_ms.append(
            (int(cur[frame_timestamp_key]) - int(prev[frame_timestamp_key])) / 1e6
        )

    default_threshold = None
    default_fraction_threshold = None
    for frame in frames:
        threshold = frame.get("pixel_threshold")
        if threshold is not None:
            default_threshold = float(threshold)
            break
    for frame in frames:
        threshold = frame.get("changed_fraction_threshold")
        if threshold is not None:
            default_fraction_threshold = float(threshold)
            break
    pixel_threshold = (
        float(args.pixel_threshold)
        if args.pixel_threshold is not None
        else (default_threshold if default_threshold is not None else 1.0)
    )

    summary_actions: list[dict[str, Any]] = []
    for action_name, window in action_windows.items():
        start_ns = int(
            window.get("drag_start_wall_ns") or window.get("action_start_wall_ns") or 0
        )
        end_ns = int(
            window.get("drag_end_wall_ns") or window.get("action_end_wall_ns") or 0
        )
        if start_ns <= 0 or end_ns <= start_ns:
            continue

        window_frames = [
            frame for frame in frames if start_ns <= frame_time_ns(frame) <= end_ns
        ]
        if any(
            frame.get("significant_change_prev") is not None for frame in window_frames
        ):
            changed_frames = [
                frame
                for frame in window_frames
                if bool(frame.get("significant_change_prev"))
            ]
        else:
            changed_frames = [
                frame
                for frame in window_frames
                if frame.get("diff_prev") is not None
                and float(frame["diff_prev"]) > pixel_threshold
            ]
        changed_times_ns = [frame_time_ns(frame) for frame in changed_frames]
        changed_intervals_ms = [
            (cur - prev) / 1e6
            for prev, cur in zip(changed_times_ns, changed_times_ns[1:])
        ]
        duration_ms = (end_ns - start_ns) / 1e6
        changed_count = len(changed_frames)
        sample_count = len(window_frames)
        changed_fps = (
            (changed_count * 1000.0 / duration_ms) if duration_ms > 0.0 else None
        )
        capture_samples_per_second = (
            (sample_count * 1000.0 / duration_ms) if duration_ms > 0.0 else None
        )
        interval_stats = _stats(changed_intervals_ms)

        action_entry: dict[str, Any] = {
            "action": action_name,
            "anchor_start": "drag_start"
            if "drag_start_wall_ns" in window
            else "action_start",
            "anchor_end": "drag_end" if "drag_end_wall_ns" in window else "action_end",
            "timestamp_domain": timestamp_domain,
            "frame_timestamp_key": frame_timestamp_key,
            "frame_timestamp_offset_ns": frame_timestamp_offset_ns,
            "start_timestamp_ns": start_ns,
            "end_timestamp_ns": end_ns,
            "start_wall_ns": start_ns,
            "end_wall_ns": end_ns,
            "duration_ms": duration_ms,
            "pixel_threshold": pixel_threshold,
            "changed_fraction_threshold": default_fraction_threshold,
            "sample_count_in_window": sample_count,
            "capture_samples_per_second": capture_samples_per_second,
            "changed_sample_count": changed_count,
            "changed_samples_per_second": changed_fps,
            "changed_interval_ms": interval_stats,
            "visible_fps_from_mean_interval": (
                1000.0 / interval_stats["mean"]
                if interval_stats["mean"] not in (None, 0.0)
                else None
            ),
            "first_changed_ms_from_start": (
                (changed_times_ns[0] - start_ns) / 1e6 if changed_times_ns else None
            ),
            "last_changed_ms_from_start": (
                (changed_times_ns[-1] - start_ns) / 1e6 if changed_times_ns else None
            ),
        }

        capture_entry = capture_actions.get(action_name)
        if capture_entry is not None:
            action_entry["capture_first_visible_ms_from_start"] = capture_entry.get(
                "first_visible_ms_from_start"
            )
            action_entry["capture_stable_ms_from_start"] = capture_entry.get(
                "stable_ms_from_start"
            )
            action_entry["capture_stable_ms_from_end"] = capture_entry.get(
                "stable_ms_from_end"
            )

        summary_actions.append(action_entry)

    summary = {
        "events_path": str(Path(args.events).resolve()),
        "frames_path": str(Path(args.frames).resolve()),
        "capture_summary_path": (
            str(Path(args.capture_summary).resolve())
            if args.capture_summary is not None
            else None
        ),
        "pixel_threshold": pixel_threshold,
        "timestamp_domain": timestamp_domain,
        "frame_timestamp_key": frame_timestamp_key,
        "frame_timestamp_offset_ns": frame_timestamp_offset_ns,
        "observed_sample_interval_ms": _stats(sample_intervals_ms),
        "observed_sample_hz": (
            1000.0 / (sum(sample_intervals_ms) / len(sample_intervals_ms))
            if sample_intervals_ms
            else None
        ),
        "actions": summary_actions,
    }

    output_path = Path(args.output)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
