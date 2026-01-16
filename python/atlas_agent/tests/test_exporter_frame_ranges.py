from __future__ import annotations

import sys
from pathlib import Path

# Add src layout to sys.path for local runs
ROOT_DIR = Path(__file__).resolve().parents[2]
SRC_DIR = ROOT_DIR / "src"
if str(SRC_DIR) not in sys.path:
    sys.path.insert(0, str(SRC_DIR))

import atlas_agent.exporter as exporter  # type: ignore  # noqa: E402


def _get_flag_value(args: list[str], flag: str) -> str:
    idx = args.index(flag)
    return args[idx + 1]


def test_preview_frames_converts_end_to_exclusive(monkeypatch, tmp_path):
    captured: list[list[str]] = []

    def fake_run(args: list[str]) -> int:
        captured.append(list(args))
        return 0

    monkeypatch.setattr(exporter, "_run", fake_run)

    exporter.preview_frames(
        atlas_bin="/tmp/atlas",
        animation_path=tmp_path / "a.animation3d",
        out_dir=tmp_path / "frames",
        fps=30,
        start=0,
        end=0,  # inclusive: request a single frame
        width=320,
        height=240,
        overwrite=True,
        dummy_output=str(tmp_path / "dummy.mp4"),
    )

    assert captured
    args = captured[0]
    assert _get_flag_value(args, "--output_start_frame") == "0"
    # CLI uses exclusive end; wrapper must pass 1 for a single-frame export.
    assert _get_flag_value(args, "--output_end_frame") == "1"
    # Preview should disable tiling to avoid intermediate tile images.
    assert _get_flag_value(args, "--output_tile_size") == "0"
    assert _get_flag_value(args, "--output_tile_border") == "0"
    # Preview should name images deterministically via the prefix.
    assert _get_flag_value(args, "--output_image_name_prefix") == "atlas_preview"


def test_preview_frames_preserves_negative_end(monkeypatch, tmp_path):
    captured: list[list[str]] = []

    def fake_run(args: list[str]) -> int:
        captured.append(list(args))
        return 0

    monkeypatch.setattr(exporter, "_run", fake_run)

    exporter.preview_frames(
        atlas_bin="/tmp/atlas",
        animation_path=tmp_path / "a.animation3d",
        out_dir=tmp_path / "frames",
        fps=30,
        start=0,
        end=-1,  # "duration"
        width=320,
        height=240,
        overwrite=True,
        dummy_output=str(tmp_path / "dummy.mp4"),
    )

    assert captured
    assert _get_flag_value(captured[0], "--output_end_frame") == "-1"


def test_export_video_converts_end_to_exclusive(monkeypatch, tmp_path):
    captured: list[list[str]] = []

    def fake_run(args: list[str]) -> int:
        captured.append(list(args))
        return 0

    monkeypatch.setattr(exporter, "_run", fake_run)

    exporter.export_video(
        atlas_bin="/tmp/atlas",
        animation_path=tmp_path / "a.animation3d",
        output_video=tmp_path / "out.mp4",
        fps=30,
        start=10,
        end=10,  # inclusive: request a single frame
        width=320,
        height=240,
        overwrite=True,
        use_gpu_devices=None,
    )

    assert captured
    args = captured[0]
    assert _get_flag_value(args, "--output_start_frame") == "10"
    assert _get_flag_value(args, "--output_end_frame") == "11"


def test_export_video_preserves_negative_end(monkeypatch, tmp_path):
    captured: list[list[str]] = []

    def fake_run(args: list[str]) -> int:
        captured.append(list(args))
        return 0

    monkeypatch.setattr(exporter, "_run", fake_run)

    exporter.export_video(
        atlas_bin="/tmp/atlas",
        animation_path=tmp_path / "a.animation3d",
        output_video=tmp_path / "out.mp4",
        fps=30,
        start=0,
        end=-1,  # "duration"
        width=320,
        height=240,
        overwrite=True,
        use_gpu_devices=None,
    )

    assert captured
    assert _get_flag_value(captured[0], "--output_end_frame") == "-1"
