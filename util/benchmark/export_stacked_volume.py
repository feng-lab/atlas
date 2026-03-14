#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
import math
import shutil
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable

import numpy as np
import vtk
from vtk.util.numpy_support import numpy_to_vtk

import zimg


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Stack multiple source volumes along Z and stream the result to "
            "Atlas .nim, ParaView .mhd/.zraw, and optional ParaView .vtpd."
        )
    )
    parser.add_argument(
        "--input",
        action="append",
        type=Path,
        required=True,
        help="Input volume path. Repeat to append multiple volumes along Z in order.",
    )
    parser.add_argument(
        "--input-format",
        required=True,
        choices=["HDF5Img", "ZeissLsm", "MetaImage"],
        help="Source file format for every input.",
    )
    parser.add_argument(
        "--output-dir", type=Path, required=True, help="Output directory"
    )
    parser.add_argument("--output-prefix", required=True, help="Output filename prefix")
    parser.add_argument(
        "--channel-number",
        type=int,
        default=None,
        help="Optional 1-based channel number to export as a single-channel stacked volume.",
    )
    parser.add_argument(
        "--voxel-size-x", type=float, required=True, help="Output voxel size X"
    )
    parser.add_argument(
        "--voxel-size-y", type=float, required=True, help="Output voxel size Y"
    )
    parser.add_argument(
        "--voxel-size-z", type=float, required=True, help="Output voxel size Z"
    )
    parser.add_argument(
        "--voxel-unit",
        default="um",
        choices=["none", "nm", "um", "mm", "cm", "hm", "m", "km", "inch"],
        help="Output voxel size unit",
    )
    parser.add_argument(
        "--write-vtpd",
        action="store_true",
        help="Also write a blockwise .vtpd dataset for ParaView.",
    )
    parser.add_argument(
        "--skip-nim",
        action="store_true",
        help="Keep an existing .nim output and skip rewriting it.",
    )
    parser.add_argument(
        "--skip-metaimage",
        action="store_true",
        help="Keep an existing .mhd/.zraw output pair and skip rewriting it.",
    )
    parser.add_argument(
        "--block-size-x", type=int, default=1024, help="VTK block size X"
    )
    parser.add_argument(
        "--block-size-y", type=int, default=1024, help="VTK block size Y"
    )
    parser.add_argument(
        "--block-size-z", type=int, default=128, help="VTK block size Z"
    )
    return parser.parse_args()


@dataclass(frozen=True)
class InputVolume:
    path: Path
    depth: int
    z0: int
    z1: int


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


def _cleanup_previous_outputs(
    output_dir: Path,
    output_prefix: str,
    *,
    remove_nim: bool,
    remove_metaimage: bool,
    remove_vtpd: bool,
) -> None:
    targets = [output_dir / f"{output_prefix}_export_metadata.json"]
    if remove_nim:
        targets.append(output_dir / f"{output_prefix}.nim")
    if remove_metaimage:
        targets.extend(
            [
                output_dir / f"{output_prefix}.mhd",
                output_dir / f"{output_prefix}.raw",
                output_dir / f"{output_prefix}.zraw",
            ]
        )
    if remove_vtpd:
        targets.append(output_dir / f"{output_prefix}.vtpd")
    for target in targets:
        if target.exists():
            target.unlink()

    block_dir = output_dir / f"{output_prefix}_blocks"
    if remove_vtpd and block_dir.exists():
        shutil.rmtree(block_dir)


def _read_info(path: Path, fmt: zimg.FileFormat) -> zimg.ZImgInfo:
    infos = zimg.ZImg.readImgInfos(str(path), fmt)
    if len(infos) != 1:
        raise ValueError(f"Expected exactly one scene in {path}, got {len(infos)}")
    return infos[0]


