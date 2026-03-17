#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import numpy as np
from PIL import Image, ImageDraw

REPO_ROOT = Path(__file__).resolve().parents[2]
ATLAS_AGENT_SRC = REPO_ROOT / "python" / "atlas_agent" / "src"
if str(ATLAS_AGENT_SRC) not in sys.path:
    sys.path.insert(0, str(ATLAS_AGENT_SRC))

from atlas_agent.scene_rpc import SceneClient

from atlas_fidelity_render import (
    _clear_existing_scene_objects,
    _configure_loaded_dataset,
    _discover_param_keys,
    _load_dataset,
    _prepare_scene_once,
)
from atlas_volume_benchmark import _set_canvas_size
from volume_benchmark_common import EventLogger, GenericCamera


DEFAULT_RENDER_ROOT = (
    REPO_ROOT
    / "large_test_image"
    / "fidelity_validation"
    / "high_res_20220219_fidelity_render_mip_zoom06_v2_coarse2_rawmip_v1"
)
DEFAULT_SUMMARY_JSON = (
    REPO_ROOT
    / "large_test_image"
    / "fidelity_validation"
    / "high_res_20220219_fidelity_render_mip_zoom06_v2_screenshot_summary_v1"
    / "summary.json"
)
DEFAULT_OUTPUT_DIR = (
    REPO_ROOT
    / "large_test_image"
    / "fidelity_validation"
    / "inspection"
    / "roi03_mip_adaptive_maxdiff_v1"
)
DEFAULT_ROI_LABEL = "roi03_cx10500_cy10000"
DEFAULT_MODE = "MIP Opaque"
DIFF_THRESHOLD = 200


@dataclass(frozen=True)
class LoadedCase:
    condition: str
    object_id: int
    dataset_path: Path


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Load one fidelity-validation ROI into Atlas for manual inspection. "
            "The script loads the retained reference, adaptive, coarse_l1, and coarse_l2 "
            "datasets aligned to the same global ROI coordinates and sets the exact "
            "benchmark camera used for the retained render."
        )
    )
    parser.add_argument("--address", default="localhost:50051")
    parser.add_argument("--render-root", type=Path, default=DEFAULT_RENDER_ROOT)
    parser.add_argument("--summary-json", type=Path, default=DEFAULT_SUMMARY_JSON)
    parser.add_argument("--output-dir", type=Path, default=DEFAULT_OUTPUT_DIR)
    parser.add_argument("--roi-label", default=DEFAULT_ROI_LABEL)
    parser.add_argument("--mode", default=DEFAULT_MODE)
    parser.add_argument(
        "--condition",
        default="adaptive",
        help="Condition to inspect against the reference when computing the hotspot.",
    )
    parser.add_argument(
        "--clear-existing",
        action="store_true",
        help="Remove existing Atlas scene objects before loading the inspection scene.",
    )
    parser.add_argument("--canvas-logical-width", type=int, default=1000)
    parser.add_argument("--canvas-logical-height", type=int, default=750)
    parser.add_argument("--task-timeout-seconds", type=float, default=300.0)
    parser.add_argument("--ready-timeout-seconds", type=float, default=120.0)
    return parser.parse_args()


def _slug(text: str) -> str:
    return text.strip().lower().replace(" ", "_").replace("-", "_").replace("/", "_")


def _load_json(path: Path) -> dict[str, Any]:
    payload = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(payload, dict):
        raise ValueError(f"{path} must contain a JSON object")
    return payload


def _load_render_payload(
    render_root: Path, roi_label: str, mode: str, condition: str
) -> dict[str, Any]:
    mode_slug = _slug(mode)
    render_path = (
        render_root / "renders" / roi_label / mode_slug / condition / "render.json"
    )
    return _load_json(render_path)


def _load_screenshot(path: Path) -> np.ndarray:
    with Image.open(path) as image:
        return np.asarray(image.convert("RGB"), dtype=np.uint8)


