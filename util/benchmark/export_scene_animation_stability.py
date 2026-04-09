#!/usr/bin/env python3

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import os
import shlex
import subprocess
import sys
import time
from collections import defaultdict
from dataclasses import asdict, dataclass
from datetime import datetime
from pathlib import Path
from typing import Any


REPO_ROOT = Path(__file__).resolve().parents[2]


@dataclass(frozen=True)
class BenchmarkPathConfig:
    atlas_path: Path
    input_root: Path
    output_parent: Path
    scene_filenames: tuple[str, ...]
    animation_filenames: tuple[str, ...]


# Edit this block when moving the benchmark to another machine or OS.
# The rest of the script derives its default Atlas/input/output paths from here.
# `atlas_path` must point to the actual Atlas executable:
# - macOS: `.../Atlas.app/Contents/MacOS/Atlas`
# - Windows: `...\\Atlas.exe`
# - Linux: exact `Atlas` executable file produced on that machine
DEFAULT_PATH_CONFIG = BenchmarkPathConfig(
    atlas_path=Path(
        "/Users/feng/code/atlas/build/Release/src/atlas/Atlas.app/Contents/MacOS/Atlas"
    ),
    input_root=Path("/Users/feng/Dropbox/atlas_test"),
    output_parent=Path("/Users/feng/Documents/test_folder"),
    scene_filenames=(
        "testscene3.scene",
        "testscene2.scene",
        "testscene5.scene",
        "testscene4.scene",
        "testscene1.scene",
        "testsene7.scene",
        "testscene6.scene",
    ),
    animation_filenames=("test.animation3d",),
)


def _default_input_paths(root: Path, filenames: tuple[str, ...]) -> list[Path]:
    return [root / filename for filename in filenames]


DEFAULT_SCENES = _default_input_paths(
    DEFAULT_PATH_CONFIG.input_root, DEFAULT_PATH_CONFIG.scene_filenames
)
DEFAULT_ANIMATIONS = _default_input_paths(
    DEFAULT_PATH_CONFIG.input_root, DEFAULT_PATH_CONFIG.animation_filenames
)
DEFAULT_ATLAS_PATH = DEFAULT_PATH_CONFIG.atlas_path
DEFAULT_OUTPUT_PARENT = DEFAULT_PATH_CONFIG.output_parent
DEFAULT_QT_PLATFORM = "auto"
DEFAULT_BACKENDS = ("opengl", "vulkan")


@dataclass(frozen=True)
class ExportCase:
    kind: str
    source_path: str
    label: str


@dataclass(frozen=True)
class RunRecord:
    kind: str
    backend: str
    qt_platform: str
    label: str
    source_path: str
    run_index: int
    output_dir: str
    command: list[str]
    command_shell: str
    returncode: int
    duration_seconds: float
    file_count: int
    total_bytes: int
    stdout_log: str
    stderr_log: str


def _timestamp() -> str:
    return datetime.now().strftime("%Y%m%d_%H%M%S")


def _default_output_root() -> Path:
    return DEFAULT_OUTPUT_PARENT / f"atlas_export_stability_{_timestamp()}"


def _slugify(name: str) -> str:
    chars: list[str] = []
    for ch in name:
        if ch.isalnum() or ch in ("-", "_", "."):
            chars.append(ch)
        else:
            chars.append("_")
    slug = "".join(chars).strip("._")
    return slug or "case"


def _resolve_atlas_binary(path_value: str) -> Path:
    candidate = Path(path_value).expanduser().resolve()

    if not candidate.is_file():
        raise FileNotFoundError(
            "Atlas executable not found at "
            f"{candidate}. Pass --atlas-path as the exact executable file: "
            "macOS `.../Atlas.app/Contents/MacOS/Atlas`, "
            "Windows `...\\\\Atlas.exe`, "
            "Linux exact `Atlas` binary."
        )

    return candidate


def _sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        while True:
            chunk = handle.read(1024 * 1024)
            if not chunk:
                break
            digest.update(chunk)
    return digest.hexdigest()


