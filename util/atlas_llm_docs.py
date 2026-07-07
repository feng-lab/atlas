import os
import shutil
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

import common_dirs

_REQUIRED_SCHEMA_FILES: tuple[str, ...] = (
    "animation3d.schema.json",
    "capabilities.json",
    "supported_file_formats.json",
)

_SCHEMA_DUMP_DIAGNOSTICS_ENV = "ATLAS_SCHEMA_DUMP_DIAGNOSTICS"
_WINDOWS_CRASH_DIAGNOSTIC_SETTLE_SECONDS_ENV = (
    "ATLAS_SCHEMA_DUMP_CRASH_DIAGNOSTIC_SETTLE_SECONDS"
)
_WINDOWS_EVENT_GRACE_SECONDS_ENV = "ATLAS_SCHEMA_DUMP_EVENT_GRACE_SECONDS"
_DIAGNOSTIC_COMMAND_TIMEOUT_SECONDS_ENV = "ATLAS_SCHEMA_DUMP_DIAGNOSTIC_TIMEOUT_SECONDS"

# Diagnostics are printed only for the current schema-dump failure window. The
# event grace covers timestamp skew and host crash-report delays without dumping
# unrelated event log history; override it from CI if WER is unusually slow.
_DEFAULT_WINDOWS_EVENT_GRACE_SECONDS = 2 * 60
_DEFAULT_WINDOWS_CRASH_DIAGNOSTIC_SETTLE_SECONDS = 3.0
_DEFAULT_DIAGNOSTIC_COMMAND_TIMEOUT_SECONDS = 60.0

_RELEVANT_ENV_VARS: tuple[str, ...] = (
    "CI",
    "GITHUB_ACTIONS",
    "GITHUB_WORKSPACE",
    "RUNNER_OS",
    "RUNNER_TEMP",
    "RUNNER_TOOL_CACHE",
    "CONDA_DEFAULT_ENV",
    "VIRTUAL_ENV",
    "VK_DRIVER_FILES",
    "VK_ICD_FILENAMES",
    "VK_LAYER_PATH",
    "VK_ADD_LAYER_PATH",
    "VK_LOADER_DRIVERS_SELECT",
    "VULKAN_SDK",
    "QT_PLUGIN_PATH",
    "QT_QPA_PLATFORM",
    "QT_QPA_PLATFORM_PLUGIN_PATH",
    "QT_DEBUG_PLUGINS",
    "LOCALAPPDATA",
    "APPDATA",
    "USERPROFILE",
    "TEMP",
    "TMP",
    "PATH",
)


def repo_schema_dir(repo_root: Path) -> Path:
    return (repo_root / "src" / "atlas" / "Resources" / "json" / "atlas").resolve()


def atlas_binary_from_atlas_dir(atlas_dir: Path) -> Path:
    if common_dirs.is_mac():
        return atlas_dir / "Contents" / "MacOS" / "Atlas"
    if common_dirs.is_windows():
        return atlas_dir / "Atlas.exe"
    return atlas_dir / "Atlas"


def missing_schema_files(out_dir: Path) -> list[str]:
    return [name for name in _REQUIRED_SCHEMA_FILES if not (out_dir / name).exists()]


def _env_truthy(name: str) -> bool:
    value = os.environ.get(name, "").strip().lower()
    return value in {"1", "true", "yes", "y", "on"}


def _running_in_ci() -> bool:
    return _env_truthy("CI") or _env_truthy("GITHUB_ACTIONS")


def _float_env(name: str, default: float) -> float:
    value = os.environ.get(name, "").strip()
    if not value:
        return default
    try:
        parsed = float(value)
    except ValueError:
        return default
    return max(0.0, parsed)


def _int_env(name: str, default: int) -> int:
    value = os.environ.get(name, "").strip()
    if not value:
        return default
    try:
        parsed = int(value)
    except ValueError:
        return default
    return max(0, parsed)


def _utc_timestamp(timestamp: float) -> str:
    return datetime.fromtimestamp(timestamp, tz=timezone.utc).isoformat()


def _format_command(args: list[str]) -> str:
    if common_dirs.is_windows():
        return subprocess.list2cmdline(args)
    return " ".join(args)


def _atlas_log_root() -> Path | None:
    local_app_data = os.environ.get("LOCALAPPDATA", "").strip()
    if not local_app_data:
        return None
    return Path(local_app_data) / "fenglab" / "Atlas" / "Logs"


def _snapshot_files(root: Path | None) -> dict[Path, int]:
    if root is None or not root.exists():
        return {}

    snapshot: dict[Path, int] = {}
    for path in root.rglob("*"):
        if not path.is_file():
            continue
        try:
            snapshot[path.resolve()] = path.stat().st_mtime_ns
        except OSError:
            continue
    return snapshot


