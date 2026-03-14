from __future__ import annotations

import argparse
import csv
import json
import math
import statistics
import subprocess
import sys
import time
from collections import defaultdict
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Any


DEFAULT_PVTHON = "/Applications/ParaView-6.1.0-RC1.app/Contents/bin/pvpython"

ACTION_METRICS = (
    "scope_duration_ms",
    "interactive_render_total_ms",
    "still_render_total_ms",
    "render_total_ms",
    "first_render_complete_ms_from_action_start",
    "final_render_complete_ms_from_action_start",
    "first_interactive_render_complete_ms_from_action_start",
    "last_interactive_render_complete_ms_from_action_start",
    "first_still_render_complete_ms_from_action_start",
    "last_still_render_complete_ms_from_action_start",
    "release_to_first_still_ms",
)


@dataclass(frozen=True)
class RunRecord:
    label: str
    warmup: bool
    output_dir: Path
    elapsed_wall_seconds: float
    process_returncode: int
    accepted_postrun_crash: bool
    timer_summary: dict[str, Any]
    frame_timeline: list[dict[str, Any]]
    memory_summary: dict[str, Any] | None


def _load_session_end_ok(events_path: Path) -> bool:
    if not events_path.exists():
        return False
    for line in events_path.read_text(encoding="utf-8").splitlines():
        if not line.strip():
            continue
        try:
            event = json.loads(line)
        except json.JSONDecodeError:
            continue
        if event.get("event") == "session_end":
            return bool(event.get("ok"))
    return False


def _artifacts_complete(run_dir: Path) -> bool:
    required = (
        run_dir / "paraview_events.jsonl",
        run_dir / "paraview_timer_summary.json",
        run_dir / "paraview_internal_frame_timeline.json",
    )
    return all(path.exists() for path in required)


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


def _fps_summary(values_ms: list[float]) -> dict[str, float | None]:
    stats = _stats(values_ms)
    mean_ms = stats["mean"]
    median_ms = stats["median"]
    p95_ms = stats["p95"]
    return {
        "mean_service_fps": (1000.0 / float(mean_ms)) if mean_ms else None,
        "median_service_fps": (1000.0 / float(median_ms)) if median_ms else None,
        "p95_service_fps": (1000.0 / float(p95_ms)) if p95_ms else None,
    }