def _write_checksums(output_path: Path, entries: list[dict[str, Any]]) -> None:
    lines = [f"{entry['sha256']}  {entry['relative_path']}" for entry in entries]
    output_path.write_text("\n".join(lines) + ("\n" if lines else ""), encoding="utf-8")


def _write_json(path: Path, payload: Any) -> None:
    path.write_text(
        json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )


def _verify_inputs(paths: list[Path], kind: str) -> list[Path]:
    verified: list[Path] = []
    missing: list[str] = []
    for path in paths:
        resolved = path.expanduser().resolve()
        if resolved.exists():
            verified.append(resolved)
        else:
            missing.append(str(resolved))
    if missing:
        joined = "\n".join(missing)
        raise FileNotFoundError(f"Missing {kind} inputs:\n{joined}")
    return verified


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Run representative Atlas scene and animation exports across OpenGL and Vulkan, "
            "persist all outputs under a structured result tree, and record per-file checksums "
            "for later export-stability analysis."
        )
    )
    parser.add_argument(
        "--atlas-path",
        default=str(DEFAULT_ATLAS_PATH),
        help=(
            "Exact Atlas executable path. "
            "macOS: .../Atlas.app/Contents/MacOS/Atlas; "
            "Windows: ...\\\\Atlas.exe; "
            "Linux: exact Atlas binary path."
        ),
    )
    parser.add_argument(
        "--output-root",
        default=str(_default_output_root()),
        help="Result root directory",
    )
    parser.add_argument(
        "--scene",
        action="append",
        default=None,
        help="Scene file to export. Repeat to override/add scenes.",
    )
    parser.add_argument(
        "--animation",
        action="append",
        default=None,
        help="Animation file to export. Repeat to override/add animations.",
    )
    parser.add_argument(
        "--backend",
        action="append",
        choices=DEFAULT_BACKENDS,
        default=None,
        help="Render backend to test. Repeat for both backends.",
    )
    parser.add_argument(
        "--scene-runs",
        type=int,
        default=5,
        help="Number of repeated exports per scene/backend",
    )
    parser.add_argument(
        "--animation-runs",
        type=int,
        default=1,
        help="Number of repeated exports per animation/backend",
    )
    parser.add_argument(
        "--scene-width", type=int, default=8000, help="Scene export width"
    )
    parser.add_argument(
        "--scene-height", type=int, default=8000, help="Scene export height"
    )
    parser.add_argument(
        "--animation-width", type=int, default=3840, help="Animation export width"
    )
    parser.add_argument(
        "--animation-height", type=int, default=2160, help="Animation export height"
    )
    parser.add_argument(
        "--animation-fps", type=int, default=30, help="Animation export frame rate"
    )
    parser.add_argument(
        "--qt-platform",
        default=DEFAULT_QT_PLATFORM,
        help=(
            "Qt platform plugin to pass to Atlas. Use 'auto' to select a backend-aware default "
            "(macOS OpenGL omits the platform override; Windows omits it for both backends; "
            "Linux uses offscreen). Use 'none' to suppress the platform override explicitly."
        ),
    )
    parser.add_argument(
        "--qt-platform-opengl",
        default=None,
        help=(
            "Optional Qt platform override for OpenGL runs only. Accepts a plugin name, 'auto', "
            "or 'none'."
        ),
    )
    parser.add_argument(
        "--qt-platform-vulkan",
        default=None,
        help=(
            "Optional Qt platform override for Vulkan runs only. Accepts a plugin name, 'auto', "
            "or 'none'."
        ),
    )
    parser.add_argument(
        "--extra-arg",
        action="append",
        default=[],
        help="Additional raw Atlas argument appended to every invocation",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Print commands and write the manifest without running Atlas",
    )
    return parser.parse_args()


def _base_command(
    atlas_binary: Path,
    backend: str,
    qt_platform: str,
    extra_args: list[str],
) -> list[str]:
    command = [str(atlas_binary)]
    if qt_platform:
        command.extend(["-platform", qt_platform])
    command.append(f"--atlas_default_render_backend={backend}")
    command.extend(extra_args)
    return command


