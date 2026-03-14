from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path
from typing import Any

REPO_ROOT = Path(__file__).resolve().parents[2]
ATLAS_AGENT_SRC = REPO_ROOT / "python" / "atlas_agent" / "src"
if str(ATLAS_AGENT_SRC) not in sys.path:
    sys.path.insert(0, str(ATLAS_AGENT_SRC))

from atlas_agent.scene_rpc import SceneClient

from process_memory_sampler import ProcessMemorySampler
from volume_benchmark_common import (
    BenchmarkAction,
    BenchmarkSpec,
    EventLogger,
    interpolate_action_cameras,
    load_benchmark_spec,
    sleep_until,
)


CAMERA_JSON_KEY = "Camera 3DCamera"
FULL_RESOLUTION_JSON_KEY = "Full Resolution Rendering Bool"
COMPOSITING_JSON_KEY = "Compositing StringIntOption"
BOUND_BOX_JSON_KEY = "Bound Box StringIntOption"
NO_BOUND_BOX_VALUE = "No Bound Box"
BACKGROUND_SCOPE_ID = 1
AXIS_SCOPE_ID = 2
SHOW_BACKGROUND_JSON_KEY = "Show Background Bool"
SHOW_AXIS_JSON_KEY = "Show Axis Bool"
COMPOSITING_MODE_CHOICES = (
    "Direct Volume Rendering",
    "Maximum Intensity Projection",
    "MIP Opaque",
    "Local MIP",
    "Local MIP Opaque",
    "ISO Surface",
    "X Ray",
)


def _loaded_ids(task_status: dict[str, Any]) -> list[int]:
    load = task_status.get("load") or {}
    ids = load.get("loadedIds")
    if ids is None:
        ids = load.get("loaded_ids")
    if ids is not None:
        return [int(x) for x in ids]

    objects = load.get("objects") or []
    out: list[int] = []
    for obj in objects:
        if not isinstance(obj, dict):
            continue
        obj_id = obj.get("id")
        if obj_id is None:
            continue
        out.append(int(obj_id))
    return out


def _wait_ready(
    client: SceneClient, ids: list[int], timeout_sec: float
) -> dict[str, Any]:
    return client.wait_for_objects_ready(
        ids,
        timeout_sec=float(timeout_sec),
        poll_interval_sec=0.1,
    )


def _apply_camera(client: SceneClient, camera_payload: dict[str, Any]) -> None:
    ok = client.apply_params(
        [
            {
                "id": 0,
                "json_key": CAMERA_JSON_KEY,
                "value": camera_payload,
            }
        ]
    )
    if not ok:
        raise RuntimeError("ApplySceneParams failed while setting the benchmark camera")


def _set_canvas_size(
    client: SceneClient,
    logger: EventLogger,
    logical_width: int | None,
    logical_height: int | None,
    *,
    stage: str,
) -> None:
    if logical_width is None or logical_height is None:
        return
    canvas_size = client.set_3d_canvas_size(
        logical_width=int(logical_width),
        logical_height=int(logical_height),
    )
    if not canvas_size.get("ok", False):
        raise RuntimeError(
            f"Set3DCanvasSize failed: {json.dumps(canvas_size, sort_keys=True)}"
        )
    logger.log("canvas_size_set", app="atlas", stage=stage, canvas_size=canvas_size)


def _enable_full_resolution(
    client: SceneClient,
    ids: list[int],
    logger: EventLogger,
) -> list[int]:
    target_ids: list[int] = []
    for obj_id in ids:
        params = client.list_params(id=int(obj_id))
        for param in getattr(params, "params", []) or []:
            if str(getattr(param, "json_key", "") or "") == FULL_RESOLUTION_JSON_KEY:
                target_ids.append(int(obj_id))
                break

    if not target_ids:
        logger.log(
            "full_resolution_not_supported",
            app="atlas",
            ids=ids,
            json_key=FULL_RESOLUTION_JSON_KEY,
        )
        return []

    ok = client.apply_params(
        [
            {
                "id": int(obj_id),
                "json_key": FULL_RESOLUTION_JSON_KEY,
                "value": True,
            }
            for obj_id in target_ids
        ]
    )
    if not ok:
        raise RuntimeError(
            "ApplySceneParams failed while enabling Atlas full-resolution rendering"
        )
    logger.log(
        "full_resolution_enabled",
        app="atlas",
        ids=target_ids,
        json_key=FULL_RESOLUTION_JSON_KEY,
    )
    return target_ids


