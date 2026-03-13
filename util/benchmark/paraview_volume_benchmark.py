from __future__ import annotations

import argparse
import json
import os
import sys
import time
from pathlib import Path

script_dir_env = os.environ.get("PARAVIEW_BENCHMARK_SCRIPT_DIR")
if script_dir_env:
    SCRIPT_DIR = Path(script_dir_env).resolve()
elif "__file__" in globals():
    SCRIPT_DIR = Path(__file__).resolve().parent
elif sys.argv and sys.argv[0]:
    SCRIPT_DIR = Path(sys.argv[0]).resolve().parent
else:
    SCRIPT_DIR = Path.cwd()
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

BLEND_MODE_VALUES = {
    "composite": 0,
    "maximum-intensity": 1,
    "minimum-intensity": 2,
    "average-intensity": 3,
    "additive": 4,
    "isosurface": 5,
    "slice": 6,
}
DETERMINISTIC_MODES = (
    "interactive-plus-final",
    "direct-final",
)
TIMER_ACTION_PREFIX = "Benchmark Action::"
INTERACTIVE_RENDER_EVENT = "Interactive Render"
STILL_RENDER_EVENT = "Still Render"
TIMER_EVENT_TYPE_INVALID = -1
TIMER_EVENT_TYPE_STANDALONE = 0
TIMER_EVENT_TYPE_START = 1
TIMER_EVENT_TYPE_END = 2
TIMER_EVENT_TYPE_INSERTED = 3
TIMER_MAX_ENTRIES = 1_000_000

from paraview.simple import (
    ColorBy,
    GetActiveViewOrCreate,
    GetColorTransferFunction,
    GetOpacityTransferFunction,
    HideUnusedScalarBars,
    OpenDataFile,
    SaveScreenshot,
    Show,
)
from vtkmodules.vtkCommonSystem import vtkTimerLog

from process_memory_sampler import ProcessMemorySampler
from volume_benchmark_common import (
    BenchmarkAction,
    BenchmarkSpec,
    EventLogger,
    interpolate_action_cameras,
    load_benchmark_spec,
    sleep_until,
)


def _point_array_names(source) -> list[str]:
    names: list[str] = []
    for array in source.PointData:
        name = getattr(array, "Name", None)
        if name:
            names.append(str(name))
    return names


def _resolve_array_name(source, requested_array_name: str) -> str:
    names = _point_array_names(source)
    if requested_array_name in names:
        return requested_array_name
    if len(names) == 1:
        return names[0]
    raise RuntimeError(
        f"Requested point array {requested_array_name!r} not found. "
        f"Available point arrays: {names}"
    )


def _env_bool(name: str, default: bool = False) -> bool:
    value = os.environ.get(name)
    if value is None:
        return default
    return value.strip().lower() in {"1", "true", "yes", "on"}


def _parse_rgb_triplet_text(value: str | None) -> tuple[float, float, float] | None:
    if value is None:
        return None
    parts = [part for part in value.replace(",", " ").split() if part]
    if len(parts) != 3:
        raise ValueError(f"Expected three RGB components, got {value!r}")
    return (float(parts[0]), float(parts[1]), float(parts[2]))


def _runtime_mode() -> str:
    return os.environ.get("PARAVIEW_BENCHMARK_RUNTIME_MODE", "pvpython")