def _normalize_qt_platform(qt_platform: str | None) -> str | None:
    if qt_platform is None:
        return None

    normalized = qt_platform.strip()
    if normalized.lower() in {"", "none", "native"}:
        return ""
    return normalized


def _resolve_qt_platform(
    qt_platform: str, backend: str, backend_override: str | None = None
) -> str:
    override = _normalize_qt_platform(backend_override)
    if override is not None and override != "auto":
        return override

    global_platform = _normalize_qt_platform(qt_platform)
    if global_platform is None:
        global_platform = "auto"
    if global_platform != "auto":
        return global_platform

    # On macOS, Qt's offscreen plugin does not provide a platform OpenGL context.
    # Let Qt pick the native platform plugin for OpenGL there instead of pinning one.
    if sys.platform == "darwin" and backend == "opengl":
        return ""

    # On Windows, native Qt platform selection is the safest default for both backends.
    # OpenGL is known not to work with offscreen, and Vulkan can still be overridden explicitly.
    if sys.platform.startswith("win"):
        return ""

    return "offscreen"


def _run_process(
    command: list[str],
    stdout_log: Path,
    stderr_log: Path,
    qt_platform: str,
    dry_run: bool,
) -> tuple[int, float]:
    stdout_log.parent.mkdir(parents=True, exist_ok=True)
    stderr_log.parent.mkdir(parents=True, exist_ok=True)

    if dry_run:
        stdout_log.write_text("dry-run\n", encoding="utf-8")
        stderr_log.write_text("dry-run\n", encoding="utf-8")
        return 0, 0.0

    env = os.environ.copy()
    if qt_platform:
        env["QT_QPA_PLATFORM"] = qt_platform

    start = time.monotonic()
    with (
        stdout_log.open("w", encoding="utf-8") as stdout_handle,
        stderr_log.open("w", encoding="utf-8") as stderr_handle,
    ):
        result = subprocess.run(
            command,
            cwd=REPO_ROOT,
            env=env,
            stdout=stdout_handle,
            stderr=stderr_handle,
            text=True,
            check=False,
        )
    elapsed = time.monotonic() - start
    return result.returncode, elapsed


def _hash_output_files(
    run_dir: Path, files: list[Path]
) -> tuple[list[dict[str, Any]], int]:
    entries: list[dict[str, Any]] = []
    total_bytes = 0
    for path in sorted(files):
        stat = path.stat()
        total_bytes += stat.st_size
        entries.append(
            {
                "relative_path": path.relative_to(run_dir).as_posix(),
                "absolute_path": str(path),
                "size_bytes": stat.st_size,
                "sha256": _sha256_file(path),
            }
        )
    return entries, total_bytes


def _build_scene_command(
    atlas_binary: Path,
    backend: str,
    qt_platform: str,
    extra_args: list[str],
    source: Path,
    output_image: Path,
    width: int,
    height: int,
) -> list[str]:
    command = _base_command(atlas_binary, backend, qt_platform, extra_args)
    command.extend(
        [
            "--run_export_3d_scene",
            "--filename",
            str(source),
            "--output_filename",
            str(output_image),
            "--output_width",
            str(width),
            "--output_height",
            str(height),
            "--overwrite",
        ]
    )
    return command


def _build_animation_command(
    atlas_binary: Path,
    backend: str,
    qt_platform: str,
    extra_args: list[str],
    source: Path,
    output_video: Path,
    frames_dir: Path,
    width: int,
    height: int,
    fps: int,
) -> list[str]:
    command = _base_command(atlas_binary, backend, qt_platform, extra_args)
    command.extend(
        [
            "--run_export_3d_animation",
            "--filename",
            str(source),
            "--output_filename",
            str(output_video),
            "--output_fps",
            str(fps),
            "--output_width",
            str(width),
            "--output_height",
            str(height),
            "--output_image_folder_name",
            str(frames_dir),
            "--output_image_name_prefix",
            "frame_",
            "--output_image_name_field_width",
            "6",
            "--skip_video_compression",
            "--overwrite",
        ]
    )
    return command


