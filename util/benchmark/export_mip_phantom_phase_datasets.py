#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
import math
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any

import numpy as np
import zimg

from volume_benchmark_common import GenericCamera


REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_OUTPUT_ROOT = (
    REPO_ROOT
    / "large_test_image"
    / "fidelity_validation"
    / "mip_phantom_phase_validation_v2"
    / "datasets"
)
DEFAULT_LARGE_DEPTH = 384
DEFAULT_ROI_WIDTH = 1024
DEFAULT_ROI_HEIGHT = 1024
DEFAULT_ROI_DEPTH = 384
DEFAULT_SHALLOW_FEATURE_Z = 24.0
DEFAULT_DEEP_FEATURE_Z = 330.0
DEEP_LINE_Y0 = 220.0
DEEP_LINE_Y1 = 860.0
SHALLOW_LINE_Y0 = 220.0
SHALLOW_LINE_Y1 = 860.0


@dataclass(frozen=True)
class PhantomRoi:
    x0: int
    x1: int
    y0: int
    y1: int
    z0: int
    z1: int


@dataclass(frozen=True)
class PhantomFeature:
    feature_id: str
    kind: str
    description: str
    intensity: int
    params: dict[str, Any]
    probe_points_local_xyz: list[list[float]]


class SparsePhantomSliceProvider(zimg.ZImgSliceProvider):
    def __init__(
        self,
        *,
        width: int,
        height: int,
        depth: int,
        voxel_size_xyz: tuple[float, float, float],
        features: list[PhantomFeature],
    ) -> None:
        super().__init__()
        self._width = int(width)
        self._height = int(height)
        self._depth = int(depth)
        self._features = list(features)
        self._info = zimg.ZImgInfo(
            self._width,
            self._height,
            self._depth,
            1,
            1,
            1,
            zimg.VoxelFormat.Unsigned,
        )
        self._info.validBitCount = 8
        self._info.voxelSizeX = float(voxel_size_xyz[0])
        self._info.voxelSizeY = float(voxel_size_xyz[1])
        self._info.voxelSizeZ = float(voxel_size_xyz[2])
        self._info.voxelSizeUnit = zimg.VoxelSizeUnit.um

    def imgInfo(self) -> zimg.ZImgInfo:
        return self._info

    def slice(self, z: int, t: int) -> zimg.ZImg:
        if int(t) != 0:
            raise ValueError(f"phantom provider only supports t=0, got {t}")
        frame = np.zeros((1, 1, self._height, self._width), dtype=np.uint8)
        canvas = frame[0, 0]
        for feature in self._features:
            _draw_feature_on_slice(canvas, feature, int(z))
        return zimg.ZImg(frame, layout="CZYX", copy_if_needed=True)

    def allSlices(self, t: int) -> zimg.ZImg:
        if int(t) != 0:
            raise ValueError(f"phantom provider only supports t=0, got {t}")
        arr = np.zeros((1, self._depth, self._height, self._width), dtype=np.uint8)
        for z in range(self._depth):
            canvas = arr[0, z]
            for feature in self._features:
                _draw_feature_on_slice(canvas, feature, int(z))
        return zimg.ZImg(arr, layout="CZYX", copy_if_needed=True)

    def wholeImg(self) -> zimg.ZImg:
        return self.allSlices(0)


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Export an anisotropic synthetic MIP phantom for phase-stability validation. "
            "The output includes one large adaptive Atlas dataset plus resident "
            "level-0 / level-1 / level-2 ROI control datasets and a manifest "
            "describing thin-line persistence and pair-separation targets."
        )
    )
    parser.add_argument("--output-root", type=Path, default=DEFAULT_OUTPUT_ROOT)
    parser.add_argument("--large-width", type=int, default=4096)
    parser.add_argument("--large-height", type=int, default=4096)
    parser.add_argument("--large-depth", type=int, default=DEFAULT_LARGE_DEPTH)
    parser.add_argument("--roi-width", type=int, default=DEFAULT_ROI_WIDTH)
    parser.add_argument("--roi-height", type=int, default=DEFAULT_ROI_HEIGHT)
    parser.add_argument("--roi-depth", type=int, default=DEFAULT_ROI_DEPTH)
    parser.add_argument("--voxel-size-z", type=float, default=20.0)
    parser.add_argument(
        "--compression",
        default="AUTO",
        choices=("AUTO", "NONE"),
        help="Compression mode for the generated .nim datasets.",
    )
    parser.add_argument(
        "--camera-distance-scale",
        type=float,
        default=2.7,
        help=(
            "Recommended minification factor relative to the ROI fit camera. "
            "Values > 1.0 zoom out and make the target set more minified."
        ),
    )
    parser.add_argument(
        "--overwrite",
        action="store_true",
        help="Overwrite an existing non-empty output root.",
    )
    return parser.parse_args()