def _validate_inputs(
    input_paths: list[Path],
    fmt: zimg.FileFormat,
    channel_index: int | None,
) -> tuple[zimg.ZImgInfo, list[InputVolume], int]:
    infos = [_read_info(path, fmt) for path in input_paths]
    reference = infos[0]
    if int(reference.numTimes) != 1:
        raise ValueError(f"Expected a single timepoint, got {reference.numTimes}")
    if (
        int(reference.bytesPerVoxel) != 1
        or reference.voxelFormat != zimg.VoxelFormat.Unsigned
    ):
        raise ValueError(
            "This exporter currently expects uint8 data; "
            f"got {reference.voxelFormat} / {reference.bytesPerVoxel} B per voxel"
        )
    if channel_index is not None and not (
        0 <= channel_index < int(reference.numChannels)
    ):
        raise ValueError(
            f"--channel-number must be in [1, {reference.numChannels}], got {channel_index + 1}"
        )

    comparable0 = (
        int(reference.width),
        int(reference.height),
        int(reference.numChannels),
        int(reference.numTimes),
        int(reference.bytesPerVoxel),
        int(reference.voxelFormat),
    )

    volumes: list[InputVolume] = []
    z_cursor = 0
    for path, info in zip(input_paths, infos, strict=True):
        comparable = (
            int(info.width),
            int(info.height),
            int(info.numChannels),
            int(info.numTimes),
            int(info.bytesPerVoxel),
            int(info.voxelFormat),
        )
        if comparable != comparable0:
            raise ValueError(
                f"Input metadata mismatch in {path}: {comparable} != {comparable0}"
            )
        depth = int(info.depth)
        volumes.append(
            InputVolume(path=path, depth=depth, z0=z_cursor, z1=z_cursor + depth)
        )
        z_cursor += depth

    return reference, volumes, z_cursor


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


def _read_roi_czyx(
    path: Path,
    fmt: zimg.FileFormat,
    *,
    x0: int,
    x1: int,
    y0: int,
    y1: int,
    z0: int,
    z1: int,
    num_channels: int,
    channel_index: int | None,
) -> np.ndarray:
    if channel_index is None:
        c0, c1 = 0, num_channels
    else:
        c0, c1 = channel_index, channel_index + 1
    region = zimg.ZImgRegion((x0, y0, z0, c0, 0), (x1, y1, z1, c1, 1))
    img = zimg.ZImg(str(path), region=region, format=fmt)
    arrays = img.to_arrays("numpy")
    if len(arrays) != 1:
        raise ValueError(f"Expected one timepoint ROI from {path}, got {len(arrays)}")
    arr = np.asarray(arrays[0])
    if arr.ndim != 4:
        raise ValueError(f"Expected CZYX ROI from {path}, got shape {arr.shape}")
    return np.ascontiguousarray(arr)


def _read_stacked_block_czyx(
    *,
    volumes: list[InputVolume],
    fmt: zimg.FileFormat,
    width: int,
    height: int,
    total_depth: int,
    num_channels: int,
    channel_index: int | None,
    block: BlockBounds,
) -> np.ndarray:
    out_channels = 1 if channel_index is not None else num_channels
    out = np.zeros(
        (out_channels, block.depth, block.height, block.width), dtype=np.uint8
    )
    for volume in volumes:
        overlap_z0 = max(block.z0, volume.z0)
        overlap_z1 = min(block.z1, volume.z1)
        if overlap_z0 >= overlap_z1:
            continue
        source_z0 = overlap_z0 - volume.z0
        source_z1 = overlap_z1 - volume.z0
        chunk = _read_roi_czyx(
            volume.path,
            fmt,
            x0=block.x0,
            x1=block.x1,
            y0=block.y0,
            y1=block.y1,
            z0=source_z0,
            z1=source_z1,
            num_channels=num_channels,
            channel_index=channel_index,
        )
        out_z0 = overlap_z0 - block.z0
        out_z1 = overlap_z1 - block.z0
        out[:, out_z0:out_z1, :, :] = chunk
    return out