def _collect_scene_outputs(output_image: Path) -> list[Path]:
    return [output_image] if output_image.is_file() else []


def _collect_animation_outputs(frames_dir: Path) -> list[Path]:
    return sorted(path for path in frames_dir.glob("*.png") if path.is_file())


def _run_scene_case(
    atlas_binary: Path,
    output_root: Path,
    backend: str,
    qt_platform: str,
    extra_args: list[str],
    source: Path,
    run_index: int,
    width: int,
    height: int,
    backend_qt_platform: str | None,
    dry_run: bool,
) -> tuple[RunRecord, list[dict[str, Any]]]:
    label = source.name
    run_dir = output_root / backend / "scene" / _slugify(label) / f"run_{run_index:02d}"
    run_dir.mkdir(parents=True, exist_ok=False)

    output_image = run_dir / "export.png"
    stdout_log = run_dir / "stdout.log"
    stderr_log = run_dir / "stderr.log"
    resolved_qt_platform = _resolve_qt_platform(
        qt_platform, backend, backend_qt_platform
    )

    command = _build_scene_command(
        atlas_binary=atlas_binary,
        backend=backend,
        qt_platform=resolved_qt_platform,
        extra_args=extra_args,
        source=source,
        output_image=output_image,
        width=width,
        height=height,
    )
    returncode, duration_seconds = _run_process(
        command=command,
        stdout_log=stdout_log,
        stderr_log=stderr_log,
        qt_platform=resolved_qt_platform,
        dry_run=dry_run,
    )

    files = _collect_scene_outputs(output_image)
    file_entries, total_bytes = _hash_output_files(run_dir, files)
    _write_checksums(run_dir / "checksums.sha256", file_entries)

    run_record = RunRecord(
        kind="scene",
        backend=backend,
        qt_platform=resolved_qt_platform,
        label=label,
        source_path=str(source),
        run_index=run_index,
        output_dir=str(run_dir),
        command=command,
        command_shell=" ".join(shlex.quote(part) for part in command),
        returncode=returncode,
        duration_seconds=duration_seconds,
        file_count=len(file_entries),
        total_bytes=total_bytes,
        stdout_log=str(stdout_log),
        stderr_log=str(stderr_log),
    )
    _write_json(
        run_dir / "run.json",
        {
            "run": asdict(run_record),
            "files": file_entries,
        },
    )

    return run_record, file_entries


def _run_animation_case(
    atlas_binary: Path,
    output_root: Path,
    backend: str,
    qt_platform: str,
    extra_args: list[str],
    source: Path,
    run_index: int,
    width: int,
    height: int,
    fps: int,
    backend_qt_platform: str | None,
    dry_run: bool,
) -> tuple[RunRecord, list[dict[str, Any]]]:
    label = source.name
    run_dir = (
        output_root / backend / "animation" / _slugify(label) / f"run_{run_index:02d}"
    )
    run_dir.mkdir(parents=True, exist_ok=False)

    frames_dir = run_dir / "frames"
    frames_dir.mkdir(parents=True, exist_ok=False)
    output_video = run_dir / f"{source.stem}.mp4"
    stdout_log = run_dir / "stdout.log"
    stderr_log = run_dir / "stderr.log"
    resolved_qt_platform = _resolve_qt_platform(
        qt_platform, backend, backend_qt_platform
    )

    command = _build_animation_command(
        atlas_binary=atlas_binary,
        backend=backend,
        qt_platform=resolved_qt_platform,
        extra_args=extra_args,
        source=source,
        output_video=output_video,
        frames_dir=frames_dir,
        width=width,
        height=height,
        fps=fps,
    )
    returncode, duration_seconds = _run_process(
        command=command,
        stdout_log=stdout_log,
        stderr_log=stderr_log,
        qt_platform=resolved_qt_platform,
        dry_run=dry_run,
    )

    files = _collect_animation_outputs(frames_dir)
    file_entries, total_bytes = _hash_output_files(run_dir, files)
    _write_checksums(run_dir / "checksums.sha256", file_entries)

    run_record = RunRecord(
        kind="animation",
        backend=backend,
        qt_platform=resolved_qt_platform,
        label=label,
        source_path=str(source),
        run_index=run_index,
        output_dir=str(run_dir),
        command=command,
        command_shell=" ".join(shlex.quote(part) for part in command),
        returncode=returncode,
        duration_seconds=duration_seconds,
        file_count=len(file_entries),
        total_bytes=total_bytes,
        stdout_log=str(stdout_log),
        stderr_log=str(stderr_log),
    )
    _write_json(
        run_dir / "run.json",
        {
            "run": asdict(run_record),
            "files": file_entries,
        },
    )

    return run_record, file_entries


