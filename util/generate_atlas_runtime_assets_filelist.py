import os

import common_dirs
from generate_atlas_deps_filelist import generate_atlas_deps_filelist, process_files
from logger import setup_logger


if __name__ == "__main__":
    setup_logger()

    files_info = process_files(common_dirs.static_atlas_runtime_assets_dir())
    generate_atlas_deps_filelist(
        files_info,
        os.path.join(common_dirs.atlas_util_dir(), "atlas_runtime_assets_filelist.py"),
    )
