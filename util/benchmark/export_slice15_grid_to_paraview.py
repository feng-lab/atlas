#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable

import numpy as np
import vtk
from vtk.util.numpy_support import numpy_to_vtk

import zimg


GRID_LAYOUT = [
    [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0],
    [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0],
    [0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 0, 0],
    [0, 0, 0, 7, 8, 9, 10, 11, 12, 13, 14, 0],
    [0, 0, 0, 15, 16, 17, 18, 19, 20, 21, 22, 0],
    [0, 0, 0, 23, 24, 25, 26, 27, 28, 0, 0, 0],
    [0, 0, 0, 29, 30, 31, 32, 33, 34, 0, 0, 0],
    [0, 0, 35, 36, 37, 38, 39, 40, 41, 0, 0, 0],
    [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0],
    [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0],
]

DEFAULT_INPUT_DIR = HOME / "Dropbox" / "atlas_test" / "slice15"
DEFAULT_OUTPUT_DIR = HOME / "Dropbox" / "atlas_test" / "slice15_paraview"
DEFAULT_PATTERN = "slice15_L{tile_id}_Sum.lsm"


@dataclass(frozen=True)
class TilePlacement:
    tile_id: int
    grid_row: int
    grid_col: int
    path: Path


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Export the sparse 41-tile slice15 Zeiss LSM grid into ParaView and Atlas formats. "
            "Outputs: blocked .vtpd, dense .mhd/.raw, and dense .nim."
        )
    )
    parser.add_argument("--input-dir", type=Path, default=DEFAULT_INPUT_DIR)
    parser.add_argument("--output-dir", type=Path, default=DEFAULT_OUTPUT_DIR)
    parser.add_argument("--tile-pattern", default=DEFAULT_PATTERN)
    parser.add_argument(
        "--channel-number",
        type=int,
        default=None,
        help=(
            "Optional 1-based channel number to export as a single-channel dataset. "
            "If omitted, export all channels."
        ),
    )
    parser.add_argument(
        "--output-prefix",
        default=None,
        help="Optional filename prefix override. Defaults to slice15_rgb or slice15_chN.",
    )
    return parser.parse_args()


def crop_layout(layout: list[list[int]]) -> tuple[list[list[int]], int, int]:
    nonzero_rows = [
        idx for idx, row in enumerate(layout) if any(tile_id != 0 for tile_id in row)
    ]
    nonzero_cols = [
        idx for idx in range(len(layout[0])) if any(row[idx] != 0 for row in layout)
    ]
    if not nonzero_rows or not nonzero_cols:
        raise ValueError("Grid layout does not contain any tiles")

    row_start = nonzero_rows[0]
    row_end = nonzero_rows[-1] + 1
    col_start = nonzero_cols[0]
    col_end = nonzero_cols[-1] + 1
    cropped = [row[col_start:col_end] for row in layout[row_start:row_end]]
    return cropped, row_start, col_start


def iter_layout(layout: list[list[int]]) -> Iterable[tuple[int, int, int]]:
    for row, row_values in enumerate(layout):
        for col, tile_id in enumerate(row_values):
            yield row, col, tile_id


def build_tile_placements(
    layout: list[list[int]], input_dir: Path, tile_pattern: str
) -> list[TilePlacement]:
    placements: list[TilePlacement] = []
    seen: set[int] = set()
    for row, col, tile_id in iter_layout(layout):
        if tile_id == 0:
            continue
        path = input_dir / tile_pattern.format(tile_id=tile_id)
        if not path.exists():
            raise FileNotFoundError(f"Missing tile for grid entry {tile_id}: {path}")
        if tile_id in seen:
            raise ValueError(f"Duplicate tile id in grid layout: {tile_id}")
        seen.add(tile_id)
        placements.append(
            TilePlacement(tile_id=tile_id, grid_row=row, grid_col=col, path=path)
        )
    expected_ids = set(range(1, 42))
    if seen != expected_ids:
        raise ValueError(
            f"Grid layout must contain tiles 1..41 exactly once, got {sorted(seen)}"
        )
    return placements