def _verify_direct_array_ctor() -> None:
    probe = np.zeros((1, 1, 2, 2), dtype=np.uint8)
    try:
        zimg.ZImg(probe, layout="CZYX", copy_if_needed=True)
    except AttributeError as exc:
        raise RuntimeError(
            "The active zimg wheel does not include the direct-array constructor fix. "
            "Rebuild/install the updated wheel from this repo before running the "
            "phantom exporter. The failing wrapper path is the same '_owners' issue "
            "seen in direct ZImg(arr) calls."
        ) from exc


def _coarse_dims(
    width: int, height: int, depth: int, level: int
) -> tuple[int, int, int]:
    factor = 2**level
    return (math.ceil(width / factor), math.ceil(height / factor), depth)


def _save_img(
    img: zimg.ZImg, path: Path, paras: zimg.ZImgWriteParameters
) -> zimg.ZImgInfo:
    if path.exists():
        path.unlink()
    path.parent.mkdir(parents=True, exist_ok=True)
    img.save(str(path), zimg.FileFormat.HDF5Img, paras)
    return zimg.ZImg.readImgInfos(str(path), zimg.FileFormat.HDF5Img)[0]


def _crop_region(roi: PhantomRoi) -> zimg.ZImgRegion:
    return zimg.ZImgRegion(
        (roi.x0, roi.y0, roi.z0, 0, 0), (roi.x1, roi.y1, roi.z1, 1, 1)
    )


def _line_probes(
    start: tuple[float, float, float], end: tuple[float, float, float], count: int
) -> list[list[float]]:
    if count <= 1:
        return [[float(start[0]), float(start[1]), float(start[2])]]
    out: list[list[float]] = []
    for idx in range(count):
        t = idx / float(count - 1)
        out.append(
            [
                float(start[0] + (end[0] - start[0]) * t),
                float(start[1] + (end[1] - start[1]) * t),
                float(start[2] + (end[2] - start[2]) * t),
            ]
        )
    return out