class StackedVolumeBlockProvider(zimg.ZImgBlockProvider):
    def __init__(
        self,
        *,
        blocks: list[BlockBounds],
        volumes: list[InputVolume],
        fmt: zimg.FileFormat,
        reference_info: zimg.ZImgInfo,
        width: int,
        height: int,
        total_depth: int,
        channel_index: int | None,
        spacing_xyz: tuple[float, float, float],
        voxel_unit: zimg.VoxelSizeUnit,
    ) -> None:
        super().__init__()
        self._blocks = blocks
        self._volumes = volumes
        self._fmt = fmt
        self._reference_info = reference_info
        self._width = width
        self._height = height
        self._total_depth = total_depth
        self._channel_index = channel_index
        out_channels = (
            1 if channel_index is not None else int(reference_info.numChannels)
        )
        self._info = zimg.ZImgInfo(
            width,
            height,
            total_depth,
            out_channels,
            1,
            int(reference_info.bytesPerVoxel),
            reference_info.voxelFormat,
        )
        self._info.voxelSizeUnit = voxel_unit
        self._info.voxelSizeX = float(spacing_xyz[0])
        self._info.voxelSizeY = float(spacing_xyz[1])
        self._info.voxelSizeZ = float(spacing_xyz[2])
        self._info.validBitCount = reference_info.validBitCount
        if channel_index is None:
            self._info.channelNames = list(reference_info.channelNames)
            self._info.channelColors = list(reference_info.channelColors)
        else:
            self._info.channelNames = [reference_info.channelNames[channel_index]]
            self._info.channelColors = [reference_info.channelColors[channel_index]]

    def imgInfo(self) -> zimg.ZImgInfo:
        return self._info

    def numBlocks(self) -> int:
        return len(self._blocks)

    def blockCoord(self, block_idx: int) -> zimg.ZVoxelCoordinate:
        block = self._blocks[block_idx]
        return zimg.ZVoxelCoordinate(block.x0, block.y0, block.z0, 0, 0)

    def block(self, block_idx: int) -> zimg.ZImg:
        block = self._blocks[block_idx]
        volume = next(
            (
                candidate
                for candidate in self._volumes
                if candidate.z0 <= block.z0 and block.z1 <= candidate.z1
            ),
            None,
        )
        if volume is None:
            raise ValueError(
                f"Provider block {block.index} is not covered by any source volume"
            )
        if self._channel_index is None:
            c0, c1 = 0, int(self._reference_info.numChannels)
        else:
            c0, c1 = self._channel_index, self._channel_index + 1
        region = zimg.ZImgRegion(
            (block.x0, block.y0, block.z0 - volume.z0, c0, 0),
            (block.x1, block.y1, block.z1 - volume.z0, c1, 1),
        )
        return zimg.ZImg(str(volume.path), region=region, format=self._fmt)


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