def _new_or_modified_files(
    root: Path | None, snapshot: dict[Path, int], started_at: float
) -> list[Path]:
    if root is None or not root.exists():
        return []

    result: list[Path] = []
    for path in root.rglob("*"):
        if not path.is_file():
            continue
        try:
            resolved = path.resolve()
            stat = path.stat()
        except OSError:
            continue
        if (
            resolved not in snapshot
            or snapshot[resolved] != stat.st_mtime_ns
            or stat.st_mtime >= started_at
        ):
            result.append(path)

    return sorted(result, key=lambda p: str(p).casefold())


def _print_section(title: str) -> None:
    print(f"\n===== {title} =====", flush=True)


def _print_path_info(path: Path, label: str) -> None:
    try:
        exists = path.exists()
    except OSError as exc:
        print(f"{label}: {path} (stat failed: {exc})", flush=True)
        return

    if not exists:
        print(f"{label}: {path} (missing)", flush=True)
        return

    try:
        stat = path.stat()
    except OSError as exc:
        print(f"{label}: {path} (exists, stat failed: {exc})", flush=True)
        return

    kind = "dir" if path.is_dir() else "file"
    size = "" if path.is_dir() else f", size={stat.st_size}"
    print(
        f"{label}: {path} ({kind}{size}, mtime_utc={_utc_timestamp(stat.st_mtime)})",
        flush=True,
    )


def _print_directory_listing(path: Path, label: str) -> None:
    _print_section(label)
    if not path.exists():
        print(f"{path} (missing)", flush=True)
        return
    if not path.is_dir():
        print(f"{path} (not a directory)", flush=True)
        return

    for child in sorted(path.iterdir(), key=lambda p: p.name.casefold()):
        _print_path_info(child, child.name)


