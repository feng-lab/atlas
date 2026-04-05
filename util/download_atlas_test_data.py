import logging
import os
import zipfile

import atlas_file_hosts
import common_dirs
from atlas_test_data_filelist import files_to_download as raw_files_to_download
import download_utils
from logger import setup_logger

logger = logging.getLogger(__name__)

_BENCHMARK_ZIP_FILENAME = "benchmark.zip"
_BENCHMARK_FOLDERNAME = "benchmark"
_BENCHMARK_STAMP_FILENAME = ".atlas_benchmark_zip_sha256"


def _unpack_benchmark_zip(atlas_test_data_dir: str) -> None:
    zip_path = os.path.join(atlas_test_data_dir, _BENCHMARK_ZIP_FILENAME)
    benchmark_dir = os.path.join(atlas_test_data_dir, _BENCHMARK_FOLDERNAME)
    stamp_path = os.path.join(benchmark_dir, _BENCHMARK_STAMP_FILENAME)

    if not os.path.exists(zip_path):
        logger.info("No %s found; skipping benchmark unpack.", _BENCHMARK_ZIP_FILENAME)
        return

    if not zipfile.is_zipfile(zip_path):
        logger.critical(
            "%s exists but is not a valid zip archive: %s",
            _BENCHMARK_ZIP_FILENAME,
            zip_path,
        )
        return

    zip_sha256 = download_utils.calculate_checksum(zip_path)
    if os.path.isdir(benchmark_dir) and os.path.isfile(stamp_path):
        try:
            with open(stamp_path, "r", encoding="utf-8") as f:
                stamped_sha256 = f.read().strip()
            if stamped_sha256 == zip_sha256:
                logger.info(
                    "Benchmark already unpacked with matching checksum. (%s)",
                    zip_sha256,
                )
                return
        except OSError:
            # Fall back to re-extract.
            pass

    with zipfile.ZipFile(zip_path, mode="r") as zf:
        names = [
            nm
            for nm in zf.namelist()
            if nm and not nm.endswith("/") and not nm.startswith("__MACOSX/")
        ]
    if not names:
        logger.critical(
            "%s contains no files to extract: %s",
            _BENCHMARK_ZIP_FILENAME,
            zip_path,
        )
        return

    # Prefer extracting to the atlas_test_data root when the archive already
    # contains the desired `benchmark/` prefix. Otherwise, extract into the
    # `benchmark/` folder to keep the expanded contents scoped for filelist
    # generation and cleanup policies.
    has_benchmark_prefix = any(
        nm.startswith(_BENCHMARK_FOLDERNAME + "/") for nm in names
    )
    extract_dir = atlas_test_data_dir if has_benchmark_prefix else benchmark_dir

    logger.info("Unpacking %s -> %s", zip_path, extract_dir)
    common_dirs.rm_tree(benchmark_dir)
    os.makedirs(extract_dir, exist_ok=True)
    common_dirs.unpack_file_to_folder(zip_path, extract_dir)

    if not os.path.isdir(benchmark_dir):
        logger.critical(
            "Expected %s to create %s but it does not exist. zip=%s",
            _BENCHMARK_ZIP_FILENAME,
            benchmark_dir,
            zip_path,
        )
        return

    try:
        with open(stamp_path, "w", encoding="utf-8", newline="\n") as f:
            f.write(zip_sha256)
            f.write("\n")
    except OSError as e:
        logger.warning("Failed to write benchmark unpack stamp %s: %s", stamp_path, e)


def download_atlas_test_data():
    if common_dirs.is_internal_dev_environment():
        logger.info("skip downloading atlas test data")
        _unpack_benchmark_zip(common_dirs.static_atlas_test_data_dir())
        return
    files_to_download = atlas_file_hosts.with_static_urls(
        raw_files_to_download,
        static_subdir="atlas_test_data",
    )
    atlas_test_data_dir = os.path.join(
        common_dirs.atlas_repository_dir(), "atlas_test_data"
    )
    download_utils.sync_files(
        files_to_download,
        atlas_test_data_dir,
        check_os=False,
        preserve_relpaths=[_BENCHMARK_FOLDERNAME],
    )
    _unpack_benchmark_zip(atlas_test_data_dir)


if __name__ == "__main__":
    logger = setup_logger()

    download_atlas_test_data()
