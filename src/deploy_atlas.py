#!/Users/feng/miniconda3/bin/python3

import os
import sys
import shutil
import subprocess


if sys.version_info[0] < 3 or sys.version_info[1] < 5:
    sys.stderr.write('Error: need python 3.5 or higher\n')
    sys.exit(1)


def get_bak_file_name(orig_file: str):
    return orig_file + '.bak'


def deploy_atlas():
    print('current interpreter: ' + sys.executable)

    curr_dir = os.path.abspath(os.path.dirname(__file__))

    binary_dir = os.path.normpath(os.path.join(curr_dir, 'cmake-build-release', 'atlas'))
    assert os.path.exists(binary_dir)

    target_dir = os.path.join(os.path.expanduser('~'), 'Google Drive', "jinny'lab", 'software')
    assert os.path.exists(target_dir)

    qt_dir = os.path.join(os.path.expanduser('~'), 'Qt', "5.7", 'clang_64', 'bin')
    assert os.path.exists(qt_dir)

    print('currDIR:', curr_dir)
    print('binaryDIR:', binary_dir)
    print('targetDIR:', target_dir)
    print('qtDIR:', qt_dir)

    app_name = 'Atlas.app'
    zip_name = 'atlas.app.new.zip'
    app_bak_name = get_bak_file_name(app_name)
    if os.path.exists(os.path.join(binary_dir, app_name)):
        shutil.copytree(os.path.join(binary_dir, app_name), os.path.join(binary_dir, app_bak_name),
                        symlinks=True)
        shell = sys.platform.startswith('win')
        subprocess.run([os.path.join(qt_dir, 'macdeployqt'), app_name],
                       cwd=binary_dir, shell=shell, check=True)
        if os.path.exists(os.path.join(binary_dir, zip_name)):
            os.remove(os.path.join(binary_dir, zip_name))
        subprocess.run(['zip', '--quiet', '--recurse-paths', '--symlinks', zip_name, app_name],
                       cwd=binary_dir, shell=shell, check=True)
        shutil.rmtree(os.path.join(binary_dir, app_name), ignore_errors=False)
        os.replace(os.path.join(binary_dir, app_bak_name), os.path.join(binary_dir, app_name))
        shutil.copy2(os.path.join(binary_dir, zip_name), os.path.join(target_dir, zip_name))
    else:
        sys.stderr.write('Error: atlas is not built yet.\n')
        sys.exit(1)


if __name__ == "__main__":
    deploy_atlas()
