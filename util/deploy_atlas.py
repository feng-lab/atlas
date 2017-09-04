import os
import sys
import shutil
import subprocess

import common_dirs
import linuxdeployqt


def get_bak_file_name(orig_file: str):
    return orig_file + '.bak'


def deploy_atlas():
    print('current interpreter: ' + sys.executable)

    binary_dir = common_dirs.binary_dir()
    print('binaryDIR:', binary_dir)
    print('deployTargetDIR:', common_dirs.deploy_target_dir())
    print('qtBaseDIR:', common_dirs.qt_base_dir())

    if sys.platform.startswith('darwin'):
        app_name = 'Atlas.app'
        zip_name = 'atlas.app.new.zip'
        app_bak_name = get_bak_file_name(app_name)
        if os.path.exists(os.path.join(binary_dir, app_name)):
            shutil.copytree(os.path.join(binary_dir, app_name), os.path.join(binary_dir, app_bak_name),
                            symlinks=True)
            subprocess.run([os.path.join(common_dirs.qt_bin_dir(), 'macdeployqt'), app_name],
                           cwd=binary_dir, shell=False, check=True)
            if os.path.exists(os.path.join(binary_dir, zip_name)):
                os.remove(os.path.join(binary_dir, zip_name))
            subprocess.run(['zip', '--quiet', '--recurse-paths', '--symlinks', zip_name, app_name],
                           cwd=binary_dir, shell=False, check=True)
            shutil.rmtree(os.path.join(binary_dir, app_name), ignore_errors=False)
            os.replace(os.path.join(binary_dir, app_bak_name), os.path.join(binary_dir, app_name))
            shutil.copy2(os.path.join(binary_dir, zip_name), os.path.join(common_dirs.deploy_target_dir(), zip_name))
        else:
            sys.stderr.write('Error: atlas is not built yet.\n')
            sys.exit(1)
    elif sys.platform.startswith('linux'):
        app_name = "Atlas"
        zip_name = "Atlas_linux.zip"
        if os.path.exists(os.path.join(binary_dir, app_name)):
            shutil.rmtree(os.path.join(common_dirs.deploy_target_dir(), 'Atlas.AppDir'), ignore_errors=True)
            linuxdeployqt.linuxdeployqt(os.path.join(binary_dir, app_name),
                                        os.path.join(common_dirs.deploy_target_dir(), 'Atlas.AppDir'),
                                        common_dirs.qt_base_dir())
            if os.path.exists(os.path.join(common_dirs.deploy_target_dir(), zip_name)):
                os.remove(os.path.join(common_dirs.deploy_target_dir(), zip_name))
            subprocess.run(['zip', '--quiet', '--recurse-paths', '--symlinks', zip_name, 'Atlas.AppDir'],
                           cwd=common_dirs.deploy_target_dir(), shell=False, check=True)
        else:
            sys.stderr.write('Error: atlas is not built yet.\n')
            sys.exit(1)


if __name__ == "__main__":
    deploy_atlas()
