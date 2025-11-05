import os
import argparse
import sys
import shutil
import subprocess
import xml.etree.ElementTree as eTree
import datetime
import zipfile
import logging

import common_dirs
import linuxdeployqt
import build_ext_libs
from logger import setup_logger

logger = logging.getLogger(__name__)


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
    tree.find('Version').text = '4.7.0'  # todo: get version and date from qt components.xml
    tree.find('ReleaseDate').text = '2024-02-15'
    # Write back to file
    tree.write(file, encoding="utf-8", xml_declaration=True)


def build_atlas_package(is_debug_version: bool = False):
    logger.info(f'current interpreter: {sys.executable}')

    binary_dir = common_dirs.atlas_binary_dir()
    logger.info(f'binaryDIR: {binary_dir}')
    logger.info(f'deployTargetDIR: {common_dirs.deploy_target_dir()}')
    logger.info(f'qtBaseDIR: {common_dirs.qt_base_dir()}')

    if common_dirs.is_mac():
        app_name = 'Atlas.app'

        shutil.rmtree(os.path.join(common_dirs.deploy_target_dir(), app_name), ignore_errors=True)

        if os.path.exists(os.path.join(binary_dir, app_name)):
            shutil.copytree(os.path.join(binary_dir, app_name),
                            os.path.join(common_dirs.deploy_target_dir(), app_name),
                            symlinks=True)
            subprocess.run([os.path.join(common_dirs.qt_bin_dir(), 'macdeployqt'), app_name],
                           cwd=common_dirs.deploy_target_dir(), shell=False, check=True)
        else:
            logger.critical('atlas is not built yet')

        binary_dir = common_dirs.atlas_binary_dir(arm64=True)
        logger.info(f'arm64 binaryDIR: {binary_dir}')
        arm64_app_name = 'Atlas_arm64.app'

        shutil.rmtree(os.path.join(common_dirs.deploy_target_dir(), arm64_app_name), ignore_errors=True)

        if os.path.exists(os.path.join(binary_dir, app_name)):
            shutil.copytree(os.path.join(binary_dir, app_name),
                            os.path.join(common_dirs.deploy_target_dir(), arm64_app_name),
                            symlinks=True)
            subprocess.run([os.path.join(common_dirs.qt_bin_dir(), 'macdeployqt'), arm64_app_name],
                           cwd=common_dirs.deploy_target_dir(), shell=False, check=True)
        else:
            logger.critical('arm64 atlas is not built yet')

        filename = os.path.join(common_dirs.deploy_target_dir(), arm64_app_name, 'Contents', 'MacOS', 'Atlas')
        target_filename = os.path.join(common_dirs.deploy_target_dir(), app_name, 'Contents', 'MacOS', 'Atlas')
        logger.info(f'merge {filename} to {target_filename}')
        subprocess.run(['lipo', '-create', filename, target_filename, '-output', target_filename],
                       shell=False, check=True)

        # Ensure LLM docs are generated in repo and copied into the .app
        repo_root = common_dirs.atlas_repository_dir()
        repo_llm_dir = os.path.join(repo_root, 'src', 'atlas', 'Resources', 'json', 'atlas')
        os.makedirs(repo_llm_dir, exist_ok=True)
        # Generate missing docs using the agent CLI; use deploy .app as --atlas-dir
        subprocess.run([sys.executable, '-m', 'tools.atlas_agent', '--prepare-llm-docs-only',
                        '--atlas-dir', os.path.join(common_dirs.deploy_target_dir(), app_name)],
                       cwd=repo_root, shell=False, check=False)
        # Copy repo docs into the deployed app
        target_llm_dir = os.path.join(common_dirs.deploy_target_dir(), app_name, 'Contents', 'Resources', 'json', 'atlas')
        os.makedirs(target_llm_dir, exist_ok=True)
        shutil.copytree(repo_llm_dir, target_llm_dir, dirs_exist_ok=True)

        subprocess.run(['codesign', '--force', '--deep', '--sign', '-',
                        os.path.join(common_dirs.deploy_target_dir(), app_name)], shell=False, check=True)
    elif common_dirs.is_linux():
        app_name = "Atlas"

        shutil.rmtree(os.path.join(common_dirs.deploy_target_dir(), 'Atlas.AppDir'), ignore_errors=True)

        if os.path.exists(os.path.join(binary_dir, app_name)):
            linuxdeployqt.linuxdeployqt(os.path.join(binary_dir, app_name),
                                        os.path.join(common_dirs.deploy_target_dir(), 'Atlas.AppDir'),
                                        common_dirs.qt_base_dir(),
                                        is_debug_version=is_debug_version)
            # Ensure LLM docs are generated in repo and copied into AppDir
            repo_root = common_dirs.atlas_repository_dir()
            repo_llm_dir = os.path.join(repo_root, 'src', 'atlas', 'Resources', 'json', 'atlas')
            os.makedirs(repo_llm_dir, exist_ok=True)
            subprocess.run([sys.executable, '-m', 'tools.atlas_agent', '--prepare-llm-docs-only',
                            '--atlas-dir', os.path.join(common_dirs.deploy_target_dir(), 'Atlas.AppDir')],
                           cwd=repo_root, shell=False, check=False)
            target_llm_dir = os.path.join(common_dirs.deploy_target_dir(), 'Atlas.AppDir', 'Resources', 'json', 'atlas')
            os.makedirs(target_llm_dir, exist_ok=True)
            shutil.copytree(repo_llm_dir, target_llm_dir, dirs_exist_ok=True)
        else:
            logger.critical('atlas is not built yet')
    else:
        app_name = 'Atlas.exe'

        shutil.rmtree(os.path.join(common_dirs.deploy_target_dir(), 'Atlas'), ignore_errors=True)

        if os.path.exists(os.path.join(binary_dir, app_name)):
            os.mkdir(os.path.join(common_dirs.deploy_target_dir(), 'Atlas'))
            shutil.copy2(os.path.join(binary_dir, app_name),
                         os.path.join(common_dirs.deploy_target_dir(), 'Atlas'))
            shutil.copytree(os.path.join(binary_dir, 'Resources'),
                            os.path.join(common_dirs.deploy_target_dir(), 'Atlas', 'Resources'),
                            symlinks=True)
            shutil.copytree(os.path.join(binary_dir, 'vulkan'),
                            os.path.join(common_dirs.deploy_target_dir(), 'Atlas', 'vulkan'),
                            symlinks=True)
            # build_ext_libs.glob_copy(os.path.join(common_dirs.assimp_redist_dir(), 'assimp*.dll'),
            #                          os.path.join(common_dirs.deploy_target_dir(), 'Atlas'))
            build_ext_libs.glob_copy(os.path.join(common_dirs.freeimage_redist_dir(), '*.dll'),
                                     os.path.join(common_dirs.deploy_target_dir(), 'Atlas'))
            # build_ext_libs.glob_copy(os.path.join(common_dirs.ext_build_dir(), 'bin', 'vtktoken-*.dll'),
            #                          os.path.join(common_dirs.deploy_target_dir(), 'Atlas'))
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
            subprocess.run([os.path.join(common_dirs.qt_bin_dir(), 'windeployqt'), '--no-translations', app_name],
                           cwd=os.path.join(common_dirs.deploy_target_dir(), 'Atlas'), shell=False, check=True, env=env)
            # Ensure LLM docs are generated in repo and copied into deploy folder
            repo_root = common_dirs.atlas_repository_dir()
            repo_llm_dir = os.path.join(repo_root, 'src', 'atlas', 'Resources', 'json', 'atlas')
            os.makedirs(repo_llm_dir, exist_ok=True)
            subprocess.run([sys.executable, '-m', 'tools.atlas_agent', '--prepare-llm-docs-only',
                            '--atlas-dir', os.path.join(common_dirs.deploy_target_dir(), 'Atlas')],
                           cwd=repo_root, shell=False, check=False)
            target_llm_dir = os.path.join(common_dirs.deploy_target_dir(), 'Atlas', 'Resources', 'json', 'atlas')
            os.makedirs(target_llm_dir, exist_ok=True)
            shutil.copytree(repo_llm_dir, target_llm_dir, dirs_exist_ok=True)
        else:
            logger.critical('atlas is not built yet')


