import os
import sys
import shutil
import subprocess

import common_dirs
import linuxdeployqt
import build_ext_libs


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
        zip_name = 'atlas.app.zip'
        app_bak_name = get_bak_file_name(app_name)
        if os.path.exists(os.path.join(binary_dir, app_name)):
            if os.path.exists(os.path.join(common_dirs.deploy_target_dir(), zip_name)):
                os.remove(os.path.join(common_dirs.deploy_target_dir(), zip_name))

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

            shutil.move(os.path.join(binary_dir, zip_name), common_dirs.deploy_target_dir())
            shutil.copy2(os.path.join(common_dirs.deploy_target_dir(), zip_name),
                         os.path.join(os.path.expanduser('~'), 'Google Drive', "lab", 'software', zip_name))
        else:
            sys.stderr.write('Error: atlas is not built yet.\n')
            sys.exit(1)
    elif sys.platform.startswith('linux'):
        app_name = "Atlas"
        zip_name = "atlas-linux.zip"
        if os.path.exists(os.path.join(binary_dir, app_name)):
            shutil.rmtree(os.path.join(common_dirs.deploy_target_dir(), 'Atlas.AppDir'), ignore_errors=True)
            linuxdeployqt.linuxdeployqt(os.path.join(binary_dir, app_name),
                                        os.path.join(common_dirs.deploy_target_dir(), 'Atlas.AppDir'),
                                        common_dirs.qt_base_dir())
            if os.path.exists(os.path.join(common_dirs.deploy_target_dir(), zip_name)):
                os.remove(os.path.join(common_dirs.deploy_target_dir(), zip_name))
            subprocess.run(['zip', '--quiet', '--recurse-paths', '--symlinks', zip_name, 'Atlas.AppDir'],
                           cwd=common_dirs.deploy_target_dir(), shell=False, check=True)
            subprocess.run(['scp', zip_name,
                            'feng@labmacpro:"/Users/feng/Google Drive/lab/software/"'],
                           cwd=common_dirs.deploy_target_dir(), shell=False, check=True)
        else:
            sys.stderr.write('Error: atlas is not built yet.\n')
            sys.exit(1)
    else:
        app_name = 'Atlas.exe'
        zip_base_name = 'atlas-win'
        zip_name = zip_base_name + '.zip'
        if os.path.exists(os.path.join(binary_dir, app_name)):
            if os.path.exists(os.path.join(common_dirs.deploy_target_dir(), zip_name)):
                os.remove(os.path.join(common_dirs.deploy_target_dir(), zip_name))
            shutil.rmtree(os.path.join(common_dirs.deploy_target_dir(), 'Atlas'), ignore_errors=True)
            os.mkdir(os.path.join(common_dirs.deploy_target_dir(), 'Atlas'))
            shutil.copy2(os.path.join(binary_dir, app_name),
                         os.path.join(common_dirs.deploy_target_dir(), 'Atlas'))
            shutil.copytree(os.path.join(binary_dir, 'Resources'),
                            os.path.join(common_dirs.deploy_target_dir(), 'Atlas', 'Resources'),
                            symlinks=True)
            build_ext_libs.glob_copy(os.path.join(common_dirs.ext_dir(), 'assimp', 'bin', '*.dll'),
                                     os.path.join(common_dirs.deploy_target_dir(), 'Atlas'))
            build_ext_libs.glob_copy(os.path.join(common_dirs.ext_dir(), 'freeimage', '*.dll'),
                                     os.path.join(common_dirs.deploy_target_dir(), 'Atlas'))
            shutil.copy2(os.path.join('C:', os.sep, 'Program Files (x86)', 'IntelSWTools', 'compilers_and_libraries',
                                      'windows', 'redist', 'intel64', 'tbb', 'vc14', 'tbb.dll'),
                         os.path.join(common_dirs.deploy_target_dir(), 'Atlas'))

            env = build_ext_libs.get_vcvars_environment()
            subprocess.run([os.path.join(common_dirs.qt_bin_dir(), 'windeployqt'), app_name],
                           cwd=os.path.join(common_dirs.deploy_target_dir(), 'Atlas'), shell=False, check=True, env=env)
            shutil.make_archive(os.path.join(common_dirs.deploy_target_dir(), zip_base_name),
                                'zip',
                                os.path.join(common_dirs.deploy_target_dir(), 'Atlas'))
            shutil.copy2(os.path.join(common_dirs.deploy_target_dir(), zip_name),
                         os.path.join('Z:', os.sep, 'Google Drive', "lab", 'software', zip_name))
        else:
            sys.stderr.write('Error: atlas is not built yet.\n')
            sys.exit(1)


if __name__ == "__main__":
    deploy_atlas()