def _max_diff_summary(
    *, summary_json: Path, roi_label: str, mode: str, condition: str
) -> dict[str, Any]:
    summary = _load_json(summary_json)
    rows = summary.get("rows")
    if not isinstance(rows, list):
        raise ValueError(f"{summary_json} does not contain rows")
    row = next(
        (
            item
            for item in rows
            if item.get("roi_label") == roi_label
            and item.get("mode") == mode
            and item.get("condition") == condition
        ),
        None,
    )
    if not isinstance(row, dict):
        raise ValueError(
            f"No summary row found for roi={roi_label} mode={mode} condition={condition}"
        )
    return row


def _compute_diff_artifacts(
    *,
    reference_path: Path,
    candidate_path: Path,
    coarse_l1_path: Path,
    coarse_l2_path: Path,
    output_dir: Path,
) -> dict[str, Any]:
    output_dir.mkdir(parents=True, exist_ok=True)
    reference = _load_screenshot(reference_path)
    candidate = _load_screenshot(candidate_path)
    if reference.shape != candidate.shape:
        raise ValueError(
            f"Reference and candidate shapes differ: {reference.shape} vs {candidate.shape}"
        )

    diff = np.abs(reference.astype(np.int16) - candidate.astype(np.int16))
    max_channel_diff = diff.max(axis=2)
    max_diff = int(max_channel_diff.max())
    y, x = np.argwhere(max_channel_diff == max_diff)[0]

    hot_mask = max_channel_diff >= DIFF_THRESHOLD
    hot_points = np.argwhere(hot_mask)
    if hot_points.size > 0:
        y0, x0 = hot_points.min(axis=0)
        y1, x1 = hot_points.max(axis=0)
        hot_bbox = [int(x0), int(y0), int(x1), int(y1)]
    else:
        hot_bbox = None

    ref_pixel = reference[int(y), int(x)].tolist()
    cand_pixel = candidate[int(y), int(x)].tolist()

    ref_out = output_dir / "reference_annotated.png"
    cand_out = output_dir / "candidate_annotated.png"
    grid_out = output_dir / "comparison_crop_grid.png"
    grid_all_out = output_dir / "comparison_crop_grid_all_conditions.png"

    _write_annotated_image(
        image=reference,
        out_path=ref_out,
        point=(int(x), int(y)),
        bbox=hot_bbox,
    )
    _write_annotated_image(
        image=candidate,
        out_path=cand_out,
        point=(int(x), int(y)),
        bbox=hot_bbox,
    )
    _write_crop_grid(
        tiles=[
            ("reference", reference),
            ("adaptive", candidate),
        ],
        out_path=grid_out,
        point=(int(x), int(y)),
        crop_size=160,
    )
    _write_crop_grid(
        tiles=[
            ("reference", reference),
            ("adaptive", candidate),
            ("coarse_l1", _load_screenshot(coarse_l1_path)),
            ("coarse_l2", _load_screenshot(coarse_l2_path)),
        ],
        out_path=grid_all_out,
        point=(int(x), int(y)),
        crop_size=160,
    )

    return {
        "max_diff": max_diff,
        "max_diff_xy": [int(x), int(y)],
        "reference_pixel_rgb": ref_pixel,
        "candidate_pixel_rgb": cand_pixel,
        "diff_ge_200_count": int(hot_mask.sum()),
        "diff_ge_200_bbox_xyxy": hot_bbox,
        "reference_annotated_path": str(ref_out),
        "candidate_annotated_path": str(cand_out),
        "comparison_crop_grid_path": str(grid_out),
        "comparison_crop_grid_all_conditions_path": str(grid_all_out),
    }


def _write_annotated_image(
    *,
    image: np.ndarray,
    out_path: Path,
    point: tuple[int, int],
    bbox: list[int] | None,
) -> None:
    pil = Image.fromarray(image, mode="RGB")
    draw = ImageDraw.Draw(pil)
    x, y = point
    cross = 18
    draw.line((x - cross, y, x + cross, y), fill=(255, 0, 0), width=3)
    draw.line((x, y - cross, x, y + cross), fill=(255, 0, 0), width=3)
    if bbox is not None:
        draw.rectangle(tuple(bbox), outline=(0, 255, 255), width=3)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    pil.save(out_path)