def _parse_args() -> argparse.Namespace:
    script_dir = Path(__file__).resolve().parent
    default_output_root = (
        Path("/Users/feng/Dropbox/atlas_test/slice15_paraview")
        / "benchmarks"
        / f"paraview_deterministic_{datetime.now().strftime('%Y%m%d_%H%M%S')}"
    )

    parser = argparse.ArgumentParser(
        description=(
            "Run the ParaView deterministic benchmark repeatedly, persist every run, "
            "and compute aggregate per-action and per-frame statistics."
        )
    )
    parser.add_argument("--pvpython", default=DEFAULT_PVTHON, help="Path to pvpython")
    parser.add_argument(
        "--launch-wrapper",
        default="",
        help=(
            "Optional launcher placed before pvpython, for example "
            "launch_paraview_with_ospray_fix.sh."
        ),
    )
    parser.add_argument(
        "--benchmark-script",
        default=str(script_dir / "paraview_volume_benchmark.py"),
        help="Path to the ParaView benchmark driver script",
    )
    parser.add_argument(
        "--dataset",
        default="/Users/feng/Dropbox/atlas_test/slice15_paraview/slice15_ch2_grid_atlasscenespace.vtpd",
        help="ParaView dataset path",
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
        "--warmup-runs", type=int, default=1, help="Number of warm-up runs"
    )
    parser.add_argument(
        "--measured-runs", type=int, default=7, help="Number of measured runs"
    )
    parser.add_argument(
        "--array-name", default="channels", help="Point-data array to render"
    )
    parser.add_argument(
        "--channel-mode",
        choices=("component", "magnitude", "rgb-direct"),
        default="component",
        help="ParaView channel mode",
    )
    parser.add_argument(
        "--component", type=int, default=0, help="Component for channel-mode=component"
    )
    parser.add_argument(
        "--blend-mode",
        choices=(
            "composite",
            "maximum-intensity",
            "minimum-intensity",
            "average-intensity",
            "additive",
            "isosurface",
            "slice",
        ),
        default="maximum-intensity",
        help="Volume blend mode",
    )
    parser.add_argument(
        "--volume-rendering-mode",
        choices=("smart", "ray-cast-only", "gpu-based", "ospray-based"),
        default="smart",
        help="Explicit ParaView volume rendering mode.",
    )
    parser.add_argument(
        "--data-range-min",
        type=float,
        default=0.0,
        help="Transfer-function lower bound",
    )
    parser.add_argument(
        "--data-range-max",
        type=float,
        default=255.0,
        help="Transfer-function upper bound",
    )
    parser.add_argument(
        "--color-min-rgb",
        type=float,
        nargs=3,
        default=(0.0, 0.0, 0.0),
        metavar=("R", "G", "B"),
        help="RGB color at the transfer-function minimum",
    )
    parser.add_argument(
        "--color-max-rgb",
        type=float,
        nargs=3,
        default=(0.99215686, 0.0, 0.0),
        metavar=("R", "G", "B"),
        help="RGB color at the transfer-function maximum",
    )
    parser.add_argument(
        "--deterministic-mode",
        choices=("interactive-plus-final", "direct-final"),
        default="interactive-plus-final",
        help="Deterministic ParaView benchmark mode",
    )
    parser.add_argument(
        "--pre-action-delay-seconds",
        type=float,
        default=0.2,
        help="Delay after action_start before the driver mutates the view",
    )
    parser.add_argument(
        "--sample-rss",
        action="store_true",
        default=True,
        help="Sample ParaView RSS during each run",
    )
    parser.add_argument(
        "--rss-sample-interval-seconds",
        type=float,
        default=0.1,
        help="RSS sampling interval during each run",
    )
    parser.add_argument(
        "--start-delay-seconds",
        type=float,
        default=0.0,
        help="Optional initial delay inside each ParaView run",
    )
    parser.add_argument(
        "--enable-ray-tracing",
        action="store_true",
        help="Enable ParaView view-level ray tracing for the benchmark runs.",
    )
    parser.add_argument(
        "--ray-tracing-backend",
        default="",
        help="Optional view-level ray tracing backend, e.g. 'OSPRay raycaster'.",
    )
    parser.add_argument(
        "--samples-per-pixel",
        type=int,
        default=None,
        help="Optional view-level SamplesPerPixel override.",
    )
    parser.add_argument(
        "--progressive-passes",
        type=int,
        default=None,
        help="Optional view-level ProgressivePasses override.",
    )
    parser.add_argument(
        "--ambient-samples",
        type=int,
        default=None,
        help="Optional view-level AmbientSamples override.",
    )
    parser.add_argument(
        "--denoise",
        type=int,
        choices=(0, 1),
        default=None,
        help="Optional view-level denoise toggle.",
    )
    parser.add_argument(
        "--extra-arg",
        action="append",
        default=[],
        help="Additional raw arguments forwarded to paraview_volume_benchmark.py",
    )
    return parser.parse_args()


