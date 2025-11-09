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
) -> int:
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
        str(end),
        "--output_width",
        str(width),
        "--output_height",
        str(height),
        "--output_image_folder_name",
        str(out_dir),
        "--skip_video_compression",
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
        str(end),
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