def _write_crop_grid(
    *,
    tiles: list[tuple[str, np.ndarray]],
    out_path: Path,
    point: tuple[int, int],
    crop_size: int,
) -> None:
    x, y = point
    half = crop_size // 2
    reference = tiles[0][1]
    x0 = max(0, x - half)
    y0 = max(0, y - half)
    x1 = min(reference.shape[1], x0 + crop_size)
    y1 = min(reference.shape[0], y0 + crop_size)
    x0 = max(0, x1 - crop_size)
    y0 = max(0, y1 - crop_size)

    crops = [
        (label, Image.fromarray(image[y0:y1, x0:x1], mode="RGB"))
        for label, image in tiles
    ]
    tile_width = crops[0][1].width
    tile_height = crops[0][1].height
    label_band = 24
    canvas = Image.new(
        "RGB",
        (tile_width * len(crops), tile_height + label_band),
        color=(0, 0, 0),
    )
    draw = ImageDraw.Draw(canvas)
    local_x = x - x0
    local_y = y - y0
    for index, (label, crop) in enumerate(crops):
        offset = index * tile_width
        canvas.paste(crop, (offset, label_band))
        draw.text((offset + 6, 4), label, fill=(255, 255, 0))
        draw.line(
            (
                offset + local_x - 10,
                label_band + local_y,
                offset + local_x + 10,
                label_band + local_y,
            ),
            fill=(255, 0, 0),
            width=2,
        )
        draw.line(
            (
                offset + local_x,
                label_band + local_y - 10,
                offset + local_x,
                label_band + local_y + 10,
            ),
            fill=(255, 0, 0),
            width=2,
        )
    out_path.parent.mkdir(parents=True, exist_ok=True)
    canvas.save(out_path)


def _camera_from_render_payload(payload: dict[str, Any]) -> GenericCamera:
    return GenericCamera.from_json(payload["camera"])


def _globalized_coord_transform(payload: dict[str, Any]) -> dict[str, Any] | None:
    coord_transform = payload.get("coord_transform")
    if not isinstance(coord_transform, dict):
        return None
    roi_metadata = payload.get("roi_metadata")
    if not isinstance(roi_metadata, dict):
        raise ValueError("render payload missing roi_metadata")
    bounds = roi_metadata.get("bounds_xyz")
    if not isinstance(bounds, dict):
        raise ValueError("render payload missing bounds_xyz")
    x_bounds = bounds.get("x")
    y_bounds = bounds.get("y")
    if not (
        isinstance(x_bounds, list)
        and len(x_bounds) == 2
        and isinstance(y_bounds, list)
        and len(y_bounds) == 2
    ):
        raise ValueError(f"Malformed bounds_xyz in render payload: {bounds}")

    updated = json.loads(json.dumps(coord_transform))
    updated["Translation Vec3"] = [float(x_bounds[0]), float(y_bounds[0]), 0.0]
    return updated


def _ensure_scene_is_empty(client: SceneClient, *, clear_existing: bool) -> None:
    resp = client.list_objects()
    objects = list(getattr(resp, "objects", []) or [])
    if not objects:
        return
    if clear_existing:
        _clear_existing_scene_objects(client)
        return
    ids = [int(getattr(obj, "id", 0)) for obj in objects]
    raise RuntimeError(
        "Atlas already has scene objects loaded. Re-run with --clear-existing to replace "
        f"them. Existing ids: {ids}"
    )


def _configure_case(
    client: SceneClient,
    *,
    payload: dict[str, Any],
    camera: GenericCamera,
    obj_id: int,
    param_keys: Any,
    use_globalized_transform: bool,
) -> None:
    coord_transform = (
        _globalized_coord_transform(payload)
        if use_globalized_transform
        else payload.get("coord_transform")
    )
    _configure_loaded_dataset(
        client,
        obj_id=int(obj_id),
        param_keys=param_keys,
        compositing_mode=str(payload["mode"]),
        sampling_rate=float(payload["sampling_rate"]),
        display_range_min=float(payload["display_range"][0]),
        display_range_max=float(payload["display_range"][1]),
        camera=camera,
        transfer_function_payload=payload["transfer_function"],
        hide_bound_box=True,
        enable_full_resolution=bool(payload["enable_full_resolution"]),
        coord_transform_payload=coord_transform
        if isinstance(coord_transform, dict)
        else None,
        local_cut_spans=payload.get("local_cut_spans"),
    )


