import os
import common_dirs
from download_utils import sync_files
from atlas_deps_filelist import files_to_download


def download_atlas_deps():
    if not common_dirs.is_my_computer():
        sync_files(files_to_download, os.path.join(common_dirs.atlas_repository_dir(), 'atlas_deps'))


if __name__ == "__main__":
    download_atlas_deps()