def _run_command(
    args: argparse.Namespace, run_dir: Path, warmup: bool, run_index: int
) -> RunRecord:
    label = f"warmup_{run_index:02d}" if warmup else f"run_{run_index:02d}"
    run_dir.mkdir(parents=True, exist_ok=True)
    stdout_path = run_dir / "stdout.log"
    stderr_path = run_dir / "stderr.log"

    command: list[str] = []
    if args.launch_wrapper:
        command.extend([str(Path(args.launch_wrapper).resolve()), "pvpython"])
    else:
        command.append(str(Path(args.pvpython).resolve()))

    command.extend(
        [
            str(Path(args.benchmark_script).resolve()),
            "--dataset",
            str(Path(args.dataset).resolve()),
            "--camera-spec",
            str(Path(args.camera_spec).resolve()),
            "--output-dir",
            str(run_dir.resolve()),
            "--array-name",
            args.array_name,
            "--channel-mode",
            args.channel_mode,
            "--component",
            str(int(args.component)),
            "--blend-mode",
            args.blend_mode,
            "--volume-rendering-mode",
            args.volume_rendering_mode,
            "--data-range-min",
            str(float(args.data_range_min)),
            "--data-range-max",
            str(float(args.data_range_max)),
            "--color-min-rgb",
            *(str(float(v)) for v in args.color_min_rgb),
            "--color-max-rgb",
            *(str(float(v)) for v in args.color_max_rgb),
            "--deterministic-mode",
            args.deterministic_mode,
            "--pre-action-delay-seconds",
            str(float(args.pre_action_delay_seconds)),
            "--start-delay-seconds",
            str(float(args.start_delay_seconds)),
        ]
    )
    if args.sample_rss:
        command.extend(
            [
                "--sample-rss",
                "--rss-sample-interval-seconds",
                str(float(args.rss_sample_interval_seconds)),
            ]
        )
    if args.enable_ray_tracing:
        command.append("--enable-ray-tracing")
    if args.ray_tracing_backend:
        command.extend(["--ray-tracing-backend", args.ray_tracing_backend])
    if args.samples_per_pixel is not None:
        command.extend(["--samples-per-pixel", str(int(args.samples_per_pixel))])
    if args.progressive_passes is not None:
        command.extend(["--progressive-passes", str(int(args.progressive_passes))])
    if args.ambient_samples is not None:
        command.extend(["--ambient-samples", str(int(args.ambient_samples))])
    if args.denoise is not None:
        command.extend(["--denoise", str(int(args.denoise))])
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
    accepted_postrun_crash = False
    if completed.returncode != 0:
        if completed.returncode == -11 and _artifacts_complete(run_dir):
            if _load_session_end_ok(run_dir / "paraview_events.jsonl"):
                accepted_postrun_crash = True
        if not accepted_postrun_crash:
            raise RuntimeError(
                f"ParaView benchmark run {label} failed with exit code {completed.returncode}. "
                f"See {stdout_path} and {stderr_path}."
            )

    timer_summary_path = run_dir / "paraview_timer_summary.json"
    frame_timeline_path = run_dir / "paraview_internal_frame_timeline.json"
    memory_summary_path = run_dir / "paraview_memory_summary.json"
    timer_summary = json.loads(timer_summary_path.read_text(encoding="utf-8"))
    frame_timeline = json.loads(frame_timeline_path.read_text(encoding="utf-8"))
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
        accepted_postrun_crash=accepted_postrun_crash,
        timer_summary=timer_summary,
        frame_timeline=frame_timeline,
        memory_summary=memory_summary,
    )


def _write_csv(path: Path, rows: list[dict[str, Any]], fieldnames: list[str]) -> None:
    with path.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fieldnames)
        writer.writeheader()
        for row in rows:
            writer.writerow(row)