def _set_compositing_mode(
    client: SceneClient,
    ids: list[int],
    logger: EventLogger,
    compositing_mode: str,
) -> list[int]:
    target_ids: list[int] = []
    for obj_id in ids:
        params = client.list_params(id=int(obj_id))
        for param in getattr(params, "params", []) or []:
            if str(getattr(param, "json_key", "") or "") == COMPOSITING_JSON_KEY:
                target_ids.append(int(obj_id))
                break

    if not target_ids:
        logger.log(
            "compositing_mode_not_supported",
            app="atlas",
            ids=ids,
            json_key=COMPOSITING_JSON_KEY,
            requested_value=compositing_mode,
        )
        return []

    ok = client.apply_params(
        [
            {
                "id": int(obj_id),
                "json_key": COMPOSITING_JSON_KEY,
                "value": compositing_mode,
            }
            for obj_id in target_ids
        ]
    )
    if not ok:
        raise RuntimeError(
            "ApplySceneParams failed while setting Atlas compositing mode"
        )
    logger.log(
        "compositing_mode_applied",
        app="atlas",
        ids=target_ids,
        json_key=COMPOSITING_JSON_KEY,
        value=compositing_mode,
    )
    return target_ids


def _set_bound_box_mode(
    client: SceneClient,
    ids: list[int],
    logger: EventLogger,
    bound_box_mode: str,
) -> list[int]:
    target_ids: list[int] = []
    for obj_id in ids:
        params = client.list_params(id=int(obj_id))
        for param in getattr(params, "params", []) or []:
            if str(getattr(param, "json_key", "") or "") == BOUND_BOX_JSON_KEY:
                target_ids.append(int(obj_id))
                break

    if not target_ids:
        logger.log(
            "bound_box_mode_not_supported",
            app="atlas",
            ids=ids,
            json_key=BOUND_BOX_JSON_KEY,
            requested_value=bound_box_mode,
        )
        return []

    ok = client.apply_params(
        [
            {
                "id": int(obj_id),
                "json_key": BOUND_BOX_JSON_KEY,
                "value": bound_box_mode,
            }
            for obj_id in target_ids
        ]
    )
    if not ok:
        raise RuntimeError("ApplySceneParams failed while setting Atlas bound box mode")
    logger.log(
        "bound_box_mode_applied",
        app="atlas",
        ids=target_ids,
        json_key=BOUND_BOX_JSON_KEY,
        value=bound_box_mode,
    )
    return target_ids


def _set_scope_bool_param(
    client: SceneClient,
    scope_id: int,
    json_key: str,
    value: bool,
    logger: EventLogger,
    *,
    event_name: str,
    unsupported_event_name: str,
) -> bool:
    params = client.list_params(id=int(scope_id))
    supported = any(
        str(getattr(param, "json_key", "") or "") == json_key
        for param in getattr(params, "params", []) or []
    )
    if not supported:
        logger.log(
            unsupported_event_name,
            app="atlas",
            scope_id=int(scope_id),
            json_key=json_key,
            requested_value=bool(value),
        )
        return False

    ok = client.apply_params(
        [
            {
                "id": int(scope_id),
                "json_key": json_key,
                "value": bool(value),
            }
        ]
    )
    if not ok:
        raise RuntimeError(
            f"ApplySceneParams failed while setting scope {scope_id} parameter {json_key!r}"
        )
    logger.log(
        event_name,
        app="atlas",
        scope_id=int(scope_id),
        json_key=json_key,
        value=bool(value),
    )
    return True


def _write_screenshot(
    client: SceneClient,
    output_dir: Path,
    action_name: str,
    width: int,
    height: int,
) -> None:
    result = client.screenshot_3d(
        width=width,
        height=height,
        path=output_dir / f"{action_name}.png",
        overwrite=True,
    )
    if not result.get("ok", False):
        raise RuntimeError(
            f"TakeScreenshot3D failed for action {action_name!r}: {result.get('error', '')}"
        )


