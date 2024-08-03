import os

from generate_atlas_deps_filelist import generate_atlas_deps_filelist, process_files
import common_dirs

if __name__ == "__main__":
    base_url = "https://neutracing.com/static/atlas_test_data"
    backup_base_url = "https://fenglab.xyz/static/atlas_test_data"
    folder_path = common_dirs.static_atlas_test_data_dir()

    files_info = process_files(folder_path, base_url, backup_base_url)
    generate_atlas_deps_filelist(files_info, os.path.join(common_dirs.atlas_util_dir(), 'atlas_test_data_filelist.py'))
