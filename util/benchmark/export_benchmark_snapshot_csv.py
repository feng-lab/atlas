#!/usr/bin/env python3

from __future__ import annotations

import csv
import json
from dataclasses import dataclass
from pathlib import Path
from typing import Any


SCRIPT_DIR = Path(__file__).resolve().parent


@dataclass(frozen=True)
class SessionSpec:
    dataset: str
    session: str
    input_desc: str
    render_mode: str
    aggregate_summary_path: Path
    kind: str  # "paraview" or "atlas"


@dataclass(frozen=True)
class GuiSessionSpec:
    suite: str
    dataset: str
    session: str
    software: str
    input_desc: str
    render_mode: str
    drag_duration_seconds: float
    aggregate_summary_path: Path


SLICE15_ROOT = Path("/Users/feng/Dropbox/atlas_test/slice15_paraview/benchmarks")
LARGE_ROOT = Path("/Users/feng/code/atlas/large_test_image/benchmarks")


RETAINED_SESSIONS: tuple[SessionSpec, ...] = (
    SessionSpec(
        dataset="slice15_ch2",
        session="ParaView GPU MIP",
        input_desc="Blocked .vtpd",
        render_mode="GPU Based + maximum-intensity",
        aggregate_summary_path=SLICE15_ROOT
        / "paraview_deterministic_interactive_plus_final_2000x1500/aggregate/summary.json",
        kind="paraview",
    ),
    SessionSpec(
        dataset="slice15_ch2",
        session="ParaView GPU Composite",
        input_desc="Blocked .vtpd",
        render_mode="GPU Based + composite",
        aggregate_summary_path=SLICE15_ROOT
        / "paraview_gpu_deterministic_interactive_plus_final_2000x1500_composite_v1/aggregate/summary.json",
        kind="paraview",
    ),
    SessionSpec(
        dataset="slice15_ch2",
        session="ParaView OSPRay Composite",
        input_desc="Dense .mhd/.zraw",
        render_mode="OSPRay Based + composite",
        aggregate_summary_path=SLICE15_ROOT
        / "paraview_ospray_deterministic_interactive_plus_final_2000x1500_composite_v3/aggregate/summary.json",
        kind="paraview",
    ),
    SessionSpec(
        dataset="slice15_ch2",
        session="Atlas MIP",
        input_desc="Dense .nim",
        render_mode="Maximum Intensity Projection",
        aggregate_summary_path=SLICE15_ROOT
        / "atlas_deterministic_interactive_plus_final_2000x1500_mip_v5_parity/aggregate/summary.json",
        kind="atlas",
    ),
    SessionSpec(
        dataset="slice15_ch2",
        session="Atlas DVR",
        input_desc="Dense .nim",
        render_mode="Direct Volume Rendering",
        aggregate_summary_path=SLICE15_ROOT
        / "atlas_deterministic_interactive_plus_final_2000x1500_dvr_v2_parity/aggregate/summary.json",
        kind="atlas",
    ),
    SessionSpec(
        dataset="slice15_ch2_gpufit_1024x1024x980",
        session="ParaView GPU MIP",
        input_desc="Dense .mhd/.zraw",
        render_mode="GPU Based + maximum-intensity",
        aggregate_summary_path=LARGE_ROOT
        / "paraview_gpufit_deterministic_interactive_plus_final_2000x1500_mip_v1/aggregate/summary.json",
        kind="paraview",
    ),
    SessionSpec(
        dataset="slice15_ch2_gpufit_1024x1024x980",
        session="ParaView GPU Composite",
        input_desc="Dense .mhd/.zraw",
        render_mode="GPU Based + composite",
        aggregate_summary_path=LARGE_ROOT
        / "paraview_gpufit_deterministic_interactive_plus_final_2000x1500_composite_v1/aggregate/summary.json",
        kind="paraview",
    ),
    SessionSpec(
        dataset="slice15_ch2_gpufit_1024x1024x980",
        session="ParaView OSPRay Composite",
        input_desc="Dense .mhd/.zraw",
        render_mode="OSPRay Based + composite",
        aggregate_summary_path=LARGE_ROOT
        / "paraview_gpufit_ospray_deterministic_interactive_plus_final_2000x1500_composite_v1/aggregate/summary.json",
        kind="paraview",
    ),
    SessionSpec(
        dataset="slice15_ch2_gpufit_1024x1024x980",
        session="Atlas MIP",
        input_desc="Dense .nim",
        render_mode="Maximum Intensity Projection",
        aggregate_summary_path=LARGE_ROOT
        / "atlas_gpufit_deterministic_interactive_plus_final_2000x1500_mip_v2_clean/aggregate/summary.json",
        kind="atlas",
    ),
    SessionSpec(
        dataset="slice15_ch2_gpufit_1024x1024x980",
        session="Atlas DVR",
        input_desc="Dense .nim",
        render_mode="Direct Volume Rendering",
        aggregate_summary_path=LARGE_ROOT
        / "atlas_gpufit_deterministic_interactive_plus_final_2000x1500_dvr_v1_clean/aggregate/summary.json",
        kind="atlas",
    ),
    SessionSpec(
        dataset="slice15_ch2_x2z",
        session="ParaView GPU MIP",
        input_desc="Blocked .vtpd",
        render_mode="GPU Based + maximum-intensity",
        aggregate_summary_path=LARGE_ROOT
        / "paraview_x2z_deterministic_interactive_plus_final_2000x1500_mip_v1/aggregate/summary.json",
        kind="paraview",
    ),
    SessionSpec(
        dataset="slice15_ch2_x2z",
        session="ParaView GPU Composite",
        input_desc="Blocked .vtpd",
        render_mode="GPU Based + composite",
        aggregate_summary_path=LARGE_ROOT
        / "paraview_x2z_deterministic_interactive_plus_final_2000x1500_composite_v1/aggregate/summary.json",
        kind="paraview",
    ),
    SessionSpec(
        dataset="slice15_ch2_x2z",
        session="ParaView OSPRay Composite",
        input_desc="Dense .mhd/.zraw",
        render_mode="OSPRay Based + composite",
        aggregate_summary_path=LARGE_ROOT
        / "paraview_x2z_ospray_deterministic_interactive_plus_final_2000x1500_composite_v1/aggregate/summary.json",
        kind="paraview",
    ),
    SessionSpec(
        dataset="slice15_ch2_x2z",
        session="Atlas MIP",
        input_desc="Dense .nim",
        render_mode="Maximum Intensity Projection",
        aggregate_summary_path=LARGE_ROOT
        / "atlas_x2z_deterministic_interactive_plus_final_2000x1500_mip_v1/aggregate/summary.json",
        kind="atlas",
    ),
    SessionSpec(
        dataset="slice15_ch2_x2z",
        session="Atlas DVR",
        input_desc="Dense .nim",
        render_mode="Direct Volume Rendering",
        aggregate_summary_path=LARGE_ROOT
        / "atlas_x2z_deterministic_interactive_plus_final_2000x1500_dvr_v1/aggregate/summary.json",
        kind="atlas",
    ),
    SessionSpec(
        dataset="high_res_20220219_stitched_all_spacing_0p1_0p1_2_um",
        session="Atlas MIP",
        input_desc="Dense .nim",
        render_mode="Maximum Intensity Projection",
        aggregate_summary_path=LARGE_ROOT
        / "atlas_high_res_deterministic_interactive_plus_final_2000x1500_mip_v1/aggregate/summary.json",
        kind="atlas",
    ),
    SessionSpec(
        dataset="high_res_20220219_stitched_all_spacing_0p1_0p1_2_um",
        session="Atlas DVR",
        input_desc="Dense .nim",
        render_mode="Direct Volume Rendering",
        aggregate_summary_path=LARGE_ROOT
        / "atlas_high_res_deterministic_interactive_plus_final_2000x1500_dvr_v1/aggregate/summary.json",
        kind="atlas",
    ),
)