def _run_action(
    client: SceneClient,
    spec: BenchmarkSpec,
    action: BenchmarkAction,
    logger: EventLogger,
    output_dir: Path,
    capture_screenshots: bool,
    pre_action_delay_seconds: float,
    step_hold_seconds: float | None,
) -> None:
    logger.log(
        "action_start",
        action=action.name,
        kind=action.kind,
        duration_seconds=action.duration_seconds,
        steps=action.steps,
        step_hold_seconds=step_hold_seconds,
    )
    if pre_action_delay_seconds > 0:
        time.sleep(pre_action_delay_seconds)

    cameras = interpolate_action_cameras(spec, action)
    start_monotonic_ns = time.monotonic_ns()
    interval_ns = 0
    if action.kind == "interpolate":
        if step_hold_seconds is not None and step_hold_seconds > 0:
            interval_ns = int(float(step_hold_seconds) * 1e9)
        elif action.steps > 0:
            interval_ns = int(action.duration_seconds * 1e9 / action.steps)

    for step_index, camera in enumerate(cameras, start=1):
        if step_index > 1 and interval_ns > 0:
            sleep_until(start_monotonic_ns + (step_index - 1) * interval_ns)
        logger.log(
            "action_step",
            action=action.name,
            step=step_index,
            steps=len(cameras),
            camera=camera.to_json(),
        )
        _apply_camera(client, camera.to_atlas_typed_value())

    logger.log("action_end", action=action.name)
    if action.settle_seconds > 0:
        time.sleep(action.settle_seconds)
    logger.log("action_settle_complete", action=action.name)

    if capture_screenshots:
        _write_screenshot(
            client,
            output_dir,
            action.name,
            spec.viewport_width,
            spec.viewport_height,
        )


