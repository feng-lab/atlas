from __future__ import annotations

import argparse
import csv
import json
import math
import re
import statistics
import subprocess
import time
from collections import defaultdict
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


OPEN_METRICS = (
    "open_load_and_camera_ms",
    "open_total_to_first_preview_ms",
    "open_total_to_final_ms",
    "open_postload_to_first_preview_ms",
    "open_postload_to_final_ms",
    "open_preview_engine_elapsed_ms",
    "open_final_engine_elapsed_ms",
)

STEP_METRICS = (
    "preview_client_ms",
    "preview_engine_elapsed_ms",
    "final_client_ms",
    "final_engine_elapsed_ms",
)

FAST_PATTERN = "ATLAS_BENCHMARK_FAST_PREVIEW_DONE"
FINAL_PATTERN = "ATLAS_BENCHMARK_RENDER_FINISHED"

LOG_TIMESTAMP_RE = re.compile(
    r"^[IWEF](?:(?P<year>\d{4})(?P<month>\d{2})(?P<day>\d{2})|(?P<month2>\d{2})(?P<day2>\d{2})) "
    r"(?P<hour>\d{2}):(?P<minute>\d{2}):(?P<second>\d{2})\.(?P<fraction>\d+)"
)
FAST_RE = re.compile(
    r"ATLAS_BENCHMARK_FAST_PREVIEW_DONE(?:\s+elapsed_ms=(?P<elapsed>[-+0-9.eE]+))?"
    r"(?:\s+progress=(?P<progress>[-+0-9.eE]+))?"
)
FINAL_RE = re.compile(
    r"ATLAS_BENCHMARK_RENDER_FINISHED(?:\s+elapsed_ms=(?P<elapsed>[-+0-9.eE]+))?"
    r"(?:\s+progress=(?P<progress>[-+0-9.eE]+))?"
    r"(?:\s+source=(?P<source>\S+))?"
)


@dataclass(frozen=True)
class RunRecord:
    label: str
    warmup: bool
    output_dir: Path
    elapsed_wall_seconds: float
    events: list[dict[str, Any]]
    log_entries: list[dict[str, Any]]
    open_metrics: dict[str, Any]
    step_metrics: list[dict[str, Any]]
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
            "sum": None,
            "min": None,
            "max": None,
            "mean": None,
            "median": None,
            "stdev": None,
            "cv": None,
            "p05": None,
            "p10": None,
            "p25": None,
            "p75": None,
            "p90": None,
            "p95": None,
            "p99": None,
            "iqr": None,
        }
    mean_value = statistics.fmean(ordered)
    stdev_value = statistics.stdev(ordered) if len(ordered) > 1 else 0.0
    p25 = _quantile(ordered, 0.25)
    p75 = _quantile(ordered, 0.75)
    return {
        "count": len(ordered),
        "sum": float(sum(ordered)),
        "min": float(min(ordered)),
        "max": float(max(ordered)),
        "mean": float(mean_value),
        "median": float(statistics.median(ordered)),
        "stdev": float(stdev_value),
        "cv": float(stdev_value / mean_value) if mean_value else None,
        "p05": _quantile(ordered, 0.05),
        "p10": _quantile(ordered, 0.10),
        "p25": p25,
        "p75": p75,
        "p90": _quantile(ordered, 0.90),
        "p95": _quantile(ordered, 0.95),
        "p99": _quantile(ordered, 0.99),
        "iqr": (float(p75) - float(p25))
        if p25 is not None and p75 is not None
        else None,
    }


