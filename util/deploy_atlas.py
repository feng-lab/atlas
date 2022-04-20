import os
import sys
import shutil
import subprocess
import xml.etree.ElementTree as eTree
import datetime
import zipfile

import common_dirs
import linuxdeployqt
import build_ext_libs


def get_bak_file_name(orig_file: str):
    return orig_file + '.bak'


def update_pacakge_xml_version(template_file: str, file: str):
    tree = eTree.parse(template_file)
    tree.find('Version').text = '{0:%Y.%m.%d.%H}'.format(datetime.datetime.now())
    tree.find('ReleaseDate').text = '{0:%Y-%m-%d}'.format(datetime.datetime.now())
    # Write back to file
    tree.write(file, encoding="utf-8", xml_declaration=True)


def update_maintenance_pacakge_xml_version(template_file: str, file: str):
    tree = eTree.parse(template_file)
    tree.find('Version').text = '4.3.0'  # todo: get version and date from qt components.xml
    tree.find('ReleaseDate').text = '2022-02-24'
    # Write back to file
    tree.write(file, encoding="utf-8", xml_declaration=True)


def build_atlas_package():
    print('current interpreter: ' + sys.executable)

    binary_dir = common_dirs.atlas_binary_dir()
    print('binaryDIR:', binary_dir)
    print('deployTargetDIR:', common_dirs.deploy_target_dir())
    print('qtBaseDIR:', common_dirs.qt_base_dir())

    if common_dirs.is_mac():
        app_name = 'Atlas.app'
        zip_name = 'atlas-macOS.zip'
        shutil.rmtree(os.path.join(common_dirs.deploy_target_dir(), app_name), ignore_errors=True)
        if os.path.exists(os.path.join(common_dirs.deploy_target_dir(), zip_name)):
            os.remove(os.path.join(common_dirs.deploy_target_dir(), zip_name))

        if os.path.exists(os.path.join(binary_dir, app_name)):
            shutil.copytree(os.path.join(binary_dir, app_name),
                            os.path.join(common_dirs.deploy_target_dir(), app_name),
                            symlinks=True)
            subprocess.run([os.path.join(common_dirs.qt_bin_dir(), 'macdeployqt'), app_name],
                           cwd=common_dirs.deploy_target_dir(), shell=False, check=True)

            subprocess.run([os.path.join(common_dirs.deploy_target_dir(), app_name, 'Contents', 'MacOS', 'Atlas'),
                            '--run_unit_tests'], shell=False, check=True)
            subprocess.run(['codesign', '--force', '--deep', '--sign', '-',
                            os.path.join(common_dirs.deploy_target_dir(), app_name)], shell=False, check=True)

            subprocess.run(['zip', '--quiet', '--recurse-paths', '--symlinks', zip_name, app_name],
                           cwd=common_dirs.deploy_target_dir(), shell=False, check=True)
        else:
            sys.stderr.write('Error: atlas is not built yet.\n')
            sys.exit(1)
    elif common_dirs.is_linux():
        app_name = "Atlas"
        zip_name = "atlas-Linux.zip"
        shutil.rmtree(os.path.join(common_dirs.deploy_target_dir(), 'Atlas.AppDir'), ignore_errors=True)
        if os.path.exists(os.path.join(common_dirs.deploy_target_dir(), zip_name)):
            os.remove(os.path.join(common_dirs.deploy_target_dir(), zip_name))

        if os.path.exists(os.path.join(binary_dir, app_name)):
            linuxdeployqt.linuxdeployqt(os.path.join(binary_dir, app_name),
                                        os.path.join(common_dirs.deploy_target_dir(), 'Atlas.AppDir'),
                                        common_dirs.qt_base_dir())
            subprocess.run([os.path.join(common_dirs.deploy_target_dir(), 'Atlas.AppDir', 'Atlas'),
                            '--run_unit_tests'], shell=False, check=True)

            subprocess.run(['zip', '--quiet', '--recurse-paths', '--symlinks', zip_name, 'Atlas.AppDir'],
                           cwd=common_dirs.deploy_target_dir(), shell=False, check=True)
        else:
            sys.stderr.write('Error: atlas is not built yet.\n')
            sys.exit(1)
    else:
        app_name = 'Atlas.exe'
        zip_name = 'atlas-Windows.zip'
        shutil.rmtree(os.path.join(common_dirs.deploy_target_dir(), 'Atlas'), ignore_errors=True)
        if os.path.exists(os.path.join(common_dirs.deploy_target_dir(), zip_name)):
            os.remove(os.path.join(common_dirs.deploy_target_dir(), zip_name))

        if os.path.exists(os.path.join(binary_dir, app_name)):
            os.mkdir(os.path.join(common_dirs.deploy_target_dir(), 'Atlas'))
            shutil.copy2(os.path.join(binary_dir, app_name),
                         os.path.join(common_dirs.deploy_target_dir(), 'Atlas'))
            shutil.copytree(os.path.join(binary_dir, 'Resources'),
                            os.path.join(common_dirs.deploy_target_dir(), 'Atlas', 'Resources'),
                            symlinks=True)
            # build_ext_libs.glob_copy(os.path.join(common_dirs.assimp_redist_dir(), 'assimp*.dll'),
            #                          os.path.join(common_dirs.deploy_target_dir(), 'Atlas'))
            build_ext_libs.glob_copy(os.path.join(common_dirs.freeimage_redist_dir(), '*.dll'),
                                     os.path.join(common_dirs.deploy_target_dir(), 'Atlas'))
            shutil.copy2(os.path.join(common_dirs.tbb_redist_dir(), 'tbb12.dll'),
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
                           check=True)

            shutil.make_archive(os.path.join(common_dirs.deploy_target_dir(), zip_name[0:-4]),
                                'zip',
                                root_dir=common_dirs.deploy_target_dir(),
                                base_dir='Atlas')
        else:
            sys.stderr.write('Error: atlas is not built yet.\n')
            sys.exit(1)


