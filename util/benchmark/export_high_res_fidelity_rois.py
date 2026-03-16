#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
import math
from dataclasses import asdict, dataclass
from pathlib import Path

import zimg


DEFAULT_INPUT = Path(
    "/Users/feng/code/atlas/large_test_image/high_res_20220219_stitched_all_spacing_0p1_0p1_2_um.nim"
)
DEFAULT_OUTPUT_ROOT = Path(
    "/Users/feng/code/atlas/large_test_image/fidelity_validation/high_res_20220219_roi_validation_v1"
)
DEFAULT_CENTERS = [
    (16800, 4300),
    (13600, 7100),
    (23300, 8700),
    (4100, 10000),
]


@dataclass(frozen=True)
class RoiRequest:
    label: str
    center_x: int
    center_y: int
    x0: int
    x1: int
    y0: int
    y1: int
    z0: int
    z1: int


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Export memory-fit fidelity-validation ROIs from the high_res_20220219 "
            "dataset as standalone Atlas .nim volumes. For each ROI, the exporter writes "
            "a native-resolution dataset plus coarse L1/L2 control datasets."
        )
    )
    parser.add_argument(
        "--input",
        type=Path,
        default=DEFAULT_INPUT,
        help="Source .nim dataset to crop.",
    )
    parser.add_argument(
        "--output-root",
        type=Path,
        default=DEFAULT_OUTPUT_ROOT,
        help="Output root directory for the fidelity-validation ROI datasets.",
    )
    parser.add_argument(
        "--input-format",
        default="HDF5Img",
        choices=["HDF5Img"],
        help="Source file format hint for zimg.",
    )
    parser.add_argument(
        "--roi-width",
        type=int,
        default=2048,
        help="ROI width in voxels.",
    )
    parser.add_argument(
        "--roi-height",
        type=int,
        default=2048,
        help="ROI height in voxels.",
    )
    parser.add_argument(
        "--roi-depth",
        type=int,
        default=169,
        help="ROI depth in voxels.",
    )
    parser.add_argument(
        "--z-start",
        type=int,
        default=0,
        help="ROI Z start coordinate.",
    )
    parser.add_argument(
        "--coarse-level",
        action="append",
        type=int,
        default=None,
        help=(
            "Atlas-style coarse level to export as a standalone control dataset. "
            "Repeat to export multiple levels. Defaults to L1 and L2."
        ),
    )
    parser.add_argument(
        "--compression",
        default="AUTO",
        choices=["AUTO", "NONE"],
        help="Output NIM compression mode.",
    )
    parser.add_argument(
        "--center",
        action="append",
        default=None,
        help=(
            "Optional ROI center in x,y voxel coordinates. Repeat to override the default "
            "center list while preserving the roi01/roi02 ordering."
        ),
    )
    parser.add_argument(
        "--overwrite",
        action="store_true",
        help="Overwrite an existing output root.",
    )
    return parser.parse_args()


def _parse_centers(raw_centers: list[str] | None) -> list[tuple[int, int]]:
    if not raw_centers:
        return list(DEFAULT_CENTERS)
    centers: list[tuple[int, int]] = []
    for raw in raw_centers:
        parts = [part.strip() for part in str(raw).split(",")]
        if len(parts) != 2:
            raise ValueError(f"Center {raw!r} must use x,y format")
        centers.append((int(parts[0]), int(parts[1])))
    return centers


def _sanitize_label_token(value: int) -> str:
    return str(int(value))


def _default_roi_requests(
    *,
    centers: list[tuple[int, int]],
    source_width: int,
    source_height: int,
    source_depth: int,
    roi_width: int,
    roi_height: int,
    roi_depth: int,
    z_start: int,
) -> list[RoiRequest]:
    requests: list[RoiRequest] = []
    half_width = roi_width // 2
    half_height = roi_height // 2
    z0 = int(z_start)
    z1 = z0 + int(roi_depth)
    if z0 < 0 or z1 > source_depth:
        raise ValueError(
            f"Requested Z bounds [{z0}, {z1}) exceed source depth {source_depth}"
        )

    for index, (center_x, center_y) in enumerate(centers, start=1):
        x0 = int(center_x) - half_width
        x1 = x0 + roi_width
        y0 = int(center_y) - half_height
        y1 = y0 + roi_height
        if x0 < 0 or x1 > source_width:
            raise ValueError(
                f"ROI {index} X bounds [{x0}, {x1}) exceed source width {source_width}"
            )
        if y0 < 0 or y1 > source_height:
            raise ValueError(
                f"ROI {index} Y bounds [{y0}, {y1}) exceed source height {source_height}"
            )
        label = (
            f"roi{index:02d}_cx{_sanitize_label_token(center_x)}"
            f"_cy{_sanitize_label_token(center_y)}"
        )
        requests.append(
            RoiRequest(
                label=label,
                center_x=int(center_x),
                center_y=int(center_y),
                x0=x0,
                x1=x1,
                y0=y0,
                y1=y1,
                z0=z0,
                z1=z1,
            )
        )
    return requests