def _parse_args() -> argparse.Namespace:
    script_dir = Path(__file__).resolve().parent
    default_output_root = (
        Path("/Users/feng/Dropbox/atlas_test/slice15_paraview")
        / "benchmarks"
        / f"atlas_deterministic_{datetime.now().strftime('%Y%m%d_%H%M%S')}"
    )
    parser = argparse.ArgumentParser(
        description=(
            "Run the Atlas deterministic benchmark repeatedly, persist every run, "
            "and aggregate open/step timing statistics from RPC markers plus Atlas log lines."
        )
    )
    parser.add_argument(
        "--driver-script",
        default=str(script_dir / "atlas_volume_benchmark.py"),
        help="Path to the Atlas benchmark driver script",
    )
    parser.add_argument(
        "--dataset",
        default="/Users/feng/Dropbox/atlas_test/slice15_paraview/slice15_ch2_dense.nim",
        help="Atlas dataset path",
    )
    parser.add_argument(
        "--camera-spec",
        default="/Users/feng/Dropbox/atlas_test/slice15_paraview/slice15_scene_camera_exact_2000x1500.json",
        help="Shared benchmark camera spec",
    )
    parser.add_argument(
        "--output-root",
        default=str(default_output_root),
        help="Root directory for warm-up, measured runs, and aggregate outputs",
    )
    parser.add_argument(
        "--atlas-log-path",
        required=True,
        help="Path to the active Atlas application log file that contains the benchmark lines",
    )
    parser.add_argument(
        "--address", default="localhost:50051", help="Atlas Scene RPC address"
    )
    parser.add_argument(
        "--canvas-logical-width",
        type=int,
        default=None,
        help="Optional live 3D canvas width in logical Qt pixels for every run",
    )
    parser.add_argument(
        "--canvas-logical-height",
        type=int,
        default=None,
        help="Optional live 3D canvas height in logical Qt pixels for every run",
    )
    parser.add_argument(
        "--warmup-runs", type=int, default=1, help="Number of warm-up runs"
    )
    parser.add_argument(
        "--measured-runs", type=int, default=7, help="Number of measured runs"
    )
    parser.add_argument(
        "--task-timeout-seconds",
        type=float,
        default=300.0,
        help="Timeout for Atlas StartLoadTask/WaitTask",
    )
    parser.add_argument(
        "--ready-timeout-seconds",
        type=float,
        default=120.0,
        help="Timeout for Atlas WaitForObjectsReady",
    )
    parser.add_argument(
        "--pre-action-delay-seconds",
        type=float,
        default=0.2,
        help="Delay after action_start before the driver mutates the scene",
    )
    parser.add_argument(
        "--step-hold-seconds",
        type=float,
        default=2.0,
        help=(
            "Fixed delay between successive camera commands for interpolate actions. "
            "Use a value large enough for Atlas to finish preview/final before the next step."
        ),
    )
    parser.add_argument(
        "--sample-rss",
        action="store_true",
        help="Sample Atlas RSS during each run. Requires --atlas-pid.",
    )
    parser.add_argument(
        "--atlas-pid", type=int, default=None, help="Atlas process ID for RSS sampling"
    )
    parser.add_argument(
        "--rss-sample-interval-seconds",
        type=float,
        default=0.1,
        help="RSS sampling interval during each run",
    )
    parser.add_argument(
        "--capture-screenshots",
        action="store_true",
        help="Save one screenshot after each action settles",
    )
    parser.add_argument(
        "--disable-full-resolution",
        action="store_true",
        help="Do not enable Atlas 'Full Resolution Rendering' during the benchmark runs",
    )
    parser.add_argument(
        "--log-year",
        type=int,
        default=datetime.now().year,
        help="Fallback year to use when parsing glog timestamps that only include month/day",
    )
    parser.add_argument(
        "--extra-arg",
        action="append",
        default=[],
        help="Additional raw arguments forwarded to atlas_volume_benchmark.py",
    )
    return parser.parse_args()


def _load_jsonl(path: Path) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line:
            continue
        rows.append(json.loads(line))
    return rows


def _glog_line_time_ns(line: str, fallback_year: int) -> int | None:
    match = LOG_TIMESTAMP_RE.match(line)
    if not match:
        return None
    if match.group("year") is not None:
        year = int(match.group("year"))
        month = int(match.group("month"))
        day = int(match.group("day"))
    else:
        year = int(fallback_year)
        month = int(match.group("month2"))
        day = int(match.group("day2"))
    hour = int(match.group("hour"))
    minute = int(match.group("minute"))
    second = int(match.group("second"))
    fraction = match.group("fraction")
    fraction_ns = int((fraction + "000000000")[:9])
    # Atlas glog timestamps are emitted in UTC. Parse them in UTC so the
    # render markers line up with the RPC event wall-clock timestamps.
    base = datetime(year, month, day, hour, minute, second, tzinfo=timezone.utc)
    return int(base.timestamp()) * 1_000_000_000 + fraction_ns


def _parse_log_entries(text: str, fallback_year: int) -> list[dict[str, Any]]:
    entries: list[dict[str, Any]] = []
    for line in text.splitlines():
        kind = None
        match = FAST_RE.search(line)
        if match:
            kind = "preview"
        else:
            match = FINAL_RE.search(line)
            if match:
                kind = "final"
        if kind is None or match is None:
            continue
        wall_time_ns = _glog_line_time_ns(line, fallback_year)
        if wall_time_ns is None:
            continue
        elapsed_ms = match.groupdict().get("elapsed")
        progress = match.groupdict().get("progress")
        source = match.groupdict().get("source")
        entries.append(
            {
                "kind": kind,
                "wall_time_ns": wall_time_ns,
                "elapsed_ms": float(elapsed_ms) if elapsed_ms is not None else None,
                "progress": float(progress) if progress is not None else None,
                "source": source,
                "line": line,
            }
        )
    entries.sort(key=lambda entry: int(entry["wall_time_ns"]))
    return entries