def _write_run_summary_csv(output_root: Path, runs: list[RunRecord]) -> None:
    path = output_root / "run_summary.csv"
    with path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(
            [
                "kind",
                "backend",
                "qt_platform",
                "label",
                "source_path",
                "run_index",
                "returncode",
                "duration_seconds",
                "file_count",
                "total_bytes",
                "output_dir",
                "stdout_log",
                "stderr_log",
                "command_shell",
            ]
        )
        for run in runs:
            writer.writerow(
                [
                    run.kind,
                    run.backend,
                    run.qt_platform,
                    run.label,
                    run.source_path,
                    run.run_index,
                    run.returncode,
                    f"{run.duration_seconds:.6f}",
                    run.file_count,
                    run.total_bytes,
                    run.output_dir,
                    run.stdout_log,
                    run.stderr_log,
                    run.command_shell,
                ]
            )


def _write_file_hashes_csv(output_root: Path, rows: list[dict[str, Any]]) -> None:
    path = output_root / "file_hashes.csv"
    with path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(
            [
                "kind",
                "backend",
                "label",
                "source_path",
                "run_index",
                "relative_path",
                "size_bytes",
                "sha256",
                "output_dir",
            ]
        )
        for row in rows:
            writer.writerow(
                [
                    row["kind"],
                    row["backend"],
                    row["label"],
                    row["source_path"],
                    row["run_index"],
                    row["relative_path"],
                    row["size_bytes"],
                    row["sha256"],
                    row["output_dir"],
                ]
            )


def _build_stability_summary(rows: list[dict[str, Any]]) -> dict[str, Any]:
    grouped: dict[tuple[str, str, str, str], list[dict[str, Any]]] = defaultdict(list)
    for row in rows:
        key = (row["kind"], row["backend"], row["label"], row["relative_path"])
        grouped[key].append(row)

    summary_rows: list[dict[str, Any]] = []
    for (kind, backend, label, relative_path), group_rows in sorted(grouped.items()):
        hashes = sorted({row["sha256"] for row in group_rows})
        summary_rows.append(
            {
                "kind": kind,
                "backend": backend,
                "label": label,
                "relative_path": relative_path,
                "run_count": len(group_rows),
                "unique_hash_count": len(hashes),
                "all_hashes_identical": len(hashes) == 1,
                "hashes": hashes,
            }
        )

    return {
        "generated_at": datetime.now().isoformat(),
        "entries": summary_rows,
    }


