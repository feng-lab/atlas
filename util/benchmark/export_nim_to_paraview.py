#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
import math
import shutil
from dataclasses import dataclass
from pathlib import Path

import numpy as np
import vtk
from vtk.util.numpy_support import numpy_to_vtk

import zimg


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Export a source .nim volume into a corrected-spacing .nim for Atlas "
            "and a blockwise .vtpd dataset for ParaView."
        )
    )
    parser.add_argument("--input", type=Path, required=True, help="Source .nim path")
    parser.add_argument(
        "--output-dir", type=Path, required=True, help="Output directory"
    )
    parser.add_argument(
        "--output-prefix",
        default=None,
        help="Output filename prefix. Defaults to the input stem plus a spacing suffix.",
    )
    parser.add_argument(
        "--voxel-size-x", type=float, default=0.1, help="Output voxel size X"
    )
    parser.add_argument(
        "--voxel-size-y", type=float, default=0.1, help="Output voxel size Y"
    )
    parser.add_argument(
        "--voxel-size-z", type=float, default=2.0, help="Output voxel size Z"
    )
    parser.add_argument(
        "--voxel-unit",
        default="um",
        choices=["none", "nm", "um", "mm", "cm", "hm", "m", "km", "inch"],
        help="Output voxel size unit for the corrected .nim",
    )
    parser.add_argument(
        "--block-size-x", type=int, default=1024, help="VTK block size X"
    )
    parser.add_argument(
        "--block-size-y", type=int, default=1024, help="VTK block size Y"
    )
    parser.add_argument(
        "--block-size-z", type=int, default=169, help="VTK block size Z"
    )
    parser.add_argument(
        "--source-format",
        default="HDF5Img",
        choices=["HDF5Img"],
        help="Source file format hint for zimg",
    )
    return parser.parse_args()


def _sanitize_float_token(value: float) -> str:
    token = f"{value:g}"
    return token.replace("-", "m").replace(".", "p")


def _default_prefix(
    input_path: Path, spacing_xyz: tuple[float, float, float], voxel_unit: str
) -> str:
    sx, sy, sz = (_sanitize_float_token(v) for v in spacing_xyz)
    return f"{input_path.stem}_spacing_{sx}_{sy}_{sz}_{voxel_unit}"


def _cleanup_previous_outputs(output_dir: Path, output_prefix: str) -> None:
    targets = [
        output_dir / f"{output_prefix}.nim",
        output_dir / f"{output_prefix}.vtpd",
        output_dir / f"{output_prefix}_export_metadata.json",
    ]
    for target in targets:
        if target.exists():
            target.unlink()

    piece_dir = output_dir / f"{output_prefix}_blocks"
    if piece_dir.exists():
        shutil.rmtree(piece_dir)


def _source_info(input_path: Path, source_format: zimg.FileFormat) -> zimg.ZImgInfo:
    infos = zimg.ZImg.readImgInfos(str(input_path), source_format)
    if len(infos) != 1:
        raise ValueError(
            f"Expected exactly one scene in {input_path}, got {len(infos)}"
        )
    return infos[0]


def _validate_source(info: zimg.ZImgInfo) -> None:
    if int(info.numTimes) != 1:
        raise ValueError(f"Expected a single timepoint, got {info.numTimes}")
    if int(info.bytesPerVoxel) != 1 or info.voxelFormat != zimg.VoxelFormat.Unsigned:
        raise ValueError(
            "This exporter currently expects uint8 data; "
            f"got {info.voxelFormat} / {info.bytesPerVoxel} B per voxel"
        )


@dataclass(frozen=True)
class BlockBounds:
    index: int
    x0: int
    x1: int
    y0: int
    y1: int
    z0: int
    z1: int

    @property
    def width(self) -> int:
        return self.x1 - self.x0

    @property
    def height(self) -> int:
        return self.y1 - self.y0

    @property
    def depth(self) -> int:
        return self.z1 - self.z0


def _enumerate_blocks(
    width: int, height: int, depth: int, block_x: int, block_y: int, block_z: int
) -> list[BlockBounds]:
    blocks: list[BlockBounds] = []
    index = 0
    for z0 in range(0, depth, block_z):
        z1 = min(z0 + block_z, depth)
        for y0 in range(0, height, block_y):
            y1 = min(y0 + block_y, height)
            for x0 in range(0, width, block_x):
                x1 = min(x0 + block_x, width)
                blocks.append(
                    BlockBounds(index=index, x0=x0, x1=x1, y0=y0, y1=y1, z0=z0, z1=z1)
                )
                index += 1
    return blocks


