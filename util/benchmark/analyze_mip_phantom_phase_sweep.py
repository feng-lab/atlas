#!/usr/bin/env python3

from __future__ import annotations

import argparse
import csv
import json
import math
from pathlib import Path
from typing import Any

import matplotlib
import numpy as np
from PIL import Image, ImageDraw, ImageFont

matplotlib.use("Agg")
from matplotlib import pyplot as plt

from atlas_fidelity_render import _camera_basis
from volume_benchmark_common import GenericCamera


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Analyze MIP phantom phase-sweep screenshots. The primary readout is "
            "thin-line persistence and pair-separation stability across subpixel phases."
        )
    )
    parser.add_argument("--render-manifest", type=Path, required=True)
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=None,
        help="Output directory for summary artifacts. Defaults to <render_root>/analysis.",
    )
    parser.add_argument(
        "--peak-radius",
        type=int,
        default=2,
        help="Search radius in pixels for line-recall local-peak detection around each expected probe.",
    )
    parser.add_argument(
        "--detection-threshold",
        type=float,
        default=64.0,
        help="Intensity threshold used to classify a line probe as recovered or a profile peak as detected.",
    )
    parser.add_argument(
        "--pair-valley-ratio-threshold",
        type=float,
        default=0.85,
        help=(
            "Maximum valley/min-peak ratio allowed for a pair profile to count as visibly split. "
            "Lower values require a clearer valley between the two peaks."
        ),
    )
    parser.add_argument(
        "--profile-step-pixels",
        type=float,
        default=1.0,
        help="Sample spacing in pixels for pair-separation profile extraction.",
    )
    parser.add_argument(
        "--strip-padding",
        type=int,
        default=12,
        help="Padding around the projected target bbox when saving per-target phase strips.",
    )
    return parser.parse_args()


def _load_json(path: Path) -> dict[str, Any]:
    payload = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(payload, dict):
        raise ValueError(f"{path} must contain a JSON object")
    return payload


def _load_rgb(path: Path) -> np.ndarray:
    with Image.open(path) as image:
        return np.asarray(image.convert("RGB"), dtype=np.uint8)


def _intensity(rgb: np.ndarray) -> np.ndarray:
    return rgb.max(axis=2).astype(np.float32)


def _project_point(
    camera: GenericCamera,
    point_xyz: tuple[float, float, float],
    width: int,
    height: int,
) -> tuple[float, float]:
    forward, right, up = _camera_basis(camera)
    vx = float(point_xyz[0]) - float(camera.eye[0])
    vy = float(point_xyz[1]) - float(camera.eye[1])
    vz = float(point_xyz[2]) - float(camera.eye[2])
    x_cam = vx * right[0] + vy * right[1] + vz * right[2]
    y_cam = vx * up[0] + vy * up[1] + vz * up[2]
    z_cam = vx * forward[0] + vy * forward[1] + vz * forward[2]
    if z_cam <= 1e-6:
        raise ValueError(f"projected point is behind the camera: {point_xyz}")
    tan_half_v = math.tan(math.radians(float(camera.field_of_view_degrees)) / 2.0)
    aspect = float(width) / float(height)
    x_ndc = x_cam / (z_cam * tan_half_v * aspect)
    y_ndc = y_cam / (z_cam * tan_half_v)
    x_pixel = (x_ndc * 0.5 + 0.5) * float(width)
    y_pixel = (0.5 - y_ndc * 0.5) * float(height)
    return (x_pixel, y_pixel)


def _patch_bounds(
    x: float, y: float, radius: int, width: int, height: int
) -> tuple[int, int, int, int]:
    x0 = max(0, int(math.floor(x)) - radius)
    x1 = min(width, int(math.floor(x)) + radius + 1)
    y0 = max(0, int(math.floor(y)) - radius)
    y1 = min(height, int(math.floor(y)) + radius + 1)
    if x0 >= x1 or y0 >= y1:
        raise ValueError(
            f"empty patch for projected point {(x, y)} and radius {radius}"
        )
    return x0, x1, y0, y1


def _normalize_2d(vec: tuple[float, float]) -> tuple[float, float]:
    length = math.hypot(vec[0], vec[1])
    if length <= 1e-12:
        return (0.0, 0.0)
    return (vec[0] / length, vec[1] / length)


def _phase_sort_key(record: dict[str, Any]) -> tuple[str, int]:
    return (str(record["condition"]), int(record["phase_index"]))


def _phase_translation_xyz(record: dict[str, Any]) -> tuple[float, float, float]:
    phase_translation = record.get("phase_translation_xyz")
    if isinstance(phase_translation, list) and len(phase_translation) == 3:
        return (
            float(phase_translation[0]),
            float(phase_translation[1]),
            float(phase_translation[2]),
        )
    translation = record.get("translation_xyz")
    if isinstance(translation, list) and len(translation) == 3:
        return (float(translation[0]), float(translation[1]), float(translation[2]))
    raise ValueError("phase record is missing phase_translation_xyz / translation_xyz")