def _run_open_action(
    client: SceneClient,
    dataset: str,
    spec: BenchmarkSpec,
    action: BenchmarkAction,
    logger: EventLogger,
    output_dir: Path,
    capture_screenshots: bool,
    task_timeout_seconds: float,
    ready_timeout_seconds: float,
    pre_action_delay_seconds: float,
    canvas_logical_width: int | None,
    canvas_logical_height: int | None,
    enable_full_resolution: bool,
    compositing_mode: str | None,
    hide_bound_box: bool,
) -> list[int]:
    logger.log(
        "action_start",
        action=action.name,
        kind="load",
        duration_seconds=0.0,
        steps=1,
    )
    if pre_action_delay_seconds > 0:
        time.sleep(pre_action_delay_seconds)

    logger.log("dataset_load_start", app="atlas")
    task_id = client.start_load_task([dataset], set_visible=True)
    logger.log("task_started", app="atlas", task_id=task_id)

    task_status = client.wait_task(
        task_id,
        timeout_sec=float(task_timeout_seconds),
        poll_interval_sec=0.2,
    )
    logger.log("task_terminal", app="atlas", task_id=task_id, task_status=task_status)

    ids = _loaded_ids(task_status)
    if not ids:
        raise RuntimeError(
            f"Atlas load task produced no objects: {json.dumps(task_status, sort_keys=True)}"
        )

    ready = _wait_ready(client, ids, timeout_sec=float(ready_timeout_seconds))
    logger.log("objects_ready", app="atlas", ids=ids, ready_status=ready)
    if not ready.get("ok", False):
        raise RuntimeError(
            f"Atlas objects never became ready: {json.dumps(ready, sort_keys=True)}"
        )

    deleted = client.delete_task(task_id)
    logger.log("task_deleted", app="atlas", task_id=task_id, ok=deleted)

    if enable_full_resolution:
        _enable_full_resolution(client, ids, logger)
    if compositing_mode is not None:
        _set_compositing_mode(client, ids, logger, compositing_mode)
    if hide_bound_box:
        _set_bound_box_mode(client, ids, logger, NO_BOUND_BOX_VALUE)
    _set_canvas_size(
        client,
        logger,
        canvas_logical_width,
        canvas_logical_height,
        stage="post_load",
    )

    cameras = interpolate_action_cameras(spec, action)
    if len(cameras) != 1:
        raise RuntimeError("open action must resolve to exactly one camera state")
    logger.log(
        "open_target_view_requested",
        app="atlas",
        camera=cameras[0].to_json(),
    )
    _apply_camera(client, cameras[0].to_atlas_typed_value())

    logger.log("dataset_load_done", app="atlas", ids=ids)
    logger.log("action_end", action=action.name)
    if action.settle_seconds > 0:
        time.sleep(action.settle_seconds)
    logger.log("action_settle_complete", action=action.name)

    if capture_screenshots:
        _write_screenshot(
            client,
            output_dir,
            action.name,
            spec.viewport_width,
            spec.viewport_height,
        )

    return ids


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Drive Atlas volume rendering actions through the live Scene RPC. "
            "This script logs action markers for the screen-capture observer."
        )
    )
    parser.add_argument(
        "--dataset", required=True, help="Atlas dataset path, e.g. a .nim file"
    )
    parser.add_argument(
        "--camera-spec", required=True, help="Benchmark camera/action JSON"
    )
    parser.add_argument(
        "--output-dir",
        required=True,
        help="Directory for logs and optional screenshots",
    )
    parser.add_argument(
        "--address", default="localhost:50051", help="Atlas Scene RPC address"
    )
    parser.add_argument(
        "--canvas-logical-width",
        type=int,
        default=None,
        help=(
            "Optional live 3D canvas width in logical Qt pixels. "
            "Use this to control the on-screen render size for deterministic benchmarking."
        ),
    )
    parser.add_argument(
        "--canvas-logical-height",
        type=int,
        default=None,
        help="Optional live 3D canvas height in logical Qt pixels.",
    )
    parser.add_argument(
        "--disable-full-resolution",
        action="store_true",
        help=(
            "Do not enable Atlas 'Full Resolution Rendering' on loaded image objects. "
            "By default the benchmark enables it when the parameter is available."
        ),
    )
    parser.add_argument(
        "--compositing-mode",
        choices=COMPOSITING_MODE_CHOICES,
        default=None,
        help=(
            "Optional Atlas volume compositing mode applied to loaded image objects, "
            "for example 'MIP Opaque' or 'Direct Volume Rendering'."
        ),
    )
    parser.add_argument(
        "--hide-background",
        action="store_true",
        help="Hide the Atlas background pseudo-object during benchmark runs.",
    )
    parser.add_argument(
        "--hide-axis",
        action="store_true",
        help="Hide the Atlas axis pseudo-object during benchmark runs.",
    )
    parser.add_argument(
        "--hide-bound-box",
        action="store_true",
        help="Set loaded Atlas image objects to 'No Bound Box' during benchmark runs.",
    )
    parser.add_argument(
        "--task-timeout-seconds",
        type=float,
        default=300.0,
        help="Timeout for the asynchronous Atlas load task",
    )
    parser.add_argument(
        "--ready-timeout-seconds",
        type=float,
        default=120.0,
        help="Timeout for waiting until loaded objects are engine-ready",
    )
    parser.add_argument(
        "--capture-screenshots",
        action="store_true",
        help="Save one 3D screenshot after each action settles",
    )
    parser.add_argument(
        "--pre-action-delay-seconds",
        type=float,
        default=0.2,
        help=(
            "Short delay after logging action_start and before mutating the scene. "
            "This gives the external capture observer time to sample the baseline frame."
        ),
    )
    parser.add_argument(
        "--step-hold-seconds",
        type=float,
        default=None,
        help=(
            "Optional fixed delay between successive camera commands for interpolate actions. "
            "Use this for deterministic runs where each state should be given time to finish "
            "before the next command is sent."
        ),
    )
    parser.add_argument(
        "--sample-rss",
        action="store_true",
        help="Sample RSS during the run. Requires --rss-target-pid for the Atlas process.",
    )
    parser.add_argument(
        "--rss-target-pid",
        type=int,
        default=None,
        help="Atlas process ID to sample when --sample-rss is enabled.",
    )
    parser.add_argument(
        "--rss-sample-interval-seconds",
        type=float,
        default=0.1,
        help="Sampling interval for --sample-rss.",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    spec = load_benchmark_spec(args.camera_spec)
    logger = EventLogger(output_dir / "atlas_events.jsonl")
    client = SceneClient(address=args.address)
    memory_sampler = None
    memory_summary_path = output_dir / "atlas_memory_summary.json"
    loaded_ids: list[int] = []

    try:
        if args.sample_rss and not args.rss_target_pid:
            raise ValueError(
                "--sample-rss requires --rss-target-pid for the Atlas process"
            )
        if (args.canvas_logical_width is None) != (args.canvas_logical_height is None):
            raise ValueError(
                "--canvas-logical-width and --canvas-logical-height must be provided together"
            )

        logger.log(
            "session_start",
            app="atlas",
            dataset=str(Path(args.dataset).resolve()),
            camera_spec=str(Path(args.camera_spec).resolve()),
            viewport={"width": spec.viewport_width, "height": spec.viewport_height},
            canvas_logical_size=(
                None
                if args.canvas_logical_width is None
                else {
                    "width": int(args.canvas_logical_width),
                    "height": int(args.canvas_logical_height),
                }
            ),
            step_hold_seconds=args.step_hold_seconds,
            compositing_mode=args.compositing_mode,
            hide_background=bool(args.hide_background),
            hide_axis=bool(args.hide_axis),
            hide_bound_box=bool(args.hide_bound_box),
        )

        if args.sample_rss:
            memory_sampler = ProcessMemorySampler(
                pid=int(args.rss_target_pid),
                interval_seconds=float(args.rss_sample_interval_seconds),
                output_path=output_dir / "atlas_rss_samples.jsonl",
            )
            memory_sampler.start()
            logger.log(
                "rss_sampling_started",
                app="atlas",
                pid=int(args.rss_target_pid),
                interval_seconds=float(args.rss_sample_interval_seconds),
            )

        client.ensure_view()
        logger.log("engine_ready", app="atlas")
        if args.hide_background:
            _set_scope_bool_param(
                client,
                BACKGROUND_SCOPE_ID,
                SHOW_BACKGROUND_JSON_KEY,
                False,
                logger,
                event_name="background_hidden",
                unsupported_event_name="background_hide_not_supported",
            )
        if args.hide_axis:
            _set_scope_bool_param(
                client,
                AXIS_SCOPE_ID,
                SHOW_AXIS_JSON_KEY,
                False,
                logger,
                event_name="axis_hidden",
                unsupported_event_name="axis_hide_not_supported",
            )
        _set_canvas_size(
            client,
            logger,
            args.canvas_logical_width,
            args.canvas_logical_height,
            stage="pre_load",
        )
        actions = list(spec.actions)

        if actions and actions[0].name == "open":
            loaded_ids = _run_open_action(
                client,
                str(Path(args.dataset).resolve()),
                spec,
                actions[0],
                logger,
                output_dir,
                args.capture_screenshots,
                args.task_timeout_seconds,
                args.ready_timeout_seconds,
                args.pre_action_delay_seconds,
                args.canvas_logical_width,
                args.canvas_logical_height,
                not args.disable_full_resolution,
                args.compositing_mode,
                args.hide_bound_box,
            )
            actions = actions[1:]
        else:
            logger.log("dataset_load_start", app="atlas")
            task_id = client.start_load_task(
                [str(Path(args.dataset).resolve())], set_visible=True
            )
            logger.log("task_started", app="atlas", task_id=task_id)

            task_status = client.wait_task(
                task_id,
                timeout_sec=float(args.task_timeout_seconds),
                poll_interval_sec=0.2,
            )
            logger.log(
                "task_terminal", app="atlas", task_id=task_id, task_status=task_status
            )

            ids = _loaded_ids(task_status)
            if not ids:
                raise RuntimeError(
                    f"Atlas load task produced no objects: {json.dumps(task_status, sort_keys=True)}"
                )
            loaded_ids = list(ids)

            ready = _wait_ready(
                client, ids, timeout_sec=float(args.ready_timeout_seconds)
            )
            logger.log("objects_ready", app="atlas", ids=ids, ready_status=ready)
            if not ready.get("ok", False):
                raise RuntimeError(
                    f"Atlas objects never became ready: {json.dumps(ready, sort_keys=True)}"
                )

            # Finished tasks remain registered until deleted. Cleaning it up avoids
            # leaving a stale finished task in Atlas's task bookkeeping.
            deleted = client.delete_task(task_id)
            logger.log("task_deleted", app="atlas", task_id=task_id, ok=deleted)

            if not args.disable_full_resolution:
                _enable_full_resolution(client, ids, logger)
            if args.compositing_mode is not None:
                _set_compositing_mode(client, ids, logger, args.compositing_mode)
            if args.hide_bound_box:
                _set_bound_box_mode(client, ids, logger, NO_BOUND_BOX_VALUE)
            _set_canvas_size(
                client,
                logger,
                args.canvas_logical_width,
                args.canvas_logical_height,
                stage="post_load",
            )

            logger.log("dataset_load_done", app="atlas", ids=ids)

        for action in actions:
            _run_action(
                client,
                spec,
                action,
                logger,
                output_dir,
                args.capture_screenshots,
                args.pre_action_delay_seconds,
                args.step_hold_seconds,
            )

        logger.log("session_end", app="atlas", ok=True)
        return 0
    except Exception as exc:
        logger.log("session_end", app="atlas", ok=False, error=str(exc))
        raise
    finally:
        if loaded_ids:
            try:
                removed = client.remove_objects(loaded_ids, allow_unsaved=True)
                logger.log("objects_removed", app="atlas", ids=loaded_ids, ok=removed)
            except Exception as exc:
                logger.log(
                    "objects_removed",
                    app="atlas",
                    ids=loaded_ids,
                    ok=False,
                    error=str(exc),
                )
        if memory_sampler is not None:
            memory_summary = memory_sampler.stop()
            memory_summary_path.write_text(
                json.dumps(memory_summary, indent=2) + "\n",
                encoding="utf-8",
            )
            logger.log(
                "rss_sampling_finished",
                app="atlas",
                summary_path=str(memory_summary_path),
            )
        logger.close()


if __name__ == "__main__":
    raise SystemExit(main())