def _coarse_levels_from_args(args: argparse.Namespace) -> list[int]:
    raw_levels = args.coarse_level if args.coarse_level is not None else [1, 2]
    levels = sorted(set(int(level) for level in raw_levels))
    if not levels:
        raise ValueError("At least one coarse level must be selected")
    for level in levels:
        if level < 1:
            raise ValueError(f"Coarse level must be >= 1, got {level}")
    return levels


def _validate_source(info: zimg.ZImgInfo) -> None:
    if int(info.numTimes) != 1:
        raise ValueError(f"Expected a single timepoint, got {info.numTimes}")
    if int(info.numChannels) != 1:
        raise ValueError(f"Expected a single-channel dataset, got {info.numChannels}")
    if int(info.bytesPerVoxel) != 1 or info.voxelFormat != zimg.VoxelFormat.Unsigned:
        raise ValueError(
            "This exporter currently expects uint8 data; "
            f"got {info.voxelFormat} / {info.bytesPerVoxel} B per voxel"
        )


def _write_paras_from_args(args: argparse.Namespace) -> zimg.ZImgWriteParameters:
    paras = zimg.ZImgWriteParameters()
    paras.compression = getattr(zimg.Compression, args.compression)
    return paras


def _save_img(
    img: zimg.ZImg,
    path: Path,
    paras: zimg.ZImgWriteParameters,
) -> zimg.ZImgInfo:
    if path.exists():
        path.unlink()
    img.save(str(path), zimg.FileFormat.HDF5Img, paras)
    return zimg.ZImg.readImgInfos(str(path), zimg.FileFormat.HDF5Img)[0]


def _coarse_dims(info: zimg.ZImgInfo, level: int) -> tuple[int, int, int]:
    factor = 2**level
    width = math.ceil(int(info.width) / factor)
    height = math.ceil(int(info.height) / factor)
    depth = int(info.depth)
    return width, height, depth


def _roi_region(roi: RoiRequest) -> zimg.ZImgRegion:
    return zimg.ZImgRegion(
        (roi.x0, roi.y0, roi.z0, 0, 0), (roi.x1, roi.y1, roi.z1, 1, 1)
    )


def _metadata_for_saved_output(
    *,
    path: Path,
    saved_info: zimg.ZImgInfo,
    coarse_level: int,
    source_factor_xy: int,
) -> dict[str, object]:
    return {
        "path": str(path),
        "coarse_level": coarse_level,
        "xy_downsample_factor": source_factor_xy,
        "shape_czyx": [
            int(saved_info.numChannels),
            int(saved_info.depth),
            int(saved_info.height),
            int(saved_info.width),
        ],
        "voxel_size_xyz": [
            float(saved_info.voxelSizeX),
            float(saved_info.voxelSizeY),
            float(saved_info.voxelSizeZ),
        ],
        "voxel_size_unit": str(saved_info.voxelSizeUnit),
    }