def _target_bbox(
    *,
    camera: GenericCamera,
    probe_points_scene_xyz: list[list[float]],
    translation_xyzs: list[tuple[float, float, float]],
    width: int,
    height: int,
    padding: int,
) -> tuple[int, int, int, int]:
    xs: list[float] = []
    ys: list[float] = []
    for translation_xyz in translation_xyzs:
        for probe in probe_points_scene_xyz:
            px, py = _project_point(
                camera,
                (
                    float(probe[0]) + float(translation_xyz[0]),
                    float(probe[1]) + float(translation_xyz[1]),
                    float(probe[2]) + float(translation_xyz[2]),
                ),
                width,
                height,
            )
            xs.append(px)
            ys.append(py)
    x0 = max(0, int(math.floor(min(xs))) - padding)
    x1 = min(width, int(math.ceil(max(xs))) + padding + 1)
    y0 = max(0, int(math.floor(min(ys))) - padding)
    y1 = min(height, int(math.ceil(max(ys))) + padding + 1)
    return x0, x1, y0, y1


def _probe_metrics(
    image: np.ndarray, expected_xy: tuple[float, float], radius: int
) -> dict[str, float]:
    height, width = image.shape
    x0, x1, y0, y1 = _patch_bounds(
        expected_xy[0], expected_xy[1], radius, width, height
    )
    patch = image[y0:y1, x0:x1]
    local_max = float(patch.max())
    max_positions = np.argwhere(patch == patch.max())
    max_y, max_x = max_positions[0]
    peak_x = float(x0 + max_x)
    peak_y = float(y0 + max_y)
    offset = math.hypot(peak_x - expected_xy[0], peak_y - expected_xy[1])
    weights = patch.astype(np.float64)
    weight_sum = float(weights.sum())
    if weight_sum > 1e-6:
        yy, xx = np.mgrid[y0:y1, x0:x1]
        centroid_x = float((weights * xx).sum() / weight_sum)
        centroid_y = float((weights * yy).sum() / weight_sum)
        centroid_error = math.hypot(
            centroid_x - expected_xy[0], centroid_y - expected_xy[1]
        )
    else:
        centroid_x = float(expected_xy[0])
        centroid_y = float(expected_xy[1])
        centroid_error = 0.0
    return {
        "expected_x": float(expected_xy[0]),
        "expected_y": float(expected_xy[1]),
        "peak_x": peak_x,
        "peak_y": peak_y,
        "peak_value": local_max,
        "peak_offset_pixels": float(offset),
        "centroid_x": centroid_x,
        "centroid_y": centroid_y,
        "centroid_error_pixels": float(centroid_error),
    }


def _bilinear_sample(image: np.ndarray, x: float, y: float) -> float:
    height, width = image.shape
    x = min(max(x, 0.0), float(width - 1))
    y = min(max(y, 0.0), float(height - 1))
    x0 = int(math.floor(x))
    x1 = min(x0 + 1, width - 1)
    y0 = int(math.floor(y))
    y1 = min(y0 + 1, height - 1)
    tx = x - float(x0)
    ty = y - float(y0)
    top = float(image[y0, x0]) * (1.0 - tx) + float(image[y0, x1]) * tx
    bottom = float(image[y1, x0]) * (1.0 - tx) + float(image[y1, x1]) * tx
    return top * (1.0 - ty) + bottom * ty


def _extract_profile(
    *,
    image: np.ndarray,
    center_xy: tuple[float, float],
    tangent_xy: tuple[float, float],
    normal_xy: tuple[float, float],
    half_length_pixels: float,
    average_half_width_pixels: float,
    step_pixels: float,
) -> tuple[np.ndarray, np.ndarray]:
    tangent_xy = _normalize_2d(tangent_xy)
    normal_xy = _normalize_2d(normal_xy)
    if tangent_xy == (0.0, 0.0) and normal_xy == (0.0, 0.0):
        raise ValueError("profile basis is degenerate")
    if tangent_xy == (0.0, 0.0):
        tangent_xy = (-normal_xy[1], normal_xy[0])
    if normal_xy == (0.0, 0.0):
        normal_xy = (-tangent_xy[1], tangent_xy[0])

    positions = np.arange(
        -float(half_length_pixels),
        float(half_length_pixels) + float(step_pixels) * 0.5,
        float(step_pixels),
        dtype=np.float32,
    )
    tangential_offsets = np.arange(
        -float(average_half_width_pixels),
        float(average_half_width_pixels) + 0.5,
        1.0,
        dtype=np.float32,
    )
    if tangential_offsets.size == 0:
        tangential_offsets = np.asarray([0.0], dtype=np.float32)

    profile = np.zeros_like(positions, dtype=np.float32)
    for index, offset in enumerate(positions):
        samples: list[float] = []
        for tangent_offset in tangential_offsets:
            x = (
                float(center_xy[0])
                + float(normal_xy[0]) * float(offset)
                + float(tangent_xy[0]) * float(tangent_offset)
            )
            y = (
                float(center_xy[1])
                + float(normal_xy[1]) * float(offset)
                + float(tangent_xy[1]) * float(tangent_offset)
            )
            samples.append(_bilinear_sample(image, x, y))
        profile[index] = float(np.mean(samples))
    return positions, profile


