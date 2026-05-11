"""Optional Bio-Formats runtime support for zimg."""

from __future__ import annotations

import hashlib
import os
import sys
import tempfile
import urllib.request
from pathlib import Path

from . import _imgpy

BIOFORMATS_RUNTIME = {
    "version": "8.5.0",
    "url": "https://downloads.openmicroscopy.org/bio-formats/8.5.0/artifacts/bioformats_package.jar",
    "size": 53843906,
    "sha256": "c6e60665d53a334b66e4d635340151f403dfe57a64704c573dd4c03b873befb9",
}

_bundled_path = Path(__file__).resolve().parent / "jars" / "bioformats_package.jar"
_configured_path: Path | None = _bundled_path if _bundled_path.is_file() else None


def configure(path: str | os.PathLike[str]) -> Path:
    """Use an existing ``bioformats_package.jar`` for this Python process."""

    jar_path = Path(path).expanduser().resolve()
    if not jar_path.is_file():
        raise FileNotFoundError(f"Bio-Formats jar does not exist: {jar_path}")
    _imgpy._set_bioformats_jar_path(str(jar_path))
    global _configured_path
    _configured_path = jar_path
    return jar_path


def configure_java(path: str | os.PathLike[str]) -> Path:
    """Use a specific Java executable for this Python process."""

    java_path = Path(path).expanduser().resolve()
    if not java_path.is_file():
        raise FileNotFoundError(f"Java executable does not exist: {java_path}")
    if not os.access(java_path, os.X_OK):
        raise PermissionError(f"Java path is not executable: {java_path}")
    _imgpy._set_bioformats_java_executable_path(str(java_path))
    return java_path


def download() -> Path:
    """Download the pinned Bio-Formats runtime jar, verify it, and configure zimg."""

    target = _cache_dir() / "bioformats_package.jar"
    if target.exists() and _matches_runtime(target):
        return configure(target)

    target.parent.mkdir(parents=True, exist_ok=True)
    fd, tmp_name = tempfile.mkstemp(
        prefix="bioformats-", suffix=".jar.tmp", dir=target.parent
    )
    tmp_path = Path(tmp_name)
    try:
        with os.fdopen(fd, "wb") as out:
            with urllib.request.urlopen(BIOFORMATS_RUNTIME["url"]) as response:
                while True:
                    chunk = response.read(1024 * 1024)
                    if not chunk:
                        break
                    out.write(chunk)
        _verify_runtime(tmp_path)
        tmp_path.replace(target)
    except Exception:
        try:
            tmp_path.unlink()
        except FileNotFoundError:
            pass
        raise

    return configure(target)


def is_available() -> bool:
    """Return whether zimg currently has a complete Bio-Formats runtime."""

    return bool(_imgpy._has_bioformats_runtime_support())


def runtime_paths() -> dict[str, Path | None]:
    """Return the Java executable, bridge jar, and Bio-Formats jar paths in use."""

    raw = _imgpy._bioformats_runtime_paths()
    return {key: Path(value) if value else None for key, value in raw.items()}


def ensure_available() -> None:
    """Print runtime paths and raise unless Bio-Formats is ready to use."""

    paths = runtime_paths()
    labels = {
        "java_executable": "Java executable",
        "bridge_jar": "Atlas Bio-Formats bridge jar",
        "bioformats_jar": "Bio-Formats jar",
    }
    for key, label in labels.items():
        print(f"{label}: {paths[key] or 'not configured'}")

    if is_available():
        print("Bio-Formats runtime available: True")
        return

    missing = [str(item) for item in _imgpy._missing_bioformats_runtime_files()]
    missing_suffix = "" if not missing else f" (missing: {', '.join(missing)})"
    print(f"Bio-Formats runtime available: False{missing_suffix}")
    details = ", ".join(
        f"{key}={value if value is not None else 'not configured'}"
        for key, value in paths.items()
    )
    raise RuntimeError(
        "zimg Bio-Formats runtime is not available. "
        f"Missing: {', '.join(missing)}. Runtime paths: {details}"
    )


def path() -> Path | None:
    """Return the Bio-Formats jar configured through this module, if any."""

    return _configured_path


def _cache_dir() -> Path:
    version = BIOFORMATS_RUNTIME["version"]
    if sys.platform == "darwin":
        root = Path.home() / "Library" / "Caches" / "zimg"
    elif os.name == "nt":
        root = Path.home() / "AppData" / "Local" / "zimg" / "Cache"
    else:
        root = Path.home() / ".cache" / "zimg"
    return root / "bioformats" / version


def _matches_runtime(path: Path) -> bool:
    try:
        _verify_runtime(path)
    except OSError:
        return False
    except ValueError:
        return False
    return True


def _verify_runtime(path: Path) -> None:
    expected_size = int(BIOFORMATS_RUNTIME["size"])
    actual_size = path.stat().st_size
    if actual_size != expected_size:
        raise ValueError(
            f"Bio-Formats jar has size {actual_size} bytes, expected {expected_size}: {path}"
        )

    expected_sha256 = str(BIOFORMATS_RUNTIME["sha256"])
    digest = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            digest.update(chunk)
    actual_sha256 = digest.hexdigest()
    if actual_sha256 != expected_sha256:
        raise ValueError(
            f"Bio-Formats jar sha256 is {actual_sha256}, expected {expected_sha256}: {path}"
        )