def _aggregate_runs(
    root: Path, args: argparse.Namespace, runs: list[RunRecord]
) -> None:
    aggregate_dir = root / "aggregate"
    aggregate_dir.mkdir(parents=True, exist_ok=True)

    measured_runs = [run for run in runs if not run.warmup]
    warmup_runs = [run for run in runs if run.warmup]

    manifest = {
        "pvpython": str(Path(args.pvpython).resolve()),
        "launch_wrapper": (
            str(Path(args.launch_wrapper).resolve()) if args.launch_wrapper else None
        ),
        "benchmark_script": str(Path(args.benchmark_script).resolve()),
        "dataset": str(Path(args.dataset).resolve()),
        "camera_spec": str(Path(args.camera_spec).resolve()),
        "deterministic_mode": args.deterministic_mode,
        "volume_rendering_mode": args.volume_rendering_mode,
        "warmup_runs": args.warmup_runs,
        "measured_runs": args.measured_runs,
        "run_count_total": len(runs),
        "run_labels": [run.label for run in runs],
        "warmup_labels": [run.label for run in warmup_runs],
        "measured_labels": [run.label for run in measured_runs],
        "accepted_postrun_crash_labels": [
            run.label for run in runs if run.accepted_postrun_crash
        ],
        "ray_tracing": {
            "enable_ray_tracing": bool(args.enable_ray_tracing),
            "backend": args.ray_tracing_backend or None,
            "samples_per_pixel": args.samples_per_pixel,
            "progressive_passes": args.progressive_passes,
            "ambient_samples": args.ambient_samples,
            "denoise": args.denoise,
        },
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
            "process_returncode": run.process_returncode,
            "accepted_postrun_crash": run.accepted_postrun_crash,
            "timer_summary_path": str(run.output_dir / "paraview_timer_summary.json"),
            "frame_timeline_path": str(
                run.output_dir / "paraview_internal_frame_timeline.json"
            ),
            "memory_summary_path": (
                str(run.output_dir / "paraview_memory_summary.json")
                if run.memory_summary
                else None
            ),
        }
        for run in runs
    ]
    (aggregate_dir / "runs.json").write_text(
        json.dumps(run_records_json, indent=2) + "\n", encoding="utf-8"
    )

    action_metric_values: dict[str, dict[str, list[float]]] = defaultdict(
        lambda: defaultdict(list)
    )
    action_run_rows: list[dict[str, Any]] = []
    pooled_frame_values: dict[str, dict[str, list[float]]] = defaultdict(
        lambda: defaultdict(list)
    )
    pooled_completion_values: dict[str, dict[str, list[float]]] = defaultdict(
        lambda: defaultdict(list)
    )
    frame_index_duration_values: dict[str, dict[str, dict[int, list[float]]]] = (
        defaultdict(lambda: defaultdict(lambda: defaultdict(list)))
    )
    frame_index_completion_values: dict[str, dict[str, dict[int, list[float]]]] = (
        defaultdict(lambda: defaultdict(lambda: defaultdict(list)))
    )
    all_measured_frames_path = aggregate_dir / "all_measured_frames.jsonl"
    memory_peaks: list[float] = []
    memory_first: list[float] = []
    memory_last: list[float] = []
    per_run_wall_seconds: list[float] = []

    with all_measured_frames_path.open("w", encoding="utf-8") as all_frames_stream:
        for run in measured_runs:
            per_run_wall_seconds.append(float(run.elapsed_wall_seconds))
            action_entries = run.timer_summary.get("actions") or []
            action_by_name = {str(entry["action"]): entry for entry in action_entries}

            for action_name, entry in action_by_name.items():
                row = {
                    "run_label": run.label,
                    "action": action_name,
                    "render_event_count": int(entry.get("render_event_count", 0)),
                }
                for metric_name in ACTION_METRICS:
                    value = entry.get(metric_name)
                    row[metric_name] = value
                    if value is not None:
                        action_metric_values[action_name][metric_name].append(
                            float(value)
                        )
                action_run_rows.append(row)

            for frame in run.frame_timeline:
                frame_row = dict(frame)
                frame_row["run_label"] = run.label
                all_frames_stream.write(json.dumps(frame_row, sort_keys=True) + "\n")

                action_name = str(frame["action"])
                kind = str(frame["kind"])
                duration_ms = float(frame["duration_ms"])
                completion_ms = float(frame["end_ms_from_action_start"])
                kind_frame_index = int(frame["kind_frame_index"])

                pooled_frame_values[action_name][kind].append(duration_ms)
                pooled_completion_values[action_name][kind].append(completion_ms)
                frame_index_duration_values[action_name][kind][kind_frame_index].append(
                    duration_ms
                )
                frame_index_completion_values[action_name][kind][
                    kind_frame_index
                ].append(completion_ms)

            if run.memory_summary:
                if run.memory_summary.get("peak_rss_bytes") is not None:
                    memory_peaks.append(float(run.memory_summary["peak_rss_bytes"]))
                if run.memory_summary.get("first_rss_bytes") is not None:
                    memory_first.append(float(run.memory_summary["first_rss_bytes"]))
                if run.memory_summary.get("last_rss_bytes") is not None:
                    memory_last.append(float(run.memory_summary["last_rss_bytes"]))

    _write_csv(
        aggregate_dir / "action_metrics_by_run.csv",
        action_run_rows,
        ["run_label", "action", "render_event_count", *ACTION_METRICS],
    )
    (aggregate_dir / "action_metrics_by_run.json").write_text(
        json.dumps(action_run_rows, indent=2) + "\n", encoding="utf-8"
    )

    action_metric_stats: dict[str, dict[str, Any]] = {}
    for action_name, metric_map in action_metric_values.items():
        action_metric_stats[action_name] = {}
        for metric_name, values in metric_map.items():
            action_metric_stats[action_name][metric_name] = _stats(values)
    (aggregate_dir / "action_metric_stats.json").write_text(
        json.dumps(action_metric_stats, indent=2) + "\n", encoding="utf-8"
    )

    pooled_frame_stats: dict[str, dict[str, Any]] = {}
    pooled_frame_rows: list[dict[str, Any]] = []
    for action_name, kind_map in pooled_frame_values.items():
        pooled_frame_stats[action_name] = {}
        for kind, values in kind_map.items():
            duration_stats = _stats(values)
            completion_stats = _stats(pooled_completion_values[action_name][kind])
            fps_stats = _fps_summary(values)
            entry = {
                "duration_ms": duration_stats,
                "completion_ms_from_action_start": completion_stats,
                "service_fps": fps_stats,
            }
            pooled_frame_stats[action_name][kind] = entry
            pooled_frame_rows.append(
                {
                    "action": action_name,
                    "kind": kind,
                    "frame_count": duration_stats["count"],
                    "duration_mean_ms": duration_stats["mean"],
                    "duration_median_ms": duration_stats["median"],
                    "duration_p95_ms": duration_stats["p95"],
                    "duration_stdev_ms": duration_stats["stdev"],
                    "duration_cv": duration_stats["cv"],
                    "completion_mean_ms": completion_stats["mean"],
                    "completion_median_ms": completion_stats["median"],
                    "completion_p95_ms": completion_stats["p95"],
                    "mean_service_fps": fps_stats["mean_service_fps"],
                    "median_service_fps": fps_stats["median_service_fps"],
                    "p95_service_fps": fps_stats["p95_service_fps"],
                }
            )
    (aggregate_dir / "pooled_frame_stats.json").write_text(
        json.dumps(pooled_frame_stats, indent=2) + "\n", encoding="utf-8"
    )
    _write_csv(
        aggregate_dir / "pooled_frame_stats.csv",
        pooled_frame_rows,
        [
            "action",
            "kind",
            "frame_count",
            "duration_mean_ms",
            "duration_median_ms",
            "duration_p95_ms",
            "duration_stdev_ms",
            "duration_cv",
            "completion_mean_ms",
            "completion_median_ms",
            "completion_p95_ms",
            "mean_service_fps",
            "median_service_fps",
            "p95_service_fps",
        ],
    )

    frame_index_stats: dict[str, dict[str, dict[str, Any]]] = {}
    frame_index_rows: list[dict[str, Any]] = []
    for action_name, kind_map in frame_index_duration_values.items():
        frame_index_stats[action_name] = {}
        for kind, index_map in kind_map.items():
            frame_index_stats[action_name][kind] = {}
            for kind_frame_index, values in sorted(index_map.items()):
                duration_stats = _stats(values)
                completion_stats = _stats(
                    frame_index_completion_values[action_name][kind][kind_frame_index]
                )
                fps_stats = _fps_summary(values)
                frame_index_stats[action_name][kind][str(kind_frame_index)] = {
                    "duration_ms": duration_stats,
                    "completion_ms_from_action_start": completion_stats,
                    "service_fps": fps_stats,
                }
                frame_index_rows.append(
                    {
                        "action": action_name,
                        "kind": kind,
                        "kind_frame_index": kind_frame_index,
                        "sample_count": duration_stats["count"],
                        "duration_mean_ms": duration_stats["mean"],
                        "duration_median_ms": duration_stats["median"],
                        "duration_p95_ms": duration_stats["p95"],
                        "duration_stdev_ms": duration_stats["stdev"],
                        "completion_mean_ms": completion_stats["mean"],
                        "completion_median_ms": completion_stats["median"],
                        "completion_p95_ms": completion_stats["p95"],
                        "mean_service_fps": fps_stats["mean_service_fps"],
                    }
                )
    (aggregate_dir / "frame_index_stats.json").write_text(
        json.dumps(frame_index_stats, indent=2) + "\n", encoding="utf-8"
    )
    _write_csv(
        aggregate_dir / "frame_index_stats.csv",
        frame_index_rows,
        [
            "action",
            "kind",
            "kind_frame_index",
            "sample_count",
            "duration_mean_ms",
            "duration_median_ms",
            "duration_p95_ms",
            "duration_stdev_ms",
            "completion_mean_ms",
            "completion_median_ms",
            "completion_p95_ms",
            "mean_service_fps",
        ],
    )

    memory_stats = {
        "peak_rss_bytes": _stats(memory_peaks),
        "first_rss_bytes": _stats(memory_first),
        "last_rss_bytes": _stats(memory_last),
        "run_elapsed_wall_seconds": _stats(per_run_wall_seconds),
    }
    (aggregate_dir / "memory_stats.json").write_text(
        json.dumps(memory_stats, indent=2) + "\n", encoding="utf-8"
    )

    summary = {
        "config": manifest,
        "action_metric_stats": action_metric_stats,
        "pooled_frame_stats": pooled_frame_stats,
        "frame_index_stats_path": str(aggregate_dir / "frame_index_stats.json"),
        "memory_stats": memory_stats,
    }
    (aggregate_dir / "summary.json").write_text(
        json.dumps(summary, indent=2) + "\n", encoding="utf-8"
    )

    lines = [
        "# ParaView deterministic benchmark",
        "",
        f"- Dataset: `{Path(args.dataset).resolve()}`",
        f"- Camera spec: `{Path(args.camera_spec).resolve()}`",
        f"- Mode: `{args.deterministic_mode}`",
        f"- Volume rendering mode: `{args.volume_rendering_mode}`",
        f"- Runs: `{args.warmup_runs}` warm-up + `{args.measured_runs}` measured",
        f"- Output root: `{root}`",
        "",
    ]
    accepted_postrun_crashes = [run for run in runs if run.accepted_postrun_crash]
    if accepted_postrun_crashes:
        lines.extend(
            [
                "- Note: some runs exited with `SIGSEGV` after writing complete artifacts and were "
                "accepted as successful post-run teardown crashes.",
                f"- Accepted post-run crash labels: `{', '.join(run.label for run in accepted_postrun_crashes)}`",
                "",
            ]
        )
    lines.extend(
        [
            "## Key results",
            "",
        ]
    )
    for action_name in sorted(pooled_frame_stats.keys()):
        lines.append(f"### {action_name}")
        interactive_entry = pooled_frame_stats[action_name].get("interactive")
        still_entry = pooled_frame_stats[action_name].get("still")
        if interactive_entry:
            duration_stats = interactive_entry["duration_ms"]
            fps_stats = interactive_entry["service_fps"]
            lines.append(
                "- interactive frames: "
                f"mean `{duration_stats['mean']:.3f} ms`, "
                f"median `{duration_stats['median']:.3f} ms`, "
                f"p95 `{duration_stats['p95']:.3f} ms`, "
                f"cv `{duration_stats['cv']:.4f}`, "
                f"mean service fps `{fps_stats['mean_service_fps']:.3f}`"
            )
        if still_entry:
            duration_stats = still_entry["duration_ms"]
            lines.append(
                "- still frames: "
                f"mean `{duration_stats['mean']:.3f} ms`, "
                f"median `{duration_stats['median']:.3f} ms`, "
                f"p95 `{duration_stats['p95']:.3f} ms`"
            )
        release_stats = action_metric_stats.get(action_name, {}).get(
            "release_to_first_still_ms"
        )
        if release_stats and release_stats["count"]:
            lines.append(
                "- release to first still: "
                f"mean `{release_stats['mean']:.3f} ms`, "
                f"median `{release_stats['median']:.3f} ms`, "
                f"p95 `{release_stats['p95']:.3f} ms`"
            )
        final_stats = action_metric_stats.get(action_name, {}).get(
            "final_render_complete_ms_from_action_start"
        )
        if final_stats and final_stats["count"]:
            lines.append(
                "- final render complete from action start: "
                f"mean `{final_stats['mean']:.3f} ms`, "
                f"median `{final_stats['median']:.3f} ms`, "
                f"p95 `{final_stats['p95']:.3f} ms`"
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
            "- `aggregate/action_metric_stats.json`: run-level action metric statistics",
            "- `aggregate/pooled_frame_stats.json`: pooled interactive/still frame statistics",
            "- `aggregate/frame_index_stats.json`: per-frame-position statistics across runs",
            "- `aggregate/all_measured_frames.jsonl`: every measured frame with run labels",
            "- `run_XX/`: raw per-run timer events, timer summary, frame timeline, and RSS samples",
            "",
        ]
    )
    (aggregate_dir / "summary.md").write_text("\n".join(lines), encoding="utf-8")


