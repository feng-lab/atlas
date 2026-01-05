import json
import logging
import os

import common_dirs
import download_utils
from logger import setup_logger

logger = logging.getLogger(__name__)


def _should_skip_filename(filename: str) -> bool:
    # Skip common OS-generated metadata files.
    if filename in {"Thumbs.db", "desktop.ini"}:
        return True

    # Skip filenames that contain control characters (e.g. the macOS Finder "Icon\\r" file).
    return any(ord(ch) < 32 or ord(ch) == 127 for ch in filename)


def process_files(folder_path: str) -> list[dict]:
    files_info = []

    for root, dirs, files in os.walk(folder_path):
        for filename in files:
            if filename.startswith('.'):
                continue
            local_path = os.path.join(root, filename)
            relative_path = os.path.relpath(local_path, folder_path).replace(
                os.sep, "/"
            )

            if _should_skip_filename(filename):
                logger.info("Skipping metadata file: %r", relative_path)
                continue

            size = os.path.getsize(local_path)
            if size == 0:
                logger.info("Skipping empty file: %r", relative_path)
                continue

            logger.info(f"Processing {relative_path}...")

            checksum = download_utils.calculate_checksum(local_path)

            files_info.append(
                {"filename": relative_path, "size": size, "checksum": checksum}
            )

    return files_info


def generate_atlas_deps_filelist(
    files_info: list[dict],
    output_file: str,
) -> None:
    """Write a Python file defining `files_to_download` (data-only)."""
    raw = []
    for fi in files_info:
        raw.append(
            {
                "expected_size": fi["size"],
                "expected_sha256": fi["checksum"],
                "filename": fi["filename"],
            }
        )
    with open(output_file, "w", newline="\n") as f:
        f.write("files_to_download = ")
        json.dump(raw, f, indent=4)
        f.write("\n")


if __name__ == "__main__":
    logger = setup_logger()

    folder_path = common_dirs.static_src_package_dir()

    files_info = process_files(folder_path)
    generate_atlas_deps_filelist(
        files_info,
        os.path.join(common_dirs.atlas_util_dir(), "atlas_deps_filelist.py"),
    )
