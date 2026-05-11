#!/usr/bin/env python3
"""Developer-only helper for publishing the Atlas Bio-Formats bridge jar."""

from __future__ import annotations

import argparse
import importlib
import logging
import shutil
import subprocess
import sys
from pathlib import Path

import common_dirs
import download_utils
from generate_atlas_deps_filelist import (
    generate_atlas_deps_filelist,
    process_files,
)
from logger import setup_logger

logger = logging.getLogger(__name__)


def build_bridge_jar() -> Path:
    bioformats_package_jar = (
        Path(common_dirs.ext_build_dir()) / "jars" / "bioformats_package.jar"
    )
    if not bioformats_package_jar.is_file():
        raise FileNotFoundError(
            "Bio-Formats package jar is required before building the bridge: "
            f"{bioformats_package_jar}"
        )

    bridge_dir = Path(common_dirs.atlas_repository_dir()) / "src" / "bioformats_bridge"
    maven_command = ("mvn", "-DskipTests", "clean", "package")
    logger.info("Running in %s: %s", bridge_dir, " ".join(maven_command))
    subprocess.run(maven_command, cwd=bridge_dir, check=True)

    jar_path = bridge_dir / "target" / "atlas-bioformats-bridge.jar"
    if not jar_path.is_file():
        raise FileNotFoundError(f"Maven build did not produce expected jar: {jar_path}")
    return jar_path


def copy_bridge_to_local_runtime_jars(source_jar: Path) -> Path:
    if not source_jar.is_file():
        raise FileNotFoundError(f"Bridge jar does not exist: {source_jar}")

    destination = (
        Path(common_dirs.ext_build_dir()) / "jars" / "atlas-bioformats-bridge.jar"
    )
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source_jar, destination)
    logger.info("Copied %s to %s", source_jar, destination)
    return destination


def copy_bridge_to_static_runtime_assets(source_jar: Path) -> Path:
    if not source_jar.is_file():
        raise FileNotFoundError(f"Bridge jar does not exist: {source_jar}")

    static_runtime_assets_dir = (
        Path(common_dirs.static_deploy_folder()) / "atlas_runtime_assets"
    )
    if not static_runtime_assets_dir.is_dir():
        raise FileNotFoundError(
            "Static Atlas runtime assets directory does not exist: "
            f"{static_runtime_assets_dir}. This developer publishing helper "
            "expects the private static deploy mirror to be available."
        )

    destination = static_runtime_assets_dir / "jars" / "atlas-bioformats-bridge.jar"
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source_jar, destination)
    logger.info("Copied %s to %s", source_jar, destination)
    return destination


def regenerate_runtime_assets_filelist() -> None:
    static_runtime_assets_dir = (
        Path(common_dirs.static_deploy_folder()) / "atlas_runtime_assets"
    )
    if not static_runtime_assets_dir.is_dir():
        raise FileNotFoundError(
            "Static Atlas runtime assets directory does not exist: "
            f"{static_runtime_assets_dir}. This developer publishing helper "
            "expects the private static deploy mirror to be available."
        )

    files_info = process_files(str(static_runtime_assets_dir))
    manifest_path = (
        Path(common_dirs.atlas_util_dir()) / "atlas_runtime_assets_filelist.py"
    )
    generate_atlas_deps_filelist(files_info, str(manifest_path))
    logger.info("Regenerated %s", manifest_path)


def sync_runtime_assets_to_r2() -> None:
    # Import after regenerating atlas_runtime_assets_filelist.py so the sync
    # helper sees the freshly written checksum in this process.
    importlib.invalidate_caches()
    sys.modules.pop("atlas_runtime_assets_filelist", None)

    import publish_r2

    publish_r2.sync_static_target(
        target="atlas_runtime_assets",
        dry_run=False,
        allow_partial=False,
    )


def _log_published_jar_summary(jar_path: Path) -> None:
    logger.info(
        "Runtime bridge jar: %s size=%d sha256=%s",
        jar_path,
        jar_path.stat().st_size,
        download_utils.calculate_checksum(str(jar_path)),
    )


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Build the Atlas Bio-Formats bridge jar, update local and static "
            "runtime assets, regenerate the runtime asset manifest, and sync "
            "atlas_runtime_assets to R2."
        )
    )
    parser.add_argument(
        "--skip-publish",
        action="store_true",
        help="Stop after copying the jar and regenerating atlas_runtime_assets_filelist.py.",
    )
    return parser.parse_args()


def main() -> None:
    setup_logger()
    args = _parse_args()

    jar_path = build_bridge_jar()
    local_runtime_jar = copy_bridge_to_local_runtime_jars(jar_path)
    static_runtime_jar = copy_bridge_to_static_runtime_assets(jar_path)
    _log_published_jar_summary(local_runtime_jar)
    _log_published_jar_summary(static_runtime_jar)
    regenerate_runtime_assets_filelist()

    if args.skip_publish:
        logger.info("Skipping R2 sync because --skip-publish was provided.")
        return

    sync_runtime_assets_to_r2()


if __name__ == "__main__":
    main()