def _parse_args() -> argparse.Namespace:
    dataset_default = os.environ.get("PARAVIEW_BENCHMARK_DATASET")
    camera_spec_default = os.environ.get("PARAVIEW_BENCHMARK_CAMERA_SPEC")
    output_dir_default = os.environ.get("PARAVIEW_BENCHMARK_OUTPUT_DIR")
    array_name_default = os.environ.get("PARAVIEW_BENCHMARK_ARRAY_NAME", "channels")
    channel_mode_default = os.environ.get(
        "PARAVIEW_BENCHMARK_CHANNEL_MODE", "component"
    )
    component_default = int(os.environ.get("PARAVIEW_BENCHMARK_COMPONENT", "0"))
    capture_screens_default = _env_bool(
        "PARAVIEW_BENCHMARK_CAPTURE_SCREENSHOTS", default=False
    )
    start_delay_default = float(
        os.environ.get("PARAVIEW_BENCHMARK_START_DELAY_SECONDS", "0.0")
    )
    pre_action_delay_default = float(
        os.environ.get("PARAVIEW_BENCHMARK_PRE_ACTION_DELAY_SECONDS", "0.2")
    )
    blend_mode_default = os.environ.get("PARAVIEW_BENCHMARK_BLEND_MODE", "composite")
    data_range_min_default = os.environ.get("PARAVIEW_BENCHMARK_DATA_RANGE_MIN")
    data_range_max_default = os.environ.get("PARAVIEW_BENCHMARK_DATA_RANGE_MAX")
    color_min_default = _parse_rgb_triplet_text(
        os.environ.get("PARAVIEW_BENCHMARK_COLOR_MIN_RGB")
    )
    color_max_default = _parse_rgb_triplet_text(
        os.environ.get("PARAVIEW_BENCHMARK_COLOR_MAX_RGB")
    )
    deterministic_mode_default = os.environ.get(
        "PARAVIEW_BENCHMARK_DETERMINISTIC_MODE", "interactive-plus-final"
    )
    sample_rss_default = _env_bool("PARAVIEW_BENCHMARK_SAMPLE_RSS", default=False)
    rss_target_pid_default = int(
        os.environ.get("PARAVIEW_BENCHMARK_RSS_TARGET_PID", str(os.getpid()))
    )
    rss_sample_interval_default = float(
        os.environ.get("PARAVIEW_BENCHMARK_RSS_SAMPLE_INTERVAL_SECONDS", "0.1")
    )

    parser = argparse.ArgumentParser(
        description=(
            "Drive ParaView volume actions from Python. "
            "Run with pvpython, or load into the ParaView GUI as a macro and "
            "provide the same arguments through a wrapper."
        )
    )
    parser.add_argument(
        "--dataset",
        default=dataset_default,
        required=dataset_default is None,
        help="ParaView dataset path, e.g. .vtpd or .mhd",
    )
    parser.add_argument(
        "--camera-spec",
        default=camera_spec_default,
        required=camera_spec_default is None,
        help="Benchmark camera/action JSON",
    )
    parser.add_argument(
        "--output-dir",
        default=output_dir_default,
        required=output_dir_default is None,
        help="Directory for logs and optional screenshots",
    )
    parser.add_argument(
        "--array-name",
        default=array_name_default,
        help="Point-data array to volume render",
    )
    parser.add_argument(
        "--channel-mode",
        choices=("component", "magnitude", "rgb-direct"),
        default=channel_mode_default,
        help=(
            "component: render one selected component with a single transfer function; "
            "magnitude: ParaView's stock multi-component magnitude path; "
            "rgb-direct: bypass transfer functions and treat 3-component data as direct RGB."
        ),
    )
    parser.add_argument(
        "--component",
        type=int,
        default=component_default,
        help="Selected component for --channel-mode component",
    )
    parser.add_argument(
        "--capture-screenshots",
        action="store_true",
        default=capture_screens_default,
        help="Save one screenshot after each action settles",
    )
    parser.add_argument(
        "--start-delay-seconds",
        type=float,
        default=start_delay_default,
        help="Sleep before loading the dataset so a GUI window can be positioned first",
    )
    parser.add_argument(
        "--pre-action-delay-seconds",
        type=float,
        default=pre_action_delay_default,
        help=(
            "Short delay after logging action_start and before mutating the view. "
            "This gives the external capture observer time to sample the baseline frame."
        ),
    )
    parser.add_argument(
        "--blend-mode",
        choices=tuple(BLEND_MODE_VALUES.keys()),
        default=blend_mode_default,
        help="Volume blend mode. maximum-intensity matches Atlas MIP scenes.",
    )
    parser.add_argument(
        "--data-range-min",
        type=float,
        default=float(data_range_min_default)
        if data_range_min_default is not None
        else None,
        help="Optional lower bound for the transfer-function range.",
    )
    parser.add_argument(
        "--data-range-max",
        type=float,
        default=float(data_range_max_default)
        if data_range_max_default is not None
        else None,
        help="Optional upper bound for the transfer-function range.",
    )
    parser.add_argument(
        "--color-min-rgb",
        type=float,
        nargs=3,
        default=color_min_default,
        metavar=("R", "G", "B"),
        help="Optional RGB color at the transfer-function minimum.",
    )
    parser.add_argument(
        "--color-max-rgb",
        type=float,
        nargs=3,
        default=color_max_default,
        metavar=("R", "G", "B"),
        help="Optional RGB color at the transfer-function maximum.",
    )
    parser.add_argument(
        "--deterministic-mode",
        choices=DETERMINISTIC_MODES,
        default=deterministic_mode_default,
        help=(
            "interactive-plus-final: interactive steps, then one still render; "
            "direct-final: jump to the final state and still-render once."
        ),
    )
    parser.add_argument(
        "--sample-rss",
        action="store_true",
        default=sample_rss_default,
        help="Sample process RSS during the run and write a summary JSON next to the timer artifacts.",
    )
    parser.add_argument(
        "--rss-target-pid",
        type=int,
        default=rss_target_pid_default,
        help="Process ID to sample for --sample-rss. Defaults to the current ParaView process.",
    )
    parser.add_argument(
        "--rss-sample-interval-seconds",
        type=float,
        default=rss_sample_interval_default,
        help="Sampling interval for --sample-rss.",
    )
    return parser.parse_args()


