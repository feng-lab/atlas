import os

import common_dirs
from generate_atlas_deps_filelist import generate_atlas_deps_filelist, process_files
from logger import setup_logger

_SKIP_DIR_PREFIXES = ("benchmark/",)

if __name__ == "__main__":
    setup_logger()

    folder_path = common_dirs.static_atlas_test_data_dir()

    files_info = process_files(folder_path)
    files_info = [
        fi
        for fi in files_info
        if not any(fi["filename"].startswith(p) for p in _SKIP_DIR_PREFIXES)
    ]
    generate_atlas_deps_filelist(
        files_info,
        os.path.join(common_dirs.atlas_util_dir(), "atlas_test_data_filelist.py"),
    )