def _read_log_slice(path: Path, start_offset: int, end_offset: int) -> str:
    with path.open("rb") as stream:
        stream.seek(max(0, start_offset))
        return stream.read(max(0, end_offset - start_offset)).decode(
            "utf-8", errors="replace"
        )


def _first_matching(
    log_entries: list[dict[str, Any]],
    *,
    kind: str,
    start_ns: int,
    end_ns: int,
) -> dict[str, Any] | None:
    for entry in log_entries:
        ts = int(entry["wall_time_ns"])
        if ts < start_ns:
            continue
        if ts >= end_ns:
            break
        if entry["kind"] == kind:
            return entry
    return None


def _extract_run_metrics(
    events: list[dict[str, Any]], log_entries: list[dict[str, Any]]
) -> tuple[dict[str, Any], list[dict[str, Any]]]:
    action_start_events = [
        event for event in events if event.get("event") == "action_start"
    ]
    session_end = next(
        (event for event in reversed(events) if event.get("event") == "session_end"),
        None,
    )
    session_end_ns = int(session_end["wall_time_ns"]) if session_end else 2**63 - 1

    action_start_ns_by_name: dict[str, int] = {}
    ordered_action_names: list[str] = []
    for event in action_start_events:
        action = str(event["action"])
        action_start_ns_by_name[action] = int(event["wall_time_ns"])
        ordered_action_names.append(action)

    next_action_start_ns: dict[str, int] = {}
    for index, action in enumerate(ordered_action_names):
        if index + 1 < len(ordered_action_names):
            next_action_start_ns[action] = int(
                action_start_events[index + 1]["wall_time_ns"]
            )
        else:
            next_action_start_ns[action] = session_end_ns

    open_metrics: dict[str, Any] = {"action": "open"}
    open_start_ns = action_start_ns_by_name.get("open")
    dataset_load_done = next(
        (event for event in events if event.get("event") == "dataset_load_done"),
        None,
    )
    if open_start_ns is not None and dataset_load_done is not None:
        dataset_load_done_ns = int(dataset_load_done["wall_time_ns"])
        boundary_ns = next_action_start_ns.get("open", session_end_ns)
        preview_entry = _first_matching(
            log_entries,
            kind="preview",
            start_ns=dataset_load_done_ns,
            end_ns=boundary_ns,
        )
        final_entry = _first_matching(
            log_entries, kind="final", start_ns=dataset_load_done_ns, end_ns=boundary_ns
        )
        open_metrics.update(
            {
                "open_start_ns": open_start_ns,
                "dataset_load_done_ns": dataset_load_done_ns,
                "window_end_ns": boundary_ns,
                "open_load_and_camera_ms": (dataset_load_done_ns - open_start_ns)
                / 1_000_000.0,
                "preview_found": preview_entry is not None,
                "final_found": final_entry is not None,
                "open_total_to_first_preview_ms": (
                    (int(preview_entry["wall_time_ns"]) - open_start_ns) / 1_000_000.0
                    if preview_entry is not None
                    else None
                ),
                "open_total_to_final_ms": (
                    (int(final_entry["wall_time_ns"]) - open_start_ns) / 1_000_000.0
                    if final_entry is not None
                    else None
                ),
                "open_postload_to_first_preview_ms": (
                    (int(preview_entry["wall_time_ns"]) - dataset_load_done_ns)
                    / 1_000_000.0
                    if preview_entry is not None
                    else None
                ),
                "open_postload_to_final_ms": (
                    (int(final_entry["wall_time_ns"]) - dataset_load_done_ns)
                    / 1_000_000.0
                    if final_entry is not None
                    else None
                ),
                "open_preview_engine_elapsed_ms": (
                    float(preview_entry["elapsed_ms"])
                    if preview_entry is not None
                    else None
                ),
                "open_final_engine_elapsed_ms": (
                    float(final_entry["elapsed_ms"])
                    if final_entry is not None
                    else None
                ),
                "preview_log_entry": preview_entry,
                "final_log_entry": final_entry,
            }
        )

    step_metrics: list[dict[str, Any]] = []
    for action in ordered_action_names:
        if action == "open":
            continue
        step_events = [
            event
            for event in events
            if event.get("event") == "action_step"
            and str(event.get("action")) == action
        ]
        action_boundary_ns = next_action_start_ns.get(action, session_end_ns)
        for index, event in enumerate(step_events):
            step_start_ns = int(event["wall_time_ns"])
            if index + 1 < len(step_events):
                boundary_ns = int(step_events[index + 1]["wall_time_ns"])
            else:
                boundary_ns = action_boundary_ns
            preview_entry = _first_matching(
                log_entries, kind="preview", start_ns=step_start_ns, end_ns=boundary_ns
            )
            final_entry = _first_matching(
                log_entries, kind="final", start_ns=step_start_ns, end_ns=boundary_ns
            )
            step_metrics.append(
                {
                    "action": action,
                    "step": int(event["step"]),
                    "steps": int(event["steps"]),
                    "step_start_ns": step_start_ns,
                    "window_end_ns": boundary_ns,
                    "window_ms": (boundary_ns - step_start_ns) / 1_000_000.0,
                    "preview_found": preview_entry is not None,
                    "final_found": final_entry is not None,
                    "preview_client_ms": (
                        (int(preview_entry["wall_time_ns"]) - step_start_ns)
                        / 1_000_000.0
                        if preview_entry is not None
                        else None
                    ),
                    "preview_engine_elapsed_ms": (
                        float(preview_entry["elapsed_ms"])
                        if preview_entry is not None
                        else None
                    ),
                    "final_client_ms": (
                        (int(final_entry["wall_time_ns"]) - step_start_ns) / 1_000_000.0
                        if final_entry is not None
                        else None
                    ),
                    "final_engine_elapsed_ms": (
                        float(final_entry["elapsed_ms"])
                        if final_entry is not None
                        else None
                    ),
                    "preview_source": preview_entry.get("source")
                    if preview_entry is not None
                    else None,
                    "final_source": final_entry.get("source")
                    if final_entry is not None
                    else None,
                }
            )

    return open_metrics, step_metrics