def main() -> int:
    args = _parse_args()
    root = Path(args.output_root).resolve()
    root.mkdir(parents=True, exist_ok=True)

    config = {
        "pvpython": str(Path(args.pvpython).resolve()),
        "launch_wrapper": (
            str(Path(args.launch_wrapper).resolve()) if args.launch_wrapper else None
        ),
        "benchmark_script": str(Path(args.benchmark_script).resolve()),
        "dataset": str(Path(args.dataset).resolve()),
        "camera_spec": str(Path(args.camera_spec).resolve()),
        "deterministic_mode": args.deterministic_mode,
        "warmup_runs": int(args.warmup_runs),
        "measured_runs": int(args.measured_runs),
        "array_name": args.array_name,
        "channel_mode": args.channel_mode,
        "component": int(args.component),
        "blend_mode": args.blend_mode,
        "volume_rendering_mode": args.volume_rendering_mode,
        "data_range_min": float(args.data_range_min),
        "data_range_max": float(args.data_range_max),
        "color_min_rgb": [float(v) for v in args.color_min_rgb],
        "color_max_rgb": [float(v) for v in args.color_max_rgb],
        "pre_action_delay_seconds": float(args.pre_action_delay_seconds),
        "sample_rss": bool(args.sample_rss),
        "rss_sample_interval_seconds": float(args.rss_sample_interval_seconds),
        "start_delay_seconds": float(args.start_delay_seconds),
        "enable_ray_tracing": bool(args.enable_ray_tracing),
        "ray_tracing_backend": args.ray_tracing_backend or None,
        "samples_per_pixel": args.samples_per_pixel,
        "progressive_passes": args.progressive_passes,
        "ambient_samples": args.ambient_samples,
        "denoise": args.denoise,
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
    print(f"Wrote ParaView deterministic benchmark results to {root}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
