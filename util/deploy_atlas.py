import os
import sys
import shutil
import subprocess
import xml.etree.ElementTree as eTree
import datetime

import common_dirs
import linuxdeployqt
import build_ext_libs


def get_bak_file_name(orig_file: str):
    return orig_file + '.bak'


def update_pacakge_xml_version(file: str):
    tree = eTree.parse(file)
    tree.find('Version').text = '{0:%Y.%m.%d}'.format(datetime.datetime.now())
    tree.find('ReleaseDate').text = '{0:%Y-%m-%d}'.format(datetime.datetime.now())
    # Write back to file
    tree.write(file)


def deploy_atlas() -> bool:
    print('current interpreter: ' + sys.executable)

    binary_dir = common_dirs.atlas_binary_dir()
    print('binaryDIR:', binary_dir)
    print('deployTargetDIR:', common_dirs.deploy_target_dir())
    print('qtBaseDIR:', common_dirs.qt_base_dir())

    if sys.platform.startswith('darwin'):
        app_name = 'Atlas.app'
        zip_name = 'atlas.app.zip'
        app_bak_name = get_bak_file_name(app_name)
        if os.path.exists(os.path.join(binary_dir, app_name)):
            shutil.copytree(os.path.join(binary_dir, app_name), os.path.join(binary_dir, app_bak_name),
                            symlinks=True)
            subprocess.run([os.path.join(common_dirs.qt_bin_dir(), 'macdeployqt'), app_name],
                           cwd=binary_dir, shell=False, check=True)
            subprocess.run([os.path.join(binary_dir, app_name, 'Contents', 'MacOS', 'Atlas'),
                            '--run_unit_tests'], shell=False, check=True)

            if os.path.exists(os.path.join(binary_dir, zip_name)):
                os.remove(os.path.join(binary_dir, zip_name))
            subprocess.run(['zip', '--quiet', '--recurse-paths', '--symlinks', zip_name, app_name],
                           cwd=binary_dir, shell=False, check=True)
            shutil.rmtree(os.path.join(binary_dir, app_name), ignore_errors=False)
            os.replace(os.path.join(binary_dir, app_bak_name), os.path.join(binary_dir, app_name))

            if os.path.exists(os.path.join(common_dirs.deploy_target_dir(), zip_name)):
                os.remove(os.path.join(common_dirs.deploy_target_dir(), zip_name))
            shutil.move(os.path.join(binary_dir, zip_name), common_dirs.deploy_target_dir())

            shutil.copy2(os.path.join(common_dirs.deploy_target_dir(), zip_name),
                         os.path.join(os.path.expanduser('~'), 'Google Drive', "lab", 'software', zip_name))
            # shutil.copy2(os.path.join(common_dirs.deploy_target_dir(), zip_name),
            #              os.path.join('/', 'Volumes', "fs3017", 'eeum', 'software', zip_name))
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
            subprocess.run([os.path.join(common_dirs.deploy_target_dir(), 'Atlas.AppDir', 'Atlas'),
                            '--run_unit_tests'], shell=False, check=True)

            if os.path.exists(os.path.join(common_dirs.deploy_target_dir(), zip_name)):
                os.remove(os.path.join(common_dirs.deploy_target_dir(), zip_name))
            subprocess.run(['zip', '--quiet', '--recurse-paths', '--symlinks', zip_name, 'Atlas.AppDir'],
                           cwd=common_dirs.deploy_target_dir(), shell=False, check=True)
            subprocess.run(['scp', zip_name,
                            'feng@labmacpro:"/Users/feng/Google Drive/lab/software/"'],
                           cwd=common_dirs.deploy_target_dir(), shell=False, check=True)
            # subprocess.run(['scp', zip_name,
            #                 'feng@labmacpro:"/Volumes/fs3017/eeum/software/"'],
            #                cwd=common_dirs.deploy_target_dir(), shell=False, check=True)
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
            build_ext_libs.glob_copy(os.path.join(common_dirs.assimp_redist_dir(), '*.dll'),
                                     os.path.join(common_dirs.deploy_target_dir(), 'Atlas'))
            build_ext_libs.glob_copy(os.path.join(common_dirs.freeimage_redist_dir(), '*.dll'),
                                     os.path.join(common_dirs.deploy_target_dir(), 'Atlas'))
            shutil.copy2(os.path.join(common_dirs.tbb_redist_dir(), 'tbb.dll'),
                         os.path.join(common_dirs.deploy_target_dir(), 'Atlas'))
            build_ext_libs.glob_copy(
                os.path.join(common_dirs.vc_CRT_redist_dir(), '*.dll'),
                os.path.join(common_dirs.deploy_target_dir(), 'Atlas'))
            build_ext_libs.glob_copy(
                os.path.join(common_dirs.vc_CXXAMP_redist_dir(), '*.dll'),
                os.path.join(common_dirs.deploy_target_dir(), 'Atlas'))
            build_ext_libs.glob_copy(
                os.path.join(common_dirs.vc_OpenMP_redist_dir(), '*.dll'),
                os.path.join(common_dirs.deploy_target_dir(), 'Atlas'))

            env = build_ext_libs.get_vcvars_environment()
            subprocess.run([os.path.join(common_dirs.qt_bin_dir(), 'windeployqt'), app_name],
                           cwd=os.path.join(common_dirs.deploy_target_dir(), 'Atlas'), shell=False, check=True, env=env)
            subprocess.run([os.path.join(common_dirs.deploy_target_dir(), 'Atlas', 'Atlas'),
                            '--run_unit_tests'], shell=True,
                           check=False)  # todo: fix returned non-zero exit status 3221226356.

            shutil.make_archive(os.path.join(common_dirs.deploy_target_dir(), zip_base_name),
                                'zip',
                                os.path.join(common_dirs.deploy_target_dir(), 'Atlas'))
            shutil.copy2(os.path.join(common_dirs.deploy_target_dir(), zip_name),
                         os.path.join('Z:', os.sep, 'Google Drive', "lab", 'software', zip_name))
        else:
            sys.stderr.write('Error: atlas is not built yet.\n')
            sys.exit(1)

    return True


