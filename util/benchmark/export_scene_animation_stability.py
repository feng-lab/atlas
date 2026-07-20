#!/usr/bin/env python3

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
import os
import platform
import shlex
import statistics
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


DEFAULT_SCENE_FILENAMES = (
    "testscene3.scene",
    "testscene2.scene",
    "testscene5.scene",
    "testscene4.scene",
    "testscene1.scene",
    "testsene7.scene",
    "testscene6.scene",
)
DEFAULT_ANIMATION_FILENAMES = ("test.animation3d",)


def _default_atlas_path() -> Path:
    if sys.platform == "darwin":
        return (
            REPO_ROOT
            / "build"
            / "Release"
            / "src"
            / "atlas"
            / "Atlas.app"
            / "Contents"
            / "MacOS"
            / "Atlas"
        )

    if sys.platform.startswith("win"):
        return REPO_ROOT / "deploy" / "Atlas" / "Atlas.exe"

    return REPO_ROOT / "build" / "Release" / "src" / "atlas" / "Atlas"


def _default_output_parent() -> Path:
    if sys.platform.startswith("win"):
        return Path("D:/test_folder")

    return Path.home() / "Documents" / "test_folder"


# Platform defaults for the known benchmark machine layouts. Override the CLI
# flags when running from a different build/deploy or test-data location.
DEFAULT_PATH_CONFIG = BenchmarkPathConfig(
    atlas_path=_default_atlas_path(),
    input_root=Path.home() / "Dropbox" / "atlas_test",
    output_parent=_default_output_parent(),
    scene_filenames=DEFAULT_SCENE_FILENAMES,
    animation_filenames=DEFAULT_ANIMATION_FILENAMES,
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
DEFAULT_ANIMATION_DURATION_SECONDS = 10.0


@dataclass(frozen=True)
class ExportCase:
    kind: str
    source_path: str
    label: str
    output_key: str


@dataclass(frozen=True)
class RunRecord:
    kind: str
    backend: str
    qt_platform: str
    label: str
    source_path: str
    source_sha256: str
    run_index: int
    output_dir: str
    command: list[str]
    command_shell: str
    returncode: int
    timed_out: bool
    duration_seconds: float
    file_count: int
    expected_file_count: int
    total_bytes: int
    success: bool
    failure_reasons: tuple[str, ...]
    perf_summary_path: str | None
    perf_summary_frame_count: int
    perf_summary_profile: str | None
    perf_summary_unavailable_metrics: tuple[str, ...]
    perf_summary_errors: tuple[str, ...]
    stdout_log: str
    stderr_log: str


@dataclass(frozen=True)
class PerfSummaryParseResult:
    record_count: int
    profile: str | None
    unavailable_metrics: tuple[str, ...]
    errors: tuple[str, ...]


# kind, backend, display label, source hash, absolute source path. The path is
# part of the identity because two intentionally selected inputs can have the
# same basename and byte-for-byte contents while still being separate cases.
RunGroupKey = tuple[str, str, str, str, str]


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


def _build_export_cases(kind: str, paths: list[Path]) -> list[ExportCase]:
    path_strings = [str(path) for path in paths]
    if len(path_strings) != len(set(path_strings)):
        duplicates = sorted(
            path for path in set(path_strings) if path_strings.count(path) > 1
        )
        raise ValueError(
            f"Duplicate {kind} input path(s) would repeat the same benchmark case: "
            + ", ".join(duplicates)
        )

    slug_counts: dict[str, int] = defaultdict(int)
    for path in paths:
        slug_counts[_slugify(path.name)] += 1

    cases: list[ExportCase] = []
    for path in paths:
        output_key = _slugify(path.name)
        if slug_counts[output_key] > 1:
            path_digest = hashlib.sha256(str(path).encode("utf-8")).hexdigest()
            output_key = f"path_{path_digest}"
        cases.append(
            ExportCase(
                kind=kind,
                source_path=str(path),
                label=path.name,
                output_key=output_key,
            )
        )
    if len({case.output_key for case in cases}) != len(cases):
        raise RuntimeError(
            f"Could not construct unique output directories for {kind} inputs"
        )
    return cases


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


def _command_output(
    command: list[str], timeout_seconds: float = 15.0, allow_empty: bool = False
) -> str | None:
    try:
        result = subprocess.run(
            command,
            cwd=REPO_ROOT,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
            check=False,
            timeout=timeout_seconds,
        )
    except (OSError, subprocess.TimeoutExpired):
        return None
    if result.returncode != 0:
        return None
    output = result.stdout.strip()
    return output if output or allow_empty else None


def _git_metadata() -> dict[str, Any]:
    revision = _command_output(["git", "rev-parse", "HEAD"])
    status = _command_output(
        ["git", "status", "--porcelain", "--untracked-files=normal"],
        allow_empty=True,
    )
    return {
        "revision": revision,
        "dirty": None if status is None else bool(status),
    }


def _cpu_model() -> str | None:
    if sys.platform == "darwin":
        return _command_output(["sysctl", "-n", "machdep.cpu.brand_string"])
    if sys.platform.startswith("linux"):
        cpuinfo = Path("/proc/cpuinfo")
        if cpuinfo.is_file():
            for line in cpuinfo.read_text(
                encoding="utf-8", errors="replace"
            ).splitlines():
                if line.lower().startswith("model name") and ":" in line:
                    return line.split(":", 1)[1].strip() or None
    return platform.processor() or os.environ.get("PROCESSOR_IDENTIFIER") or None


def _total_memory_bytes() -> int | None:
    try:
        page_size = os.sysconf("SC_PAGE_SIZE")
        physical_pages = os.sysconf("SC_PHYS_PAGES")
    except (AttributeError, OSError, ValueError):
        return None
    if not isinstance(page_size, int) or not isinstance(physical_pages, int):
        return None
    return page_size * physical_pages


def _macos_gpu_devices() -> list[dict[str, Any]]:
    output = _command_output(
        ["system_profiler", "SPDisplaysDataType", "-json"], timeout_seconds=30.0
    )
    if output is None:
        return []
    try:
        payload = json.loads(output)
    except json.JSONDecodeError:
        return []
    adapters = payload.get("SPDisplaysDataType")
    if not isinstance(adapters, list):
        return []
    keys = (
        "_name",
        "sppci_model",
        "spdisplays_vendor",
        "spdisplays_device-id",
        "spdisplays_vram",
        "spdisplays_vram_shared",
        "spdisplays_metal",
    )
    return [
        {key: adapter[key] for key in keys if key in adapter}
        for adapter in adapters
        if isinstance(adapter, dict)
    ]


def _windows_gpu_devices() -> list[dict[str, Any]]:
    output = _command_output(
        [
            "powershell",
            "-NoProfile",
            "-Command",
            (
                "Get-CimInstance Win32_VideoController | "
                "Select-Object Name,DriverVersion,AdapterRAM,PNPDeviceID | "
                "ConvertTo-Json -Compress"
            ),
        ],
        timeout_seconds=30.0,
    )
    if output is None:
        return []
    try:
        payload = json.loads(output)
    except json.JSONDecodeError:
        return []
    devices = payload if isinstance(payload, list) else [payload]
    return [device for device in devices if isinstance(device, dict)]


def _linux_gpu_devices() -> list[dict[str, Any]]:
    devices: list[dict[str, Any]] = []
    for card in sorted(Path("/sys/class/drm").glob("card[0-9]*")):
        if not card.name.removeprefix("card").isdigit():
            continue
        device_path = card / "device"
        if not device_path.exists():
            continue
        device: dict[str, Any] = {"card": card.name}
        uevent_path = device_path / "uevent"
        if uevent_path.is_file():
            for line in uevent_path.read_text(
                encoding="utf-8", errors="replace"
            ).splitlines():
                if "=" not in line:
                    continue
                key, value = line.split("=", 1)
                device[key.lower()] = value
        driver_path = device_path / "driver"
        if driver_path.exists():
            try:
                device["driver"] = driver_path.resolve().name
            except OSError:
                pass
        devices.append(device)
    return devices


def _host_metadata(
    gpu_name_override: str | None, gpu_driver_override: str | None
) -> dict[str, Any]:
    if sys.platform == "darwin":
        discovered_gpus = _macos_gpu_devices()
    elif sys.platform.startswith("win"):
        discovered_gpus = _windows_gpu_devices()
    elif sys.platform.startswith("linux"):
        discovered_gpus = _linux_gpu_devices()
    else:
        discovered_gpus = []

    return {
        "os": {
            "system": platform.system(),
            "release": platform.release(),
            "version": platform.version(),
            "machine": platform.machine(),
            "python_platform": platform.platform(),
        },
        "cpu": {
            "model": _cpu_model(),
            "logical_cpu_count": os.cpu_count(),
        },
        "memory": {"total_bytes": _total_memory_bytes()},
        "gpu": {
            "name_override": gpu_name_override,
            "driver_override": gpu_driver_override,
            "discovered_devices": discovered_gpus,
            "vulkan_environment": {
                key: os.environ.get(key)
                for key in ("VK_DRIVER_FILES", "VK_ICD_FILENAMES")
                if os.environ.get(key) is not None
            },
        },
    }


def _file_metadata(path: Path) -> dict[str, Any]:
    stat = path.stat()
    return {
        "path": str(path),
        "size_bytes": stat.st_size,
        "mtime_ns": stat.st_mtime_ns,
        "device": stat.st_dev,
        "inode": stat.st_ino,
        "sha256": _sha256_file(path),
    }


def _assert_file_identity(path: Path, expected: dict[str, Any]) -> None:
    try:
        stat = path.stat()
    except OSError as exc:
        raise RuntimeError(
            f"Benchmark provenance file became unavailable: {path}: {exc}"
        ) from exc
    observed = {
        "size_bytes": stat.st_size,
        "mtime_ns": stat.st_mtime_ns,
        "device": stat.st_dev,
        "inode": stat.st_ino,
    }
    changed_fields = [
        name for name, value in observed.items() if value != expected.get(name)
    ]
    if not changed_fields:
        return
    observed_hash = _sha256_file(path)
    raise RuntimeError(
        f"Benchmark provenance changed during the matrix: {path}; "
        f"changed identity fields={changed_fields}; "
        f"expected_sha256={expected.get('sha256')}; observed_sha256={observed_hash}"
    )


def _animation_duration_seconds(source: Path) -> float:
    try:
        payload = json.loads(source.read_text(encoding="utf-8-sig"))
        animation = payload["Animation3D"]
    except (KeyError, TypeError, json.JSONDecodeError) as exc:
        raise ValueError(
            f"Could not read Animation3D object from {source}: {exc}"
        ) from exc
    if not isinstance(animation, dict):
        raise ValueError(f"Animation3D in {source} is not an object")
    raw_duration = animation.get("Duration", DEFAULT_ANIMATION_DURATION_SECONDS)
    try:
        duration = float(raw_duration)
    except (TypeError, ValueError) as exc:
        raise ValueError(
            f"Invalid Animation3D.Duration in {source}: {raw_duration}"
        ) from exc
    if not math.isfinite(duration) or duration < 0.0:
        raise ValueError(f"Invalid Animation3D.Duration in {source}: {duration}")
    return duration


def _expected_animation_frame_count(
    source: Path, fps: int, start_frame: int, end_frame: int
) -> int:
    total_frames = max(1, math.ceil(_animation_duration_seconds(source) * fps))
    if start_frame < 0 or start_frame >= total_frames:
        raise ValueError(
            f"Animation start frame {start_frame} is outside [0, {total_frames}) for {source}"
        )
    resolved_end = (
        total_frames if end_frame < 0 or end_frame > total_frames else end_frame
    )
    if resolved_end <= start_frame:
        raise ValueError(
            f"Animation end frame {end_frame} does not follow start frame "
            f"{start_frame} for {source}"
        )
    return resolved_end - start_frame


def _stats(values: list[float]) -> dict[str, float | int | None]:
    if not values:
        return {
            "count": 0,
            "mean": None,
            "median": None,
            "stddev": None,
            "min": None,
            "max": None,
            "p95": None,
        }
    ordered = sorted(values)

    def percentile(q: float) -> float:
        if len(ordered) == 1:
            return ordered[0]
        position = q * (len(ordered) - 1)
        lower = int(position)
        upper = min(lower + 1, len(ordered) - 1)
        fraction = position - lower
        return ordered[lower] + (ordered[upper] - ordered[lower]) * fraction

    return {
        "count": len(ordered),
        "mean": statistics.fmean(ordered),
        "median": statistics.median(ordered),
        "stddev": statistics.pstdev(ordered) if len(ordered) > 1 else 0.0,
        "min": ordered[0],
        "max": ordered[-1],
        "p95": percentile(0.95),
    }


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
        if resolved.is_file():
            verified.append(resolved)
        else:
            missing.append(str(resolved))
    if missing:
        joined = "\n".join(missing)
        raise FileNotFoundError(f"Missing {kind} inputs:\n{joined}")
    return verified


def _validate_args(args: argparse.Namespace) -> None:
    positive_values = {
        "scene_runs": args.scene_runs,
        "animation_runs": args.animation_runs,
        "scene_width": args.scene_width,
        "scene_height": args.scene_height,
        "animation_width": args.animation_width,
        "animation_height": args.animation_height,
        "animation_fps": args.animation_fps,
    }
    invalid = {name: value for name, value in positive_values.items() if value <= 0}
    if invalid:
        raise ValueError(f"Benchmark counts and dimensions must be positive: {invalid}")
    if args.animation_start_frame < 0:
        raise ValueError("--animation-start-frame must be non-negative")
    if args.child_timeout_seconds < 0.0:
        raise ValueError("--child-timeout-seconds must be non-negative")
    if (
        args.animation_end_frame >= 0
        and args.animation_end_frame <= args.animation_start_frame
    ):
        raise ValueError(
            "--animation-end-frame must be -1 or greater than --animation-start-frame"
        )


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
        "--animation-start-frame",
        type=int,
        default=0,
        help="First animation frame to export (inclusive)",
    )
    parser.add_argument(
        "--animation-end-frame",
        type=int,
        default=-1,
        help="Last animation frame to export (exclusive); -1 exports through the end",
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
        "--child-timeout-seconds",
        type=float,
        default=0.0,
        help=(
            "Maximum wall time for each Atlas export process. Zero keeps the "
            "historical unlimited wait. A timeout fails that run and continues."
        ),
    )
    parser.add_argument(
        "--vulkan-perf-mode",
        choices=("off", "light", "full"),
        default="light",
        help=(
            "Atlas performance instrumentation used for Vulkan runs. Non-off modes "
            "write a unique NDJSON summary inside every run directory."
        ),
    )
    parser.add_argument(
        "--run-label",
        default="",
        help="Optional human-readable label such as 'baseline' or 'candidate'",
    )
    parser.add_argument(
        "--baseline-root",
        default=None,
        help=(
            "Optional prior result root produced by this script. When supplied, write "
            "a complete baseline/candidate duration and output-hash comparison."
        ),
    )
    parser.add_argument(
        "--gpu-name",
        default=None,
        help="Optional authoritative GPU name stored in the run manifest",
    )
    parser.add_argument(
        "--gpu-driver",
        default=None,
        help="Optional authoritative GPU driver/version stored in the run manifest",
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
    perf_mode: str | None = None,
    perf_summary_path: Path | None = None,
) -> list[str]:
    command = [str(atlas_binary)]
    if qt_platform:
        command.extend(["-platform", qt_platform])
    command.extend(extra_args)
    # Append the harness-selected startup backend and perf flags after unrestricted
    # --extra-arg values so those command-line settings cannot be replaced accidentally.
    command.append(f"--atlas_default_render_backend={backend}")
    if perf_mode is not None:
        command.append(f"--atlas_perf_mode={perf_mode}")
    if perf_summary_path is not None:
        command.append(f"--atlas_perf_summary=json:{perf_summary_path}")
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
    timeout_seconds: float,
    dry_run: bool,
) -> tuple[int, float, bool]:
    stdout_log.parent.mkdir(parents=True, exist_ok=True)
    stderr_log.parent.mkdir(parents=True, exist_ok=True)

    if dry_run:
        stdout_log.write_text("dry-run\n", encoding="utf-8")
        stderr_log.write_text("dry-run\n", encoding="utf-8")
        return 0, 0.0, False

    env = os.environ.copy()
    if qt_platform:
        env["QT_QPA_PLATFORM"] = qt_platform
    else:
        # "none"/"native" must mean native selection even when the parent
        # shell exported an offscreen platform for an earlier benchmark.
        env.pop("QT_QPA_PLATFORM", None)

    start = time.monotonic()
    with (
        stdout_log.open("w", encoding="utf-8") as stdout_handle,
        stderr_log.open("w", encoding="utf-8") as stderr_handle,
    ):
        try:
            result = subprocess.run(
                command,
                cwd=REPO_ROOT,
                env=env,
                stdout=stdout_handle,
                stderr=stderr_handle,
                text=True,
                check=False,
                timeout=timeout_seconds if timeout_seconds > 0.0 else None,
            )
        except subprocess.TimeoutExpired:
            elapsed = time.monotonic() - start
            stderr_handle.write(
                f"Atlas exceeded child timeout of {timeout_seconds:.3f} seconds\n"
            )
            return 124, elapsed, True
        except OSError as exc:
            stderr_handle.write(f"Failed to execute Atlas: {exc}\n")
            elapsed = time.monotonic() - start
            return 127, elapsed, False
    elapsed = time.monotonic() - start
    return result.returncode, elapsed, False


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
    perf_mode: str | None = None,
    perf_summary_path: Path | None = None,
) -> list[str]:
    command = _base_command(
        atlas_binary,
        backend,
        qt_platform,
        extra_args,
        perf_mode=perf_mode,
        perf_summary_path=perf_summary_path,
    )
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
    start_frame: int = 0,
    end_frame: int = -1,
    perf_mode: str | None = None,
    perf_summary_path: Path | None = None,
) -> list[str]:
    command = _base_command(
        atlas_binary,
        backend,
        qt_platform,
        extra_args,
        perf_mode=perf_mode,
        perf_summary_path=perf_summary_path,
    )
    command.extend(
        [
            "--run_export_3d_animation",
            "--filename",
            str(source),
            "--output_filename",
            str(output_video),
            "--output_fps",
            str(fps),
            "--output_start_frame",
            str(start_frame),
            "--output_end_frame",
            str(end_frame),
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


def _vulkan_perf_summary_config(
    backend: str, run_dir: Path, vulkan_perf_mode: str
) -> tuple[str | None, Path | None]:
    if backend != "vulkan":
        # Keep OpenGL runs free of collector overhead even if an unrestricted
        # --extra-arg attempted to enable performance collection.
        return "off", None
    if vulkan_perf_mode == "off":
        return "off", None
    return vulkan_perf_mode, run_dir / "vulkan_perf_summary.ndjson"


_PERF_SCHEMA = "atlas.perf.frame"
_CURRENT_PERF_SCHEMA_VERSION = 1
_CURRENT_PERF_PROFILE = f"{_PERF_SCHEMA}/v{_CURRENT_PERF_SCHEMA_VERSION}"
_LEGACY_UNVERSIONED_PERF_PROFILE = "legacy-unversioned-v0"

_NEW_PERF_STATS = (
    "submissions",
    "fence_waits",
)

_LEGACY_UNVERSIONED_PERF_STATS = (
    "all_ms",
    "all_samples",
    "bound_set_rewrites",
    "clears",
    "cones_staged",
    "upload_hi",
    "descriptor_sets",
    "descriptor_writes_recording",
    "ellipsoids_staged",
    "fonts_staged",
    "lines_staged",
    "loads",
    "meshes_staged",
    "pipelines_bound",
    "pipelines_created",
    "readback",
    "segments",
    "spheres_staged",
    "static_staged",
)

_CURRENT_PERF_STATS = _LEGACY_UNVERSIONED_PERF_STATS + _NEW_PERF_STATS
_LEGACY_UNVERSIONED_PERF_TOP_LEVEL_KEYS = frozenset(
    {"frame", "cpu_ms", "gpu_ms", "gpu_scoped_ms", "top", "stats"}
)
_CURRENT_PERF_TOP_LEVEL_KEYS = _LEGACY_UNVERSIONED_PERF_TOP_LEVEL_KEYS | {
    "schema",
    "schema_version",
}


def _is_json_number(value: Any) -> bool:
    return (
        isinstance(value, (int, float))
        and not isinstance(value, bool)
        and math.isfinite(float(value))
    )


def _describe_key_mismatch(
    *, actual: set[str], expected: frozenset[str] | set[str]
) -> str:
    details: list[str] = []
    missing = sorted(expected - actual)
    unexpected = sorted(actual - expected)
    if missing:
        details.append(f"missing keys: {', '.join(missing)}")
    if unexpected:
        details.append(f"unexpected keys: {', '.join(unexpected)}")
    return "; ".join(details)


def _parse_perf_summary(path: Path | None) -> PerfSummaryParseResult:
    if path is None or not path.is_file():
        return PerfSummaryParseResult(0, None, (), ())
    record_count = 0
    errors: list[str] = []
    profiles: set[str] = set()
    with path.open("r", encoding="utf-8", errors="replace") as handle:
        for line_number, line in enumerate(handle, start=1):
            if not line.strip():
                continue
            try:
                record = json.loads(line)
            except json.JSONDecodeError as exc:
                errors.append(f"line {line_number} is not valid JSON: {exc}")
                continue
            record_count += 1
            if not isinstance(record, dict):
                errors.append(f"line {line_number} is not a JSON object")
                continue

            schema_present = "schema" in record
            version_present = "schema_version" in record
            profile: str | None = None
            required_stats: tuple[str, ...] = ()
            expected_top_level_keys: frozenset[str] | set[str] | None = None
            stats = record.get("stats")
            if schema_present or version_present:
                if not schema_present or not version_present:
                    errors.append(
                        f"line {line_number} must provide schema and schema_version together"
                    )
                elif record.get("schema") != _PERF_SCHEMA:
                    errors.append(
                        f"line {line_number} has unsupported perf schema "
                        f"{record.get('schema')!r}"
                    )
                elif not isinstance(record.get("schema_version"), int) or isinstance(
                    record.get("schema_version"), bool
                ):
                    errors.append(
                        f"line {line_number} has no integer perf schema_version"
                    )
                elif record["schema_version"] != _CURRENT_PERF_SCHEMA_VERSION:
                    errors.append(
                        f"line {line_number} has unsupported {_PERF_SCHEMA} schema version "
                        f"{record['schema_version']}"
                    )
                else:
                    profile = _CURRENT_PERF_PROFILE
                    required_stats = _CURRENT_PERF_STATS
                    expected_top_level_keys = _CURRENT_PERF_TOP_LEVEL_KEYS
            elif isinstance(stats, dict):
                has_submission_count = "submissions" in stats
                has_fence_wait_count = "fence_waits" in stats
                if has_submission_count != has_fence_wait_count:
                    errors.append(
                        f"line {line_number} is an ambiguous unversioned perf record with only one "
                        "submission metric"
                    )
                elif has_submission_count:
                    errors.append(
                        f"line {line_number} has the current perf fields but no schema marker"
                    )
                else:
                    profile = _LEGACY_UNVERSIONED_PERF_PROFILE
                    required_stats = _LEGACY_UNVERSIONED_PERF_STATS
                    expected_top_level_keys = _LEGACY_UNVERSIONED_PERF_TOP_LEVEL_KEYS

            if profile is not None:
                profiles.add(profile)
            if expected_top_level_keys is not None:
                key_error = _describe_key_mismatch(
                    actual=set(record), expected=expected_top_level_keys
                )
                if key_error:
                    errors.append(
                        f"line {line_number} does not match {profile} top-level shape "
                        f"({key_error})"
                    )

            if not isinstance(record.get("frame"), int) or isinstance(
                record.get("frame"), bool
            ):
                errors.append(f"line {line_number} has no integer frame token")
            for field in ("cpu_ms", "gpu_ms", "gpu_scoped_ms"):
                if not _is_json_number(record.get(field)):
                    errors.append(f"line {line_number} has no numeric {field}")
            top = record.get("top")
            if not isinstance(top, list):
                errors.append(f"line {line_number} has no top array")
            else:
                for top_index, item in enumerate(top):
                    if not isinstance(item, dict):
                        errors.append(
                            f"line {line_number} top[{top_index}] is not an object"
                        )
                        continue
                    if set(item) != {"label", "ms", "pct"}:
                        errors.append(
                            f"line {line_number} top[{top_index}] has an invalid shape"
                        )
                    if not isinstance(item.get("label"), str):
                        errors.append(
                            f"line {line_number} top[{top_index}] has no string label"
                        )
                    for field in ("ms", "pct"):
                        if not _is_json_number(item.get(field)):
                            errors.append(
                                f"line {line_number} top[{top_index}] has no numeric {field}"
                            )
            if not isinstance(stats, dict):
                errors.append(f"line {line_number} has no stats object")
                continue
            if profile is not None:
                expected_stats = frozenset(required_stats)
                key_error = _describe_key_mismatch(
                    actual=set(stats), expected=expected_stats
                )
                if key_error:
                    errors.append(
                        f"line {line_number} does not match {profile} stats shape "
                        f"({key_error})"
                    )
            for field in required_stats:
                if not _is_json_number(stats.get(field)):
                    errors.append(f"line {line_number} stats has no numeric {field}")
    if len(profiles) > 1:
        errors.append(
            "perf summary mixes record profiles: " + ", ".join(sorted(profiles))
        )
    profile = next(iter(profiles)) if len(profiles) == 1 else None
    unavailable_metrics = (
        _NEW_PERF_STATS if profile == _LEGACY_UNVERSIONED_PERF_PROFILE else ()
    )
    return PerfSummaryParseResult(
        record_count,
        profile,
        unavailable_metrics,
        tuple(errors),
    )


def _validate_run_outputs(
    *,
    returncode: int,
    file_entries: list[dict[str, Any]],
    expected_file_count: int,
    perf_summary_path: Path | None,
    perf_summary_frame_count: int,
    perf_summary_errors: tuple[str, ...],
    timed_out: bool,
    timeout_seconds: float,
    dry_run: bool,
) -> tuple[str, ...]:
    if dry_run:
        return ()

    failures: list[str] = []
    if timed_out:
        failures.append(
            f"Atlas exceeded child timeout of {timeout_seconds:.3f} seconds"
        )
    if returncode != 0:
        failures.append(f"Atlas exited with code {returncode}")
    if len(file_entries) != expected_file_count:
        failures.append(
            f"expected {expected_file_count} output file(s), found {len(file_entries)}"
        )
    zero_byte_outputs = [
        entry["relative_path"] for entry in file_entries if entry["size_bytes"] <= 0
    ]
    if zero_byte_outputs:
        failures.append(f"zero-byte output file(s): {', '.join(zero_byte_outputs)}")
    if perf_summary_path is not None and perf_summary_frame_count == 0:
        failures.append(f"Vulkan perf summary is missing or empty: {perf_summary_path}")
    failures.extend(f"Vulkan perf summary {error}" for error in perf_summary_errors)
    return tuple(failures)


def _run_scene_case(
    atlas_binary: Path,
    output_root: Path,
    backend: str,
    qt_platform: str,
    extra_args: list[str],
    source: Path,
    source_sha256: str,
    output_key: str,
    run_index: int,
    width: int,
    height: int,
    backend_qt_platform: str | None,
    vulkan_perf_mode: str,
    child_timeout_seconds: float,
    dry_run: bool,
) -> tuple[RunRecord, list[dict[str, Any]]]:
    label = source.name
    run_dir = output_root / backend / "scene" / output_key / f"run_{run_index:02d}"
    run_dir.mkdir(parents=True, exist_ok=False)

    output_image = run_dir / "export.png"
    stdout_log = run_dir / "stdout.log"
    stderr_log = run_dir / "stderr.log"
    resolved_qt_platform = _resolve_qt_platform(
        qt_platform, backend, backend_qt_platform
    )
    perf_mode, perf_summary_path = _vulkan_perf_summary_config(
        backend, run_dir, vulkan_perf_mode
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
        perf_mode=perf_mode,
        perf_summary_path=perf_summary_path,
    )
    returncode, duration_seconds, timed_out = _run_process(
        command=command,
        stdout_log=stdout_log,
        stderr_log=stderr_log,
        qt_platform=resolved_qt_platform,
        timeout_seconds=child_timeout_seconds,
        dry_run=dry_run,
    )

    files = _collect_scene_outputs(output_image)
    file_entries, total_bytes = _hash_output_files(run_dir, files)
    _write_checksums(run_dir / "checksums.sha256", file_entries)
    perf_summary = _parse_perf_summary(perf_summary_path)
    failures = _validate_run_outputs(
        returncode=returncode,
        file_entries=file_entries,
        expected_file_count=1,
        perf_summary_path=perf_summary_path,
        perf_summary_frame_count=perf_summary.record_count,
        perf_summary_errors=perf_summary.errors,
        timed_out=timed_out,
        timeout_seconds=child_timeout_seconds,
        dry_run=dry_run,
    )

    run_record = RunRecord(
        kind="scene",
        backend=backend,
        qt_platform=resolved_qt_platform,
        label=label,
        source_path=str(source),
        source_sha256=source_sha256,
        run_index=run_index,
        output_dir=str(run_dir),
        command=command,
        command_shell=" ".join(shlex.quote(part) for part in command),
        returncode=returncode,
        timed_out=timed_out,
        duration_seconds=duration_seconds,
        file_count=len(file_entries),
        expected_file_count=1,
        total_bytes=total_bytes,
        success=not failures,
        failure_reasons=failures,
        perf_summary_path=str(perf_summary_path) if perf_summary_path else None,
        perf_summary_frame_count=perf_summary.record_count,
        perf_summary_profile=perf_summary.profile,
        perf_summary_unavailable_metrics=perf_summary.unavailable_metrics,
        perf_summary_errors=perf_summary.errors,
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
    source_sha256: str,
    output_key: str,
    run_index: int,
    width: int,
    height: int,
    fps: int,
    start_frame: int,
    end_frame: int,
    expected_frame_count: int,
    backend_qt_platform: str | None,
    vulkan_perf_mode: str,
    child_timeout_seconds: float,
    dry_run: bool,
) -> tuple[RunRecord, list[dict[str, Any]]]:
    label = source.name
    run_dir = output_root / backend / "animation" / output_key / f"run_{run_index:02d}"
    run_dir.mkdir(parents=True, exist_ok=False)

    frames_dir = run_dir / "frames"
    frames_dir.mkdir(parents=True, exist_ok=False)
    output_video = run_dir / f"{source.stem}.mp4"
    stdout_log = run_dir / "stdout.log"
    stderr_log = run_dir / "stderr.log"
    resolved_qt_platform = _resolve_qt_platform(
        qt_platform, backend, backend_qt_platform
    )
    perf_mode, perf_summary_path = _vulkan_perf_summary_config(
        backend, run_dir, vulkan_perf_mode
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
        start_frame=start_frame,
        end_frame=end_frame,
        perf_mode=perf_mode,
        perf_summary_path=perf_summary_path,
    )
    returncode, duration_seconds, timed_out = _run_process(
        command=command,
        stdout_log=stdout_log,
        stderr_log=stderr_log,
        qt_platform=resolved_qt_platform,
        timeout_seconds=child_timeout_seconds,
        dry_run=dry_run,
    )

    files = _collect_animation_outputs(frames_dir)
    file_entries, total_bytes = _hash_output_files(run_dir, files)
    _write_checksums(run_dir / "checksums.sha256", file_entries)
    perf_summary = _parse_perf_summary(perf_summary_path)
    failures = _validate_run_outputs(
        returncode=returncode,
        file_entries=file_entries,
        expected_file_count=expected_frame_count,
        perf_summary_path=perf_summary_path,
        perf_summary_frame_count=perf_summary.record_count,
        perf_summary_errors=perf_summary.errors,
        timed_out=timed_out,
        timeout_seconds=child_timeout_seconds,
        dry_run=dry_run,
    )

    run_record = RunRecord(
        kind="animation",
        backend=backend,
        qt_platform=resolved_qt_platform,
        label=label,
        source_path=str(source),
        source_sha256=source_sha256,
        run_index=run_index,
        output_dir=str(run_dir),
        command=command,
        command_shell=" ".join(shlex.quote(part) for part in command),
        returncode=returncode,
        timed_out=timed_out,
        duration_seconds=duration_seconds,
        file_count=len(file_entries),
        expected_file_count=expected_frame_count,
        total_bytes=total_bytes,
        success=not failures,
        failure_reasons=failures,
        perf_summary_path=str(perf_summary_path) if perf_summary_path else None,
        perf_summary_frame_count=perf_summary.record_count,
        perf_summary_profile=perf_summary.profile,
        perf_summary_unavailable_metrics=perf_summary.unavailable_metrics,
        perf_summary_errors=perf_summary.errors,
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
                "source_sha256",
                "run_index",
                "returncode",
                "timed_out",
                "success",
                "failure_reasons",
                "duration_seconds",
                "file_count",
                "expected_file_count",
                "total_bytes",
                "perf_summary_path",
                "perf_summary_frame_count",
                "perf_summary_profile",
                "perf_summary_unavailable_metrics",
                "perf_summary_errors",
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
                    run.source_sha256,
                    run.run_index,
                    run.returncode,
                    run.timed_out,
                    run.success,
                    " | ".join(run.failure_reasons),
                    f"{run.duration_seconds:.6f}",
                    run.file_count,
                    run.expected_file_count,
                    run.total_bytes,
                    run.perf_summary_path or "",
                    run.perf_summary_frame_count,
                    run.perf_summary_profile or "",
                    " | ".join(run.perf_summary_unavailable_metrics),
                    " | ".join(run.perf_summary_errors),
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
                "source_sha256",
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
                    row["source_sha256"],
                    row["run_index"],
                    row["relative_path"],
                    row["size_bytes"],
                    row["sha256"],
                    row["output_dir"],
                ]
            )


def _build_stability_summary(
    rows: list[dict[str, Any]],
    expected_groups: dict[RunGroupKey, int] | None = None,
) -> dict[str, Any]:
    grouped: dict[tuple[str, str, str, str, str, str], list[dict[str, Any]]] = (
        defaultdict(list)
    )
    for row in rows:
        key = (
            row["kind"],
            row["backend"],
            row["label"],
            row["source_sha256"],
            row["source_path"],
            row["relative_path"],
        )
        grouped[key].append(row)

    summary_rows: list[dict[str, Any]] = []
    for (
        kind,
        backend,
        label,
        source_sha256,
        source_path,
        relative_path,
    ), group_rows in sorted(grouped.items()):
        hashes = sorted({row["sha256"] for row in group_rows})
        expected_run_count = (
            expected_groups.get((kind, backend, label, source_sha256, source_path))
            if expected_groups is not None
            else None
        )
        complete_run_coverage = (
            len(group_rows) == expected_run_count
            if expected_run_count is not None
            else None
        )
        summary_rows.append(
            {
                "kind": kind,
                "backend": backend,
                "label": label,
                "source_sha256": source_sha256,
                "source_path": source_path,
                "relative_path": relative_path,
                "run_count": len(group_rows),
                "expected_run_count": expected_run_count,
                "complete_run_coverage": complete_run_coverage,
                "unique_hash_count": len(hashes),
                "all_hashes_identical": len(hashes) == 1,
                "stable_and_complete": len(hashes) == 1
                and complete_run_coverage is not False,
                "hashes": hashes,
            }
        )

    return {
        "generated_at": datetime.now().isoformat(),
        "entries": summary_rows,
    }


def _run_group_key(run: RunRecord) -> RunGroupKey:
    return (
        run.kind,
        run.backend,
        run.label,
        run.source_sha256,
        run.source_path,
    )


def _group_dict_key(entry: dict[str, Any]) -> RunGroupKey:
    return (
        str(entry["kind"]),
        str(entry["backend"]),
        str(entry["label"]),
        str(entry["source_sha256"]),
        str(entry["source_path"]),
    )


def _build_aggregate_summary(
    *,
    runs: list[RunRecord],
    expected_groups: dict[RunGroupKey, int],
    expected_source_paths: dict[RunGroupKey, str],
    harness_errors: list[str],
    stability_summary: dict[str, Any] | None,
    dry_run: bool,
    run_label: str,
) -> dict[str, Any]:
    grouped: dict[RunGroupKey, list[RunRecord]] = defaultdict(list)
    for run in runs:
        grouped[_run_group_key(run)].append(run)

    group_entries: list[dict[str, Any]] = []
    failures: list[dict[str, Any]] = []
    for key in sorted(set(expected_groups) | set(grouped)):
        kind, backend, label, source_sha256, source_path = key
        group_runs = sorted(grouped.get(key, []), key=lambda run: run.run_index)
        expected_run_count = expected_groups.get(key, 0)
        successful_runs = [run for run in group_runs if run.success]
        complete = len(group_runs) == expected_run_count
        group_success = complete and len(successful_runs) == expected_run_count
        durations = [run.duration_seconds for run in successful_runs]
        group_entries.append(
            {
                "kind": kind,
                "backend": backend,
                "label": label,
                "source_path": expected_source_paths.get(key, source_path),
                "source_sha256": source_sha256,
                "expected_run_count": expected_run_count,
                "recorded_run_count": len(group_runs),
                "successful_run_count": len(successful_runs),
                "complete": complete,
                "success": group_success,
                "duration_seconds": _stats(durations),
            }
        )
        if not complete:
            failures.append(
                {
                    "kind": kind,
                    "backend": backend,
                    "label": label,
                    "source_path": source_path,
                    "source_sha256": source_sha256,
                    "run_index": None,
                    "reasons": [
                        f"expected {expected_run_count} run(s), recorded {len(group_runs)}"
                    ],
                }
            )
        for run in group_runs:
            if not run.success:
                failures.append(
                    {
                        "kind": run.kind,
                        "backend": run.backend,
                        "label": run.label,
                        "source_path": run.source_path,
                        "source_sha256": run.source_sha256,
                        "run_index": run.run_index,
                        "reasons": list(run.failure_reasons),
                        "stdout_log": run.stdout_log,
                        "stderr_log": run.stderr_log,
                    }
                )

    backend_comparisons: list[dict[str, Any]] = []
    by_case: dict[tuple[str, str, str, str], dict[str, dict[str, Any]]] = defaultdict(
        dict
    )
    for entry in group_entries:
        by_case[
            (
                entry["kind"],
                entry["label"],
                entry["source_sha256"],
                entry["source_path"],
            )
        ][entry["backend"]] = entry
    for (kind, label, source_sha256, source_path), backends in sorted(by_case.items()):
        opengl = backends.get("opengl")
        vulkan = backends.get("vulkan")
        if opengl is None or vulkan is None:
            continue
        opengl_median = opengl["duration_seconds"]["median"]
        vulkan_median = vulkan["duration_seconds"]["median"]
        delta_seconds = None
        delta_percent = None
        if opengl_median is not None and vulkan_median is not None:
            delta_seconds = vulkan_median - opengl_median
            if opengl_median != 0.0:
                delta_percent = 100.0 * delta_seconds / opengl_median
        backend_comparisons.append(
            {
                "kind": kind,
                "label": label,
                "source_sha256": source_sha256,
                "source_path": source_path,
                "opengl_median_seconds": opengl_median,
                "vulkan_median_seconds": vulkan_median,
                "vulkan_minus_opengl_seconds": delta_seconds,
                "vulkan_minus_opengl_percent": delta_percent,
            }
        )

    stability_failure_count = 0
    if stability_summary is not None:
        for entry in stability_summary.get("entries", []):
            reasons: list[str] = []
            if entry.get("complete_run_coverage") is False:
                reasons.append(
                    f"output {entry['relative_path']} appeared in "
                    f"{entry['run_count']} of {entry['expected_run_count']} run(s)"
                )
            if not entry.get("all_hashes_identical", False):
                reasons.append(
                    f"output {entry['relative_path']} is nondeterministic; "
                    f"hashes={entry.get('hashes', [])}"
                )
            if not reasons:
                continue
            stability_failure_count += 1
            failures.append(
                {
                    "kind": entry["kind"],
                    "backend": entry["backend"],
                    "label": entry["label"],
                    "source_path": entry["source_path"],
                    "source_sha256": entry["source_sha256"],
                    "run_index": None,
                    "reasons": reasons,
                }
            )

    expected_run_count = sum(expected_groups.values())
    all_complete = len(runs) == expected_run_count and all(
        entry["complete"] for entry in group_entries
    )
    all_successful = (
        all(run.success for run in runs)
        and not harness_errors
        and stability_failure_count == 0
    )
    if dry_run and all_complete and all_successful:
        status = "dry_run"
    elif all_complete and all_successful:
        status = "passed"
    else:
        status = "failed"

    return {
        "schema_version": 1,
        "generated_at": datetime.now().isoformat(),
        "run_label": run_label,
        "status": status,
        "dry_run": dry_run,
        "expected_run_count": expected_run_count,
        "recorded_run_count": len(runs),
        "successful_run_count": sum(run.success for run in runs),
        "all_runs_complete": all_complete,
        "groups": group_entries,
        "backend_comparisons": backend_comparisons,
        "stability_failure_count": stability_failure_count,
        "failures": failures,
        "harness_errors": harness_errors,
    }


def _format_number(value: float | int | None, digits: int = 3) -> str:
    if value is None:
        return "n/a"
    return f"{float(value):.{digits}f}"


def _markdown_text(value: Any) -> str:
    return str(value).replace("|", "\\|").replace("\n", " ")


def _write_aggregate_markdown(output_root: Path, summary: dict[str, Any]) -> None:
    lines = [
        "# Atlas Export Benchmark Summary",
        "",
        f"Status: **{summary['status']}**",
        "",
        f"Runs: {summary['recorded_run_count']} recorded / "
        f"{summary['expected_run_count']} expected; "
        f"{summary['successful_run_count']} successful.",
        "",
        "## Per-case durations",
        "",
        "| Kind | Backend | Case | Successful / expected | Median (s) | P95 (s) | Mean (s) |",
        "| --- | --- | --- | ---: | ---: | ---: | ---: |",
    ]
    for entry in summary["groups"]:
        stats = entry["duration_seconds"]
        lines.append(
            (
                "| {kind} | {backend} | {label} | {successful} / {expected} | "
                "{median} | {p95} | {mean} |"
            ).format(
                kind=_markdown_text(entry["kind"]),
                backend=_markdown_text(entry["backend"]),
                label=_markdown_text(entry["label"]),
                successful=entry["successful_run_count"],
                expected=entry["expected_run_count"],
                median=_format_number(stats["median"]),
                p95=_format_number(stats["p95"]),
                mean=_format_number(stats["mean"]),
            )
        )

    if summary["backend_comparisons"]:
        lines.extend(
            [
                "",
                "## Vulkan versus OpenGL",
                "",
                "Negative deltas mean Vulkan completed faster.",
                "",
                "| Kind | Case | OpenGL median (s) | Vulkan median (s) | "
                "Vulkan - OpenGL (s) | Delta |",
                "| --- | --- | ---: | ---: | ---: | ---: |",
            ]
        )
        for entry in summary["backend_comparisons"]:
            delta_percent = entry["vulkan_minus_opengl_percent"]
            lines.append(
                "| {kind} | {label} | {gl} | {vk} | {delta} | {percent} |".format(
                    kind=_markdown_text(entry["kind"]),
                    label=_markdown_text(entry["label"]),
                    gl=_format_number(entry["opengl_median_seconds"]),
                    vk=_format_number(entry["vulkan_median_seconds"]),
                    delta=_format_number(entry["vulkan_minus_opengl_seconds"]),
                    percent=(
                        "n/a"
                        if delta_percent is None
                        else f"{float(delta_percent):+.2f}%"
                    ),
                )
            )

    if summary["failures"] or summary["harness_errors"]:
        lines.extend(["", "## Failures", ""])
        for failure in summary["failures"]:
            location = "/".join(
                str(part)
                for part in (
                    failure["backend"],
                    failure["kind"],
                    failure["label"],
                    failure.get("run_index"),
                )
                if part is not None
            )
            lines.append(
                f"- {_markdown_text(location)}: "
                + "; ".join(_markdown_text(reason) for reason in failure["reasons"])
            )
        for error in summary["harness_errors"]:
            lines.append(f"- Harness: {_markdown_text(error)}")

    (output_root / "aggregate_summary.md").write_text(
        "\n".join(lines) + "\n", encoding="utf-8"
    )


def _stability_signatures(
    stability_summary: dict[str, Any],
) -> dict[RunGroupKey, dict[str, tuple[str, ...]]]:
    signatures: dict[RunGroupKey, dict[str, tuple[str, ...]]] = defaultdict(dict)
    for entry in stability_summary.get("entries", []):
        key = _group_dict_key(entry)
        signatures[key][str(entry["relative_path"])] = tuple(entry.get("hashes", []))
    return signatures


def _stable_complete_groups(
    stability_summary: dict[str, Any],
) -> dict[RunGroupKey, bool]:
    values: dict[RunGroupKey, list[bool]] = defaultdict(list)
    for entry in stability_summary.get("entries", []):
        values[_group_dict_key(entry)].append(bool(entry.get("stable_and_complete")))
    return {key: bool(group) and all(group) for key, group in values.items()}


def _manifest_input_signature(manifest: dict[str, Any]) -> list[dict[str, Any]]:
    signature: list[dict[str, Any]] = []
    for entry in manifest.get("input_files", []):
        if not isinstance(entry, dict):
            continue
        signature.append(
            {
                "kind": entry.get("kind"),
                "path": entry.get("path"),
                "sha256": entry.get("sha256"),
                "expected_frame_count": entry.get("expected_frame_count"),
            }
        )
    return sorted(signature, key=lambda entry: json.dumps(entry, sort_keys=True))


def _manifest_compatibility(
    baseline: dict[str, Any], candidate: dict[str, Any]
) -> dict[str, Any]:
    baseline_values = {
        "host": baseline.get("host"),
        "backends": baseline.get("backends"),
        "qt_platform": baseline.get("qt_platform"),
        "qt_platform_overrides": baseline.get("qt_platform_overrides"),
        "scene_runs": baseline.get("scene_runs"),
        "animation_runs": baseline.get("animation_runs"),
        "scene_size": baseline.get("scene_size"),
        "animation_size": baseline.get("animation_size"),
        "extra_args": baseline.get("extra_args"),
        "vulkan_perf_mode": baseline.get("vulkan_perf_mode"),
        "child_timeout_seconds": baseline.get("child_timeout_seconds", 0.0),
        "inputs": _manifest_input_signature(baseline),
    }
    candidate_values = {
        "host": candidate.get("host"),
        "backends": candidate.get("backends"),
        "qt_platform": candidate.get("qt_platform"),
        "qt_platform_overrides": candidate.get("qt_platform_overrides"),
        "scene_runs": candidate.get("scene_runs"),
        "animation_runs": candidate.get("animation_runs"),
        "scene_size": candidate.get("scene_size"),
        "animation_size": candidate.get("animation_size"),
        "extra_args": candidate.get("extra_args"),
        "vulkan_perf_mode": candidate.get("vulkan_perf_mode"),
        "child_timeout_seconds": candidate.get("child_timeout_seconds", 0.0),
        "inputs": _manifest_input_signature(candidate),
    }
    checks = [
        {
            "field": field,
            "compatible": baseline_values[field] == candidate_values[field],
            "baseline": baseline_values[field],
            "candidate": candidate_values[field],
        }
        for field in baseline_values
    ]
    return {
        "compatible": all(check["compatible"] for check in checks),
        "checks": checks,
        "mismatches": [check for check in checks if not check["compatible"]],
    }


def _build_baseline_comparison(
    *,
    baseline_root: Path,
    candidate_manifest: dict[str, Any],
    candidate_summary: dict[str, Any],
    candidate_stability: dict[str, Any],
) -> dict[str, Any]:
    baseline_summary_path = baseline_root / "aggregate_summary.json"
    baseline_stability_path = baseline_root / "stability_summary.json"
    baseline_manifest_path = baseline_root / "manifest.json"
    if (
        not baseline_summary_path.is_file()
        or not baseline_stability_path.is_file()
        or not baseline_manifest_path.is_file()
    ):
        raise FileNotFoundError(
            "Baseline root must contain manifest.json, aggregate_summary.json, "
            f"and stability_summary.json: {baseline_root}"
        )
    baseline_summary = json.loads(baseline_summary_path.read_text(encoding="utf-8"))
    baseline_stability = json.loads(baseline_stability_path.read_text(encoding="utf-8"))
    baseline_manifest = json.loads(baseline_manifest_path.read_text(encoding="utf-8"))
    compatibility = _manifest_compatibility(baseline_manifest, candidate_manifest)

    baseline_groups = {
        _group_dict_key(entry): entry for entry in baseline_summary.get("groups", [])
    }
    candidate_groups = {
        _group_dict_key(entry): entry for entry in candidate_summary.get("groups", [])
    }
    baseline_signatures = _stability_signatures(baseline_stability)
    candidate_signatures = _stability_signatures(candidate_stability)
    baseline_stability_status = _stable_complete_groups(baseline_stability)
    candidate_stability_status = _stable_complete_groups(candidate_stability)

    cases: list[dict[str, Any]] = []
    for key in sorted(set(baseline_groups) | set(candidate_groups)):
        kind, backend, label, source_sha256, source_path = key
        baseline = baseline_groups.get(key)
        candidate = candidate_groups.get(key)
        baseline_median = (
            baseline["duration_seconds"]["median"] if baseline is not None else None
        )
        candidate_median = (
            candidate["duration_seconds"]["median"] if candidate is not None else None
        )
        delta_seconds = None
        delta_percent = None
        if (
            compatibility["compatible"]
            and baseline_median is not None
            and candidate_median is not None
        ):
            delta_seconds = candidate_median - baseline_median
            if baseline_median != 0.0:
                delta_percent = 100.0 * delta_seconds / baseline_median
        baseline_outputs = baseline_signatures.get(key, {})
        candidate_outputs = candidate_signatures.get(key, {})
        baseline_stable = baseline_stability_status.get(key, False)
        candidate_stable = candidate_stability_status.get(key, False)
        baseline_success = bool(baseline is not None and baseline.get("success"))
        candidate_success = bool(candidate is not None and candidate.get("success"))
        cases.append(
            {
                "kind": kind,
                "backend": backend,
                "label": label,
                "source_sha256": source_sha256,
                "source_path": source_path,
                "baseline_present": baseline is not None,
                "candidate_present": candidate is not None,
                "baseline_median_seconds": baseline_median,
                "candidate_median_seconds": candidate_median,
                "candidate_minus_baseline_seconds": delta_seconds,
                "candidate_minus_baseline_percent": delta_percent,
                "baseline_outputs_stable": baseline_stable,
                "candidate_outputs_stable": candidate_stable,
                "output_hash_sets_match": baseline_success
                and candidate_success
                and baseline_stable
                and candidate_stable
                and baseline_outputs == candidate_outputs,
            }
        )

    return {
        "schema_version": 1,
        "generated_at": datetime.now().isoformat(),
        "baseline_root": str(baseline_root),
        "baseline_run_label": baseline_summary.get("run_label", ""),
        "candidate_run_label": candidate_summary.get("run_label", ""),
        "status": "compatible" if compatibility["compatible"] else "incompatible",
        "compatibility": compatibility,
        "cases": cases,
    }


def _write_comparison_markdown(output_root: Path, comparison: dict[str, Any]) -> None:
    lines = [
        "# Atlas Baseline/Candidate Comparison",
        "",
        f"Baseline: `{_markdown_text(comparison['baseline_root'])}`",
        "",
        f"Compatibility: **{comparison['status']}**",
        "",
        "Positive deltas mean the candidate completed more slowly.",
    ]
    if not comparison["compatibility"]["compatible"]:
        lines.extend(["", "## Compatibility mismatches", ""])
        for mismatch in comparison["compatibility"]["mismatches"]:
            lines.append(
                f"- `{_markdown_text(mismatch['field'])}`: baseline="
                f"`{_markdown_text(json.dumps(mismatch['baseline'], sort_keys=True))}`; "
                f"candidate=`{_markdown_text(json.dumps(mismatch['candidate'], sort_keys=True))}`"
            )
        lines.extend(
            ["", "Timing deltas are suppressed until these settings match.", ""]
        )
    lines.extend(
        [
            "",
            "| Kind | Backend | Case | Baseline median (s) | Candidate median (s) | "
            "Delta (s) | Delta | Stable outputs | Hash sets match |",
            "| --- | --- | --- | ---: | ---: | ---: | ---: | --- | --- |",
        ]
    )
    for entry in comparison["cases"]:
        delta_percent = entry["candidate_minus_baseline_percent"]
        stable_outputs = (
            f"{entry['baseline_outputs_stable']} / {entry['candidate_outputs_stable']}"
        )
        lines.append(
            (
                "| {kind} | {backend} | {label} | {baseline} | {candidate} | "
                "{delta} | {percent} | {stable} | {match} |"
            ).format(
                kind=_markdown_text(entry["kind"]),
                backend=_markdown_text(entry["backend"]),
                label=_markdown_text(entry["label"]),
                baseline=_format_number(entry["baseline_median_seconds"]),
                candidate=_format_number(entry["candidate_median_seconds"]),
                delta=_format_number(entry["candidate_minus_baseline_seconds"]),
                percent=(
                    "n/a" if delta_percent is None else f"{float(delta_percent):+.2f}%"
                ),
                stable=stable_outputs,
                match=entry["output_hash_sets_match"],
            )
        )
    (output_root / "comparison.md").write_text(
        "\n".join(lines) + "\n", encoding="utf-8"
    )


def main() -> int:
    args = _parse_args()
    _validate_args(args)

    atlas_binary = _resolve_atlas_binary(args.atlas_path)
    baseline_root = (
        Path(args.baseline_root).expanduser().resolve()
        if args.baseline_root is not None
        else None
    )
    if baseline_root is not None:
        required_baseline_files = (
            baseline_root / "manifest.json",
            baseline_root / "aggregate_summary.json",
            baseline_root / "stability_summary.json",
        )
        missing_baseline_files = [
            str(path) for path in required_baseline_files if not path.is_file()
        ]
        if missing_baseline_files:
            raise FileNotFoundError(
                "Baseline result is incomplete; missing:\n"
                + "\n".join(missing_baseline_files)
            )

    output_root = Path(args.output_root).expanduser().resolve()
    if output_root.exists():
        raise FileExistsError(f"Output root already exists: {output_root}")

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
    if len(backends) != len(set(backends)):
        raise ValueError(f"Duplicate --backend values are not allowed: {backends}")

    atlas_binary_metadata = _file_metadata(atlas_binary)
    input_metadata = {
        str(path): {"kind": "scene", **_file_metadata(path)} for path in scenes
    }
    input_metadata.update(
        {
            str(path): {
                "kind": "animation",
                **_file_metadata(path),
                "duration_seconds": _animation_duration_seconds(path),
                "expected_frame_count": _expected_animation_frame_count(
                    path,
                    args.animation_fps,
                    args.animation_start_frame,
                    args.animation_end_frame,
                ),
            }
            for path in animations
        }
    )
    output_root.mkdir(parents=True, exist_ok=False)

    manifest = {
        "schema_version": 1,
        "generated_at": datetime.now().isoformat(),
        "run_label": args.run_label,
        "repo_root": str(REPO_ROOT),
        "git": _git_metadata(),
        "atlas_binary": str(atlas_binary),
        "atlas_binary_metadata": atlas_binary_metadata,
        "output_root": str(output_root),
        "host_platform": sys.platform,
        "host": _host_metadata(args.gpu_name, args.gpu_driver),
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
            "start_frame": args.animation_start_frame,
            "end_frame": args.animation_end_frame,
        },
        "extra_args": args.extra_arg,
        "vulkan_perf_mode": args.vulkan_perf_mode,
        "child_timeout_seconds": args.child_timeout_seconds,
        "scenes": [str(path) for path in scenes],
        "animations": [str(path) for path in animations],
        "input_files": list(input_metadata.values()),
        "dataset_hashes": {
            path: metadata["sha256"] for path, metadata in input_metadata.items()
        },
        "baseline_root": str(baseline_root) if baseline_root else None,
        "dry_run": bool(args.dry_run),
    }
    _write_json(output_root / "manifest.json", manifest)

    run_records: list[RunRecord] = []
    file_hash_rows: list[dict[str, Any]] = []
    backend_qt_platform_overrides = {
        "opengl": args.qt_platform_opengl,
        "vulkan": args.qt_platform_vulkan,
    }

    scene_cases = _build_export_cases("scene", scenes)
    animation_cases = _build_export_cases("animation", animations)

    expected_groups: dict[RunGroupKey, int] = {}
    expected_source_paths: dict[RunGroupKey, str] = {}
    for backend in backends:
        for case in (*scene_cases, *animation_cases):
            source_metadata = input_metadata[case.source_path]
            key = (
                case.kind,
                backend,
                case.label,
                source_metadata["sha256"],
                case.source_path,
            )
            expected_groups[key] = (
                args.scene_runs if case.kind == "scene" else args.animation_runs
            )
            expected_source_paths[key] = case.source_path

    harness_errors: list[str] = []
    try:
        for backend in backends:
            for case in scene_cases:
                source = Path(case.source_path)
                source_sha256 = input_metadata[case.source_path]["sha256"]
                for run_index in range(1, args.scene_runs + 1):
                    _assert_file_identity(atlas_binary, atlas_binary_metadata)
                    _assert_file_identity(source, input_metadata[case.source_path])
                    run_record, file_entries = _run_scene_case(
                        atlas_binary=atlas_binary,
                        output_root=output_root,
                        backend=backend,
                        qt_platform=args.qt_platform,
                        extra_args=args.extra_arg,
                        source=source,
                        source_sha256=source_sha256,
                        output_key=case.output_key,
                        run_index=run_index,
                        width=args.scene_width,
                        height=args.scene_height,
                        backend_qt_platform=backend_qt_platform_overrides.get(backend),
                        vulkan_perf_mode=args.vulkan_perf_mode,
                        child_timeout_seconds=args.child_timeout_seconds,
                        dry_run=args.dry_run,
                    )
                    _assert_file_identity(atlas_binary, atlas_binary_metadata)
                    _assert_file_identity(source, input_metadata[case.source_path])
                    run_records.append(run_record)
                    for entry in file_entries:
                        file_hash_rows.append(
                            {
                                "kind": "scene",
                                "backend": backend,
                                "label": case.label,
                                "source_path": case.source_path,
                                "source_sha256": source_sha256,
                                "run_index": run_index,
                                "relative_path": entry["relative_path"],
                                "size_bytes": entry["size_bytes"],
                                "sha256": entry["sha256"],
                                "output_dir": run_record.output_dir,
                            }
                        )

            for case in animation_cases:
                source = Path(case.source_path)
                animation_metadata = input_metadata[case.source_path]
                source_sha256 = animation_metadata["sha256"]
                expected_frame_count = animation_metadata["expected_frame_count"]
                for run_index in range(1, args.animation_runs + 1):
                    _assert_file_identity(atlas_binary, atlas_binary_metadata)
                    _assert_file_identity(source, animation_metadata)
                    run_record, file_entries = _run_animation_case(
                        atlas_binary=atlas_binary,
                        output_root=output_root,
                        backend=backend,
                        qt_platform=args.qt_platform,
                        extra_args=args.extra_arg,
                        source=source,
                        source_sha256=source_sha256,
                        output_key=case.output_key,
                        run_index=run_index,
                        width=args.animation_width,
                        height=args.animation_height,
                        fps=args.animation_fps,
                        start_frame=args.animation_start_frame,
                        end_frame=args.animation_end_frame,
                        expected_frame_count=expected_frame_count,
                        backend_qt_platform=backend_qt_platform_overrides.get(backend),
                        vulkan_perf_mode=args.vulkan_perf_mode,
                        child_timeout_seconds=args.child_timeout_seconds,
                        dry_run=args.dry_run,
                    )
                    _assert_file_identity(atlas_binary, atlas_binary_metadata)
                    _assert_file_identity(source, animation_metadata)
                    run_records.append(run_record)
                    for entry in file_entries:
                        file_hash_rows.append(
                            {
                                "kind": "animation",
                                "backend": backend,
                                "label": case.label,
                                "source_path": case.source_path,
                                "source_sha256": source_sha256,
                                "run_index": run_index,
                                "relative_path": entry["relative_path"],
                                "size_bytes": entry["size_bytes"],
                                "sha256": entry["sha256"],
                                "output_dir": run_record.output_dir,
                            }
                        )
    except Exception as exc:  # Keep a useful partial aggregate on harness failures.
        harness_errors.append(f"{type(exc).__name__}: {exc}")

    try:
        _assert_file_identity(atlas_binary, atlas_binary_metadata)
        for source_path, source_metadata in input_metadata.items():
            _assert_file_identity(Path(source_path), source_metadata)
    except Exception as exc:
        harness_errors.append(
            f"Final provenance check failed: {type(exc).__name__}: {exc}"
        )

    _write_run_summary_csv(output_root, run_records)
    _write_file_hashes_csv(output_root, file_hash_rows)
    stability_summary = _build_stability_summary(
        file_hash_rows, expected_groups=expected_groups
    )
    _write_json(output_root / "stability_summary.json", stability_summary)

    aggregate_summary = _build_aggregate_summary(
        runs=run_records,
        expected_groups=expected_groups,
        expected_source_paths=expected_source_paths,
        harness_errors=harness_errors,
        stability_summary=stability_summary,
        dry_run=args.dry_run,
        run_label=args.run_label,
    )
    if baseline_root is not None:
        try:
            comparison = _build_baseline_comparison(
                baseline_root=baseline_root,
                candidate_manifest=manifest,
                candidate_summary=aggregate_summary,
                candidate_stability=stability_summary,
            )
            _write_json(output_root / "comparison.json", comparison)
            _write_comparison_markdown(output_root, comparison)
            if not comparison["compatibility"]["compatible"]:
                harness_errors.append(
                    "Baseline/candidate settings or hardware are incompatible; "
                    "timing deltas were suppressed"
                )
                aggregate_summary = _build_aggregate_summary(
                    runs=run_records,
                    expected_groups=expected_groups,
                    expected_source_paths=expected_source_paths,
                    harness_errors=harness_errors,
                    stability_summary=stability_summary,
                    dry_run=args.dry_run,
                    run_label=args.run_label,
                )
        except Exception as exc:
            harness_errors.append(
                f"Baseline comparison failed: {type(exc).__name__}: {exc}"
            )
            aggregate_summary = _build_aggregate_summary(
                runs=run_records,
                expected_groups=expected_groups,
                expected_source_paths=expected_source_paths,
                harness_errors=harness_errors,
                stability_summary=stability_summary,
                dry_run=args.dry_run,
                run_label=args.run_label,
            )
    _write_json(output_root / "aggregate_summary.json", aggregate_summary)
    _write_aggregate_markdown(output_root, aggregate_summary)

    print(f"Atlas binary: {atlas_binary}")
    print(f"Results written to: {output_root}")
    print(f"Runs recorded: {len(run_records)}")
    print(f"Files hashed: {len(file_hash_rows)}")
    if args.dry_run:
        print("Dry run only; Atlas commands were not executed.")

    if aggregate_summary["status"] == "failed":
        print(
            f"Benchmark failed; see {output_root / 'aggregate_summary.md'}",
            file=sys.stderr,
        )
        return 1
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:  # pragma: no cover - CLI failure reporting
        print(str(exc), file=sys.stderr)
        raise SystemExit(1)
