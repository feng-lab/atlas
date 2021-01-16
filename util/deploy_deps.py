import os
import sys
import shutil
import subprocess
import datetime
import zipfile

import common_dirs


def deploy_deps():
    if common_dirs.is_mac():
        zip_folder = os.path.join(common_dirs.atlas_repository_dir(), 'deploy', 'deps', 'common')

        dir_name = 'atlas_deps'
        zip_name = os.path.join(zip_folder, f'atlas-deps-{datetime.date.today():%Y-%m-%d}.zip')
        if not os.path.exists(zip_name):
            subprocess.run(['zip', '--quiet', '--recurse-paths', '--symlinks', zip_name, dir_name],
                           cwd=os.path.join(common_dirs.src_package_dir(), '..'),
                           shell=False, check=True)

        dir_name = 'atlas_test_data'
        zip_name = os.path.join(zip_folder, f'atlas-test-data-{datetime.date.today():%Y-%m-%d}.zip')
        if not os.path.exists(zip_name):
            subprocess.run(['zip', '--quiet', '--recurse-paths', '--symlinks', zip_name, dir_name],
                           cwd=os.path.join(common_dirs.atlas_test_data_dir(), '..'),
                           shell=False, check=True)

        subprocess.run(['rsync', '-av', '--delete', 'common',
                        'feng@vultrL:"/home/feng/code/proxy/static/deps/"'],
                       cwd=os.path.join(common_dirs.deploy_target_dir(), 'deps'), shell=False, check=True)

    if common_dirs.is_mac():
        suffix = 'macOS'
        zip_folder = os.path.join(common_dirs.atlas_repository_dir(), 'deploy', 'deps', suffix)

        dir_name = 'Qt'
        zip_name = os.path.join(zip_folder, f'dep-qt-{datetime.date.today():%Y-%m-%d}.zip')
        if not os.path.exists(zip_name):
            subprocess.run(['zip', '--quiet', '--recurse-paths', '--symlinks', zip_name, dir_name],
                           cwd=os.path.join(common_dirs.qt_install_dir(), '..'),
                           shell=False, check=True)

        dir_name = 'intel'
        zip_name = os.path.join(zip_folder, f'dep-intel-{datetime.date.today():%Y-%m-%d}.zip')
        if not os.path.exists(zip_name):
            subprocess.run(['zip', '--quiet', '--recurse-paths', '--symlinks', zip_name, dir_name],
                           cwd=os.path.join(common_dirs.intel_sw_dir(), '..'),
                           shell=False, check=True)

        subprocess.run(['rsync', '-av', '--delete', suffix,
                        'feng@vultrL:"/home/feng/code/proxy/static/deps/"'],
                       cwd=os.path.join(common_dirs.deploy_target_dir(), 'deps'), shell=False, check=True)

    if common_dirs.is_linux():
        suffix = 'linux'
        zip_folder = os.path.join(common_dirs.atlas_repository_dir(), 'deploy', 'deps', suffix)

        dir_name = 'Qt'
        zip_name = os.path.join(zip_folder, f'dep-qt-{datetime.date.today():%Y-%m-%d}.zip')
        if not os.path.exists(zip_name):
            subprocess.run(['zip', '--quiet', '--recurse-paths', '--symlinks', zip_name, dir_name],
                           cwd=os.path.join(common_dirs.qt_install_dir(), '..'),
                           shell=False, check=True)

        dir_name = 'intel'
        zip_name = os.path.join(zip_folder, f'dep-intel-{datetime.date.today():%Y-%m-%d}.zip')
        if not os.path.exists(zip_name):
            subprocess.run(['zip', '--quiet', '--recurse-paths', '--symlinks', zip_name, dir_name],
                           cwd=os.path.join(common_dirs.intel_sw_dir(), '..'),
                           shell=False, check=True)

        subprocess.run(['rsync', '-av', '--delete', suffix,
                        'feng@vultrL:"/home/feng/code/proxy/static/deps/"'],
                       cwd=os.path.join(common_dirs.deploy_target_dir(), 'deps'), shell=False, check=True)


if __name__ == "__main__":
    deploy_deps()