def _write_readme(
    *,
    input_path: Path,
    output_root: Path,
    manifest_relpath: str,
    source_info: zimg.ZImgInfo,
    roi_requests: list[RoiRequest],
    coarse_levels: list[int],
) -> None:
    lines = [
        "# High-Res Fidelity Validation ROIs",
        "",
        "Generated from:",
        f"- Source dataset: `{input_path}`",
        f"- Source shape (CZYX): `{int(source_info.numChannels)} x {int(source_info.depth)} x {int(source_info.height)} x {int(source_info.width)}`",
        f"- Source voxel size: `{float(source_info.voxelSizeX)} x {float(source_info.voxelSizeY)} x {float(source_info.voxelSizeZ)} {source_info.voxelSizeUnit}`",
        "",
        "Each ROI directory contains:",
        "- `fullres.nim`: native-resolution ROI for the resident GPU reference path",
        "- `level1.nim`, `level2.nim`, ...: standalone coarse control datasets generated by resizing the native ROI while preserving physical extent",
        "- `metadata.json`: exact bounds, centers, and saved output metadata",
        "",
        f"Manifest: `{manifest_relpath}`",
        "",
        "ROI requests:",
    ]
    for roi in roi_requests:
        lines.append(
            "- "
            f"`{roi.label}`: center=({roi.center_x}, {roi.center_y}), "
            f"bounds x=[{roi.x0}, {roi.x1}), y=[{roi.y0}, {roi.y1}), z=[{roi.z0}, {roi.z1})"
        )
    lines.extend(
        [
            "",
            "Coarse control levels:",
            "- " + ", ".join(f"L{level}" for level in coarse_levels),
            "",
            "These artifacts are intended for the fidelity-validation workflow:",
            "1. Render `fullres.nim` with elevated sampling as the resident reference.",
            "2. Render the original large dataset adaptively with the matching camera.",
            "3. Render `level1.nim` and `level2.nim` as forced coarse controls.",
        ]
    )
    (output_root / "README.md").write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> None:
    args = _parse_args()

    input_path = args.input.resolve()
    output_root = args.output_root.resolve()
    source_format = getattr(zimg.FileFormat, args.input_format)
    coarse_levels = _coarse_levels_from_args(args)
    centers = _parse_centers(args.center)

    output_root.mkdir(parents=True, exist_ok=True)
    if any(output_root.iterdir()) and not args.overwrite:
        raise FileExistsError(
            f"{output_root} already exists and is not empty. Pass --overwrite to reuse it."
        )

    source_info = zimg.ZImg.readImgInfos(str(input_path), source_format)[0]
    _validate_source(source_info)

    roi_requests = _default_roi_requests(
        centers=centers,
        source_width=int(source_info.width),
        source_height=int(source_info.height),
        source_depth=int(source_info.depth),
        roi_width=int(args.roi_width),
        roi_height=int(args.roi_height),
        roi_depth=int(args.roi_depth),
        z_start=int(args.z_start),
    )

    paras = _write_paras_from_args(args)

    manifest: dict[str, object] = {
        "input": str(input_path),
        "input_format": str(source_format),
        "source_shape_czyx": [
            int(source_info.numChannels),
            int(source_info.depth),
            int(source_info.height),
            int(source_info.width),
        ],
        "source_voxel_size_xyz": [
            float(source_info.voxelSizeX),
            float(source_info.voxelSizeY),
            float(source_info.voxelSizeZ),
        ],
        "source_voxel_size_unit": str(source_info.voxelSizeUnit),
        "roi_shape_xyz": [
            int(args.roi_width),
            int(args.roi_height),
            int(args.roi_depth),
        ],
        "roi_centers_xy": [[int(cx), int(cy)] for cx, cy in centers],
        "coarse_levels": coarse_levels,
        "compression": args.compression,
        "rois": [],
    }

    for roi in roi_requests:
        roi_dir = output_root / roi.label
        roi_dir.mkdir(parents=True, exist_ok=True)

        print(f"Reading {roi.label} from {input_path.name} ...", flush=True)
        roi_img = zimg.ZImg(
            str(input_path), region=_roi_region(roi), format=source_format
        )
        roi_info = roi_img.info

        fullres_path = roi_dir / "fullres.nim"
        fullres_info = _save_img(roi_img, fullres_path, paras)
        print(
            f"  wrote fullres.nim shape={int(fullres_info.width)}x{int(fullres_info.height)}x{int(fullres_info.depth)}",
            flush=True,
        )

        outputs: list[dict[str, object]] = [
            _metadata_for_saved_output(
                path=fullres_path,
                saved_info=fullres_info,
                coarse_level=0,
                source_factor_xy=1,
            )
        ]

        for level in coarse_levels:
            coarse_width, coarse_height, coarse_depth = _coarse_dims(roi_info, level)
            coarse_img = roi_img.resize(
                coarse_width,
                coarse_height,
                coarse_depth,
                zimg.Interpolant.Cubic,
                True,
                False,
                True,
            )
            coarse_path = roi_dir / f"level{level}.nim"
            coarse_info = _save_img(coarse_img, coarse_path, paras)
            outputs.append(
                _metadata_for_saved_output(
                    path=coarse_path,
                    saved_info=coarse_info,
                    coarse_level=level,
                    source_factor_xy=2**level,
                )
            )
            print(
                f"  wrote level{level}.nim shape={int(coarse_info.width)}x{int(coarse_info.height)}x{int(coarse_info.depth)} "
                f"voxel={float(coarse_info.voxelSizeX)}x{float(coarse_info.voxelSizeY)}x{float(coarse_info.voxelSizeZ)}",
                flush=True,
            )

        roi_metadata = {
            "label": roi.label,
            "center_xy": [roi.center_x, roi.center_y],
            "bounds_xyz": {
                "x": [roi.x0, roi.x1],
                "y": [roi.y0, roi.y1],
                "z": [roi.z0, roi.z1],
            },
            "requested_shape_xyz": [
                int(args.roi_width),
                int(args.roi_height),
                int(args.roi_depth),
            ],
            "outputs": outputs,
        }
        (roi_dir / "metadata.json").write_text(
            json.dumps(roi_metadata, indent=2) + "\n", encoding="utf-8"
        )
        manifest["rois"].append(roi_metadata)

    manifest_path = output_root / "manifest.json"
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")

    _write_readme(
        input_path=input_path,
        output_root=output_root,
        manifest_relpath=manifest_path.name,
        source_info=source_info,
        roi_requests=roi_requests,
        coarse_levels=coarse_levels,
    )

    print(f"Finished writing ROI datasets under {output_root}", flush=True)
    print(f"Manifest: {manifest_path}", flush=True)


if __name__ == "__main__":
    main()