def _apply_camera(view, camera) -> None:
    state = camera.to_paraview_view_state()
    view.CameraPosition = state["CameraPosition"]
    view.CameraFocalPoint = state["CameraFocalPoint"]
    view.CameraViewUp = state["CameraViewUp"]
    view.CameraViewAngle = state["CameraViewAngle"]


def _configure_display(
    display,
    array_name: str,
    channel_mode: str,
    component: int,
    blend_mode: str,
    data_range_min: float | None,
    data_range_max: float | None,
    color_min_rgb: tuple[float, float, float] | list[float] | None,
    color_max_rgb: tuple[float, float, float] | list[float] | None,
) -> None:
    display.SetRepresentationType("Volume")
    ColorBy(display, ("POINTS", array_name))
    display.BlendMode = BLEND_MODE_VALUES[blend_mode]
    if hasattr(display, "OpacityArrayName"):
        display.OpacityArrayName = ["POINTS", array_name]
    if hasattr(display, "OpacityArray"):
        display.OpacityArray = ["POINTS", array_name]

    if channel_mode == "rgb-direct":
        display.MapScalars = 0
        display.MultiComponentsMapping = 0
        return

    display.MapScalars = 1
    display.MultiComponentsMapping = 0

    lut = GetColorTransferFunction(array_name)
    pwf = GetOpacityTransferFunction(array_name)
    if channel_mode == "magnitude":
        lut.VectorMode = 0
    else:
        lut.VectorMode = 1
        lut.VectorComponent = int(component)

    display.RescaleTransferFunctionToDataRange(True, False)

    range_min = float(lut.RGBPoints[0])
    range_max = float(lut.RGBPoints[-4])
    if data_range_min is not None:
        range_min = float(data_range_min)
    if data_range_max is not None:
        range_max = float(data_range_max)
    if range_max < range_min:
        raise ValueError(
            f"Invalid transfer-function range: min {range_min} must be <= max {range_max}"
        )

    if data_range_min is not None or data_range_max is not None:
        pwf.Points = [range_min, 0.0, 0.5, 0.0, range_max, 1.0, 0.5, 0.0]

    if color_min_rgb is not None or color_max_rgb is not None:
        min_rgb = tuple(float(v) for v in (color_min_rgb or (0.0, 0.0, 0.0)))
        max_rgb = tuple(float(v) for v in (color_max_rgb or (1.0, 1.0, 1.0)))
        lut.ColorSpace = "RGB"
        lut.RGBPoints = [
            range_min,
            min_rgb[0],
            min_rgb[1],
            min_rgb[2],
            range_max,
            max_rgb[0],
            max_rgb[1],
            max_rgb[2],
        ]
        pwf.Points = [range_min, 0.0, 0.5, 0.0, range_max, 1.0, 0.5, 0.0]