def main() -> int:
    args = _parse_args()

    atlas_binary = _resolve_atlas_binary(args.atlas_path)
    output_root = Path(args.output_root).expanduser().resolve()
    if output_root.exists():
        raise FileExistsError(f"Output root already exists: {output_root}")
    output_root.mkdir(parents=True, exist_ok=False)

    scenes = _verify_inputs(
        [
            Path(path)
            for path in (args.scene if args.scene is not None else DEFAULT_SCENES)
        ],
        "scene",
    )
    animations = _verify_inputs(
        [
            Path(path)
            for path in (
                args.animation if args.animation is not None else DEFAULT_ANIMATIONS
            )
        ],
        "animation",
    )
    backends = tuple(args.backend if args.backend is not None else DEFAULT_BACKENDS)

    manifest = {
        "generated_at": datetime.now().isoformat(),
        "repo_root": str(REPO_ROOT),
        "atlas_binary": str(atlas_binary),
        "output_root": str(output_root),
        "host_platform": sys.platform,
        "qt_platform": args.qt_platform,
        "qt_platform_overrides": {
            "opengl": args.qt_platform_opengl,
            "vulkan": args.qt_platform_vulkan,
        },
        "backends": list(backends),
        "scene_runs": args.scene_runs,
        "animation_runs": args.animation_runs,
        "scene_size": {"width": args.scene_width, "height": args.scene_height},
        "animation_size": {
            "width": args.animation_width,
            "height": args.animation_height,
            "fps": args.animation_fps,
        },
        "extra_args": args.extra_arg,
        "scenes": [str(path) for path in scenes],
        "animations": [str(path) for path in animations],
        "dry_run": bool(args.dry_run),
    }
    _write_json(output_root / "manifest.json", manifest)

    run_records: list[RunRecord] = []
    file_hash_rows: list[dict[str, Any]] = []
    backend_qt_platform_overrides = {
        "opengl": args.qt_platform_opengl,
        "vulkan": args.qt_platform_vulkan,
    }

    scene_cases = [ExportCase("scene", str(path), path.name) for path in scenes]
    animation_cases = [
        ExportCase("animation", str(path), path.name) for path in animations
    ]

    for backend in backends:
        for case in scene_cases:
            source = Path(case.source_path)
            for run_index in range(1, args.scene_runs + 1):
                run_record, file_entries = _run_scene_case(
                    atlas_binary=atlas_binary,
                    output_root=output_root,
                    backend=backend,
                    qt_platform=args.qt_platform,
                    extra_args=args.extra_arg,
                    source=source,
                    run_index=run_index,
                    width=args.scene_width,
                    height=args.scene_height,
                    backend_qt_platform=backend_qt_platform_overrides.get(backend),
                    dry_run=args.dry_run,
                )
                run_records.append(run_record)
                for entry in file_entries:
                    file_hash_rows.append(
                        {
                            "kind": "scene",
                            "backend": backend,
                            "label": case.label,
                            "source_path": case.source_path,
                            "run_index": run_index,
                            "relative_path": entry["relative_path"],
                            "size_bytes": entry["size_bytes"],
                            "sha256": entry["sha256"],
                            "output_dir": run_record.output_dir,
                        }
                    )

        for case in animation_cases:
            source = Path(case.source_path)
            for run_index in range(1, args.animation_runs + 1):
                run_record, file_entries = _run_animation_case(
                    atlas_binary=atlas_binary,
                    output_root=output_root,
                    backend=backend,
                    qt_platform=args.qt_platform,
                    extra_args=args.extra_arg,
                    source=source,
                    run_index=run_index,
                    width=args.animation_width,
                    height=args.animation_height,
                    fps=args.animation_fps,
                    backend_qt_platform=backend_qt_platform_overrides.get(backend),
                    dry_run=args.dry_run,
                )
                run_records.append(run_record)
                for entry in file_entries:
                    file_hash_rows.append(
                        {
                            "kind": "animation",
                            "backend": backend,
                            "label": case.label,
                            "source_path": case.source_path,
                            "run_index": run_index,
                            "relative_path": entry["relative_path"],
                            "size_bytes": entry["size_bytes"],
                            "sha256": entry["sha256"],
                            "output_dir": run_record.output_dir,
                        }
                    )

    _write_run_summary_csv(output_root, run_records)
    _write_file_hashes_csv(output_root, file_hash_rows)
    _write_json(
        output_root / "stability_summary.json", _build_stability_summary(file_hash_rows)
    )

    print(f"Atlas binary: {atlas_binary}")
    print(f"Results written to: {output_root}")
    print(f"Runs recorded: {len(run_records)}")
    print(f"Files hashed: {len(file_hash_rows)}")
    if args.dry_run:
        print("Dry run only; Atlas commands were not executed.")

    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:  # pragma: no cover - CLI failure reporting
        print(str(exc), file=sys.stderr)
        raise SystemExit(1)