def _local_maxima(values: np.ndarray, threshold: float) -> list[tuple[int, float]]:
    maxima: list[tuple[int, float]] = []
    for index, value in enumerate(values):
        if float(value) < float(threshold):
            continue
        prev_value = float(values[index - 1]) if index > 0 else -math.inf
        next_value = float(values[index + 1]) if index + 1 < values.size else -math.inf
        if (
            value >= prev_value
            and value >= next_value
            and (value > prev_value or value > next_value)
        ):
            maxima.append((index, float(value)))
    return maxima


def _pair_profile_metrics(
    *,
    positions: np.ndarray,
    profile: np.ndarray,
    threshold: float,
    minimum_peak_separation_pixels: float,
    valley_ratio_threshold: float,
) -> dict[str, float | int]:
    peaks = _local_maxima(profile, threshold)
    best: dict[str, float | int] | None = None
    for left_index in range(len(peaks)):
        for right_index in range(left_index + 1, len(peaks)):
            left_peak = peaks[left_index]
            right_peak = peaks[right_index]
            separation = float(abs(positions[right_peak[0]] - positions[left_peak[0]]))
            if separation < float(minimum_peak_separation_pixels):
                continue
            segment = profile[left_peak[0] : right_peak[0] + 1]
            valley_value = float(np.min(segment))
            min_peak_value = min(float(left_peak[1]), float(right_peak[1]))
            valley_ratio = (
                1.0 if min_peak_value <= 1e-6 else valley_value / min_peak_value
            )
            candidate = {
                "left_index": int(left_peak[0]),
                "right_index": int(right_peak[0]),
                "left_peak_value": float(left_peak[1]),
                "right_peak_value": float(right_peak[1]),
                "peak_separation_pixels": separation,
                "valley_value": valley_value,
                "valley_ratio": float(valley_ratio),
            }
            if best is None:
                best = candidate
                continue
            best_min_peak = min(
                float(best["left_peak_value"]), float(best["right_peak_value"])
            )
            candidate_min_peak = min(
                float(candidate["left_peak_value"]),
                float(candidate["right_peak_value"]),
            )
            if candidate_min_peak > best_min_peak + 1e-6:
                best = candidate
            elif abs(candidate_min_peak - best_min_peak) <= 1e-6 and float(
                candidate["valley_ratio"]
            ) < float(best["valley_ratio"]):
                best = candidate

    if best is None:
        return {
            "profile_max_value": float(profile.max()) if profile.size else 0.0,
            "num_profile_peaks_above_threshold": int(len(peaks)),
            "two_peak_detected": 0,
            "peak_separation_pixels": 0.0,
            "valley_ratio": 1.0,
            "left_peak_value": 0.0,
            "right_peak_value": 0.0,
        }

    best["profile_max_value"] = float(profile.max()) if profile.size else 0.0
    best["num_profile_peaks_above_threshold"] = int(len(peaks))
    best["two_peak_detected"] = (
        1 if float(best["valley_ratio"]) <= float(valley_ratio_threshold) else 0
    )
    return best


def _save_phase_strip(
    *,
    image_paths: list[Path],
    bbox: tuple[int, int, int, int],
    out_path: Path,
    title: str,
) -> None:
    x0, x1, y0, y1 = bbox
    tiles: list[Image.Image] = []
    for path in image_paths:
        with Image.open(path) as image:
            tiles.append(image.convert("RGB").crop((x0, y0, x1, y1)))
    if not tiles:
        return
    font = ImageFont.load_default()
    label_height = 18
    tile_width, tile_height = tiles[0].size
    canvas = Image.new(
        "RGB", (tile_width * len(tiles), tile_height + label_height), color=(0, 0, 0)
    )
    draw = ImageDraw.Draw(canvas)
    for index, tile in enumerate(tiles):
        canvas.paste(tile, (index * tile_width, label_height))
        draw.text(
            (index * tile_width + 2, 2), f"p{index}", fill=(255, 255, 255), font=font
        )
    draw.text((2, label_height - 14), title, fill=(255, 255, 0), font=font)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    canvas.save(out_path)


def _plot_target_metric(
    *,
    phase_indices: list[int],
    values_by_condition: dict[str, list[float]],
    ylabel: str,
    title: str,
    out_path: Path,
) -> None:
    plt.figure(figsize=(8, 4))
    for condition, values in values_by_condition.items():
        plt.plot(phase_indices, values, marker="o", label=condition)
    plt.xlabel("Phase Index")
    plt.ylabel(ylabel)
    plt.title(title)
    plt.grid(True, alpha=0.3)
    plt.legend()
    out_path.parent.mkdir(parents=True, exist_ok=True)
    plt.tight_layout()
    plt.savefig(out_path, dpi=150)
    plt.close()