def _write_screenshot(
    output_dir: Path, action_name: str, spec: BenchmarkSpec, view
) -> None:
    SaveScreenshot(
        str(output_dir / f"{action_name}.png"),
        view,
        ImageResolution=[spec.viewport_width, spec.viewport_height],
    )


def _resolve_action_cameras(
    spec: BenchmarkSpec, action: BenchmarkAction, deterministic_mode: str
) -> list:
    cameras = interpolate_action_cameras(spec, action)
    if deterministic_mode == "direct-final" and action.kind == "interpolate":
        return [cameras[-1]]
    return cameras


def _timer_action_name(action_name: str) -> str:
    return f"{TIMER_ACTION_PREFIX}{action_name}"


def _reset_timer_log() -> None:
    vtkTimerLog.ResetLog()
    vtkTimerLog.CleanupLog()
    vtkTimerLog.SetMaxEntries(TIMER_MAX_ENTRIES)
    vtkTimerLog.SetLogging(1)


def _mark_timer_action_start(action_name: str) -> None:
    vtkTimerLog.MarkStartEvent(_timer_action_name(action_name))


def _mark_timer_action_end(action_name: str) -> None:
    vtkTimerLog.MarkEndEvent(_timer_action_name(action_name))


def _collect_timer_events() -> list[dict[str, object]]:
    events: list[dict[str, object]] = []
    for index in range(int(vtkTimerLog.GetNumberOfEvents())):
        events.append(
            {
                "index": index,
                "name": str(vtkTimerLog.GetEventString(index)),
                "event_type": int(vtkTimerLog.GetEventType(index)),
                "indent": int(vtkTimerLog.GetEventIndent(index)),
                "wall_time_seconds": float(vtkTimerLog.GetEventWallTime(index)),
            }
        )
    return events


def _format_timer_log_with_indents(
    events: list[dict[str, object]], threshold_seconds: float = 0.0
) -> str:
    lines: list[str] = []
    handled_events = [False] * len(events)

    for index, event in enumerate(events):
        indent = int(event["indent"])
        event_type = int(event["event_type"])
        end_event_index = -1

        if event_type == TIMER_EVENT_TYPE_END and handled_events[index]:
            continue

        if event_type == TIMER_EVENT_TYPE_START:
            counter = 1
            while (
                counter < len(events)
                and int(events[(index + counter) % len(events)]["indent"]) > indent
            ):
                counter += 1
            if (
                counter < len(events)
                and int(events[(index + counter) % len(events)]["indent"]) == indent
            ):
                counter -= 1
                end_event_index = (index + counter) % len(events)
                handled_events[end_event_index] = True

        duration_seconds = threshold_seconds
        if event_type == TIMER_EVENT_TYPE_START and end_event_index != -1:
            duration_seconds = float(
                events[end_event_index]["wall_time_seconds"]
            ) - float(event["wall_time_seconds"])

        if duration_seconds < threshold_seconds:
            continue

        line = "    " * indent + str(event["name"])
        if end_event_index != -1:
            line += f",  {duration_seconds:.6g} seconds"
        elif event_type == TIMER_EVENT_TYPE_INSERTED:
            line += (
                f",  {float(event['wall_time_seconds']):.6g} seconds (inserted time)"
            )
        elif event_type == TIMER_EVENT_TYPE_END:
            line += " (END event without matching START event)"
        lines.append(line)

    return "\n".join(lines) + ("\n" if lines else "")


