import os
import platform
import subprocess
import logging
from pathlib import Path


def _base_args() -> list[str]:
    args: list[str] = []
    # Headless/offscreen on Windows and Linux to avoid GUI surface creation
    if platform.system() in {"Windows", "Linux"}:
        args += ["-platform", "offscreen"]
    return args


def preview_frames(
    *,
    atlas_bin: str,
    animation_path: Path,
    out_dir: Path,
    fps: int,
    start: int,
    end: int,
    width: int,
    height: int,
    overwrite: bool,
    dummy_output: str,
    image_name_prefix: str = "atlas_preview",
    image_name_field_width: int = 5,
    tile_size: int = 0,
    tile_border: int = 0,
) -> int:
    # Atlas CLI uses an exclusive end frame (i < endFrame). For agent tools we
    # treat `end` as inclusive to match the tool descriptions and to support
    # single-frame preview calls where start==end.
    end_exclusive = (int(end) + 1) if int(end) >= 0 else int(end)
    # For preview screenshots we prefer a single deterministic PNG file. Disable
    # tiled rendering so Atlas does not generate intermediate tile images.
    tile_size_i = int(tile_size)
    tile_border_i = int(tile_border)
    if tile_size_i < 0 or tile_border_i < 0:
        raise ValueError("tile_size and tile_border must be >= 0")
    args = [
        atlas_bin,
        "--run_export_3d_animation",
        "--filename",
        str(animation_path),
        "--output_filename",
        str(dummy_output),
        "--output_fps",
        str(fps),
        "--output_start_frame",
        str(start),
        "--output_end_frame",
        str(end_exclusive),
        "--output_width",
        str(width),
        "--output_height",
        str(height),
        "--output_image_folder_name",
        str(out_dir),
        "--output_image_name_prefix",
        str(image_name_prefix),
        "--output_image_name_field_width",
        str(int(image_name_field_width)),
        "--skip_video_compression",
        "--output_tile_size",
        str(tile_size_i),
        "--output_tile_border",
        str(tile_border_i),
    ]
    if overwrite:
        args.append("--overwrite")
    args += _base_args()
    return _run(args)


def export_video(
    *,
    atlas_bin: str,
    animation_path: Path,
    output_video: Path,
    fps: int,
    start: int,
    end: int,
    width: int,
    height: int,
    overwrite: bool,
    use_gpu_devices: str | None,
) -> int:
    # Atlas CLI uses an exclusive end frame (i < endFrame). For agent tools we
    # treat `end` as inclusive (and preserve -1 as "duration").
    end_exclusive = (int(end) + 1) if int(end) >= 0 else int(end)
    args = [
        atlas_bin,
        "--run_export_3d_animation",
        "--filename",
        str(animation_path),
        "--output_filename",
        str(output_video),
        "--output_fps",
        str(fps),
        "--output_start_frame",
        str(start),
        "--output_end_frame",
        str(end_exclusive),
        "--output_width",
        str(width),
        "--output_height",
        str(height),
    ]
    if overwrite:
        args.append("--overwrite")
    if use_gpu_devices:
        args += ["--use_gpu_devices", use_gpu_devices]
    args += _base_args()
    return _run(args)


def _run(args: list[str]) -> int:
    env = os.environ.copy()
    try:
        proc = subprocess.run(args, env=env, check=False)
        return proc.returncode
    except FileNotFoundError:
        logging.getLogger("atlas_agent.exporter").error("Could not execute: %s", args[0])
        return 127