def _default_features_local() -> list[PhantomFeature]:
    deep_z = DEFAULT_DEEP_FEATURE_Z
    shallow_z = DEFAULT_SHALLOW_FEATURE_Z
    return [
        PhantomFeature(
            feature_id="deep_line_phase00",
            kind="line_persistence",
            description="Deep thin vertical line with a pixel-center phase. This deeper block is intentionally pushed into an L3-like regime so forced level 0 becomes strongly phase-sensitive.",
            intensity=255,
            params={
                "shape": "line_xy",
                "start_xyz": [220.00, DEEP_LINE_Y0, deep_z],
                "end_xyz": [220.00, DEEP_LINE_Y1, deep_z],
                "radius": 0.40,
                "analysis": {
                    "type": "line_recall",
                },
            },
            probe_points_local_xyz=_line_probes(
                (220.00, 280.0, deep_z), (220.00, 800.0, deep_z), 11
            ),
        ),
        PhantomFeature(
            feature_id="deep_line_phase25",
            kind="line_persistence",
            description="Deep thin vertical line offset by a quarter-pixel phase. Forced level 0 should vary more across the sweep than adaptive in this deeper minified regime.",
            intensity=255,
            params={
                "shape": "line_xy",
                "start_xyz": [280.25, DEEP_LINE_Y0, deep_z],
                "end_xyz": [280.25, DEEP_LINE_Y1, deep_z],
                "radius": 0.40,
                "analysis": {
                    "type": "line_recall",
                },
            },
            probe_points_local_xyz=_line_probes(
                (280.25, 280.0, deep_z), (280.25, 800.0, deep_z), 11
            ),
        ),
        PhantomFeature(
            feature_id="deep_line_phase625",
            kind="line_persistence",
            description="Deep thin vertical line offset near a half-pixel phase. This uses a binary-safe subpixel position so it stays in the source phantom while remaining highly phase-sensitive for forced level 0.",
            intensity=255,
            params={
                "shape": "line_xy",
                "start_xyz": [340.625, DEEP_LINE_Y0, deep_z],
                "end_xyz": [340.625, DEEP_LINE_Y1, deep_z],
                "radius": 0.40,
                "analysis": {
                    "type": "line_recall",
                },
            },
            probe_points_local_xyz=_line_probes(
                (340.625, 280.0, deep_z), (340.625, 800.0, deep_z), 11
            ),
        ),
        PhantomFeature(
            feature_id="deep_line_phase75",
            kind="line_persistence",
            description="Deep thin vertical line offset by a three-quarter-pixel phase. The deeper dropout block should make forced level 0 visibly inconsistent across these lines.",
            intensity=255,
            params={
                "shape": "line_xy",
                "start_xyz": [400.75, DEEP_LINE_Y0, deep_z],
                "end_xyz": [400.75, DEEP_LINE_Y1, deep_z],
                "radius": 0.40,
                "analysis": {
                    "type": "line_recall",
                },
            },
            probe_points_local_xyz=_line_probes(
                (400.75, 280.0, deep_z), (400.75, 800.0, deep_z), 11
            ),
        ),
        PhantomFeature(
            feature_id="shallow_triplet_left",
            kind="line_persistence",
            description="Shallow left vertical line in the front triplet. Adaptive and forced level 0 should keep all three front lines distinct.",
            intensity=255,
            params={
                "shape": "line_xy",
                "start_xyz": [660.0, SHALLOW_LINE_Y0, shallow_z],
                "end_xyz": [660.0, SHALLOW_LINE_Y1, shallow_z],
                "radius": 0.70,
                "analysis": {
                    "type": "line_recall",
                },
            },
            probe_points_local_xyz=_line_probes(
                (660.0, 280.0, shallow_z), (660.0, 800.0, shallow_z), 9
            ),
        ),
        PhantomFeature(
            feature_id="shallow_triplet_middle",
            kind="line_persistence",
            description="Shallow middle vertical line in the front triplet. The tighter left-middle gap is intended to expose a forced level-1 merge before level 2 collapses the wider triplet structure.",
            intensity=255,
            params={
                "shape": "line_xy",
                "start_xyz": [662.75, SHALLOW_LINE_Y0, shallow_z],
                "end_xyz": [662.75, SHALLOW_LINE_Y1, shallow_z],
                "radius": 0.70,
                "analysis": {
                    "type": "line_recall",
                },
            },
            probe_points_local_xyz=_line_probes(
                (662.75, 280.0, shallow_z), (662.75, 800.0, shallow_z), 9
            ),
        ),
        PhantomFeature(
            feature_id="shallow_triplet_right",
            kind="line_persistence",
            description="Shallow right vertical line in the front triplet. The wider middle-right gap should stay visible longer than the tighter pair so forced level 1 and level 2 fail differently.",
            intensity=255,
            params={
                "shape": "line_xy",
                "start_xyz": [667.25, SHALLOW_LINE_Y0, shallow_z],
                "end_xyz": [667.25, SHALLOW_LINE_Y1, shallow_z],
                "radius": 0.70,
                "analysis": {
                    "type": "line_recall",
                },
            },
            probe_points_local_xyz=_line_probes(
                (667.25, 280.0, shallow_z), (667.25, 800.0, shallow_z), 9
            ),
        ),
    ]


def _translate_feature(
    feature: PhantomFeature, dx: int, dy: int, dz: int
) -> PhantomFeature:
    params = json.loads(json.dumps(feature.params))

    def shift_xyz_triplet(value: list[float]) -> list[float]:
        return [float(value[0] + dx), float(value[1] + dy), float(value[2] + dz)]

    if "center_xyz" in params:
        params["center_xyz"] = shift_xyz_triplet(params["center_xyz"])
    if "start_xyz" in params:
        params["start_xyz"] = shift_xyz_triplet(params["start_xyz"])
    if "end_xyz" in params:
        params["end_xyz"] = shift_xyz_triplet(params["end_xyz"])
    if "center_xy" in params:
        params["center_xy"] = [
            float(params["center_xy"][0] + dx),
            float(params["center_xy"][1] + dy),
        ]
    if "z_range" in params:
        params["z_range"] = [
            int(params["z_range"][0] + dz),
            int(params["z_range"][1] + dz),
        ]
    if "x0" in params:
        params["x0"] = float(params["x0"] + dx)
    if "y0" in params:
        params["y0"] = float(params["y0"] + dy)
    if "z" in params:
        params["z"] = float(params["z"] + dz)

    probes = [
        [float(point[0] + dx), float(point[1] + dy), float(point[2] + dz)]
        for point in feature.probe_points_local_xyz
    ]
    return PhantomFeature(
        feature_id=feature.feature_id,
        kind=feature.kind,
        description=feature.description,
        intensity=feature.intensity,
        params=params,
        probe_points_local_xyz=probes,
    )