def _write_csv(path: Path, rows: list[dict[str, Any]], fieldnames: list[str]) -> None:
    with path.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fieldnames, extrasaction="ignore")
        writer.writeheader()
        for row in rows:
            writer.writerow(row)


def _run_command(
    args: argparse.Namespace, run_dir: Path, warmup: bool, run_index: int
) -> RunRecord:
    label = f"warmup_{run_index:02d}" if warmup else f"run_{run_index:02d}"
    run_dir.mkdir(parents=True, exist_ok=True)
    stdout_path = run_dir / "stdout.log"
    stderr_path = run_dir / "stderr.log"

    log_path = Path(args.atlas_log_path).resolve()
    start_offset = log_path.stat().st_size if log_path.exists() else 0

    command = [
        "python3",
        str(Path(args.driver_script).resolve()),
        "--dataset",
        str(Path(args.dataset).resolve()),
        "--camera-spec",
        str(Path(args.camera_spec).resolve()),
        "--output-dir",
        str(run_dir.resolve()),
        "--address",
        args.address,
        "--task-timeout-seconds",
        str(float(args.task_timeout_seconds)),
        "--ready-timeout-seconds",
        str(float(args.ready_timeout_seconds)),
        "--pre-action-delay-seconds",
        str(float(args.pre_action_delay_seconds)),
        "--step-hold-seconds",
        str(float(args.step_hold_seconds)),
    ]
    if (args.canvas_logical_width is None) != (args.canvas_logical_height is None):
        raise ValueError(
            "--canvas-logical-width and --canvas-logical-height must be provided together"
        )
    if args.canvas_logical_width is not None:
        command.extend(
            [
                "--canvas-logical-width",
                str(int(args.canvas_logical_width)),
                "--canvas-logical-height",
                str(int(args.canvas_logical_height)),
            ]
        )
    if args.capture_screenshots:
        command.append("--capture-screenshots")
    if args.disable_full_resolution:
        command.append("--disable-full-resolution")
    if args.sample_rss:
        if not args.atlas_pid:
            raise ValueError("--sample-rss requires --atlas-pid")
        command.extend(
            [
                "--sample-rss",
                "--rss-target-pid",
                str(int(args.atlas_pid)),
                "--rss-sample-interval-seconds",
                str(float(args.rss_sample_interval_seconds)),
            ]
        )
    for extra_arg in args.extra_arg:
        command.append(extra_arg)

    (run_dir / "command.json").write_text(
        json.dumps(command, indent=2) + "\n", encoding="utf-8"
    )

    start = time.monotonic()
    with (
        stdout_path.open("w", encoding="utf-8") as stdout_stream,
        stderr_path.open("w", encoding="utf-8") as stderr_stream,
    ):
        completed = subprocess.run(
            command, stdout=stdout_stream, stderr=stderr_stream, check=False
        )
    elapsed_wall_seconds = time.monotonic() - start
    if completed.returncode != 0:
        raise RuntimeError(
            f"Atlas benchmark run {label} failed with exit code {completed.returncode}. "
            f"See {stdout_path} and {stderr_path}."
        )

    end_offset = log_path.stat().st_size if log_path.exists() else start_offset
    log_text = _read_log_slice(log_path, start_offset, end_offset)
    log_entries = _parse_log_entries(log_text, int(args.log_year))
    events = _load_jsonl(run_dir / "atlas_events.jsonl")
    open_metrics, step_metrics = _extract_run_metrics(events, log_entries)
    memory_summary_path = run_dir / "atlas_memory_summary.json"
    memory_summary = (
        json.loads(memory_summary_path.read_text(encoding="utf-8"))
        if memory_summary_path.exists()
        else None
    )

    (run_dir / "atlas_render_log_slice.txt").write_text(log_text, encoding="utf-8")
    (run_dir / "atlas_render_log_events.json").write_text(
        json.dumps(log_entries, indent=2) + "\n", encoding="utf-8"
    )
    (run_dir / "atlas_open_metrics.json").write_text(
        json.dumps(open_metrics, indent=2) + "\n", encoding="utf-8"
    )
    (run_dir / "atlas_step_metrics.json").write_text(
        json.dumps(step_metrics, indent=2) + "\n", encoding="utf-8"
    )

    return RunRecord(
        label=label,
        warmup=warmup,
        output_dir=run_dir,
        elapsed_wall_seconds=elapsed_wall_seconds,
        events=events,
        log_entries=log_entries,
        open_metrics=open_metrics,
        step_metrics=step_metrics,
        memory_summary=memory_summary,
    )