def _collect_timer_scopes(events: list[dict[str, object]]) -> list[dict[str, object]]:
    scopes: list[dict[str, object]] = []
    stack: list[dict[str, object]] = []
    for event in events:
        event_type = int(event["event_type"])
        if event_type == 1:
            stack.append(event)
            continue
        if event_type != 2 or not stack:
            continue
        start_event = stack.pop()
        name = str(start_event["name"])
        start_seconds = float(start_event["wall_time_seconds"])
        end_seconds = float(event["wall_time_seconds"])
        scopes.append(
            {
                "name": name,
                "start_seconds": start_seconds,
                "end_seconds": end_seconds,
                "duration_ms": max(0.0, (end_seconds - start_seconds) * 1000.0),
                "indent": int(start_event["indent"]),
            }
        )
    return scopes


def _action_frame_entries(
    action_scope: dict[str, object], render_scopes: list[dict[str, object]]
) -> list[dict[str, object]]:
    action_name = str(action_scope["name"])[len(TIMER_ACTION_PREFIX) :]
    action_start_seconds = float(action_scope["start_seconds"])
    contained_render_scopes = [
        render_scope
        for render_scope in render_scopes
        if float(render_scope["start_seconds"]) >= action_start_seconds
        and float(render_scope["end_seconds"]) <= float(action_scope["end_seconds"])
    ]
    contained_render_scopes.sort(key=lambda scope: float(scope["end_seconds"]))

    frames: list[dict[str, object]] = []
    interactive_index = 0
    still_index = 0
    for frame_index, render_scope in enumerate(contained_render_scopes, start=1):
        render_kind = (
            "interactive"
            if render_scope["name"] == INTERACTIVE_RENDER_EVENT
            else "still"
        )
        if render_kind == "interactive":
            interactive_index += 1
            kind_frame_index = interactive_index
        else:
            still_index += 1
            kind_frame_index = still_index

        start_seconds = float(render_scope["start_seconds"])
        end_seconds = float(render_scope["end_seconds"])
        frames.append(
            {
                "action": action_name,
                "frame_index": frame_index,
                "kind": render_kind,
                "kind_frame_index": kind_frame_index,
                "name": str(render_scope["name"]),
                "start_seconds": start_seconds,
                "end_seconds": end_seconds,
                "duration_ms": float(render_scope["duration_ms"]),
                "start_ms_from_action_start": (start_seconds - action_start_seconds)
                * 1000.0,
                "end_ms_from_action_start": (end_seconds - action_start_seconds)
                * 1000.0,
            }
        )
    return frames