def _draw_feature_on_slice(canvas: np.ndarray, feature: PhantomFeature, z: int) -> None:
    shape = str(feature.params["shape"])
    if shape == "line_xy":
        sx, sy, sz = feature.params["start_xyz"]
        ex, ey, ez = feature.params["end_xyz"]
        if int(round(sz)) != int(z) or int(round(ez)) != int(z):
            return
        _draw_line_segment(
            canvas,
            float(sx),
            float(sy),
            float(ex),
            float(ey),
            float(feature.params["radius"]),
            int(feature.intensity),
        )
        return
    if shape == "line_pair_xy":
        sx, sy, sz = feature.params["start_xyz"]
        ex, ey, ez = feature.params["end_xyz"]
        if int(round(sz)) != int(z) or int(round(ez)) != int(z):
            return
        ox, oy = feature.params["offset_xy"]
        radius = float(feature.params["radius"])
        _draw_line_segment(
            canvas,
            float(sx),
            float(sy),
            float(ex),
            float(ey),
            radius,
            int(feature.intensity),
        )
        _draw_line_segment(
            canvas,
            float(sx) + float(ox),
            float(sy) + float(oy),
            float(ex) + float(ox),
            float(ey) + float(oy),
            radius,
            int(feature.intensity),
        )
        return
    raise ValueError(f"unsupported phantom shape {shape!r}")


def _draw_disk(
    canvas: np.ndarray, cx: float, cy: float, radius: float, intensity: int
) -> None:
    x0 = max(0, int(math.floor(cx - radius - 1.0)))
    x1 = min(canvas.shape[1], int(math.ceil(cx + radius + 2.0)))
    y0 = max(0, int(math.floor(cy - radius - 1.0)))
    y1 = min(canvas.shape[0], int(math.ceil(cy + radius + 2.0)))
    if x0 >= x1 or y0 >= y1:
        return
    yy, xx = np.ogrid[y0:y1, x0:x1]
    mask = (xx - cx) * (xx - cx) + (yy - cy) * (yy - cy) <= radius * radius
    patch = canvas[y0:y1, x0:x1]
    patch[mask] = np.maximum(patch[mask], intensity)


def _draw_line_segment(
    canvas: np.ndarray,
    x0: float,
    y0: float,
    x1: float,
    y1: float,
    radius: float,
    intensity: int,
) -> None:
    min_x = max(0, int(math.floor(min(x0, x1) - radius - 1.0)))
    max_x = min(canvas.shape[1], int(math.ceil(max(x0, x1) + radius + 2.0)))
    min_y = max(0, int(math.floor(min(y0, y1) - radius - 1.0)))
    max_y = min(canvas.shape[0], int(math.ceil(max(y0, y1) + radius + 2.0)))
    if min_x >= max_x or min_y >= max_y:
        return
    yy, xx = np.mgrid[min_y:max_y, min_x:max_x]
    vx = x1 - x0
    vy = y1 - y0
    denom = vx * vx + vy * vy
    if denom <= 1e-12:
        _draw_disk(canvas, x0, y0, radius, intensity)
        return
    t = ((xx - x0) * vx + (yy - y0) * vy) / denom
    t = np.clip(t, 0.0, 1.0)
    proj_x = x0 + t * vx
    proj_y = y0 + t * vy
    mask = (xx - proj_x) * (xx - proj_x) + (yy - proj_y) * (
        yy - proj_y
    ) <= radius * radius
    patch = canvas[min_y:max_y, min_x:max_x]
    patch[mask] = np.maximum(patch[mask], intensity)