def read_first_info(path: Path) -> zimg.ZImgInfo:
    infos = zimg.ZImg.readImgInfos(str(path), zimg.FileFormat.ZeissLsm)
    if len(infos) != 1:
        raise ValueError(f"Expected exactly one scene in {path}, got {len(infos)}")
    return infos[0]


def validate_dataset(
    first_info: zimg.ZImgInfo, placements: list[TilePlacement]
) -> None:
    if first_info.numTimes != 1:
        raise ValueError(f"Expected a single timepoint, got {first_info.numTimes}")
    if first_info.numChannels != 3:
        raise ValueError(f"Expected exactly 3 channels, got {first_info.numChannels}")
    if (
        first_info.bytesPerVoxel != 1
        or first_info.voxelFormat != zimg.VoxelFormat.Unsigned
    ):
        raise ValueError(
            "This exporter currently expects uint8 Zeiss tiles; "
            f"got {first_info.voxelFormat} / {first_info.bytesPerVoxel} B per voxel"
        )
    for placement in placements:
        info = read_first_info(placement.path)
        comparable = (
            info.width,
            info.height,
            info.depth,
            info.numChannels,
            info.numTimes,
            info.bytesPerVoxel,
            int(info.voxelFormat),
        )
        first_comparable = (
            first_info.width,
            first_info.height,
            first_info.depth,
            first_info.numChannels,
            first_info.numTimes,
            first_info.bytesPerVoxel,
            int(first_info.voxelFormat),
        )
        if comparable != first_comparable:
            raise ValueError(
                f"Tile metadata mismatch in {placement.path}: {info} != reference {first_info}"
            )


def read_tile_czyx(path: Path) -> np.ndarray:
    img = zimg.ZImg(str(path), format=zimg.FileFormat.ZeissLsm)
    arrays = img.to_arrays("numpy")
    if len(arrays) != 1:
        raise ValueError(f"Expected one timepoint in {path}, got {len(arrays)}")
    arr = np.asarray(arrays[0])
    if arr.ndim != 4:
        raise ValueError(f"Expected CZYX array from {path}, got shape {arr.shape}")
    return arr


def czyx_to_zyxc(arr_czyx: np.ndarray) -> np.ndarray:
    return np.ascontiguousarray(np.moveaxis(arr_czyx, 0, -1))


def select_channel_czyx(arr_czyx: np.ndarray, channel_index: int | None) -> np.ndarray:
    if channel_index is None:
        return np.ascontiguousarray(arr_czyx)
    return np.ascontiguousarray(arr_czyx[channel_index : channel_index + 1, :, :, :])


def make_vtk_image(
    tile_zyxc: np.ndarray,
    origin_xyz: tuple[float, float, float],
    spacing_xyz: tuple[float, float, float],
) -> vtk.vtkImageData:
    depth, height, width, channels = tile_zyxc.shape
    flat = tile_zyxc.reshape(-1) if channels == 1 else tile_zyxc.reshape(-1, channels)
    vtk_array = numpy_to_vtk(flat, deep=False, array_type=vtk.VTK_UNSIGNED_CHAR)
    vtk_array.SetName("channels")
    image = vtk.vtkImageData()
    image.SetDimensions(width, height, depth)
    image.SetOrigin(*origin_xyz)
    image.SetSpacing(*spacing_xyz)
    image.GetPointData().SetScalars(vtk_array)
    image.GetPointData().SetActiveScalars("channels")
    vtk_array._numpy_ref = tile_zyxc
    return image


def write_vti_piece(
    tile_zyxc: np.ndarray,
    output_path: Path,
    origin_xyz: tuple[float, float, float],
    spacing_xyz: tuple[float, float, float],
) -> None:
    image = make_vtk_image(tile_zyxc, origin_xyz, spacing_xyz)
    writer = vtk.vtkXMLImageDataWriter()
    writer.SetFileName(str(output_path))
    writer.SetInputData(image)
    writer.SetDataModeToAppended()
    writer.EncodeAppendedDataOff()
    writer.SetCompressorTypeToNone()
    ok = writer.Write()
    if ok != 1:
        raise RuntimeError(f"Failed to write {output_path}")