def build_atlas_installer():
    if common_dirs.is_mac():
        suffix = 'macOS'
        app_name = 'Atlas.app'
        repo_package_name = 'atlas.7z'
        mt_app_name = '.tempMaintenanceTool'
        mt_repo_package_name = 'MaintenanceTool.7z'
        installer_base_name = 'AtlasInstaller'
        installer_app_name = 'AtlasInstaller.app'
        installer_zip_name = f'AtlasInstaller-{suffix}.zip'
    elif common_dirs.is_linux():
        suffix = 'Linux'
        app_name = 'Atlas.AppDir'
        repo_package_name = 'atlas.7z'
        mt_app_name = '.tempMaintenanceTool'
        mt_repo_package_name = 'MaintenanceTool.7z'
        installer_base_name = 'AtlasInstaller'
        installer_app_name = 'AtlasInstaller'
        installer_zip_name = f'AtlasInstaller-{suffix}.zip'
    else:
        suffix = 'Windows'
        app_name = 'Atlas'
        repo_package_name = 'atlas.7z'
        mt_app_name = 'tempMaintenanceTool.exe'
        mt_repo_package_name = 'MaintenanceTool.7z'
        installer_base_name = 'AtlasInstaller'
        installer_app_name = 'AtlasInstaller.exe'
        installer_zip_name = f'AtlasInstaller-{suffix}.zip'

    if os.path.exists(os.path.join(common_dirs.deploy_target_dir(), repo_package_name)):
        os.remove(os.path.join(common_dirs.deploy_target_dir(), repo_package_name))
    shutil.rmtree(os.path.join(common_dirs.deploy_target_dir(), suffix), ignore_errors=True)
    if os.path.exists(os.path.join(common_dirs.deploy_target_dir(), installer_zip_name)):
        os.remove(os.path.join(common_dirs.deploy_target_dir(), installer_zip_name))
    if common_dirs.is_mac():
        shutil.rmtree(os.path.join(common_dirs.deploy_target_dir(), installer_app_name), ignore_errors=True)
    elif os.path.exists(os.path.join(common_dirs.deploy_target_dir(), installer_app_name)):
        os.remove(os.path.join(common_dirs.deploy_target_dir(), installer_app_name))

    if os.path.exists(os.path.join(common_dirs.deploy_target_dir(), 'packages', 'fenglab.neutube')):
        shutil.rmtree(os.path.join(common_dirs.deploy_target_dir(), 'packages', 'fenglab.neutube'),
                      ignore_errors=False)
    shutil.copytree(os.path.join(common_dirs.ext_build_dir(), 'packages-' + suffix, 'fenglab.neutube'),
                    os.path.join(common_dirs.deploy_target_dir(), 'packages', 'fenglab.neutube'))

    if os.path.exists(os.path.join(common_dirs.deploy_target_dir(), mt_app_name)):
        os.remove(os.path.join(common_dirs.deploy_target_dir(), mt_app_name))
    if common_dirs.is_windows():
        shutil.copy(os.path.join(common_dirs.qt_installer_framework_bin_dir(), 'installerbase.exe'),
                    os.path.join(common_dirs.deploy_target_dir(), mt_app_name))
    else:
        shutil.copy(os.path.join(common_dirs.qt_installer_framework_bin_dir(), 'installerbase'),
                    os.path.join(common_dirs.deploy_target_dir(), mt_app_name))
    subprocess.run([os.path.join(common_dirs.qt_installer_framework_bin_dir(), 'archivegen'),
                    mt_repo_package_name,
                    os.path.join(common_dirs.deploy_target_dir(), mt_app_name)],
                   cwd=common_dirs.deploy_target_dir(), shell=False, check=True)
    os.remove(os.path.join(common_dirs.deploy_target_dir(), mt_app_name))
    repo_package_folder = os.path.join(common_dirs.deploy_target_dir(),
                                       'packages', 'fenglab.maintenance', 'data')
    if os.path.exists(os.path.join(repo_package_folder, mt_repo_package_name)):
        os.remove(os.path.join(repo_package_folder, mt_repo_package_name))
    shutil.move(os.path.join(common_dirs.deploy_target_dir(), mt_repo_package_name), repo_package_folder)
    update_maintenance_pacakge_xml_version(os.path.join(common_dirs.deploy_target_dir(), 'maintenance_package.xml'),
                                           os.path.join(common_dirs.deploy_target_dir(),
                                                        'packages', 'fenglab.maintenance', 'meta', 'package.xml'))

    subprocess.run([os.path.join(common_dirs.qt_installer_framework_bin_dir(), 'archivegen'),
                    '--compression', '0' if common_dirs.is_windows() else '5',
                    repo_package_name, app_name],
                   cwd=common_dirs.deploy_target_dir(), shell=False, check=True)

    repo_package_folder = os.path.join(common_dirs.deploy_target_dir(),
                                       'packages', 'fenglab.atlas', 'data')
    if os.path.exists(os.path.join(repo_package_folder, repo_package_name)):
        os.remove(os.path.join(repo_package_folder, repo_package_name))
    shutil.move(os.path.join(common_dirs.deploy_target_dir(), repo_package_name), repo_package_folder)
    update_pacakge_xml_version(os.path.join(common_dirs.deploy_target_dir(), 'atlas_package.xml'),
                               os.path.join(common_dirs.deploy_target_dir(),
                                            'packages', 'fenglab.atlas', 'meta', 'package.xml'))

    subprocess.run([os.path.join(common_dirs.qt_installer_framework_bin_dir(), 'repogen'),
                    '-p', 'packages', './' + suffix],
                   cwd=common_dirs.deploy_target_dir(), shell=False, check=True)

    subprocess.run([os.path.join(common_dirs.qt_installer_framework_bin_dir(), 'binarycreator'),
                    '--online-only', '-c', 'config/config-' + suffix + '.xml', '-p', 'packages',
                    installer_base_name],
                   cwd=common_dirs.deploy_target_dir(), shell=False, check=True)

    if common_dirs.is_windows():
        zipfile.ZipFile(os.path.join(common_dirs.deploy_target_dir(), installer_zip_name), mode='w') \
            .write(os.path.join(common_dirs.deploy_target_dir(), installer_app_name), arcname=installer_app_name)
    else:
        subprocess.run(['zip', '--quiet', '--recurse-paths', '--symlinks', installer_zip_name, installer_app_name],
                       cwd=common_dirs.deploy_target_dir(), shell=False, check=True)

    if 'feng' in os.path.expanduser("~"):
        if common_dirs.is_mac():
            out_folder = os.path.join(os.path.expanduser('~'), 'Google Drive', "code", 'my', 'proxy', 'static')
            shutil.copy2(os.path.join(common_dirs.deploy_target_dir(), installer_zip_name),
                         os.path.join(out_folder, 'installers', installer_zip_name))
            target_folder = os.path.join(out_folder, 'packages')
            if os.path.exists(os.path.join(target_folder, suffix)):
                shutil.rmtree(os.path.join(target_folder, suffix), ignore_errors=False)
            shutil.move(os.path.join(common_dirs.deploy_target_dir(), suffix), target_folder)
        elif common_dirs.is_linux():
            subprocess.run(['scp', installer_zip_name,
                            'feng@labmacpro:"/Users/feng/Google Drive/code/my/proxy/static/installers/"'],
                           cwd=common_dirs.deploy_target_dir(), shell=False, check=True)
            subprocess.run(['rsync', '-av', '--delete', suffix,
                            'feng@labmacpro:"/Users/feng/Google Drive/code/my/proxy/static/packages/"'],
                           cwd=common_dirs.deploy_target_dir(), shell=False, check=True)
            shutil.rmtree(os.path.join(common_dirs.deploy_target_dir(), suffix), ignore_errors=False)
        else:
            out_folder = os.path.join('Z:', os.sep, 'Google Drive', "code", 'my', 'proxy', 'static')
            if not os.path.exists(out_folder):
                out_folder = os.path.join(os.path.expanduser('~'), 'GoogleDrive', "code", 'my', 'proxy', 'static')
            if not os.path.exists(out_folder):
                out_folder = os.path.join(os.path.expanduser('~'), 'Google Drive', "code", 'my', 'proxy', 'static')
            shutil.copy2(os.path.join(common_dirs.deploy_target_dir(), installer_zip_name),
                         os.path.join(out_folder, 'installers', installer_zip_name))
            target_folder = os.path.join(out_folder, 'packages')
            if os.path.exists(os.path.join(target_folder, suffix)):
                shutil.rmtree(os.path.join(target_folder, suffix), ignore_errors=False)
            shutil.move(os.path.join(common_dirs.deploy_target_dir(), suffix), target_folder)


def deploy_atlas():
    build_atlas_package()
    build_atlas_installer()


if __name__ == "__main__":
    deploy_atlas()
