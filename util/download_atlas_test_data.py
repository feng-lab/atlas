import logging
import os

import common_dirs
from atlas_test_data_filelist import files_to_download
from download_utils import sync_files
from logger import setup_logger

logger = logging.getLogger(__name__)


def download_atlas_test_data():
    if common_dirs.is_my_computer():
        logger.info('skip downloading atlas test data')
        return
    sync_files(files_to_download, os.path.join(common_dirs.atlas_repository_dir(), 'atlas_test_data'), check_os=False)


if __name__ == "__main__":
    logger = setup_logger()

    download_atlas_test_data()