def _aggregate_runs(
    root: Path, args: argparse.Namespace, runs: list[RunRecord]
) -> None:
    aggregate_dir = root / "aggregate"
    aggregate_dir.mkdir(parents=True, exist_ok=True)

    measured_runs = [run for run in runs if not run.warmup]
    warmup_runs = [run for run in runs if run.warmup]

    manifest = {
        "driver_script": str(Path(args.driver_script).resolve()),
        "dataset": str(Path(args.dataset).resolve()),
        "camera_spec": str(Path(args.camera_spec).resolve()),
        "atlas_log_path": str(Path(args.atlas_log_path).resolve()),
        "address": args.address,
        "warmup_runs": args.warmup_runs,
        "measured_runs": args.measured_runs,
        "step_hold_seconds": float(args.step_hold_seconds),
        "run_count_total": len(runs),
        "run_labels": [run.label for run in runs],
        "warmup_labels": [run.label for run in warmup_runs],
        "measured_labels": [run.label for run in measured_runs],
    }
    (aggregate_dir / "manifest.json").write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8"
    )

    run_records_json = [
        {
            "label": run.label,
            "warmup": run.warmup,
            "output_dir": str(run.output_dir),
            "elapsed_wall_seconds": run.elapsed_wall_seconds,
            "open_metrics_path": str(run.output_dir / "atlas_open_metrics.json"),
            "step_metrics_path": str(run.output_dir / "atlas_step_metrics.json"),
            "log_events_path": str(run.output_dir / "atlas_render_log_events.json"),
            "memory_summary_path": (
                str(run.output_dir / "atlas_memory_summary.json")
                if run.memory_summary
                else None
            ),
        }
        for run in runs
    ]
    (aggregate_dir / "runs.json").write_text(
        json.dumps(run_records_json, indent=2) + "\n", encoding="utf-8"
    )

    open_rows: list[dict[str, Any]] = []
    open_metric_values: dict[str, list[float]] = defaultdict(list)
    all_steps_path = aggregate_dir / "all_measured_steps.jsonl"
    all_log_events_path = aggregate_dir / "all_measured_log_events.jsonl"

    action_step_rows: list[dict[str, Any]] = []
    action_step_metric_values: dict[str, dict[str, list[float]]] = defaultdict(
        lambda: defaultdict(list)
    )
    step_index_metric_values: dict[str, dict[int, dict[str, list[float]]]] = (
        defaultdict(lambda: defaultdict(lambda: defaultdict(list)))
    )
    action_completion_counts: dict[str, dict[str, int]] = defaultdict(
        lambda: defaultdict(int)
    )

    memory_peaks: list[float] = []
    memory_first: list[float] = []
    memory_last: list[float] = []
    run_wall_seconds: list[float] = []

    with (
        all_steps_path.open("w", encoding="utf-8") as all_steps_stream,
        all_log_events_path.open("w", encoding="utf-8") as all_logs_stream,
    ):
        for run in measured_runs:
            run_wall_seconds.append(float(run.elapsed_wall_seconds))
            open_row = {"run_label": run.label}
            for metric_name in OPEN_METRICS:
                value = run.open_metrics.get(metric_name)
                open_row[metric_name] = value
                if value is not None:
                    open_metric_values[metric_name].append(float(value))
            open_row["preview_found"] = bool(run.open_metrics.get("preview_found"))
            open_row["final_found"] = bool(run.open_metrics.get("final_found"))
            open_rows.append(open_row)

            for entry in run.log_entries:
                row = dict(entry)
                row["run_label"] = run.label
                all_logs_stream.write(json.dumps(row, sort_keys=True) + "\n")

            for step_row in run.step_metrics:
                row = dict(step_row)
                row["run_label"] = run.label
                action = str(row["action"])
                step = int(row["step"])
                action_completion_counts[action]["step_count"] += 1
                if row.get("preview_found"):
                    action_completion_counts[action]["preview_found_count"] += 1
                if row.get("final_found"):
                    action_completion_counts[action]["final_found_count"] += 1
                for metric_name in STEP_METRICS:
                    value = row.get(metric_name)
                    if value is not None:
                        action_step_metric_values[action][metric_name].append(
                            float(value)
                        )
                        step_index_metric_values[action][step][metric_name].append(
                            float(value)
                        )
                action_step_rows.append(row)
                all_steps_stream.write(json.dumps(row, sort_keys=True) + "\n")

            if run.memory_summary:
                if run.memory_summary.get("peak_rss_bytes") is not None:
                    memory_peaks.append(float(run.memory_summary["peak_rss_bytes"]))
                if run.memory_summary.get("first_rss_bytes") is not None:
                    memory_first.append(float(run.memory_summary["first_rss_bytes"]))
                if run.memory_summary.get("last_rss_bytes") is not None:
                    memory_last.append(float(run.memory_summary["last_rss_bytes"]))

    (aggregate_dir / "open_metrics_by_run.json").write_text(
        json.dumps(open_rows, indent=2) + "\n", encoding="utf-8"
    )
    _write_csv(
        aggregate_dir / "open_metrics_by_run.csv",
        open_rows,
        ["run_label", "preview_found", "final_found", *OPEN_METRICS],
    )

    open_metric_stats = {
        metric_name: _stats(values)
        for metric_name, values in open_metric_values.items()
    }
    (aggregate_dir / "open_metric_stats.json").write_text(
        json.dumps(open_metric_stats, indent=2) + "\n", encoding="utf-8"
    )

    (aggregate_dir / "step_metrics_by_run.json").write_text(
        json.dumps(action_step_rows, indent=2) + "\n", encoding="utf-8"
    )
    _write_csv(
        aggregate_dir / "step_metrics_by_run.csv",
        action_step_rows,
        [
            "run_label",
            "action",
            "step",
            "steps",
            "window_ms",
            "preview_found",
            "final_found",
            *STEP_METRICS,
        ],
    )

    action_step_stats: dict[str, dict[str, Any]] = {}
    action_step_rows_summary: list[dict[str, Any]] = []
    for action, metric_map in action_step_metric_values.items():
        completion = action_completion_counts[action]
        action_step_stats[action] = {
            "step_count": int(completion["step_count"]),
            "preview_found_count": int(completion["preview_found_count"]),
            "final_found_count": int(completion["final_found_count"]),
            "preview_found_rate": (
                float(completion["preview_found_count"]) / completion["step_count"]
                if completion["step_count"]
                else None
            ),
            "final_found_rate": (
                float(completion["final_found_count"]) / completion["step_count"]
                if completion["step_count"]
                else None
            ),
        }
        row = {
            "action": action,
            "step_count": int(completion["step_count"]),
            "preview_found_count": int(completion["preview_found_count"]),
            "final_found_count": int(completion["final_found_count"]),
            "preview_found_rate": action_step_stats[action]["preview_found_rate"],
            "final_found_rate": action_step_stats[action]["final_found_rate"],
        }
        for metric_name, values in metric_map.items():
            stats = _stats(values)
            action_step_stats[action][metric_name] = stats
            row[f"{metric_name}_mean"] = stats["mean"]
            row[f"{metric_name}_median"] = stats["median"]
            row[f"{metric_name}_p95"] = stats["p95"]
            row[f"{metric_name}_stdev"] = stats["stdev"]
        action_step_rows_summary.append(row)
    (aggregate_dir / "action_step_stats.json").write_text(
        json.dumps(action_step_stats, indent=2) + "\n", encoding="utf-8"
    )
    _write_csv(
        aggregate_dir / "action_step_stats.csv",
        action_step_rows_summary,
        [
            "action",
            "step_count",
            "preview_found_count",
            "final_found_count",
            "preview_found_rate",
            "final_found_rate",
            "preview_client_ms_mean",
            "preview_client_ms_median",
            "preview_client_ms_p95",
            "preview_client_ms_stdev",
            "preview_engine_elapsed_ms_mean",
            "preview_engine_elapsed_ms_median",
            "preview_engine_elapsed_ms_p95",
            "preview_engine_elapsed_ms_stdev",
            "final_client_ms_mean",
            "final_client_ms_median",
            "final_client_ms_p95",
            "final_client_ms_stdev",
            "final_engine_elapsed_ms_mean",
            "final_engine_elapsed_ms_median",
            "final_engine_elapsed_ms_p95",
            "final_engine_elapsed_ms_stdev",
        ],
    )

    step_index_stats: dict[str, dict[str, Any]] = {}
    step_index_rows: list[dict[str, Any]] = []
    for action, step_map in step_index_metric_values.items():
        step_index_stats[action] = {}
        for step, metric_map in sorted(step_map.items()):
            step_index_stats[action][str(step)] = {}
            row = {"action": action, "step": step}
            for metric_name, values in metric_map.items():
                stats = _stats(values)
                step_index_stats[action][str(step)][metric_name] = stats
                row[f"{metric_name}_mean"] = stats["mean"]
                row[f"{metric_name}_median"] = stats["median"]
                row[f"{metric_name}_p95"] = stats["p95"]
            step_index_rows.append(row)
    (aggregate_dir / "step_index_stats.json").write_text(
        json.dumps(step_index_stats, indent=2) + "\n", encoding="utf-8"
    )
    _write_csv(
        aggregate_dir / "step_index_stats.csv",
        step_index_rows,
        [
            "action",
            "step",
            "preview_client_ms_mean",
            "preview_client_ms_median",
            "preview_client_ms_p95",
            "preview_engine_elapsed_ms_mean",
            "preview_engine_elapsed_ms_median",
            "preview_engine_elapsed_ms_p95",
            "final_client_ms_mean",
            "final_client_ms_median",
            "final_client_ms_p95",
            "final_engine_elapsed_ms_mean",
            "final_engine_elapsed_ms_median",
            "final_engine_elapsed_ms_p95",
        ],
    )

    memory_stats = {
        "peak_rss_bytes": _stats(memory_peaks),
        "first_rss_bytes": _stats(memory_first),
        "last_rss_bytes": _stats(memory_last),
        "run_elapsed_wall_seconds": _stats(run_wall_seconds),
    }
    (aggregate_dir / "memory_stats.json").write_text(
        json.dumps(memory_stats, indent=2) + "\n", encoding="utf-8"
    )

    summary = {
        "config": manifest,
        "open_metric_stats": open_metric_stats,
        "action_step_stats": action_step_stats,
        "step_index_stats_path": str(aggregate_dir / "step_index_stats.json"),
        "memory_stats": memory_stats,
    }
    (aggregate_dir / "summary.json").write_text(
        json.dumps(summary, indent=2) + "\n", encoding="utf-8"
    )

    lines = [
        "# Atlas deterministic benchmark",
        "",
        f"- Dataset: `{Path(args.dataset).resolve()}`",
        f"- Camera spec: `{Path(args.camera_spec).resolve()}`",
        f"- Atlas log: `{Path(args.atlas_log_path).resolve()}`",
        f"- Runs: `{args.warmup_runs}` warm-up + `{args.measured_runs}` measured",
        f"- Step hold: `{float(args.step_hold_seconds):.3f} s`",
        f"- Output root: `{root}`",
        "",
        "## Open",
        "",
    ]
    for metric_name in (
        "open_load_and_camera_ms",
        "open_total_to_first_preview_ms",
        "open_total_to_final_ms",
        "open_postload_to_first_preview_ms",
        "open_postload_to_final_ms",
    ):
        stats = open_metric_stats.get(metric_name)
        if stats and stats["count"]:
            lines.append(
                f"- {metric_name}: mean `{stats['mean']:.3f} ms`, "
                f"median `{stats['median']:.3f} ms`, p95 `{stats['p95']:.3f} ms`"
            )
    lines.extend(["", "## Actions", ""])
    for action in sorted(action_step_stats.keys()):
        stats = action_step_stats[action]
        lines.append(f"### {action}")
        lines.append(
            f"- preview completion rate: `{stats['preview_found_count']}/{stats['step_count']}` "
            f"({stats['preview_found_rate']:.3f})"
        )
        lines.append(
            f"- final completion rate: `{stats['final_found_count']}/{stats['step_count']}` "
            f"({stats['final_found_rate']:.3f})"
        )
        for metric_name in STEP_METRICS:
            metric_stats = stats.get(metric_name)
            if metric_stats and metric_stats["count"]:
                lines.append(
                    f"- {metric_name}: mean `{metric_stats['mean']:.3f} ms`, "
                    f"median `{metric_stats['median']:.3f} ms`, p95 `{metric_stats['p95']:.3f} ms`"
                )
        lines.append("")
    peak_rss_stats = memory_stats["peak_rss_bytes"]
    if peak_rss_stats["count"]:
        lines.extend(
            [
                "## Memory",
                "",
                "- peak RSS: "
                f"mean `{peak_rss_stats['mean']:.0f} bytes`, "
                f"median `{peak_rss_stats['median']:.0f} bytes`, "
                f"p95 `{peak_rss_stats['p95']:.0f} bytes`",
                "",
            ]
        )
    lines.extend(
        [
            "## Artifacts",
            "",
            "- `aggregate/summary.json`: top-level aggregate summary",
            "- `aggregate/open_metric_stats.json`: open timing statistics",
            "- `aggregate/action_step_stats.json`: pooled rotate/zoom step statistics",
            "- `aggregate/step_index_stats.json`: step 1, step 2, ... statistics across measured runs",
            "- `aggregate/all_measured_steps.jsonl`: every measured step with run labels",
            "- `aggregate/all_measured_log_events.jsonl`: parsed Atlas preview/final log lines with run labels",
            "",
        ]
    )
    (aggregate_dir / "summary.md").write_text("\n".join(lines), encoding="utf-8")