def _read_block_czyx(
    input_path: Path,
    source_format: zimg.FileFormat,
    num_channels: int,
    block: BlockBounds,
) -> np.ndarray:
    region = zimg.ZImgRegion(
        (block.x0, block.y0, block.z0, 0, 0),
        (block.x1, block.y1, block.z1, num_channels, 1),
    )
    img = zimg.ZImg(str(input_path), region=region, format=source_format)
    arrays = img.to_arrays("numpy")
    if len(arrays) != 1:
        raise ValueError(
            f"Expected exactly one timepoint ROI from {input_path}, got {len(arrays)}"
        )
    arr = np.asarray(arrays[0])
    if arr.ndim != 4:
        raise ValueError(f"Expected CZYX array, got shape {arr.shape}")
    return np.ascontiguousarray(arr)


def _czyx_to_zyxc(arr_czyx: np.ndarray) -> np.ndarray:
    return np.ascontiguousarray(np.moveaxis(arr_czyx, 0, -1))


def _make_vtk_image(
    block_zyxc: np.ndarray,
    origin_xyz: tuple[float, float, float],
    spacing_xyz: tuple[float, float, float],
) -> vtk.vtkImageData:
    depth, height, width, channels = block_zyxc.shape
    flat = block_zyxc.reshape(-1) if channels == 1 else block_zyxc.reshape(-1, channels)
    vtk_array = numpy_to_vtk(flat, deep=False, array_type=vtk.VTK_UNSIGNED_CHAR)
    vtk_array.SetName("channels")

    image = vtk.vtkImageData()
    image.SetDimensions(width, height, depth)
    image.SetOrigin(*origin_xyz)
    image.SetSpacing(*spacing_xyz)
    image.GetPointData().SetScalars(vtk_array)
    image.GetPointData().SetActiveScalars("channels")
    vtk_array._numpy_ref = block_zyxc
    return image


def _write_vti_piece(
    block_zyxc: np.ndarray,
    output_path: Path,
    origin_xyz: tuple[float, float, float],
    spacing_xyz: tuple[float, float, float],
) -> None:
    image = _make_vtk_image(block_zyxc, origin_xyz, spacing_xyz)
    writer = vtk.vtkXMLImageDataWriter()
    writer.SetFileName(str(output_path))
    writer.SetInputData(image)
    writer.SetDataModeToAppended()
    writer.EncodeAppendedDataOff()
    writer.SetCompressorTypeToNone()
    ok = writer.Write()
    if ok != 1:
        raise RuntimeError(f"Failed to write {output_path}")


def _write_vtpd_manifest(manifest_path: Path, piece_relpaths: list[str]) -> None:
    lines = [
        '<VTKFile type="vtkPartitionedDataSet" version="1.0" byte_order="LittleEndian" header_type="UInt32">',
        "  <vtkPartitionedDataSet>",
    ]
    for index, relpath in enumerate(piece_relpaths):
        lines.append(f'    <DataSet index="{index}" file="{relpath}"/>')
    lines.extend(["  </vtkPartitionedDataSet>", "</VTKFile>"])
    manifest_path.write_text("\n".join(lines) + "\n", encoding="utf-8")


class NimRegionBlockProvider(zimg.ZImgBlockProvider):
    def __init__(
        self,
        *,
        input_path: Path,
        source_format: zimg.FileFormat,
        source_info: zimg.ZImgInfo,
        blocks: list[BlockBounds],
        spacing_xyz: tuple[float, float, float],
        voxel_unit: zimg.VoxelSizeUnit,
    ) -> None:
        super().__init__()
        self._input_path = input_path
        self._source_format = source_format
        self._source_info = source_info
        self._blocks = blocks
        self._info = zimg.ZImgInfo(
            int(source_info.width),
            int(source_info.height),
            int(source_info.depth),
            int(source_info.numChannels),
            int(source_info.numTimes),
            int(source_info.bytesPerVoxel),
            source_info.voxelFormat,
        )
        self._info.voxelSizeUnit = voxel_unit
        self._info.voxelSizeX = float(spacing_xyz[0])
        self._info.voxelSizeY = float(spacing_xyz[1])
        self._info.voxelSizeZ = float(spacing_xyz[2])
        self._info.validBitCount = source_info.validBitCount
        self._info.channelNames = list(source_info.channelNames)
        self._info.channelColors = list(source_info.channelColors)

    def imgInfo(self) -> zimg.ZImgInfo:
        return self._info

    def numBlocks(self) -> int:
        return len(self._blocks)

    def blockCoord(self, block_idx: int) -> zimg.ZVoxelCoordinate:
        block = self._blocks[block_idx]
        return zimg.ZVoxelCoordinate(block.x0, block.y0, block.z0, 0, 0)

    def block(self, block_idx: int) -> zimg.ZImg:
        block = self._blocks[block_idx]
        region = zimg.ZImgRegion(
            (block.x0, block.y0, block.z0, 0, 0),
            (block.x1, block.y1, block.z1, int(self._source_info.numChannels), 1),
        )
        return zimg.ZImg(
            str(self._input_path), region=region, format=self._source_format
        )