def _write_vtpd_manifest(manifest_path: Path, piece_relpaths: Iterable[str]) -> None:
    lines = [
        '<VTKFile type="vtkPartitionedDataSet" version="1.0" byte_order="LittleEndian" header_type="UInt32">',
        "  <vtkPartitionedDataSet>",
    ]
    for index, relpath in enumerate(piece_relpaths):
        lines.append(f'    <DataSet index="{index}" file="{relpath}"/>')
    lines.extend(["  </vtkPartitionedDataSet>", "</VTKFile>"])
    manifest_path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> None:
    args = _parse_args()
    input_paths = [path.resolve() for path in args.input]
    output_dir = args.output_dir.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)

    fmt = getattr(zimg.FileFormat, args.input_format)
    voxel_unit = getattr(zimg.VoxelSizeUnit, args.voxel_unit)
    channel_index = args.channel_number - 1 if args.channel_number is not None else None
    spacing_xyz = (
        float(args.voxel_size_x),
        float(args.voxel_size_y),
        float(args.voxel_size_z),
    )

    reference_info, volumes, total_depth = _validate_inputs(
        input_paths, fmt, channel_index
    )
    width = int(reference_info.width)
    height = int(reference_info.height)
    num_channels = int(reference_info.numChannels)
    out_channels = 1 if channel_index is not None else num_channels
    provider_block_x = min(width, 1024)
    provider_block_y = min(height, 1024)
    provider_blocks: list[BlockBounds] = []
    provider_block_index = 0
    for volume in volumes:
        for y0 in range(0, height, provider_block_y):
            y1 = min(y0 + provider_block_y, height)
            for x0 in range(0, width, provider_block_x):
                x1 = min(x0 + provider_block_x, width)
                provider_blocks.append(
                    BlockBounds(
                        index=provider_block_index,
                        x0=x0,
                        x1=x1,
                        y0=y0,
                        y1=y1,
                        z0=volume.z0,
                        z1=volume.z1,
                    )
                )
                provider_block_index += 1

    _cleanup_previous_outputs(
        output_dir,
        args.output_prefix,
        remove_nim=not args.skip_nim,
        remove_metaimage=not args.skip_metaimage,
        remove_vtpd=args.write_vtpd,
    )

    nim_path = output_dir / f"{args.output_prefix}.nim"
    mhd_path = output_dir / f"{args.output_prefix}.mhd"
    metaimage_data_path = output_dir / f"{args.output_prefix}.zraw"
    vtpd_path = output_dir / f"{args.output_prefix}.vtpd"
    block_dir = output_dir / f"{args.output_prefix}_blocks"

    provider = StackedVolumeBlockProvider(
        blocks=provider_blocks,
        volumes=volumes,
        fmt=fmt,
        reference_info=reference_info,
        width=width,
        height=height,
        total_depth=total_depth,
        channel_index=channel_index,
        spacing_xyz=spacing_xyz,
        voxel_unit=voxel_unit,
    )

    nim_paras = zimg.ZImgWriteParameters()
    nim_paras.compression = zimg.Compression.AUTO

    if args.skip_nim:
        if not nim_path.exists():
            raise FileNotFoundError(
                f"--skip-nim requested but {nim_path} does not exist"
            )
        print(f"Keeping existing NIM: {nim_path}", flush=True)
    else:
        print(f"Writing stacked NIM: {nim_path}", flush=True)
        zimg.ZImg.writeImg(str(nim_path), provider, zimg.FileFormat.HDF5Img, nim_paras)

    if args.skip_metaimage:
        if not mhd_path.exists() or not metaimage_data_path.exists():
            raise FileNotFoundError(
                f"--skip-metaimage requested but {mhd_path} / {metaimage_data_path} do not both exist"
            )
        print(f"Keeping existing MetaImage: {mhd_path}", flush=True)
    else:
        print(f"Writing stacked MetaImage: {mhd_path}", flush=True)
        zimg.ZImg.writeImg(str(mhd_path), provider, zimg.FileFormat.MetaImage)

    block_count = 0
    if args.write_vtpd:
        block_dir.mkdir(parents=True, exist_ok=True)
        vtpd_blocks = _enumerate_blocks(
            width,
            height,
            total_depth,
            int(args.block_size_x),
            int(args.block_size_y),
            int(args.block_size_z),
        )
        piece_relpaths: list[str] = []
        total_blocks = len(vtpd_blocks)
        for block in vtpd_blocks:
            arr_czyx = _read_stacked_block_czyx(
                volumes=volumes,
                fmt=fmt,
                width=width,
                height=height,
                total_depth=total_depth,
                num_channels=num_channels,
                channel_index=channel_index,
                block=block,
            )
            arr_zyxc = _czyx_to_zyxc(arr_czyx)
            piece_name = f"{args.output_prefix}_block_{block.index:03d}.vti"
            piece_path = block_dir / piece_name
            piece_relpaths.append(f"{block_dir.name}/{piece_name}")
            origin_xyz = (
                block.x0 * spacing_xyz[0],
                block.y0 * spacing_xyz[1],
                block.z0 * spacing_xyz[2],
            )
            _write_vti_piece(arr_zyxc, piece_path, origin_xyz, spacing_xyz)
            print(
                f"[{block.index + 1:03d}/{total_blocks:03d}] wrote {piece_path.name} "
                f"shape={arr_zyxc.shape}",
                flush=True,
            )
        _write_vtpd_manifest(vtpd_path, piece_relpaths)
        block_count = len(vtpd_blocks)

    metadata = {
        "inputs": [str(path) for path in input_paths],
        "input_format": str(fmt),
        "source_shape_czyx": [
            int(reference_info.numChannels),
            int(reference_info.depth),
            int(reference_info.height),
            int(reference_info.width),
        ],
        "stack_depths": [volume.depth for volume in volumes],
        "output_shape_zyxc": [total_depth, height, width, out_channels],
        "output_voxel_size_xyz": list(spacing_xyz),
        "output_voxel_size_unit": args.voxel_unit,
        "selected_channel_number": args.channel_number,
        "channel_names": (
            [reference_info.channelNames[channel_index]]
            if channel_index is not None
            else list(reference_info.channelNames)
        ),
        "provider_block_count": len(provider_blocks),
        "vtpd_block_count": block_count,
        "vtpd_block_size_xyz": (
            [int(args.block_size_x), int(args.block_size_y), int(args.block_size_z)]
            if args.write_vtpd
            else None
        ),
        "outputs": {
            "nim": str(nim_path),
            "metaimage": str(mhd_path),
            "metaimage_data": str(metaimage_data_path),
            "vtpd": str(vtpd_path) if args.write_vtpd else None,
            "block_dir": str(block_dir) if args.write_vtpd else None,
        },
    }
    metadata_path = output_dir / f"{args.output_prefix}_export_metadata.json"
    metadata_path.write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")

    print("Finished export.", flush=True)
    print(f"Atlas dataset:            {nim_path}", flush=True)
    print(f"ParaView MetaImage:       {mhd_path}", flush=True)
    if args.write_vtpd:
        print(f"ParaView blocked dataset: {vtpd_path}", flush=True)
    print(f"Metadata:                 {metadata_path}", flush=True)


if __name__ == "__main__":
    main()
