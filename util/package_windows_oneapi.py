import argparse
import hashlib
import json
import logging
import shutil
import subprocess
import tempfile
from pathlib import Path

import common_dirs
from logger import setup_logger

logger = logging.getLogger(__name__)

REQUIRED_MKL_LIBS = (
    "mkl_intel_lp64.lib",
    "mkl_tbb_thread.lib",
    "mkl_sequential.lib",
    "mkl_core.lib",
)

# oneTBB's config package treats a no-component lookup as a request for tbb,
# tbbmalloc, and tbbmalloc_proxy. Keep the minimized bundle consistent with
# that metadata so downstream dependencies can use find_package(TBB CONFIG)
# without carrying consumer-specific component patches.
REQUIRED_TBB_LIBS = (
    "tbb12.lib",
    "tbbmalloc.lib",
    "tbbmalloc_proxy.lib",
)
REQUIRED_TBB_DLLS = (
    "tbb12.dll",
    "tbbmalloc.dll",
    "tbbmalloc_proxy.dll",
)


def _default_oneapi_root() -> str:
    candidates = common_dirs.installed_intel_sw_dir_candidates()
    assert candidates, (
        "No installed Intel oneAPI candidates are defined for this platform."
    )
    return candidates[0]


def _component_version(component_latest_dir: Path) -> str:
    resolved = component_latest_dir.resolve()
    version_name = resolved.name
    if version_name.lower() == "latest":
        raise AssertionError(
            f"Failed to resolve concrete component version for {component_latest_dir}"
        )
    return version_name


def _default_output_path(oneapi_root: Path) -> Path:
    mkl_version = _component_version(oneapi_root / "mkl" / "latest")
    return (
        Path(common_dirs.src_package_dir()) / f"oneapi-mkl-{mkl_version}-windows-x64.7z"
    )


def _copy_file(src: Path, dst: Path) -> None:
    if not src.exists():
        raise AssertionError(f"Required file not found: {src}")
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, dst)


def _copy_tree(src: Path, dst: Path) -> None:
    if not src.exists():
        raise AssertionError(f"Required directory not found: {src}")
    shutil.copytree(src, dst)


def _iter_files(folder: Path):
    for path in sorted(folder.rglob("*")):
        if path.is_file():
            yield path


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _format_size(num_bytes: int) -> str:
    value = float(num_bytes)
    for unit in ("B", "KiB", "MiB", "GiB", "TiB"):
        if value < 1024.0 or unit == "TiB":
            return f"{value:.1f} {unit}"
        value /= 1024.0
    raise AssertionError("unreachable")


def _bundle_manifest(bundle_root: Path, oneapi_root: Path) -> dict:
    file_entries = []
    total_bytes = 0
    for path in _iter_files(bundle_root):
        size = path.stat().st_size
        total_bytes += size
        file_entries.append(
            {
                "path": path.relative_to(bundle_root).as_posix(),
                "size": size,
            }
        )
    return {
        "source_root": str(oneapi_root),
        "mkl_version": _component_version(oneapi_root / "mkl" / "latest"),
        "tbb_version": _component_version(oneapi_root / "tbb" / "latest"),
        "files": file_entries,
        "uncompressed_bytes": total_bytes,
    }


def _write_manifest(bundle_root: Path, oneapi_root: Path) -> dict:
    manifest = _bundle_manifest(bundle_root, oneapi_root)
    manifest_path = bundle_root / "atlas_oneapi_bundle_manifest.json"
    with manifest_path.open("w", encoding="utf-8", newline="\n") as handle:
        json.dump(manifest, handle, indent=2)
        handle.write("\n")
    return manifest


