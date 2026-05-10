import logging
import shutil
from pathlib import Path

import atlas_file_hosts
import common_dirs
import download_utils
from atlas_runtime_assets_filelist import files_to_download as raw_files_to_download
from logger import setup_logger

logger = logging.getLogger(__name__)


def _normalize_relpath(value: str) -> str:
    rel = str(value).replace("\\", "/").lstrip("/")
    if not rel:
        raise ValueError("relative path must be non-empty")
    parts = Path(rel).parts
    if any(part in ("", ".", "..") for part in parts):
        raise ValueError(f"runtime asset path must stay relative: {value!r}")
    return "/".join(parts)


def _target_dir() -> Path:
    return Path(common_dirs.ext_build_dir())


def _validate_file(path: Path, *, expected_size: int, expected_sha256: str) -> bool:
    if not path.is_file():
        return False
    if path.stat().st_size != expected_size:
        return False
    return download_utils.calculate_checksum(str(path)) == expected_sha256


def _all_targets_valid() -> bool:
    target_dir = _target_dir()
    for item in raw_files_to_download:
        rel = _normalize_relpath(item["filename"])
        if not _validate_file(
            target_dir / rel,
            expected_size=int(item["expected_size"]),
            expected_sha256=str(item["expected_sha256"]),
        ):
            return False
    return True


def _copy_from_local_static_dir(source_dir: Path) -> bool:
    copied = False
    target_dir = _target_dir()
    for item in raw_files_to_download:
        rel = _normalize_relpath(item["filename"])
        source_path = source_dir / rel
        target_path = target_dir / rel
        expected_size = int(item["expected_size"])
        expected_sha256 = str(item["expected_sha256"])

        if not _validate_file(
            source_path,
            expected_size=expected_size,
            expected_sha256=expected_sha256,
        ):
            logger.warning(
                "Local runtime asset does not match manifest: %s", source_path
            )
            return False

        if _validate_file(
            target_path,
            expected_size=expected_size,
            expected_sha256=expected_sha256,
        ):
            logger.info("Runtime asset already present: %s", target_path)
            continue

        logger.info("Copying runtime asset %s -> %s", source_path, target_path)
        target_path.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source_path, target_path)
        copied = True

    return copied or _all_targets_valid()


def _download_from_static_hosts() -> None:
    files_to_download = atlas_file_hosts.with_static_urls(
        raw_files_to_download,
        static_subdir="atlas_runtime_assets",
    )
    target_dir = _target_dir()
    for item in files_to_download:
        rel = _normalize_relpath(item["filename"])
        target_path = target_dir / rel
        target_path.parent.mkdir(parents=True, exist_ok=True)
        success = download_utils.download_file_with_resume(
            item["url"],
            item["fallback_url"],
            str(target_path),
            int(item["expected_size"]),
            str(item["expected_sha256"]),
        )
        if not success:
            raise RuntimeError(f"Failed to download runtime asset: {rel}")


def download_atlas_runtime_assets() -> None:
    if _all_targets_valid():
        logger.info("Atlas runtime assets are already present.")
        return

    local_static_dir = Path(common_dirs.static_deploy_folder()) / "atlas_runtime_assets"
    if local_static_dir.is_dir() and _copy_from_local_static_dir(local_static_dir):
        return

    _download_from_static_hosts()


if __name__ == "__main__":
    setup_logger()
    download_atlas_runtime_assets()