def pack_atlas_package():
    if common_dirs.is_mac():
        app_name = 'Atlas.app'
        zip_name = 'atlas-macOS.zip'

        if os.path.exists(os.path.join(common_dirs.deploy_target_dir(), zip_name)):
            os.remove(os.path.join(common_dirs.deploy_target_dir(), zip_name))

        subprocess.run(['zip', '--quiet', '--recurse-paths', '--symlinks', zip_name, app_name],
                       cwd=common_dirs.deploy_target_dir(), shell=False, check=True)
    elif common_dirs.is_linux():
        app_name = "Atlas.AppDir"
        zip_name = "atlas-Linux.zip"

        if os.path.exists(os.path.join(common_dirs.deploy_target_dir(), zip_name)):
            os.remove(os.path.join(common_dirs.deploy_target_dir(), zip_name))

        subprocess.run(['zip', '--quiet', '--recurse-paths', '--symlinks', zip_name, app_name],
                       cwd=common_dirs.deploy_target_dir(), shell=False, check=True)
    else:
        app_name = 'Atlas'
        zip_name = 'atlas-Windows.zip'

        if os.path.exists(os.path.join(common_dirs.deploy_target_dir(), zip_name)):
            os.remove(os.path.join(common_dirs.deploy_target_dir(), zip_name))

        shutil.make_archive(os.path.join(common_dirs.deploy_target_dir(), zip_name[0:-4]),
                            'zip',
                            root_dir=common_dirs.deploy_target_dir(),
                            base_dir=app_name)


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
                      ignore_errors=False, onerror=common_dirs.handleRemoveReadonly)
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
                    '--compression', '5',
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

    if common_dirs.is_my_computer():
        if common_dirs.is_mac():
            out_folder = common_dirs.static_deploy_folder()
            shutil.copy2(os.path.join(common_dirs.deploy_target_dir(), installer_zip_name),
                         os.path.join(out_folder, 'installers', installer_zip_name))
            target_folder = os.path.join(out_folder, 'packages')
            if os.path.exists(os.path.join(target_folder, suffix)):
                shutil.rmtree(os.path.join(target_folder, suffix), ignore_errors=False)
            shutil.move(os.path.join(common_dirs.deploy_target_dir(), suffix), target_folder)
        elif common_dirs.is_windows():
            out_folder = common_dirs.static_deploy_folder()
            shutil.copy2(os.path.join(common_dirs.deploy_target_dir(), installer_zip_name),
                         os.path.join(out_folder, 'installers', installer_zip_name))
            target_folder = os.path.join(out_folder, 'packages')
            if os.path.exists(os.path.join(target_folder, suffix)):
                shutil.rmtree(os.path.join(target_folder, suffix), ignore_errors=False,
                              onerror=common_dirs.handleRemoveReadonly)
            shutil.move(os.path.join(common_dirs.deploy_target_dir(), suffix), target_folder)


def deploy_atlas(is_debug_version: bool = False):
    build_atlas_package(is_debug_version=is_debug_version)
    if not is_debug_version:
        pack_atlas_package()
        build_atlas_installer()


if __name__ == "__main__":
    logger = setup_logger()

    parser = argparse.ArgumentParser(
        epilog=f"""
Examples:

python deploy_atlas.py [--debug-version]
""",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--debug-version", action='store_true', help="debug version")
    args = parser.parse_args()

    deploy_atlas(is_debug_version=args.debug_version)