RETAINED_GUI_SESSIONS: tuple[GuiSessionSpec, ...] = (
    GuiSessionSpec(
        suite="short_rotate_0p5s",
        dataset="slice15_ch2",
        session="ParaView GUI Rotate",
        software="ParaView",
        input_desc="Blocked .vtpd",
        render_mode="GPU Based + maximum-intensity",
        drag_duration_seconds=0.5,
        aggregate_summary_path=SLICE15_ROOT
        / "paraview_gui_rotate_slice15_ch2_gpu_mip_2000x1500_v2_centered/aggregate/summary.json",
    ),
    GuiSessionSpec(
        suite="short_rotate_0p5s",
        dataset="slice15_ch2",
        session="Atlas GUI Rotate",
        software="Atlas",
        input_desc="Dense .nim",
        render_mode="Maximum Intensity Projection",
        drag_duration_seconds=0.5,
        aggregate_summary_path=SLICE15_ROOT
        / "atlas_gui_rotate_slice15_ch2_mip_2000x1500_v3_centered/aggregate/summary.json",
    ),
    GuiSessionSpec(
        suite="sustained_rotate_2p0s",
        dataset="slice15_ch2",
        session="ParaView GUI Rotate",
        software="ParaView",
        input_desc="Blocked .vtpd",
        render_mode="GPU Based + maximum-intensity",
        drag_duration_seconds=2.0,
        aggregate_summary_path=SLICE15_ROOT
        / "paraview_gui_rotate_slice15_ch2_gpu_mip_2000x1500_rotate2s_v5_centered/aggregate/summary.json",
    ),
    GuiSessionSpec(
        suite="sustained_rotate_2p0s",
        dataset="slice15_ch2",
        session="Atlas GUI Rotate",
        software="Atlas",
        input_desc="Dense .nim",
        render_mode="Maximum Intensity Projection",
        drag_duration_seconds=2.0,
        aggregate_summary_path=SLICE15_ROOT
        / "atlas_gui_rotate_slice15_ch2_mip_2000x1500_rotate2s_v1_centered/aggregate/summary.json",
    ),
)