def write_vtpd_manifest(manifest_path: Path, piece_relpaths: list[str]) -> None:
    lines = [
        '<VTKFile type="vtkPartitionedDataSet" version="1.0" byte_order="LittleEndian" header_type="UInt32">',
        "  <vtkPartitionedDataSet>",
    ]
    for index, relpath in enumerate(piece_relpaths):
        lines.append(f'    <DataSet index="{index}" file="{relpath}"/>')
    lines.extend(["  </vtkPartitionedDataSet>", "</VTKFile>"])
    manifest_path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def write_metaimage_header(
    header_path: Path,
    raw_filename: str,
    width: int,
    height: int,
    depth: int,
    num_channels: int,
    spacing_xyz: tuple[float, float, float],
) -> None:
    header_lines = [
        "ObjectType = Image",
        "NDims = 3",
        "BinaryData = True",
        "BinaryDataByteOrderMSB = False",
        "CompressedData = False",
        "TransformMatrix = 1 0 0 0 1 0 0 0 1",
        "Offset = 0 0 0",
        "CenterOfRotation = 0 0 0",
        f"ElementSpacing = {spacing_xyz[0]} {spacing_xyz[1]} {spacing_xyz[2]}",
        f"DimSize = {width} {height} {depth}",
        "AnatomicalOrientation = ???",
    ]
    if num_channels > 1:
        header_lines.append(f"ElementNumberOfChannels = {num_channels}")
        header_lines.append("ElementType = MET_UCHAR_ARRAY")
    else:
        header_lines.append("ElementType = MET_UCHAR")
    header_lines.append(f"ElementDataFile = {raw_filename}")
    header = "\n".join(header_lines)
    header_path.write_text(header + "\n", encoding="utf-8")


class Slice15BlockProvider(zimg.ZImgBlockProvider):
    def __init__(
        self,
        placements: list[TilePlacement],
        tile_width: int,
        tile_height: int,
        tile_depth: int,
        num_channels: int,
        grid_rows: int,
        grid_cols: int,
        reference_info: zimg.ZImgInfo,
        channel_index: int | None,
    ) -> None:
        super().__init__()
        self._channel_index = channel_index
        self._placements = placements
        self._info = zimg.ZImgInfo(
            tile_width * grid_cols,
            tile_height * grid_rows,
            tile_depth,
            num_channels,
            1,
            reference_info.bytesPerVoxel,
            reference_info.voxelFormat,
        )
        self._info.voxelSizeUnit = reference_info.voxelSizeUnit
        self._info.voxelSizeX = reference_info.voxelSizeX
        self._info.voxelSizeY = reference_info.voxelSizeY
        self._info.voxelSizeZ = reference_info.voxelSizeZ
        self._info.validBitCount = reference_info.validBitCount
        if channel_index is None:
            self._info.channelNames = list(reference_info.channelNames)
            self._info.channelColors = list(reference_info.channelColors)
        else:
            self._info.channelNames = [reference_info.channelNames[channel_index]]
            self._info.channelColors = [reference_info.channelColors[channel_index]]
        self._tile_width = tile_width
        self._tile_height = tile_height

    def imgInfo(self) -> zimg.ZImgInfo:
        return self._info

    def numBlocks(self) -> int:
        return len(self._placements)

    def blockCoord(self, block_idx: int) -> zimg.ZVoxelCoordinate:
        placement = self._placements[block_idx]
        return zimg.ZVoxelCoordinate(
            placement.grid_col * self._tile_width,
            placement.grid_row * self._tile_height,
            0,
            0,
            0,
        )

    def block(self, block_idx: int) -> zimg.ZImg:
        placement = self._placements[block_idx]
        if self._channel_index is None:
            return zimg.ZImg(str(placement.path), format=zimg.FileFormat.ZeissLsm)
        tile_czyx = select_channel_czyx(
            read_tile_czyx(placement.path), self._channel_index
        )
        return zimg.ZImg(tile_czyx, layout="CZYX")