def deploy_atlas_to_server_repository():
    if sys.platform.startswith('darwin'):
        app_name = 'Atlas.app'
        zip_name = 'atlas.app.zip'
        repo_package_name = 'atlas.app.7z'
        installer_base_name = 'AtlasInstaller'
        installer_app_name = 'AtlasInstaller.app'
        installer_zip_name = 'AtlasInstaller.app.zip'
        suffix = 'macOS'
    elif sys.platform.startswith('linux'):
        app_name = 'Atlas.AppDir'
        zip_name = 'atlas-linux.zip'
        repo_package_name = 'atlas.7z'
        installer_base_name = 'AtlasInstaller'
        installer_app_name = 'AtlasInstaller'
        installer_zip_name = 'AtlasInstaller-linux.zip'
        suffix = 'linux'
    else:
        app_name = 'Atlas'
        zip_name = 'atlas-win.zip'
        repo_package_name = 'atlas.7z'
        installer_base_name = 'AtlasInstaller'
        installer_app_name = 'AtlasInstaller'
        installer_zip_base_name = 'AtlasInstaller-win'
        installer_zip_name = installer_zip_base_name + '.zip'
        suffix = 'windows'

    if os.path.exists(os.path.join(common_dirs.deploy_target_dir(), 'packages-' + suffix, 'fenglab.neutube')):
        shutil.rmtree(os.path.join(common_dirs.deploy_target_dir(), 'packages-' + suffix, 'fenglab.neutube'),
                      ignore_errors=False)
    shutil.copytree(os.path.join(common_dirs.src_package_dir(), 'packages-' + suffix, 'fenglab.neutube'),
                    os.path.join(common_dirs.deploy_target_dir(), 'packages-' + suffix, 'fenglab.neutube'))

    common_dirs.unpack_file_to_folder(os.path.join(common_dirs.deploy_target_dir(), zip_name),
                                      common_dirs.get_package_top_level_folder(
                                          os.path.join(common_dirs.deploy_target_dir(), zip_name),
                                          common_dirs.deploy_target_dir()))
    if os.path.exists(os.path.join(common_dirs.deploy_target_dir(), repo_package_name)):
        os.remove(os.path.join(common_dirs.deploy_target_dir(), repo_package_name))
    subprocess.run([os.path.join(common_dirs.qt_installer_framework_bin_dir(), 'archivegen'),
                    repo_package_name, app_name],
                   cwd=common_dirs.deploy_target_dir(), shell=False, check=True)
    shutil.rmtree(os.path.join(common_dirs.deploy_target_dir(), app_name), ignore_errors=False)

    repo_package_folder = os.path.join(common_dirs.deploy_target_dir(),
                                       'packages-macOS', 'fenglab.atlas', 'data')
    if os.path.exists(os.path.join(repo_package_folder, repo_package_name)):
        os.remove(os.path.join(repo_package_folder, repo_package_name))
    shutil.move(os.path.join(common_dirs.deploy_target_dir(), repo_package_name), repo_package_folder)
    update_pacakge_xml_version(os.path.join(common_dirs.deploy_target_dir(),
                                            'packages-' + suffix, 'fenglab.atlas', 'meta', 'package.xml'))

    shutil.rmtree(os.path.join(common_dirs.deploy_target_dir(), 'macOS'), ignore_errors=True)
    subprocess.run([os.path.join(common_dirs.qt_installer_framework_bin_dir(), 'repogen'),
                    '-p', 'packages-' + suffix, './' + suffix],
                   cwd=common_dirs.deploy_target_dir(), shell=False, check=True)

    if os.path.exists(os.path.join(common_dirs.deploy_target_dir(), installer_zip_name)):
        os.remove(os.path.join(common_dirs.deploy_target_dir(), installer_zip_name))
    shutil.rmtree(os.path.join(common_dirs.deploy_target_dir(), installer_app_name), ignore_errors=True)
    subprocess.run([os.path.join(common_dirs.qt_installer_framework_bin_dir(), 'binarycreator'),
                    '--online-only', '-c', 'config/config-' + suffix + '.xml', '-p', 'packages-' + suffix,
                    installer_base_name],
                   cwd=common_dirs.deploy_target_dir(), shell=False, check=True)
    if sys.platform.startswith('win'):
        shutil.make_archive(os.path.join(common_dirs.deploy_target_dir(), installer_zip_base_name),
                            'zip',
                            os.path.join(common_dirs.deploy_target_dir(), 'Atlas'))
    else:
        subprocess.run(['zip', '--quiet', '--recurse-paths', '--symlinks', installer_zip_name, installer_app_name],
                       cwd=common_dirs.deploy_target_dir(), shell=False, check=True)
    shutil.rmtree(os.path.join(common_dirs.deploy_target_dir(), installer_app_name), ignore_errors=False)

    if sys.platform.startswith('darwin'):
        shutil.copy2(os.path.join(common_dirs.deploy_target_dir(), installer_zip_name),
                     os.path.join(os.path.expanduser('~'), 'Google Drive', "lab", 'software', installer_zip_name))
        target_folder = os.path.join(os.path.expanduser('~'), 'Google Drive', "code", 'my', 'proxy', 'static',
                                     'packages')
        if os.path.exists(os.path.join(target_folder, suffix)):
            shutil.rmtree(os.path.join(target_folder, suffix), ignore_errors=False)
        shutil.move(os.path.join(common_dirs.deploy_target_dir(), suffix), target_folder)
    elif sys.platform.startswith('linux'):
        subprocess.run(['scp', installer_zip_name,
                        'feng@labmacpro:"/Users/feng/Google Drive/lab/software/"'],
                       cwd=common_dirs.deploy_target_dir(), shell=False, check=True)
        subprocess.run(['rsync', '-a', '--delete', suffix,
                        'feng@labmacpro:"/Users/feng/Google Drive/code/my/proxy/static/packages/' + suffix + '"'],
                       cwd=common_dirs.deploy_target_dir(), shell=False, check=True)
        shutil.rmtree(os.path.join(common_dirs.deploy_target_dir(), suffix), ignore_errors=False)
    else:
        shutil.copy2(os.path.join(common_dirs.deploy_target_dir(), installer_zip_name),
                     os.path.join('Z:', os.sep, 'Google Drive', "lab", 'software', installer_zip_name))
        target_folder = os.path.join('Z:', os.sep, 'Google Drive', "code", 'my', 'proxy', 'static', 'packages')
        if os.path.exists(os.path.join(target_folder, suffix)):
            shutil.rmtree(os.path.join(target_folder, suffix), ignore_errors=False)
        shutil.move(os.path.join(common_dirs.deploy_target_dir(), suffix), target_folder)


if __name__ == "__main__":
    if deploy_atlas():
        deploy_atlas_to_server_repository()