def _stage_bundle(oneapi_root: Path, staging_dir: Path) -> tuple[Path, dict]:
    bundle_root = staging_dir / "oneapi"
    mkl_root = (oneapi_root / "mkl" / "latest").resolve()
    tbb_root = (oneapi_root / "tbb" / "latest").resolve()

    logger.info("Staging MKL headers from %s", mkl_root / "include")
    _copy_tree(mkl_root / "include", bundle_root / "mkl" / "latest" / "include")
    for lib_name in REQUIRED_MKL_LIBS:
        logger.info("Staging MKL library %s", lib_name)
        _copy_file(
            mkl_root / "lib" / lib_name,
            bundle_root / "mkl" / "latest" / "lib" / lib_name,
        )

    logger.info("Staging TBB headers from %s", tbb_root / "include")
    _copy_tree(tbb_root / "include", bundle_root / "tbb" / "latest" / "include")
    for lib_name in REQUIRED_TBB_LIBS:
        logger.info("Staging TBB import library %s", lib_name)
        _copy_file(
            tbb_root / "lib" / lib_name,
            bundle_root / "tbb" / "latest" / "lib" / lib_name,
        )
    for dll_name in REQUIRED_TBB_DLLS:
        logger.info("Staging TBB runtime %s", dll_name)
        _copy_file(
            tbb_root / "bin" / dll_name,
            bundle_root / "tbb" / "latest" / "bin" / dll_name,
        )
    logger.info(
        "Staging TBB CMake metadata directory from %s",
        tbb_root / "lib" / "cmake" / "tbb",
    )
    _copy_tree(
        tbb_root / "lib" / "cmake" / "tbb",
        bundle_root / "tbb" / "latest" / "lib" / "cmake" / "tbb",
    )

    manifest = _write_manifest(bundle_root, oneapi_root)
    return bundle_root, manifest


def _log_bundle_summary(bundle_root: Path, manifest: dict) -> None:
    logger.info(
        "Prepared %d files (%s uncompressed)",
        len(manifest["files"]),
        _format_size(manifest["uncompressed_bytes"]),
    )
    grouped_entries = {}
    for entry in manifest["files"]:
        parts = entry["path"].split("/")
        group_name = "/".join(parts[:3]) if len(parts) >= 3 else entry["path"]
        group = grouped_entries.setdefault(group_name, {"count": 0, "bytes": 0})
        group["count"] += 1
        group["bytes"] += entry["size"]
    for group_name in sorted(grouped_entries):
        group = grouped_entries[group_name]
        logger.info(
            "  %s: %d files (%s)",
            group_name,
            group["count"],
            _format_size(group["bytes"]),
        )


def _create_archive(bundle_root: Path, output_path: Path) -> None:
    output_path.parent.mkdir(parents=True, exist_ok=True)
    if output_path.exists():
        logger.info(f"Output archive already exists: {output_path}. Will replace it.")
        output_path.unlink()

    seven_zip = Path(common_dirs.get_7za_binary())
    if not seven_zip.exists():
        raise AssertionError(f"7za binary not found: {seven_zip}")

    cmd = [
        str(seven_zip),
        "a",
        "-t7z",
        str(output_path),
        bundle_root.name,
    ]
    logger.info("Running: %s", " ".join(cmd))
    subprocess.run(cmd, cwd=bundle_root.parent, shell=False, check=True)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Create a minimized Windows oneAPI bundle for Atlas containing the MKL "
            "and TBB pieces Atlas currently uses."
        )
    )
    parser.add_argument(
        "--oneapi-root",
        default=_default_oneapi_root(),
        help=("Installed Intel oneAPI root to package (default: %(default)s)."),
    )
    parser.add_argument(
        "--output",
        help=(
            "Output .7z path. Defaults to src_package_dir()/oneapi-mkl-<mkl-version>-windows-x64.7z."
        ),
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Stage and list the selected files without creating the archive.",
    )
    return parser.parse_args()


def main() -> None:
    if not common_dirs.is_windows():
        raise AssertionError("This packaging script is only supported on Windows.")

    args = parse_args()
    oneapi_root = Path(args.oneapi_root).resolve()
    if not oneapi_root.exists():
        raise AssertionError(f"Intel oneAPI root does not exist: {oneapi_root}")
    output_path = (
        Path(args.output).resolve()
        if args.output
        else _default_output_path(oneapi_root)
    )

    logger.info("Using oneAPI root: %s", oneapi_root)
    logger.info("Planned output archive: %s", output_path)

    with tempfile.TemporaryDirectory(prefix="atlas-oneapi-") as temp_dir:
        staging_dir = Path(temp_dir)
        bundle_root, manifest = _stage_bundle(oneapi_root, staging_dir)
        _log_bundle_summary(bundle_root, manifest)
        if args.dry_run:
            logger.info("Dry run complete; archive was not created.")
            return
        _create_archive(bundle_root, output_path)

    archive_size = output_path.stat().st_size
    logger.info("Created %s (%s)", output_path, _format_size(archive_size))
    logger.info("SHA256: %s", _sha256(output_path))


if __name__ == "__main__":
    setup_logger()
    main()