def _read_text_lossy(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8", errors="replace")
    except UnicodeError:
        return path.read_bytes().decode("utf-8", errors="replace")


def _print_text_file(path: Path, label: str) -> None:
    _print_section(label)
    _print_path_info(path, "path")
    if not path.is_file():
        return
    try:
        print(_read_text_lossy(path), flush=True)
    except OSError as exc:
        print(f"failed to read {path}: {exc}", flush=True)


def _run_diagnostic_command(
    args: list[str], *, env: dict[str, str] | None = None, cwd: Path | None = None
) -> None:
    _print_section(f"Diagnostic command: {_format_command(args)}")
    timeout = _float_env(
        _DIAGNOSTIC_COMMAND_TIMEOUT_SECONDS_ENV,
        _DEFAULT_DIAGNOSTIC_COMMAND_TIMEOUT_SECONDS,
    )
    try:
        proc = subprocess.run(
            args,
            cwd=str(cwd) if cwd else None,
            env=env,
            check=False,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=timeout,
        )
    except FileNotFoundError:
        print("command not found", flush=True)
        return
    except subprocess.TimeoutExpired as exc:
        print(f"command timed out after {timeout:.1f}s: {exc}", flush=True)
        return
    except Exception as exc:
        print(f"command failed to start: {type(exc).__name__}: {exc}", flush=True)
        return

    print(f"exit={proc.returncode}", flush=True)
    if proc.stdout:
        print("--- stdout ---", flush=True)
        print(proc.stdout, flush=True)
    if proc.stderr:
        print("--- stderr ---", flush=True)
        print(proc.stderr, flush=True)


def _schema_dump_child_env() -> dict[str, str]:
    env = os.environ.copy()
    if common_dirs.is_windows() and _running_in_ci():
        # Capture Qt plugin loader detail into subprocess stderr on CI. Windows
        # GUI subprocess output is unreliable when inherited by Actions, so the
        # parent captures it and prints it only if the dump fails.
        env.setdefault("QT_DEBUG_PLUGINS", "1")
    return env


def _print_environment_snapshot(env: dict[str, str]) -> None:
    _print_section("Schema Dump Environment")
    print(f"python={sys.executable}", flush=True)
    print(f"platform={sys.platform}", flush=True)
    for name in _RELEVANT_ENV_VARS:
        value = env.get(name)
        if value is None or value == "":
            value = "<unset>"
        print(f"{name}={value}", flush=True)


def _print_process_result(proc: subprocess.CompletedProcess[str]) -> None:
    _print_section("Schema Dump Process Result")
    print(f"exit={proc.returncode}", flush=True)
    print("--- stdout ---", flush=True)
    print(proc.stdout or "<empty>", flush=True)
    print("--- stderr ---", flush=True)
    print(proc.stderr or "<empty>", flush=True)


def _print_windows_swiftshader_state(env: dict[str, str]) -> None:
    for root in (Path("C:/SwiftShader"), Path("D:/swiftshader-win64-5.0.0.1")):
        _print_directory_listing(root, f"SwiftShader Directory: {root}")
        icd = root / "vk_swiftshader_icd.json"
        if icd.exists():
            _print_text_file(icd, f"SwiftShader ICD: {icd}")

    vulkaninfo = shutil.which("vulkaninfo", path=env.get("PATH"))
    if vulkaninfo:
        _run_diagnostic_command([vulkaninfo, "--summary"], env=env)
    else:
        _print_section("Diagnostic command: vulkaninfo --summary")
        print("vulkaninfo not found on PATH", flush=True)


def _print_windows_event_log(started_at: float) -> None:
    grace_seconds = _int_env(
        _WINDOWS_EVENT_GRACE_SECONDS_ENV,
        _DEFAULT_WINDOWS_EVENT_GRACE_SECONDS,
    )
    since_unix_seconds = max(0, int(started_at) - grace_seconds)
    script = rf"""
$since = [DateTimeOffset]::FromUnixTimeSeconds({since_unix_seconds}).LocalDateTime
$events = @(
  Get-WinEvent -FilterHashtable @{{LogName='Application'; StartTime=$since}} -ErrorAction SilentlyContinue |
  Where-Object {{
    $_.ProviderName -match 'Application Error|Windows Error Reporting' -or
    $_.Message -match 'Atlas\.exe|Qt6|qoffscreen|qwindows|platform plugin|Vulkan|SwiftShader'
  }}
)
if ($events.Count -eq 0) {{
  Write-Output 'No matching Application events found in the schema dump failure window.'
  exit 0
}}
$events |
  Sort-Object TimeCreated |
  Select-Object TimeCreated,ProviderName,Id,LevelDisplayName,Message |
  Format-List | Out-String -Width 240
"""
    _run_diagnostic_command(["powershell", "-NoProfile", "-Command", script])


def _print_windows_wer_reports(started_at: float) -> None:
    _print_section("Windows Error Reporting Atlas Reports")
    roots = [
        Path("C:/ProgramData/Microsoft/Windows/WER/ReportArchive"),
        Path("C:/ProgramData/Microsoft/Windows/WER/ReportQueue"),
    ]
    found = False
    for root in roots:
        if not root.exists():
            continue
        for report_dir in sorted(root.glob("*Atlas.exe*"), key=lambda p: str(p).casefold()):
            try:
                if report_dir.stat().st_mtime < started_at - 60:
                    continue
            except OSError:
                continue
            found = True
            _print_path_info(report_dir, "report_dir")
            for child in sorted(report_dir.iterdir(), key=lambda p: p.name.casefold()):
                _print_path_info(child, child.name)
                if child.is_file() and child.suffix.casefold() in {".wer", ".txt", ".xml"}:
                    try:
                        print(_read_text_lossy(child), flush=True)
                    except OSError as exc:
                        print(f"failed to read {child}: {exc}", flush=True)
    if not found:
        print("No Atlas WER report directories were modified during this dump window.", flush=True)


def _print_windows_home_dumps(started_at: float) -> None:
    _print_section("Windows Atlas Dump Files")
    home = Path(os.environ.get("USERPROFILE", str(Path.home())))
    found = False
    for pattern in ("Atlas*.dmp", "Atlas*.mdmp", "Atlas*.dump", "Atlas*.core"):
        for dump_path in sorted(home.glob(pattern), key=lambda p: str(p).casefold()):
            try:
                if dump_path.stat().st_mtime < started_at - 60:
                    continue
            except OSError:
                continue
            found = True
            _print_path_info(dump_path, "dump")
    if not found:
        print("No Atlas dump files were modified in the user profile during this dump window.", flush=True)


def _print_new_atlas_logs(log_root: Path | None, snapshot: dict[Path, int], started_at: float) -> None:
    _print_section("Atlas File Logs Created Or Modified By Schema Dump")
    if log_root is None:
        print("LOCALAPPDATA is unset; Atlas log root cannot be resolved.", flush=True)
        return
    _print_path_info(log_root, "log_root")

    files = _new_or_modified_files(log_root, snapshot, started_at)
    if not files:
        print("No Atlas log files were created or modified during this dump window.", flush=True)
        return

    for file_path in files:
        _print_text_file(file_path, f"Atlas log: {file_path}")


def _print_schema_dump_diagnostics(
    *,
    atlas_bin: Path,
    atlas_dir: Path,
    out_dir: Path,
    args: list[str],
    env: dict[str, str],
    proc: subprocess.CompletedProcess[str],
    started_at: float,
    log_snapshot: dict[Path, int],
) -> None:
    if common_dirs.is_windows():
        time.sleep(
            _float_env(
                _WINDOWS_CRASH_DIAGNOSTIC_SETTLE_SECONDS_ENV,
                _DEFAULT_WINDOWS_CRASH_DIAGNOSTIC_SETTLE_SECONDS,
            )
        )

    _print_section("Atlas Schema Dump Diagnostics")
    print(f"command={_format_command(args)}", flush=True)
    print(f"started_at_utc={_utc_timestamp(started_at)}", flush=True)
    _print_path_info(atlas_bin, "atlas_bin")
    _print_path_info(atlas_dir, "atlas_dir")
    _print_path_info(out_dir, "out_dir")
    print(f"missing_schema_files={missing_schema_files(out_dir)}", flush=True)

    _print_process_result(proc)
    _print_environment_snapshot(env)

    _print_directory_listing(atlas_dir, "Atlas Deploy Directory")
    _print_directory_listing(atlas_dir / "platforms", "Qt Platform Plugins")
    _print_directory_listing(atlas_dir / "vulkan", "Atlas Vulkan Runtime Directory")
    _print_directory_listing(atlas_dir / "Resources" / "vulkan", "Atlas Vulkan Resource Directory")
    _print_directory_listing(out_dir, "Schema Output Directory")

    if common_dirs.is_windows():
        _print_windows_swiftshader_state(env)
        _print_new_atlas_logs(_atlas_log_root(), log_snapshot, started_at)
        _print_windows_event_log(started_at)
        _print_windows_wer_reports(started_at)
        _print_windows_home_dumps(started_at)
    else:
        _print_new_atlas_logs(_atlas_log_root(), log_snapshot, started_at)


def dump_schema_with_atlas(
    *, atlas_dir: Path, out_dir: Path
) -> subprocess.CompletedProcess[str]:
    atlas_bin = atlas_binary_from_atlas_dir(atlas_dir)
    args: list[str] = [
        str(atlas_bin),
        "--run_dump_animation3d_schema",
        "--dump_output_dir",
        str(out_dir),
        "--atlas_default_render_backend=vulkan",
        "-platform",
        "offscreen",
    ]
    env = _schema_dump_child_env()
    started_at = time.time()
    log_snapshot = _snapshot_files(_atlas_log_root())
    proc = subprocess.run(
        args,
        check=False,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        env=env,
    )

    if proc.returncode != 0 or _env_truthy(_SCHEMA_DUMP_DIAGNOSTICS_ENV):
        _print_schema_dump_diagnostics(
            atlas_bin=atlas_bin,
            atlas_dir=atlas_dir,
            out_dir=out_dir,
            args=args,
            env=env,
            proc=proc,
            started_at=started_at,
            log_snapshot=log_snapshot,
        )
    elif not common_dirs.is_windows():
        if proc.stdout:
            print(proc.stdout, end="", flush=True)
        if proc.stderr:
            print(proc.stderr, end="", flush=True)

    return proc


def _schema_dump_failure_message(
    *, atlas_bin: Path, out_dir: Path, proc: subprocess.CompletedProcess[str], missing: list[str]
) -> str:
    details: list[str] = []
    if proc.returncode != 0:
        details.append(f"Atlas schema dump exited with code {proc.returncode}")
    if missing:
        details.append(
            "Atlas schema dump did not produce required files: " + ", ".join(missing)
        )
    if not details:
        details.append("Atlas schema dump failed")
    return (
        "; ".join(details)
        + f" (atlas_bin={atlas_bin}, out_dir={out_dir}). "
        + "See schema dump diagnostics above for process output, Atlas file logs, "
        + "Vulkan/SwiftShader state, Qt plugin layout, and Windows crash reports."
    )


def ensure_llm_schema_docs(
    *,
    atlas_dir: Path,
    out_dir: Path,
    force_schema_dump: bool = False,
) -> None:
    out_dir.mkdir(parents=True, exist_ok=True)

    if force_schema_dump or missing_schema_files(out_dir):
        proc = dump_schema_with_atlas(atlas_dir=atlas_dir, out_dir=out_dir)
        missing = missing_schema_files(out_dir)
        if proc.returncode != 0 or missing:
            raise RuntimeError(
                _schema_dump_failure_message(
                    atlas_bin=atlas_binary_from_atlas_dir(atlas_dir),
                    out_dir=out_dir,
                    proc=proc,
                    missing=missing,
                )
            )