def _write_timer_log_artifacts(
    output_dir: Path,
    runtime_mode: str,
    requested_view_size: list[int],
    actual_render_window_size: list[int],
) -> None:
    timer_log_path = output_dir / "paraview_timer_log.txt"
    timer_log_standalone_path = output_dir / "paraview_timer_log_standalone.txt"
    timer_events_path = output_dir / "paraview_timer_events.json"
    timer_summary_path = output_dir / "paraview_timer_summary.json"
    frame_timeline_path = output_dir / "paraview_internal_frame_timeline.json"

    events = _collect_timer_events()
    timer_log_path.write_text(
        _format_timer_log_with_indents(events, threshold_seconds=0.0),
        encoding="utf-8",
    )
    vtkTimerLog.DumpLog(str(timer_log_standalone_path))
    timer_events_path.write_text(json.dumps(events, indent=2) + "\n", encoding="utf-8")

    scopes = _collect_timer_scopes(events)
    render_scopes = [
        scope
        for scope in scopes
        if scope["name"] in {INTERACTIVE_RENDER_EVENT, STILL_RENDER_EVENT}
    ]
    action_scopes = [
        scope for scope in scopes if str(scope["name"]).startswith(TIMER_ACTION_PREFIX)
    ]

    action_entries: list[dict[str, object]] = []
    all_frame_entries: list[dict[str, object]] = []
    for action_scope in action_scopes:
        action_name = str(action_scope["name"])[len(TIMER_ACTION_PREFIX) :]
        frame_entries = _action_frame_entries(action_scope, render_scopes)
        all_frame_entries.extend(frame_entries)
        interactive_frames = [
            frame for frame in frame_entries if frame["kind"] == "interactive"
        ]
        still_frames = [frame for frame in frame_entries if frame["kind"] == "still"]
        interactive_durations = [
            float(frame["duration_ms"]) for frame in interactive_frames
        ]
        still_durations = [float(frame["duration_ms"]) for frame in still_frames]
        first_render_complete_ms_from_action_start = None
        final_render_complete_ms_from_action_start = None
        first_interactive_render_complete_ms_from_action_start = None
        last_interactive_render_complete_ms_from_action_start = None
        first_still_render_complete_ms_from_action_start = None
        last_still_render_complete_ms_from_action_start = None
        release_to_first_still_ms = None
        if frame_entries:
            first_render_complete_ms_from_action_start = float(
                frame_entries[0]["end_ms_from_action_start"]
            )
            final_render_complete_ms_from_action_start = float(
                frame_entries[-1]["end_ms_from_action_start"]
            )
            if interactive_frames:
                first_interactive_render_complete_ms_from_action_start = float(
                    interactive_frames[0]["end_ms_from_action_start"]
                )
                last_interactive_render_complete_ms_from_action_start = float(
                    interactive_frames[-1]["end_ms_from_action_start"]
                )
            if still_frames:
                first_still_render_complete_ms_from_action_start = float(
                    still_frames[0]["end_ms_from_action_start"]
                )
                last_still_render_complete_ms_from_action_start = float(
                    still_frames[-1]["end_ms_from_action_start"]
                )
            if (
                last_interactive_render_complete_ms_from_action_start is not None
                and first_still_render_complete_ms_from_action_start is not None
            ):
                release_to_first_still_ms = (
                    first_still_render_complete_ms_from_action_start
                    - last_interactive_render_complete_ms_from_action_start
                )
        action_entries.append(
            {
                "action": action_name,
                "scope_duration_ms": float(action_scope["duration_ms"]),
                "interactive_render_total_ms": float(sum(interactive_durations)),
                "still_render_total_ms": float(sum(still_durations)),
                "render_total_ms": float(
                    sum(interactive_durations) + sum(still_durations)
                ),
                "first_render_complete_ms_from_action_start": first_render_complete_ms_from_action_start,
                "final_render_complete_ms_from_action_start": final_render_complete_ms_from_action_start,
                "first_interactive_render_complete_ms_from_action_start": first_interactive_render_complete_ms_from_action_start,
                "last_interactive_render_complete_ms_from_action_start": last_interactive_render_complete_ms_from_action_start,
                "first_still_render_complete_ms_from_action_start": first_still_render_complete_ms_from_action_start,
                "last_still_render_complete_ms_from_action_start": last_still_render_complete_ms_from_action_start,
                "release_to_first_still_ms": release_to_first_still_ms,
                "interactive_render_durations_ms": interactive_durations,
                "still_render_durations_ms": still_durations,
                "render_event_count": len(frame_entries),
            }
        )

    frame_timeline_path.write_text(
        json.dumps(all_frame_entries, indent=2) + "\n", encoding="utf-8"
    )
    timer_summary = {
        "runtime_mode": runtime_mode,
        "requested_view_size": requested_view_size,
        "actual_render_window_size": actual_render_window_size,
        "timer_log_path": str(timer_log_path),
        "timer_log_format": "indented_scopes_from_raw_events",
        "timer_log_standalone_path": str(timer_log_standalone_path),
        "timer_max_entries": TIMER_MAX_ENTRIES,
        "timer_event_count": len(events),
        "frame_timeline_path": str(frame_timeline_path),
        "actions": action_entries,
    }
    timer_summary_path.write_text(
        json.dumps(timer_summary, indent=2) + "\n", encoding="utf-8"
    )