def _voxel_aspect_scale(
    voxel_size_xyz: tuple[float, float, float],
) -> tuple[float, float, float]:
    vx, vy, vz = voxel_size_xyz
    xy = max(vx, vy)
    if not math.isfinite(xy) or xy <= 0.0 or not math.isfinite(vz) or vz <= 0.0:
        raise ValueError(
            f"Invalid voxel size for phantom scene scale: {voxel_size_xyz}"
        )
    return (1.0, 1.0, vz / xy)


def _scene_point(
    point_xyz: list[float], scene_scale_xyz: tuple[float, float, float]
) -> list[float]:
    return [
        float(point_xyz[0]) * float(scene_scale_xyz[0]),
        float(point_xyz[1]) * float(scene_scale_xyz[1]),
        float(point_xyz[2]) * float(scene_scale_xyz[2]),
    ]


def _project_feature_metadata(
    feature: PhantomFeature,
    roi: PhantomRoi,
    *,
    scene_scale_xyz: tuple[float, float, float],
) -> dict[str, Any]:
    payload = {
        "feature_id": feature.feature_id,
        "kind": feature.kind,
        "description": feature.description,
        "intensity": feature.intensity,
        "params_local": json.loads(json.dumps(feature.params)),
        "probe_points_local_xyz": json.loads(
            json.dumps(feature.probe_points_local_xyz)
        ),
        "probe_points_full_xyz": [
            [
                float(point[0] + roi.x0),
                float(point[1] + roi.y0),
                float(point[2] + roi.z0),
            ]
            for point in feature.probe_points_local_xyz
        ],
        "probe_points_scene_xyz": [
            _scene_point(
                [
                    float(point[0] + roi.x0),
                    float(point[1] + roi.y0),
                    float(point[2] + roi.z0),
                ],
                scene_scale_xyz,
            )
            for point in feature.probe_points_local_xyz
        ],
    }
    analysis = feature.params.get("analysis")
    if isinstance(analysis, dict) and str(analysis.get("type")) == "pair_separation":
        start_xyz = feature.params["start_xyz"]
        end_xyz = feature.params["end_xyz"]
        offset_xy = feature.params["offset_xy"]
        center_a = [
            float(start_xyz[0] + end_xyz[0]) * 0.5 + float(roi.x0),
            float(start_xyz[1] + end_xyz[1]) * 0.5 + float(roi.y0),
            float(start_xyz[2] + end_xyz[2]) * 0.5 + float(roi.z0),
        ]
        center_b = [
            float(center_a[0] + offset_xy[0]),
            float(center_a[1] + offset_xy[1]),
            float(center_a[2]),
        ]
        tangent = [
            float(end_xyz[0] - start_xyz[0]),
            float(end_xyz[1] - start_xyz[1]),
            0.0,
        ]
        payload["analysis"] = {
            "type": "pair_separation",
            "line_a_center_scene_xyz": _scene_point(center_a, scene_scale_xyz),
            "line_b_center_scene_xyz": _scene_point(center_b, scene_scale_xyz),
            "pair_center_scene_xyz": _scene_point(
                [
                    float(center_a[0] + center_b[0]) * 0.5,
                    float(center_a[1] + center_b[1]) * 0.5,
                    float(center_a[2]),
                ],
                scene_scale_xyz,
            ),
            "tangent_scene_xyz": _scene_point(tangent, scene_scale_xyz),
            "profile_half_length_pixels": float(analysis["profile_half_length_pixels"]),
            "profile_average_half_width_pixels": float(
                analysis["profile_average_half_width_pixels"]
            ),
            "minimum_peak_separation_pixels": float(
                analysis["minimum_peak_separation_pixels"]
            ),
        }
    elif isinstance(analysis, dict):
        payload["analysis"] = {"type": str(analysis.get("type", "line_recall"))}
    return payload