def _condition_frame_delta(records: list[dict[str, Any]]) -> tuple[float, float]:
    images = [
        _intensity(_load_rgb(Path(str(record["screenshot_path"]))))
        for record in records
    ]
    frame_deltas = [
        float(np.mean(np.abs(images[index + 1] - images[index])))
        for index in range(len(images) - 1)
    ]
    if not frame_deltas:
        return (0.0, 0.0)
    return (float(np.mean(frame_deltas)), float(np.max(frame_deltas)))


def _analyze_line_feature(
    *,
    feature: dict[str, Any],
    grouped: dict[str, list[dict[str, Any]]],
    camera: GenericCamera,
    width: int,
    height: int,
    peak_radius: int,
    detection_threshold: float,
    strip_padding: int,
    output_dir: Path,
) -> tuple[list[dict[str, Any]], list[dict[str, Any]]]:
    feature_id = str(feature["feature_id"])
    probe_points_scene_xyz = feature["probe_points_scene_xyz"]
    summary_rows: list[dict[str, Any]] = []
    phase_rows: list[dict[str, Any]] = []
    peak_plot_values: dict[str, list[float]] = {}
    recall_plot_values: dict[str, list[float]] = {}
    phase_indices: list[int] = []

    for condition, records in grouped.items():
        image_paths = [Path(str(record["screenshot_path"])) for record in records]
        phase_translation_xyzs = [_phase_translation_xyz(record) for record in records]
        bbox = _target_bbox(
            camera=camera,
            probe_points_scene_xyz=probe_points_scene_xyz,
            translation_xyzs=phase_translation_xyzs,
            width=width,
            height=height,
            padding=strip_padding,
        )
        phase_metrics: list[dict[str, Any]] = []
        for record, translation_xyz in zip(records, phase_translation_xyzs):
            image = _intensity(_load_rgb(Path(str(record["screenshot_path"]))))
            probe_metrics = []
            for probe in probe_points_scene_xyz:
                expected_xy = _project_point(
                    camera,
                    (
                        float(probe[0]) + float(translation_xyz[0]),
                        float(probe[1]) + float(translation_xyz[1]),
                        float(probe[2]) + float(translation_xyz[2]),
                    ),
                    width,
                    height,
                )
                probe_metrics.append(_probe_metrics(image, expected_xy, peak_radius))
            peak_values = [float(item["peak_value"]) for item in probe_metrics]
            peak_offsets = [float(item["peak_offset_pixels"]) for item in probe_metrics]
            centroid_errors = [
                float(item["centroid_error_pixels"]) for item in probe_metrics
            ]
            detection_flags = [
                1.0 if value >= detection_threshold else 0.0 for value in peak_values
            ]
            row = {
                "feature_id": feature_id,
                "feature_kind": "line_persistence",
                "condition": condition,
                "phase_index": int(record["phase_index"]),
                "phase_fraction": float(record["phase_fraction"]),
                "mean_probe_peak_value": float(np.mean(peak_values)),
                "min_probe_peak_value": float(np.min(peak_values)),
                "line_recall": float(np.mean(detection_flags)),
                "mean_peak_offset_pixels": float(np.mean(peak_offsets)),
                "mean_centroid_error_pixels": float(np.mean(centroid_errors)),
                "num_probes": len(probe_metrics),
                "probe_metrics": probe_metrics,
            }
            phase_rows.append(row)
            phase_metrics.append(row)

        phase_metrics.sort(key=lambda item: int(item["phase_index"]))
        phase_indices = [int(item["phase_index"]) for item in phase_metrics]
        mean_peak_series = [
            float(item["mean_probe_peak_value"]) for item in phase_metrics
        ]
        recall_series = [float(item["line_recall"]) for item in phase_metrics]
        centroid_series = [
            float(item["mean_centroid_error_pixels"]) for item in phase_metrics
        ]
        peak_plot_values[condition] = mean_peak_series
        recall_plot_values[condition] = recall_series
        summary_rows.append(
            {
                "feature_id": feature_id,
                "feature_kind": "line_persistence",
                "condition": condition,
                "mean_line_peak_value": float(np.mean(mean_peak_series)),
                "min_phase_mean_peak_value": float(np.min(mean_peak_series)),
                "line_peak_stddev": float(np.std(mean_peak_series)),
                "mean_line_recall": float(np.mean(recall_series)),
                "worst_phase_line_recall": float(np.min(recall_series)),
                "full_line_phase_fraction": float(
                    np.mean(np.asarray(recall_series) >= 0.999)
                ),
                "dropout_phase_fraction": float(
                    np.mean(np.asarray(recall_series) < 0.999)
                ),
                "max_successive_line_recall_delta": float(
                    np.max(np.abs(np.diff(recall_series)))
                    if len(recall_series) > 1
                    else 0.0
                ),
                "mean_centroid_error_pixels": float(np.mean(centroid_series)),
            }
        )
        _save_phase_strip(
            image_paths=image_paths,
            bbox=bbox,
            out_path=output_dir / "strips" / feature_id / f"{condition}.png",
            title=f"{feature_id} {condition}",
        )

    if peak_plot_values:
        _plot_target_metric(
            phase_indices=phase_indices,
            values_by_condition=peak_plot_values,
            ylabel="Mean Local Peak",
            title=f"{feature_id}: line peak across phases",
            out_path=output_dir / "plots" / f"{feature_id}_line_peak.png",
        )
        _plot_target_metric(
            phase_indices=phase_indices,
            values_by_condition=recall_plot_values,
            ylabel="Line Recall",
            title=f"{feature_id}: line recall across phases",
            out_path=output_dir / "plots" / f"{feature_id}_line_recall.png",
        )

    return summary_rows, phase_rows