def _run_action(
    view,
    spec: BenchmarkSpec,
    action: BenchmarkAction,
    logger: EventLogger,
    output_dir: Path,
    capture_screenshots: bool,
    pre_action_delay_seconds: float,
    deterministic_mode: str,
) -> None:
    logger.log(
        "action_start",
        action=action.name,
        kind=action.kind,
        duration_seconds=action.duration_seconds,
        steps=action.steps,
        deterministic_mode=deterministic_mode,
    )
    _mark_timer_action_start(action.name)
    try:
        if pre_action_delay_seconds > 0:
            time.sleep(pre_action_delay_seconds)

        cameras = _resolve_action_cameras(spec, action, deterministic_mode)
        start_monotonic_ns = time.monotonic_ns()
        interval_ns = (
            int(action.duration_seconds * 1e9 / action.steps)
            if action.kind == "interpolate"
            and deterministic_mode != "direct-final"
            and action.steps > 0
            else 0
        )

        for step_index, camera in enumerate(cameras, start=1):
            if step_index > 1 and interval_ns > 0:
                sleep_until(start_monotonic_ns + (step_index - 1) * interval_ns)
            _apply_camera(view, camera)
            if action.kind == "interpolate" and deterministic_mode != "direct-final":
                view.InteractiveRender()
            else:
                view.StillRender()
            logger.log(
                "action_step",
                action=action.name,
                step=step_index,
                steps=len(cameras),
                camera=camera.to_json(),
            )

        if (
            action.kind == "interpolate"
            and deterministic_mode == "interactive-plus-final"
        ):
            view.StillRender()
        logger.log("action_end", action=action.name)
        if action.settle_seconds > 0:
            time.sleep(action.settle_seconds)
        logger.log("action_settle_complete", action=action.name)
    finally:
        _mark_timer_action_end(action.name)

    if capture_screenshots:
        _write_screenshot(output_dir, action.name, spec, view)


def _run_open_action(
    dataset: str,
    view,
    spec: BenchmarkSpec,
    action: BenchmarkAction,
    logger: EventLogger,
    output_dir: Path,
    capture_screenshots: bool,
    requested_array_name: str,
    channel_mode: str,
    component: int,
    blend_mode: str,
    data_range_min: float | None,
    data_range_max: float | None,
    color_min_rgb: tuple[float, float, float] | list[float] | None,
    color_max_rgb: tuple[float, float, float] | list[float] | None,
    pre_action_delay_seconds: float,
):
    logger.log(
        "action_start",
        action=action.name,
        kind="load",
        duration_seconds=0.0,
        steps=1,
    )
    _mark_timer_action_start(action.name)
    try:
        if pre_action_delay_seconds > 0:
            time.sleep(pre_action_delay_seconds)

        logger.log("dataset_load_start", app="paraview")
        source = OpenDataFile(dataset)
        source.UpdatePipeline()
        resolved_array_name = _resolve_array_name(source, requested_array_name)
        display = Show(source, view)
        _configure_display(
            display,
            resolved_array_name,
            channel_mode,
            component,
            blend_mode,
            data_range_min,
            data_range_max,
            color_min_rgb,
            color_max_rgb,
        )
        HideUnusedScalarBars(view=view)

        cameras = interpolate_action_cameras(spec, action)
        if len(cameras) != 1:
            raise RuntimeError("open action must resolve to exactly one camera state")
        _apply_camera(view, cameras[0])
        view.StillRender()
        logger.log("dataset_load_done", app="paraview")
        logger.log("action_end", action=action.name)

        if action.settle_seconds > 0:
            time.sleep(action.settle_seconds)
        logger.log("action_settle_complete", action=action.name)
    finally:
        _mark_timer_action_end(action.name)

    if capture_screenshots:
        _write_screenshot(output_dir, action.name, spec, view)

    return source