def cleanup_previous_outputs(output_dir: Path, output_prefix: str) -> None:
    targets = [
        output_dir / f"{output_prefix}_dense.mhd",
        output_dir / f"{output_prefix}_dense.raw",
        output_dir / f"{output_prefix}_dense.nim",
        output_dir / f"{output_prefix}_grid.vtpd",
        output_dir / f"{output_prefix}_export_metadata.json",
    ]
    for target in targets:
        if target.exists():
            target.unlink()

    blocked_piece_dir = output_dir / f"{output_prefix}_grid"
    if blocked_piece_dir.exists():
        for child in blocked_piece_dir.iterdir():
            if child.is_file():
                child.unlink()
        blocked_piece_dir.rmdir()


def main() -> None:
    args = parse_args()
    output_dir = args.output_dir
    output_dir.mkdir(parents=True, exist_ok=True)

    cropped_layout, row_offset, col_offset = crop_layout(GRID_LAYOUT)
    placements = build_tile_placements(
        cropped_layout, args.input_dir, args.tile_pattern
    )
    reference_info = read_first_info(placements[0].path)
    validate_dataset(reference_info, placements)
    channel_index = None
    if args.channel_number is not None:
        if args.channel_number < 1 or args.channel_number > int(
            reference_info.numChannels
        ):
            raise ValueError(
                f"--channel-number must be in [1, {reference_info.numChannels}], got {args.channel_number}"
            )
        channel_index = args.channel_number - 1

    output_prefix = args.output_prefix
    if output_prefix is None:
        output_prefix = (
            f"slice15_ch{args.channel_number}"
            if args.channel_number is not None
            else "slice15_rgb"
        )
    cleanup_previous_outputs(output_dir, output_prefix)

    tile_width = int(reference_info.width)
    tile_height = int(reference_info.height)
    tile_depth = int(reference_info.depth)
    num_channels = 1 if channel_index is not None else int(reference_info.numChannels)
    grid_rows = len(cropped_layout)
    grid_cols = len(cropped_layout[0])
    whole_width = tile_width * grid_cols
    whole_height = tile_height * grid_rows
    spacing_xyz = (
        float(reference_info.voxelSizeX),
        float(reference_info.voxelSizeY),
        float(reference_info.voxelSizeZ),
    )

    dense_mhd_path = output_dir / f"{output_prefix}_dense.mhd"
    dense_raw_path = output_dir / f"{output_prefix}_dense.raw"
    dense_nim_path = output_dir / f"{output_prefix}_dense.nim"
    blocked_manifest_path = output_dir / f"{output_prefix}_grid.vtpd"
    blocked_piece_dir = output_dir / f"{output_prefix}_grid"
    blocked_piece_dir.mkdir(parents=True, exist_ok=True)

    write_metaimage_header(
        dense_mhd_path,
        dense_raw_path.name,
        whole_width,
        whole_height,
        tile_depth,
        num_channels,
        spacing_xyz,
    )

    dense_memmap = np.memmap(
        dense_raw_path,
        dtype=np.uint8,
        mode="w+",
        shape=(tile_depth, whole_height, whole_width, num_channels),
    )
    dense_memmap[:] = 0

    tile_by_id = {placement.tile_id: placement for placement in placements}
    zero_tile_zyxc = np.zeros(
        (tile_depth, tile_height, tile_width, num_channels), dtype=np.uint8
    )

    piece_relpaths: list[str] = []
    total_cells = grid_rows * grid_cols
    cell_index = 0
    for row, col, tile_id in iter_layout(cropped_layout):
        piece_name = f"{output_prefix}_grid_{cell_index:03d}.vti"
        piece_path = blocked_piece_dir / piece_name
        piece_relpaths.append(f"{blocked_piece_dir.name}/{piece_name}")
        origin_xyz = (
            col * tile_width * spacing_xyz[0],
            row * tile_height * spacing_xyz[1],
            0.0,
        )

        if tile_id == 0:
            tile_zyxc = zero_tile_zyxc
        else:
            placement = tile_by_id[tile_id]
            tile_czyx = select_channel_czyx(
                read_tile_czyx(placement.path), channel_index
            )
            tile_zyxc = czyx_to_zyxc(tile_czyx)
        y0 = row * tile_height
        y1 = y0 + tile_height
        x0 = col * tile_width
        x1 = x0 + tile_width
        dense_memmap[:, y0:y1, x0:x1, :] = tile_zyxc

        write_vti_piece(tile_zyxc, piece_path, origin_xyz, spacing_xyz)
        cell_index += 1
        print(
            f"[{cell_index:03d}/{total_cells:03d}] wrote {piece_path.name} (tile {tile_id})",
            flush=True,
        )

    dense_memmap.flush()
    del dense_memmap

    write_vtpd_manifest(blocked_manifest_path, piece_relpaths)

    nim_paras = zimg.ZImgWriteParameters()
    nim_paras.compression = zimg.Compression.AUTO
    if channel_index is None:
        nim_provider = Slice15BlockProvider(
            placements=placements,
            tile_width=tile_width,
            tile_height=tile_height,
            tile_depth=tile_depth,
            num_channels=num_channels,
            grid_rows=grid_rows,
            grid_cols=grid_cols,
            reference_info=reference_info,
            channel_index=channel_index,
        )
        zimg.ZImg.writeImg(
            str(dense_nim_path),
            nim_provider,
            zimg.FileFormat.HDF5Img,
            nim_paras,
        )
    else:
        # The current zimg Python bindings in pt12 fail when wrapping a NumPy
        # array into ZImg directly for block-provider returns. The dense
        # MetaImage path is already on disk here, so reuse that as the source
        # for the compressed .nim export.
        dense_img = zimg.ZImg(str(dense_mhd_path), format=zimg.FileFormat.MetaImage)
        dense_img.save(
            str(dense_nim_path), format=zimg.FileFormat.HDF5Img, paras=nim_paras
        )

    metadata = {
        "input_dir": str(args.input_dir),
        "output_dir": str(output_dir),
        "grid_rows": grid_rows,
        "grid_cols": grid_cols,
        "trimmed_from_original_row_offset": row_offset,
        "trimmed_from_original_col_offset": col_offset,
        "tile_shape_czyx": [
            int(reference_info.numChannels),
            int(reference_info.depth),
            int(reference_info.height),
            int(reference_info.width),
        ],
        "whole_shape_zyxc": [tile_depth, whole_height, whole_width, num_channels],
        "voxel_size_unit": str(reference_info.voxelSizeUnit),
        "voxel_size_xyz": list(spacing_xyz),
        "channel_names": (
            [reference_info.channelNames[channel_index]]
            if channel_index is not None
            else list(reference_info.channelNames)
        ),
        "selected_channel_number": args.channel_number,
        "outputs": {
            "blocked_vtpd": str(blocked_manifest_path),
            "dense_metaimage": str(dense_mhd_path),
            "dense_raw": str(dense_raw_path),
            "dense_nim": str(dense_nim_path),
        },
        "original_layout": GRID_LAYOUT,
        "cropped_layout": cropped_layout,
    }
    metadata_path = output_dir / f"{output_prefix}_export_metadata.json"
    metadata_path.write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")

    print("Finished export.", flush=True)
    print(f"Blocked ParaView dataset: {blocked_manifest_path}", flush=True)
    print(f"Dense ParaView dataset:   {dense_mhd_path}", flush=True)
    print(f"Dense Atlas dataset:      {dense_nim_path}", flush=True)
    print(f"Metadata:                 {metadata_path}", flush=True)


if __name__ == "__main__":
    main()
HOME = Path.home()