def main() -> None:
    args = _parse_args()

    input_path = args.input.resolve()
    output_dir = args.output_dir.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)

    source_format = getattr(zimg.FileFormat, args.source_format)
    voxel_unit = getattr(zimg.VoxelSizeUnit, args.voxel_unit)
    spacing_xyz = (
        float(args.voxel_size_x),
        float(args.voxel_size_y),
        float(args.voxel_size_z),
    )

    source_info = _source_info(input_path, source_format)
    _validate_source(source_info)

    width = int(source_info.width)
    height = int(source_info.height)
    depth = int(source_info.depth)
    num_channels = int(source_info.numChannels)

    output_prefix = args.output_prefix or _default_prefix(
        input_path, spacing_xyz, args.voxel_unit
    )
    _cleanup_previous_outputs(output_dir, output_prefix)

    nim_output_path = output_dir / f"{output_prefix}.nim"
    manifest_path = output_dir / f"{output_prefix}.vtpd"
    piece_dir = output_dir / f"{output_prefix}_blocks"
    piece_dir.mkdir(parents=True, exist_ok=True)

    blocks = _enumerate_blocks(
        width=width,
        height=height,
        depth=depth,
        block_x=int(args.block_size_x),
        block_y=int(args.block_size_y),
        block_z=int(args.block_size_z),
    )

    nim_provider = NimRegionBlockProvider(
        input_path=input_path,
        source_format=source_format,
        source_info=source_info,
        blocks=blocks,
        spacing_xyz=spacing_xyz,
        voxel_unit=voxel_unit,
    )
    nim_paras = zimg.ZImgWriteParameters()
    nim_paras.compression = zimg.Compression.AUTO

    print(f"Writing corrected NIM: {nim_output_path}", flush=True)
    zimg.ZImg.writeImg(
        str(nim_output_path), nim_provider, zimg.FileFormat.HDF5Img, nim_paras
    )

    piece_relpaths: list[str] = []
    total_blocks = len(blocks)
    for block in blocks:
        piece_name = f"{output_prefix}_block_{block.index:03d}.vti"
        piece_path = piece_dir / piece_name
        piece_relpaths.append(f"{piece_dir.name}/{piece_name}")

        block_czyx = _read_block_czyx(input_path, source_format, num_channels, block)
        block_zyxc = _czyx_to_zyxc(block_czyx)
        origin_xyz = (
            block.x0 * spacing_xyz[0],
            block.y0 * spacing_xyz[1],
            block.z0 * spacing_xyz[2],
        )
        _write_vti_piece(block_zyxc, piece_path, origin_xyz, spacing_xyz)
        print(
            f"[{block.index + 1:03d}/{total_blocks:03d}] wrote {piece_path.name} "
            f"shape={block_zyxc.shape}",
            flush=True,
        )

    _write_vtpd_manifest(manifest_path, piece_relpaths)

    metadata = {
        "input": str(input_path),
        "source_format": str(source_format),
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
        "output_shape_zyxc": [depth, height, width, num_channels],
        "output_voxel_size_xyz": list(spacing_xyz),
        "output_voxel_size_unit": args.voxel_unit,
        "block_size_xyz": [
            int(args.block_size_x),
            int(args.block_size_y),
            int(args.block_size_z),
        ],
        "block_count": len(blocks),
        "block_grid_xyz": [
            math.ceil(width / int(args.block_size_x)),
            math.ceil(height / int(args.block_size_y)),
            math.ceil(depth / int(args.block_size_z)),
        ],
        "channel_names": list(source_info.channelNames),
        "outputs": {
            "corrected_nim": str(nim_output_path),
            "blocked_vtpd": str(manifest_path),
            "blocked_piece_dir": str(piece_dir),
        },
    }
    metadata_path = output_dir / f"{output_prefix}_export_metadata.json"
    metadata_path.write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")

    print("Finished export.", flush=True)
    print(f"Corrected Atlas dataset:  {nim_output_path}", flush=True)
    print(f"Blocked ParaView dataset: {manifest_path}", flush=True)
    print(f"Metadata:                 {metadata_path}", flush=True)


if __name__ == "__main__":
    main()