def _load_json(path: Path) -> dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8"))


def _mean(stats: dict[str, Any] | None) -> float | None:
    if not stats:
        return None
    value = stats.get("mean")
    return float(value) if value is not None else None


def _bytes_to_gib(value: float | None) -> float | None:
    if value is None:
        return None
    return value / (1024.0**3)


def _format_float(value: float | None, digits: int = 3) -> str:
    if value is None:
        return ""
    return f"{value:.{digits}f}"


def _paraview_row(spec: SessionSpec, summary: dict[str, Any]) -> dict[str, str]:
    action_metric_stats = summary["action_metric_stats"]
    pooled_frame_stats = summary["pooled_frame_stats"]
    memory_stats = summary["memory_stats"]
    return {
        "dataset": spec.dataset,
        "session": spec.session,
        "software": "ParaView",
        "input": spec.input_desc,
        "render_mode": spec.render_mode,
        "aggregate_summary_path": str(spec.aggregate_summary_path),
        "open_first_preview_ms": "",
        "open_final_ms": _format_float(
            _mean(
                action_metric_stats["open"][
                    "final_render_complete_ms_from_action_start"
                ]
            )
        ),
        "rotate_preview_ms": _format_float(
            _mean(pooled_frame_stats["rotate"]["interactive"]["duration_ms"])
        ),
        "rotate_release_final_ms": _format_float(
            _mean(action_metric_stats["rotate"]["release_to_first_still_ms"])
        ),
        "zoom_preview_ms": _format_float(
            _mean(pooled_frame_stats["zoom"]["interactive"]["duration_ms"])
        ),
        "zoom_release_final_ms": _format_float(
            _mean(action_metric_stats["zoom"]["release_to_first_still_ms"])
        ),
        "peak_rss_gib": _format_float(
            _bytes_to_gib(_mean(memory_stats["peak_rss_bytes"]))
        ),
        "full_run_wall_s": _format_float(
            _mean(memory_stats["run_elapsed_wall_seconds"])
        ),
    }


def _atlas_last_step_mean(
    summary: dict[str, Any], action: str, metric_name: str
) -> float | None:
    step_index_stats_path = Path(summary["step_index_stats_path"])
    step_index_stats = _load_json(step_index_stats_path)
    action_steps = step_index_stats.get(action, {})
    if not action_steps:
        return None
    last_step = max(int(step) for step in action_steps.keys())
    return _mean(action_steps[str(last_step)].get(metric_name))


def _atlas_row(spec: SessionSpec, summary: dict[str, Any]) -> dict[str, str]:
    open_metric_stats = summary["open_metric_stats"]
    action_step_stats = summary["action_step_stats"]
    memory_stats = summary["memory_stats"]
    return {
        "dataset": spec.dataset,
        "session": spec.session,
        "software": "Atlas",
        "input": spec.input_desc,
        "render_mode": spec.render_mode,
        "aggregate_summary_path": str(spec.aggregate_summary_path),
        "open_first_preview_ms": _format_float(
            _mean(open_metric_stats["open_total_to_first_preview_ms"])
        ),
        "open_final_ms": _format_float(
            _mean(open_metric_stats["open_total_to_final_ms"])
        ),
        "rotate_preview_ms": _format_float(
            _mean(action_step_stats["rotate"]["preview_client_ms"])
        ),
        "rotate_release_final_ms": _format_float(
            _atlas_last_step_mean(summary, "rotate", "preview_to_final_client_ms")
        ),
        "zoom_preview_ms": _format_float(
            _mean(action_step_stats["zoom"]["preview_client_ms"])
        ),
        "zoom_release_final_ms": _format_float(
            _atlas_last_step_mean(summary, "zoom", "preview_to_final_client_ms")
        ),
        "peak_rss_gib": _format_float(
            _bytes_to_gib(_mean(memory_stats["peak_rss_bytes"]))
        ),
        "full_run_wall_s": _format_float(
            _mean(memory_stats["run_elapsed_wall_seconds"])
        ),
    }


