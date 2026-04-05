import logging
import os

import atlas_file_hosts
import common_dirs
from atlas_deps_filelist import files_to_download as raw_files_to_download
from download_utils import sync_files
from logger import setup_logger

logger = logging.getLogger(__name__)


def download_atlas_deps():
    if common_dirs.is_internal_dev_environment():
        logger.info("skip downloading atlas deps")
        return
    files_to_download = atlas_file_hosts.with_static_urls(
        raw_files_to_download,
        static_subdir="atlas_deps",
    )
    sync_files(
        files_to_download,
        os.path.join(common_dirs.atlas_repository_dir(), "atlas_deps"),
    )


if __name__ == "__main__":
    logger = setup_logger()

    download_atlas_deps()