def _write_json(path: Path, payload: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")


def main() -> int:
    args = _parse_args()
    _verify_direct_array_ctor()

    output_root = args.output_root.resolve()
    if output_root.exists() and any(output_root.iterdir()) and not args.overwrite:
        raise FileExistsError(
            f"{output_root} already exists and is not empty. Pass --overwrite to reuse it."
        )
    output_root.mkdir(parents=True, exist_ok=True)

    large_width = int(args.large_width)
    large_height = int(args.large_height)
    large_depth = int(args.large_depth)
    roi_width = int(args.roi_width)
    roi_height = int(args.roi_height)
    roi_depth = int(args.roi_depth)
    voxel_size_xyz = (1.0, 1.0, float(args.voxel_size_z))
    scene_scale_xyz = _voxel_aspect_scale(voxel_size_xyz)
    if roi_width > large_width or roi_height > large_height or roi_depth > large_depth:
        raise ValueError("ROI dimensions must not exceed the large phantom dimensions")

    roi = PhantomRoi(
        x0=(large_width - roi_width) // 2,
        x1=(large_width - roi_width) // 2 + roi_width,
        y0=(large_height - roi_height) // 2,
        y1=(large_height - roi_height) // 2 + roi_height,
        z0=0,
        z1=roi_depth,
    )
    local_features = _default_features_local()
    full_features = [
        _translate_feature(feature, roi.x0, roi.y0, roi.z0)
        for feature in local_features
    ]
    camera_fit_bounds_xyz = {
        "x": [int(roi.x0 + 160), int(roi.x0 + 900)],
        "y": [int(roi.y0 + 180), int(roi.y0 + 900)],
        # Fit only the shallow front slab in Z so one camera can still span near and far footprint regimes.
        "z": [int(roi.z0 + 0), int(roi.z0 + 36)],
    }

    write_paras = zimg.ZImgWriteParameters()
    write_paras.compression = getattr(zimg.Compression, args.compression)

    adaptive_path = output_root / "adaptive" / "mip_phantom_large.nim"
    provider = SparsePhantomSliceProvider(
        width=large_width,
        height=large_height,
        depth=large_depth,
        voxel_size_xyz=voxel_size_xyz,
        features=full_features,
    )
    if adaptive_path.exists():
        adaptive_path.unlink()
    adaptive_path.parent.mkdir(parents=True, exist_ok=True)
    zimg.ZImg.writeImg(
        str(adaptive_path), provider, zimg.FileFormat.HDF5Img, write_paras
    )
    adaptive_info = zimg.ZImg.readImgInfos(str(adaptive_path), zimg.FileFormat.HDF5Img)[
        0
    ]

    roi_img = zimg.ZImg(
        str(adaptive_path), region=_crop_region(roi), format=zimg.FileFormat.HDF5Img
    )
    fullres_path = output_root / "roi" / "fullres.nim"
    fullres_info = _save_img(roi_img, fullres_path, write_paras)

    control_infos: dict[str, dict[str, Any]] = {}
    for level in (1, 2):
        width, height, depth = _coarse_dims(roi_width, roi_height, roi_depth, level)
        coarse_img = roi_img.resize(
            width,
            height,
            depth,
            zimg.Interpolant.Cubic,
            True,
            False,
            True,
        )
        coarse_path = output_root / "roi" / f"level{level}.nim"
        coarse_info = _save_img(coarse_img, coarse_path, write_paras)
        control_infos[f"level{level}"] = {
            "path": str(coarse_path),
            "shape_czyx": [
                int(coarse_info.numChannels),
                int(coarse_info.depth),
                int(coarse_info.height),
                int(coarse_info.width),
            ],
            "xy_downsample_factor": 2**level,
            "voxel_size_xyz": [
                float(coarse_info.voxelSizeX),
                float(coarse_info.voxelSizeY),
                float(coarse_info.voxelSizeZ),
            ],
        }

    center = (
        (roi.x0 + roi.x1) * 0.5,
        (roi.y0 + roi.y1) * 0.5,
        (roi.z0 + roi.z1) * 0.5 * scene_scale_xyz[2],
    )
    camera_template = GenericCamera(
        projection="Perspective",
        eye=(center[0], center[1], center[2] - 1.0),
        center=center,
        up=(0.0, 1.0, 0.0),
        field_of_view_degrees=45.0,
        eye_separation_angle_degrees=5.0,
    )
    camera_template_payload = camera_template.to_json()
    camera_template_payload.pop("field_of_view_degrees", None)

    manifest = {
        "adaptive_dataset": str(adaptive_path),
        "large_shape_xyz": [large_width, large_height, large_depth],
        "large_voxel_size_xyz": [
            float(voxel_size_xyz[0]),
            float(voxel_size_xyz[1]),
            float(voxel_size_xyz[2]),
        ],
        "scene_scale_xyz": [
            float(scene_scale_xyz[0]),
            float(scene_scale_xyz[1]),
            float(scene_scale_xyz[2]),
        ],
        "roi": {
            "label": "mip_phantom_roi01",
            "bounds_xyz": {
                "x": [roi.x0, roi.x1],
                "y": [roi.y0, roi.y1],
                "z": [roi.z0, roi.z1],
            },
            "shape_xyz": [roi_width, roi_height, roi_depth],
            "fullres_path": str(fullres_path),
            "fullres_shape_czyx": [
                int(fullres_info.numChannels),
                int(fullres_info.depth),
                int(fullres_info.height),
                int(fullres_info.width),
            ],
            "level1": control_infos["level1"],
            "level2": control_infos["level2"],
        },
        "camera_template": camera_template_payload,
        "camera_fit_margin": 1.0,
        "camera_distance_scale": float(args.camera_distance_scale),
        "camera_fit_bounds_xyz": camera_fit_bounds_xyz,
        "phase_sweep": {
            "axis": "x",
            "phase_count": 8,
            "reference_depth_scene_z": DEFAULT_DEEP_FEATURE_Z
            * float(scene_scale_xyz[2]),
            "fraction_step": 0.125,
        },
        "features": [
            _project_feature_metadata(feature, roi, scene_scale_xyz=scene_scale_xyz)
            for feature in local_features
        ],
        "datasets": {
            "adaptive": {
                "path": str(adaptive_path),
                "shape_czyx": [
                    int(adaptive_info.numChannels),
                    int(adaptive_info.depth),
                    int(adaptive_info.height),
                    int(adaptive_info.width),
                ],
                "voxel_size_xyz": [
                    float(adaptive_info.voxelSizeX),
                    float(adaptive_info.voxelSizeY),
                    float(adaptive_info.voxelSizeZ),
                ],
            },
            "forced_level0": {
                "path": str(fullres_path),
                "shape_czyx": [
                    int(fullres_info.numChannels),
                    int(fullres_info.depth),
                    int(fullres_info.height),
                    int(fullres_info.width),
                ],
                "xy_downsample_factor": 1,
            },
            "forced_level1": control_infos["level1"],
            "forced_level2": control_infos["level2"],
        },
    }
    _write_json(output_root / "manifest.json", manifest)

    readme = [
        "# MIP Phantom Phase Validation Datasets",
        "",
        "Generated artifacts:",
        f"- Adaptive large dataset: `{adaptive_path}`",
        f"- Resident level-0 ROI: `{fullres_path}`",
        f"- Resident level-1 ROI: `{control_infos['level1']['path']}`",
        f"- Resident level-2 ROI: `{control_infos['level2']['path']}`",
        "",
        "This synthetic dataset family is intended for the MIP minification / phase-stability experiment.",
        "The large adaptive dataset is sparse and file-backed so Atlas can exercise its normal full-resolution paging path,",
        "while the resident level-0 / level-1 / level-2 ROI datasets act as fixed-resolution controls.",
        "",
        f"- Large adaptive volume: `{large_width} x {large_height} x {large_depth}` voxels",
        f"- ROI volume: `{roi_width} x {roi_height} x {roi_depth}` voxels",
        f"- Voxel size: `{voxel_size_xyz[0]} x {voxel_size_xyz[1]} x {voxel_size_xyz[2]}`",
        "",
        "Synthetic targets:",
    ]
    for feature in manifest["features"]:
        readme.append(
            f"- `{feature['feature_id']}` ({feature['kind']}): {feature['description']}"
        )
    readme.extend(
        [
            "",
            "Recommended render setup:",
            f"- Fit the stored feature-view box using `camera_fit_bounds_xyz`, then apply `camera-distance-scale = {args.camera_distance_scale}`",
            "- Sweep subpixel X phases across one pixel period using the stored phase_sweep metadata",
            "- Compare `adaptive`, `forced_level0`, `forced_level1`, and `forced_level2` on final MIP screenshots",
            "- Primary visual tasks: deep thin-line dropout under forced L0 and shallow close-pair merging under forced L2",
        ]
    )
    (output_root / "README.md").write_text("\n".join(readme) + "\n", encoding="utf-8")

    print(f"Adaptive dataset: {adaptive_path}", flush=True)
    print(f"ROI fullres:      {fullres_path}", flush=True)
    print(f"Manifest:         {output_root / 'manifest.json'}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