def main() -> int:
    args = _parse_args()
    dataset = str(Path(args.dataset).resolve())
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    spec = load_benchmark_spec(args.camera_spec)
    logger = EventLogger(output_dir / "paraview_events.jsonl")
    memory_sampler = None
    memory_summary_path = output_dir / "paraview_memory_summary.json"

    try:
        runtime_mode = _runtime_mode()
        logger.log(
            "session_start",
            app="paraview",
            dataset=dataset,
            camera_spec=str(Path(args.camera_spec).resolve()),
            viewport={"width": spec.viewport_width, "height": spec.viewport_height},
            channel_mode=args.channel_mode,
            component=int(args.component),
            blend_mode=args.blend_mode,
            runtime_mode=runtime_mode,
            deterministic_mode=args.deterministic_mode,
        )

        if args.sample_rss:
            memory_sampler = ProcessMemorySampler(
                pid=int(args.rss_target_pid),
                interval_seconds=float(args.rss_sample_interval_seconds),
                output_path=output_dir / "paraview_rss_samples.jsonl",
            )
            memory_sampler.start()
            logger.log(
                "rss_sampling_started",
                app="paraview",
                pid=int(args.rss_target_pid),
                interval_seconds=float(args.rss_sample_interval_seconds),
            )

        if args.start_delay_seconds > 0:
            logger.log(
                "startup_delay_begin",
                app="paraview",
                seconds=float(args.start_delay_seconds),
            )
            time.sleep(float(args.start_delay_seconds))
            logger.log("startup_delay_end", app="paraview")

        view = GetActiveViewOrCreate("RenderView")
        view.ViewSize = [spec.viewport_width, spec.viewport_height]
        view.OrientationAxesVisibility = 0
        render_window = view.GetRenderWindow()
        actual_render_window_size = [int(v) for v in render_window.GetSize()]
        logger.log(
            "view_info",
            app="paraview",
            runtime_mode=runtime_mode,
            requested_view_size=[int(spec.viewport_width), int(spec.viewport_height)],
            actual_render_window_size=actual_render_window_size,
        )
        _reset_timer_log()
        actions = list(spec.actions)
        source = None

        if actions and actions[0].name == "open":
            source = _run_open_action(
                dataset,
                view,
                spec,
                actions[0],
                logger,
                output_dir,
                args.capture_screenshots,
                args.array_name,
                args.channel_mode,
                args.component,
                args.blend_mode,
                args.data_range_min,
                args.data_range_max,
                args.color_min_rgb,
                args.color_max_rgb,
                args.pre_action_delay_seconds,
            )
            actions = actions[1:]
        else:
            logger.log("dataset_load_start", app="paraview")
            source = OpenDataFile(dataset)
            source.UpdatePipeline()
            resolved_array_name = _resolve_array_name(source, args.array_name)
            display = Show(source, view)
            _configure_display(
                display,
                resolved_array_name,
                args.channel_mode,
                args.component,
                args.blend_mode,
                args.data_range_min,
                args.data_range_max,
                args.color_min_rgb,
                args.color_max_rgb,
            )
            HideUnusedScalarBars(view=view)
            view.StillRender()
            logger.log("dataset_load_done", app="paraview")

        for action in actions:
            _run_action(
                view,
                spec,
                action,
                logger,
                output_dir,
                args.capture_screenshots,
                args.pre_action_delay_seconds,
                args.deterministic_mode,
            )

        render_window = view.GetRenderWindow()
        actual_render_window_size = [int(v) for v in render_window.GetSize()]
        _write_timer_log_artifacts(
            output_dir,
            runtime_mode=runtime_mode,
            requested_view_size=[int(spec.viewport_width), int(spec.viewport_height)],
            actual_render_window_size=actual_render_window_size,
        )
        logger.log("session_end", app="paraview", ok=True)
        return 0
    except Exception as exc:
        logger.log("session_end", app="paraview", ok=False, error=str(exc))
        raise
    finally:
        if memory_sampler is not None:
            memory_summary = memory_sampler.stop()
            memory_summary_path.write_text(
                json.dumps(memory_summary, indent=2) + "\n",
                encoding="utf-8",
            )
            logger.log(
                "rss_sampling_finished",
                app="paraview",
                summary_path=str(memory_summary_path),
            )
        logger.close()


if __name__ == "__main__":
    raise SystemExit(main())