def _gui_metric_mean(summary: dict[str, Any], metric_name: str) -> float | None:
    metrics = summary.get("metrics")
    if not isinstance(metrics, dict):
        return None
    metric = metrics.get(metric_name)
    if not isinstance(metric, dict):
        return None
    return _mean(metric)


def _gui_metric_count(summary: dict[str, Any], metric_name: str) -> int | None:
    metrics = summary.get("metrics")
    if not isinstance(metrics, dict):
        return None
    metric = metrics.get(metric_name)
    if not isinstance(metric, dict):
        return None
    count = metric.get("count")
    return int(count) if count is not None else None


def _gui_row(spec: GuiSessionSpec, summary: dict[str, Any]) -> dict[str, str]:
    return {
        "suite": spec.suite,
        "dataset": spec.dataset,
        "session": spec.session,
        "software": spec.software,
        "input": spec.input_desc,
        "render_mode": spec.render_mode,
        "drag_duration_s": _format_float(spec.drag_duration_seconds, digits=1),
        "measured_run_count": str(int(summary.get("measured_run_count", 0))),
        "first_visible_ms": _format_float(
            _gui_metric_mean(summary, "capture_first_visible_ms_from_start")
        ),
        "first_visible_count": str(
            _gui_metric_count(summary, "capture_first_visible_ms_from_start") or 0
        ),
        "changed_sample_count": _format_float(
            _gui_metric_mean(summary, "changed_sample_count")
        ),
        "changed_samples_per_second": _format_float(
            _gui_metric_mean(summary, "changed_samples_per_second")
        ),
        "visible_fps": _format_float(
            _gui_metric_mean(summary, "visible_fps_from_mean_interval")
        ),
        "stable_after_end_ms": _format_float(
            _gui_metric_mean(summary, "capture_stable_ms_from_end")
        ),
        "capture_hz": _format_float(_gui_metric_mean(summary, "observed_sample_hz")),
        "aggregate_summary_path": str(spec.aggregate_summary_path),
    }


def _write_csv(path: Path, rows: list[dict[str, str]], fieldnames: list[str]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)


def main() -> int:
    all_rows: list[dict[str, str]] = []
    atlas_rows: list[dict[str, str]] = []
    gui_rows: list[dict[str, str]] = []

    for spec in RETAINED_SESSIONS:
        summary = _load_json(spec.aggregate_summary_path)
        if spec.kind == "paraview":
            row = _paraview_row(spec, summary)
        else:
            row = _atlas_row(spec, summary)
            atlas_rows.append(row.copy())
        all_rows.append(row)

    for spec in RETAINED_GUI_SESSIONS:
        gui_rows.append(_gui_row(spec, _load_json(spec.aggregate_summary_path)))

    common_fields = [
        "dataset",
        "session",
        "software",
        "input",
        "render_mode",
        "open_first_preview_ms",
        "open_final_ms",
        "rotate_preview_ms",
        "rotate_release_final_ms",
        "zoom_preview_ms",
        "zoom_release_final_ms",
        "peak_rss_gib",
        "full_run_wall_s",
        "aggregate_summary_path",
    ]

    _write_csv(
        SCRIPT_DIR / "benchmark_cross_session_snapshot.csv",
        all_rows,
        common_fields,
    )
    _write_csv(
        SCRIPT_DIR / "benchmark_atlas_cross_dataset_snapshot.csv",
        atlas_rows,
        common_fields,
    )
    _write_csv(
        SCRIPT_DIR / "benchmark_gui_rotate_snapshot.csv",
        gui_rows,
        [
            "suite",
            "dataset",
            "session",
            "software",
            "input",
            "render_mode",
            "drag_duration_s",
            "measured_run_count",
            "first_visible_ms",
            "first_visible_count",
            "changed_sample_count",
            "changed_samples_per_second",
            "visible_fps",
            "stable_after_end_ms",
            "capture_hz",
            "aggregate_summary_path",
        ],
    )

    print(f"Wrote {SCRIPT_DIR / 'benchmark_cross_session_snapshot.csv'}")
    print(f"Wrote {SCRIPT_DIR / 'benchmark_atlas_cross_dataset_snapshot.csv'}")
    print(f"Wrote {SCRIPT_DIR / 'benchmark_gui_rotate_snapshot.csv'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