def _analyze_pair_feature(
    *,
    feature: dict[str, Any],
    grouped: dict[str, list[dict[str, Any]]],
    camera: GenericCamera,
    width: int,
    height: int,
    detection_threshold: float,
    valley_ratio_threshold: float,
    profile_step_pixels: float,
    strip_padding: int,
    output_dir: Path,
) -> tuple[list[dict[str, Any]], list[dict[str, Any]]]:
    feature_id = str(feature["feature_id"])
    probe_points_scene_xyz = feature["probe_points_scene_xyz"]
    analysis = feature.get("analysis")
    if not isinstance(analysis, dict):
        raise ValueError(f"pair feature {feature_id} is missing analysis payload")
    line_a_center = analysis["line_a_center_scene_xyz"]
    line_b_center = analysis["line_b_center_scene_xyz"]
    pair_center = analysis["pair_center_scene_xyz"]
    tangent_scene_xyz = analysis["tangent_scene_xyz"]
    profile_half_length = float(analysis["profile_half_length_pixels"])
    profile_average_half_width = float(analysis["profile_average_half_width_pixels"])
    minimum_peak_separation = float(analysis["minimum_peak_separation_pixels"])

    summary_rows: list[dict[str, Any]] = []
    phase_rows: list[dict[str, Any]] = []
    two_peak_plot_values: dict[str, list[float]] = {}
    valley_plot_values: dict[str, list[float]] = {}
    phase_indices: list[int] = []

    for condition, records in grouped.items():
        image_paths = [Path(str(record["screenshot_path"])) for record in records]
        phase_translation_xyzs = [_phase_translation_xyz(record) for record in records]
        bbox = _target_bbox(
            camera=camera,
            probe_points_scene_xyz=probe_points_scene_xyz,
            translation_xyzs=phase_translation_xyzs,
            width=width,
            height=height,
            padding=strip_padding,
        )
        phase_metrics: list[dict[str, Any]] = []
        for record, translation_xyz in zip(records, phase_translation_xyzs):
            image = _intensity(_load_rgb(Path(str(record["screenshot_path"]))))
            translated_line_a = (
                float(line_a_center[0]) + float(translation_xyz[0]),
                float(line_a_center[1]) + float(translation_xyz[1]),
                float(line_a_center[2]) + float(translation_xyz[2]),
            )
            translated_line_b = (
                float(line_b_center[0]) + float(translation_xyz[0]),
                float(line_b_center[1]) + float(translation_xyz[1]),
                float(line_b_center[2]) + float(translation_xyz[2]),
            )
            translated_pair_center = (
                float(pair_center[0]) + float(translation_xyz[0]),
                float(pair_center[1]) + float(translation_xyz[1]),
                float(pair_center[2]) + float(translation_xyz[2]),
            )
            translated_tangent_tip = (
                float(pair_center[0])
                + float(tangent_scene_xyz[0])
                + float(translation_xyz[0]),
                float(pair_center[1])
                + float(tangent_scene_xyz[1])
                + float(translation_xyz[1]),
                float(pair_center[2])
                + float(tangent_scene_xyz[2])
                + float(translation_xyz[2]),
            )

            line_a_xy = _project_point(camera, translated_line_a, width, height)
            line_b_xy = _project_point(camera, translated_line_b, width, height)
            pair_center_xy = _project_point(
                camera, translated_pair_center, width, height
            )
            tangent_tip_xy = _project_point(
                camera, translated_tangent_tip, width, height
            )

            normal_xy = _normalize_2d(
                (line_b_xy[0] - line_a_xy[0], line_b_xy[1] - line_a_xy[1])
            )
            tangent_xy = _normalize_2d(
                (
                    tangent_tip_xy[0] - pair_center_xy[0],
                    tangent_tip_xy[1] - pair_center_xy[1],
                )
            )
            if normal_xy == (0.0, 0.0) and tangent_xy != (0.0, 0.0):
                normal_xy = (-tangent_xy[1], tangent_xy[0])
            if tangent_xy == (0.0, 0.0) and normal_xy != (0.0, 0.0):
                tangent_xy = (-normal_xy[1], normal_xy[0])

            profile_positions, profile_values = _extract_profile(
                image=image,
                center_xy=pair_center_xy,
                tangent_xy=tangent_xy,
                normal_xy=normal_xy,
                half_length_pixels=profile_half_length,
                average_half_width_pixels=profile_average_half_width,
                step_pixels=profile_step_pixels,
            )
            profile_metrics = _pair_profile_metrics(
                positions=profile_positions,
                profile=profile_values,
                threshold=detection_threshold,
                minimum_peak_separation_pixels=minimum_peak_separation,
                valley_ratio_threshold=valley_ratio_threshold,
            )
            row = {
                "feature_id": feature_id,
                "feature_kind": "line_pair",
                "condition": condition,
                "phase_index": int(record["phase_index"]),
                "phase_fraction": float(record["phase_fraction"]),
                "two_peak_detected": int(profile_metrics["two_peak_detected"]),
                "profile_max_value": float(profile_metrics["profile_max_value"]),
                "num_profile_peaks_above_threshold": int(
                    profile_metrics["num_profile_peaks_above_threshold"]
                ),
                "peak_separation_pixels": float(
                    profile_metrics["peak_separation_pixels"]
                ),
                "valley_ratio": float(profile_metrics["valley_ratio"]),
                "left_peak_value": float(profile_metrics["left_peak_value"]),
                "right_peak_value": float(profile_metrics["right_peak_value"]),
                "expected_center_separation_pixels": float(
                    math.hypot(line_b_xy[0] - line_a_xy[0], line_b_xy[1] - line_a_xy[1])
                ),
            }
            phase_rows.append(row)
            phase_metrics.append(row)

        phase_metrics.sort(key=lambda item: int(item["phase_index"]))
        phase_indices = [int(item["phase_index"]) for item in phase_metrics]
        two_peak_series = [float(item["two_peak_detected"]) for item in phase_metrics]
        valley_series = [float(item["valley_ratio"]) for item in phase_metrics]
        two_peak_plot_values[condition] = two_peak_series
        valley_plot_values[condition] = valley_series
        valid_separations = [
            float(item["peak_separation_pixels"])
            for item in phase_metrics
            if int(item["two_peak_detected"]) == 1
        ]
        summary_rows.append(
            {
                "feature_id": feature_id,
                "feature_kind": "line_pair",
                "condition": condition,
                "two_peak_phase_fraction": float(np.mean(two_peak_series)),
                "mean_peak_separation_pixels": float(np.mean(valid_separations))
                if valid_separations
                else 0.0,
                "min_detected_peak_separation_pixels": float(np.min(valid_separations))
                if valid_separations
                else 0.0,
                "mean_valley_ratio": float(np.mean(valley_series)),
                "worst_valley_ratio": float(np.max(valley_series)),
                "mean_profile_max_value": float(
                    np.mean(
                        [float(item["profile_max_value"]) for item in phase_metrics]
                    )
                ),
                "mean_expected_center_separation_pixels": float(
                    np.mean(
                        [
                            float(item["expected_center_separation_pixels"])
                            for item in phase_metrics
                        ]
                    )
                ),
            }
        )
        _save_phase_strip(
            image_paths=image_paths,
            bbox=bbox,
            out_path=output_dir / "strips" / feature_id / f"{condition}.png",
            title=f"{feature_id} {condition}",
        )

    if two_peak_plot_values:
        _plot_target_metric(
            phase_indices=phase_indices,
            values_by_condition=two_peak_plot_values,
            ylabel="Two-Peak Detection",
            title=f"{feature_id}: pair split across phases",
            out_path=output_dir / "plots" / f"{feature_id}_two_peak.png",
        )
        _plot_target_metric(
            phase_indices=phase_indices,
            values_by_condition=valley_plot_values,
            ylabel="Valley / Min Peak",
            title=f"{feature_id}: pair valley ratio across phases",
            out_path=output_dir / "plots" / f"{feature_id}_valley_ratio.png",
        )

    return summary_rows, phase_rows


