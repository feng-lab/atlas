import os
import logging

import common_dirs
from download_utils import sync_files
from atlas_deps_filelist import files_to_download
from logger import setup_logger

logger = logging.getLogger(__name__)


def download_atlas_deps():
    if common_dirs.is_my_computer():
        logger.info('skip downloading atlas deps')
        return
    sync_files(files_to_download, os.path.join(common_dirs.atlas_repository_dir(), 'atlas_deps'))


if __name__ == "__main__":
    logger = setup_logger()

    download_atlas_deps()