def main() -> int:
    args = _parse_args()
    root = Path(args.output_root).resolve()
    root.mkdir(parents=True, exist_ok=True)

    config = {
        "driver_script": str(Path(args.driver_script).resolve()),
        "dataset": str(Path(args.dataset).resolve()),
        "camera_spec": str(Path(args.camera_spec).resolve()),
        "atlas_log_path": str(Path(args.atlas_log_path).resolve()),
        "address": args.address,
        "warmup_runs": int(args.warmup_runs),
        "measured_runs": int(args.measured_runs),
        "step_hold_seconds": float(args.step_hold_seconds),
        "task_timeout_seconds": float(args.task_timeout_seconds),
        "ready_timeout_seconds": float(args.ready_timeout_seconds),
        "pre_action_delay_seconds": float(args.pre_action_delay_seconds),
        "sample_rss": bool(args.sample_rss),
        "atlas_pid": int(args.atlas_pid) if args.atlas_pid is not None else None,
        "rss_sample_interval_seconds": float(args.rss_sample_interval_seconds),
        "capture_screenshots": bool(args.capture_screenshots),
        "log_year": int(args.log_year),
        "extra_args": list(args.extra_arg),
    }
    (root / "config.json").write_text(
        json.dumps(config, indent=2) + "\n", encoding="utf-8"
    )

    run_records: list[RunRecord] = []
    warmup_dir = root / "warmup"
    measured_dir = root / "measured"
    for index in range(1, int(args.warmup_runs) + 1):
        run_records.append(
            _run_command(
                args, warmup_dir / f"run{index:02d}", warmup=True, run_index=index
            )
        )
    for index in range(1, int(args.measured_runs) + 1):
        run_records.append(
            _run_command(
                args, measured_dir / f"run{index:02d}", warmup=False, run_index=index
            )
        )

    _aggregate_runs(root, args, run_records)
    print(f"Wrote Atlas deterministic benchmark results to {root}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