def main() -> int:
    args = _parse_args()
    render_root = args.render_root.resolve()
    output_dir = args.output_dir.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)

    reference_payload = _load_render_payload(
        render_root, args.roi_label, args.mode, "reference"
    )
    inspect_payload = _load_render_payload(
        render_root, args.roi_label, args.mode, args.condition
    )
    coarse_l1_payload = _load_render_payload(
        render_root, args.roi_label, args.mode, "coarse_l1"
    )
    coarse_l2_payload = _load_render_payload(
        render_root, args.roi_label, args.mode, "coarse_l2"
    )
    diff_summary = _max_diff_summary(
        summary_json=args.summary_json.resolve(),
        roi_label=args.roi_label,
        mode=args.mode,
        condition=args.condition,
    )
    diff_details = _compute_diff_artifacts(
        reference_path=Path(str(diff_summary["reference_path"])),
        candidate_path=Path(str(diff_summary["candidate_path"])),
        coarse_l1_path=Path(str(coarse_l1_payload["screenshot_path"])),
        coarse_l2_path=Path(str(coarse_l2_payload["screenshot_path"])),
        output_dir=output_dir,
    )

    logger = EventLogger(output_dir / "inspection_events.jsonl")
    client = SceneClient(args.address)
    client.ensure_view()
    _ensure_scene_is_empty(client, clear_existing=bool(args.clear_existing))
    _prepare_scene_once(
        client,
        logger,
        canvas_logical_width=int(args.canvas_logical_width),
        canvas_logical_height=int(args.canvas_logical_height),
        hide_background=True,
        hide_axis=True,
    )
    _set_canvas_size(
        client,
        logger,
        int(args.canvas_logical_width),
        int(args.canvas_logical_height),
        stage="inspection_setup",
    )

    camera = _camera_from_render_payload(inspect_payload)
    load_specs = [
        ("reference", reference_payload, True),
        ("adaptive", inspect_payload, False),
        ("coarse_l1", coarse_l1_payload, True),
        ("coarse_l2", coarse_l2_payload, True),
    ]
    loaded: list[LoadedCase] = []
    for condition, payload, globalize_transform in load_specs:
        ids = _load_dataset(
            client,
            Path(str(payload["dataset_path"])),
            task_timeout_seconds=float(args.task_timeout_seconds),
            ready_timeout_seconds=float(args.ready_timeout_seconds),
        )
        if len(ids) != 1:
            raise RuntimeError(
                f"Expected exactly one object for {condition}, got ids={ids}"
            )
        obj_id = int(ids[0])
        param_keys = _discover_param_keys(client, obj_id)
        _configure_case(
            client,
            payload=payload,
            camera=camera,
            obj_id=obj_id,
            param_keys=param_keys,
            use_globalized_transform=globalize_transform,
        )
        loaded.append(
            LoadedCase(
                condition=condition,
                object_id=obj_id,
                dataset_path=Path(str(payload["dataset_path"])),
            )
        )

    reference_id = next(
        case.object_id for case in loaded if case.condition == "reference"
    )
    for case in loaded:
        client.set_visibility([case.object_id], case.object_id == reference_id)

    scene_path = output_dir / f"{args.roi_label}_{_slug(args.mode)}_inspection.scene"
    client.save_scene(scene_path)

    summary = {
        "roi_label": args.roi_label,
        "mode": args.mode,
        "inspected_condition": args.condition,
        "loaded_objects": [
            {
                "condition": case.condition,
                "object_id": case.object_id,
                "dataset_path": str(case.dataset_path),
                "visible": case.object_id == reference_id,
            }
            for case in loaded
        ],
        "camera": inspect_payload["camera"],
        "diff_summary_row": diff_summary,
        "diff_details": diff_details,
        "saved_scene_path": str(scene_path),
    }
    summary_path = output_dir / "inspection_summary.json"
    summary_path.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(summary, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