def main() -> int:
    args = _parse_args()
    render_manifest = _load_json(args.render_manifest.resolve())
    output_dir = (
        args.output_dir.resolve()
        if args.output_dir is not None
        else args.render_manifest.resolve().parent / "analysis"
    )
    output_dir.mkdir(parents=True, exist_ok=True)

    camera = GenericCamera.from_json(render_manifest["camera"])
    phase_records = render_manifest.get("phase_records")
    if not isinstance(phase_records, list) or not phase_records:
        raise ValueError("render manifest must contain a non-empty phase_records array")
    features = render_manifest.get("features")
    if not isinstance(features, list) or not features:
        raise ValueError("render manifest must contain a non-empty features array")

    grouped: dict[str, list[dict[str, Any]]] = {}
    for record in phase_records:
        if isinstance(record, dict):
            grouped.setdefault(str(record["condition"]), []).append(record)
    for condition_records in grouped.values():
        condition_records.sort(key=_phase_sort_key)

    first_record = next(iter(grouped.values()))[0]
    first_image = _load_rgb(Path(str(first_record["screenshot_path"])))
    height, width = first_image.shape[:2]

    summary_rows: list[dict[str, Any]] = []
    phase_rows: list[dict[str, Any]] = []
    for feature in features:
        if not isinstance(feature, dict):
            continue
        feature_kind = str(feature.get("kind", ""))
        if feature_kind == "line_persistence":
            feature_summary_rows, feature_phase_rows = _analyze_line_feature(
                feature=feature,
                grouped=grouped,
                camera=camera,
                width=width,
                height=height,
                peak_radius=int(args.peak_radius),
                detection_threshold=float(args.detection_threshold),
                strip_padding=int(args.strip_padding),
                output_dir=output_dir,
            )
        elif feature_kind == "line_pair":
            feature_summary_rows, feature_phase_rows = _analyze_pair_feature(
                feature=feature,
                grouped=grouped,
                camera=camera,
                width=width,
                height=height,
                detection_threshold=float(args.detection_threshold),
                valley_ratio_threshold=float(args.pair_valley_ratio_threshold),
                profile_step_pixels=float(args.profile_step_pixels),
                strip_padding=int(args.strip_padding),
                output_dir=output_dir,
            )
        else:
            raise ValueError(f"unsupported phantom feature kind {feature_kind!r}")
        summary_rows.extend(feature_summary_rows)
        phase_rows.extend(feature_phase_rows)

    condition_summary_rows: list[dict[str, Any]] = []
    for condition, records in grouped.items():
        mean_frame_delta, max_frame_delta = _condition_frame_delta(records)
        line_rows = [
            row
            for row in summary_rows
            if row["condition"] == condition
            and row["feature_kind"] == "line_persistence"
        ]
        pair_rows = [
            row
            for row in summary_rows
            if row["condition"] == condition and row["feature_kind"] == "line_pair"
        ]
        condition_summary_rows.append(
            {
                "condition": condition,
                "mean_line_peak_value": float(
                    np.mean([row["mean_line_peak_value"] for row in line_rows])
                )
                if line_rows
                else 0.0,
                "mean_line_recall": float(
                    np.mean([row["mean_line_recall"] for row in line_rows])
                )
                if line_rows
                else 0.0,
                "mean_worst_phase_line_recall": float(
                    np.mean([row["worst_phase_line_recall"] for row in line_rows])
                )
                if line_rows
                else 0.0,
                "mean_full_line_phase_fraction": float(
                    np.mean([row["full_line_phase_fraction"] for row in line_rows])
                )
                if line_rows
                else 0.0,
                "mean_dropout_phase_fraction": float(
                    np.mean([row["dropout_phase_fraction"] for row in line_rows])
                )
                if line_rows
                else 0.0,
                "mean_line_peak_stddev": float(
                    np.mean([row["line_peak_stddev"] for row in line_rows])
                )
                if line_rows
                else 0.0,
                "mean_two_peak_phase_fraction": float(
                    np.mean([row["two_peak_phase_fraction"] for row in pair_rows])
                )
                if pair_rows
                else 0.0,
                "mean_pair_peak_separation_pixels": float(
                    np.mean([row["mean_peak_separation_pixels"] for row in pair_rows])
                )
                if pair_rows
                else 0.0,
                "mean_pair_valley_ratio": float(
                    np.mean([row["mean_valley_ratio"] for row in pair_rows])
                )
                if pair_rows
                else 0.0,
                "mean_frame_delta": mean_frame_delta,
                "max_frame_delta": max_frame_delta,
            }
        )

    summary_payload = {
        "render_manifest": str(args.render_manifest.resolve()),
        "peak_radius": int(args.peak_radius),
        "detection_threshold": float(args.detection_threshold),
        "pair_valley_ratio_threshold": float(args.pair_valley_ratio_threshold),
        "profile_step_pixels": float(args.profile_step_pixels),
        "rows": summary_rows,
        "condition_summary_rows": condition_summary_rows,
        "phase_rows": phase_rows,
    }
    (output_dir / "summary.json").write_text(
        json.dumps(summary_payload, indent=2) + "\n", encoding="utf-8"
    )

    with (output_dir / "summary.csv").open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(
            stream,
            fieldnames=[
                "feature_id",
                "feature_kind",
                "condition",
                "mean_line_peak_value",
                "min_phase_mean_peak_value",
                "line_peak_stddev",
                "mean_line_recall",
                "worst_phase_line_recall",
                "full_line_phase_fraction",
                "dropout_phase_fraction",
                "max_successive_line_recall_delta",
                "mean_centroid_error_pixels",
                "two_peak_phase_fraction",
                "mean_peak_separation_pixels",
                "min_detected_peak_separation_pixels",
                "mean_valley_ratio",
                "worst_valley_ratio",
                "mean_profile_max_value",
                "mean_expected_center_separation_pixels",
            ],
        )
        writer.writeheader()
        writer.writerows(summary_rows)

    lines = [
        "# MIP Phantom Phase Sweep",
        "",
        f"- Render manifest: `{args.render_manifest.resolve()}`",
        f"- Line probe peak radius: `{int(args.peak_radius)}`",
        f"- Detection threshold: `{float(args.detection_threshold)}`",
        f"- Pair valley-ratio threshold: `{float(args.pair_valley_ratio_threshold)}`",
        f"- Profile step: `{float(args.profile_step_pixels)}` pixels",
        "",
        "Metric definitions:",
        "- `mean_line_recall`: fraction of sampled line probes whose local peak reaches the detection threshold.",
        "- `worst_phase_line_recall`: minimum line recall across the phase sweep.",
        "- `full_line_phase_fraction`: fraction of phases where the full sampled line stays detected.",
        "- `dropout_phase_fraction`: fraction of phases where any sampled line segment drops out.",
        "- `two_peak_phase_fraction`: fraction of phases where the pair profile shows two distinct peaks.",
        "- `mean_pair_valley_ratio`: average valley/min-peak ratio of the pair profile; lower means cleaner separation.",
        "- `mean_frame_delta`: mean absolute image change between consecutive phases.",
        "",
        "## Condition Summary",
        "",
        "| Condition | Mean Line Peak | Mean Line Recall | Mean Worst-Phase Line Recall | Mean Full-Line Phase Fraction | Mean Dropout Phase Fraction | Mean Line Peak Stddev | Mean Two-Peak Phase Fraction | Mean Pair Separation (px) | Mean Pair Valley Ratio | Mean Frame Delta | Max Frame Delta |",
        "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |",
    ]
    for row in condition_summary_rows:
        lines.append(
            "| "
            f"{row['condition']} | "
            f"{row['mean_line_peak_value']:.3f} | "
            f"{row['mean_line_recall']:.3f} | "
            f"{row['mean_worst_phase_line_recall']:.3f} | "
            f"{row['mean_full_line_phase_fraction']:.3f} | "
            f"{row['mean_dropout_phase_fraction']:.3f} | "
            f"{row['mean_line_peak_stddev']:.3f} | "
            f"{row['mean_two_peak_phase_fraction']:.3f} | "
            f"{row['mean_pair_peak_separation_pixels']:.3f} | "
            f"{row['mean_pair_valley_ratio']:.3f} | "
            f"{row['mean_frame_delta']:.3f} | "
            f"{row['max_frame_delta']:.3f} |"
        )

    line_rows = [
        row for row in summary_rows if row["feature_kind"] == "line_persistence"
    ]
    pair_rows = [row for row in summary_rows if row["feature_kind"] == "line_pair"]
    if line_rows:
        lines.extend(
            [
                "",
                "## Line Persistence Summary",
                "",
                "| Feature | Condition | Mean Line Peak | Min Phase Mean Peak | Line Peak Stddev | Mean Line Recall | Worst-Phase Line Recall | Full-Line Phase Fraction | Dropout Phase Fraction | Max Successive Recall Delta | Mean Centroid Error (px) |",
                "| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |",
            ]
        )
        for row in line_rows:
            lines.append(
                "| "
                f"{row['feature_id']} | "
                f"{row['condition']} | "
                f"{row['mean_line_peak_value']:.3f} | "
                f"{row['min_phase_mean_peak_value']:.3f} | "
                f"{row['line_peak_stddev']:.3f} | "
                f"{row['mean_line_recall']:.3f} | "
                f"{row['worst_phase_line_recall']:.3f} | "
                f"{row['full_line_phase_fraction']:.3f} | "
                f"{row['dropout_phase_fraction']:.3f} | "
                f"{row['max_successive_line_recall_delta']:.3f} | "
                f"{row['mean_centroid_error_pixels']:.3f} |"
            )

    if pair_rows:
        lines.extend(
            [
                "",
                "## Pair Separation Summary",
                "",
                "| Feature | Condition | Two-Peak Phase Fraction | Mean Peak Separation (px) | Min Detected Separation (px) | Mean Valley Ratio | Worst Valley Ratio | Mean Profile Max | Mean Expected Center Separation (px) |",
                "| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |",
            ]
        )
        for row in pair_rows:
            lines.append(
                "| "
                f"{row['feature_id']} | "
                f"{row['condition']} | "
                f"{row['two_peak_phase_fraction']:.3f} | "
                f"{row['mean_peak_separation_pixels']:.3f} | "
                f"{row['min_detected_peak_separation_pixels']:.3f} | "
                f"{row['mean_valley_ratio']:.3f} | "
                f"{row['worst_valley_ratio']:.3f} | "
                f"{row['mean_profile_max_value']:.3f} | "
                f"{row['mean_expected_center_separation_pixels']:.3f} |"
            )
    (output_dir / "summary.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
