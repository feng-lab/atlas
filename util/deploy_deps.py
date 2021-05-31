import os
import sys
import shutil
import subprocess
import datetime
import glob

import common_dirs


def deploy_deps():
    if common_dirs.is_mac():
        zip_folder = os.path.join(common_dirs.atlas_repository_dir(), 'deploy', 'deps', 'common')
        upload = False

        dir_name = 'atlas_deps'
        zip_name = os.path.join(zip_folder, f'atlas-deps-{datetime.date.today():%Y-%m-%d}.zip')
        file_list = glob.glob(os.path.join(zip_folder, 'atlas-deps-*.zip'))
        if len(file_list) == 0:
            upload = True
            subprocess.run(['zip', '--quiet', '--recurse-paths', '--symlinks', zip_name, dir_name],
                           cwd=os.path.join(common_dirs.src_package_dir(), '..'),
                           shell=False, check=True)

        dir_name = 'atlas_test_data'
        zip_name = os.path.join(zip_folder, f'atlas-test-data-{datetime.date.today():%Y-%m-%d}.zip')
        file_list = glob.glob(os.path.join(zip_folder, 'atlas-test-data-*.zip'))
        if len(file_list) == 0:
            upload = True
            subprocess.run(['zip', '--quiet', '--recurse-paths', '--symlinks', zip_name, dir_name],
                           cwd=os.path.join(common_dirs.atlas_test_data_dir(), '..'),
                           shell=False, check=True)

        if upload:
            subprocess.run(['rsync', '-av', '--delete', 'common',
                            'feng@vultrL:"/home/feng/atlasCI/"'],
                           cwd=os.path.join(common_dirs.deploy_target_dir(), 'deps'), shell=False, check=True)

    if common_dirs.is_mac():
        suffix = 'macOS'
        zip_folder = os.path.join(common_dirs.atlas_repository_dir(), 'deploy', 'deps', suffix)
        upload = False

        dir_name = 'Qt'
        zip_name = os.path.join(zip_folder, f'dep-{dir_name}-{datetime.date.today():%Y-%m-%d}.zip')
        file_list = glob.glob(os.path.join(zip_folder, f'dep-{dir_name}-*.zip'))
        if len(file_list) == 0:
            upload = True
            subprocess.run(['zip', '--quiet', '--recurse-paths', '--symlinks', zip_name, dir_name],
                           cwd=os.path.join(common_dirs.qt_install_dir(), '..'),
                           shell=False, check=True)

        dir_name = 'oneapi'
        zip_name = os.path.join(zip_folder, f'dep-{dir_name}-{datetime.date.today():%Y-%m-%d}.zip')
        file_list = glob.glob(os.path.join(zip_folder, f'dep-{dir_name}-*.zip'))
        if len(file_list) == 0:
            upload = True
            subprocess.run(['zip', '--quiet', '--recurse-paths', '--symlinks', zip_name, dir_name],
                           cwd=os.path.join(common_dirs.intel_sw_dir(), '..'),
                           shell=False, check=True)

        if upload:
            subprocess.run(['rsync', '-av', '--delete', suffix,
                            'feng@vultrL:"/home/feng/atlasCI/"'],
                           cwd=os.path.join(common_dirs.deploy_target_dir(), 'deps'), shell=False, check=True)

    if common_dirs.is_linux():
        suffix = 'Linux'
        zip_folder = os.path.join(common_dirs.atlas_repository_dir(), 'deploy', 'deps', suffix)
        upload = False

        dir_name = 'Qt'
        zip_name = os.path.join(zip_folder, f'dep-{dir_name}-{datetime.date.today():%Y-%m-%d}.zip')
        file_list = glob.glob(os.path.join(zip_folder, f'dep-{dir_name}-*.zip'))
        if len(file_list) == 0:
            upload = True
            subprocess.run(['zip', '--quiet', '--recurse-paths', '--symlinks', zip_name, dir_name],
                           cwd=os.path.join(common_dirs.qt_install_dir(), '..'),
                           shell=False, check=True)

        # dir_name = 'intel'
        # zip_name = os.path.join(zip_folder, f'dep-{dir_name}-{datetime.date.today():%Y-%m-%d}.zip')
        # file_list = glob.glob(os.path.join(zip_folder, f'dep-{dir_name}-*.zip'))
        # if len(file_list) == 0:
        #     upload = True
        #     subprocess.run(['zip', '--quiet', '--recurse-paths', '--symlinks', zip_name, dir_name],
        #                    cwd=os.path.join(common_dirs.intel_sw_dir(), '..'),
        #                    shell=False, check=True)

        if upload:
            subprocess.run(['rsync', '-av', '--delete', suffix,
                            'feng@vultrL:"/home/feng/atlasCI/"'],
                           cwd=os.path.join(common_dirs.deploy_target_dir(), 'deps'), shell=False, check=True)


if __name__ == "__main__":
    deploy_deps()
