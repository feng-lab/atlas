import errno
import glob
import logging
import os
import shutil
import stat
import subprocess
import sys
import tarfile
import time
import zipfile

from packaging import version

logger = logging.getLogger(__name__)


def is_windows() -> bool:
    return sys.platform.startswith('win')


def is_mac() -> bool:
    return sys.platform.startswith('darwin')


def is_linux() -> bool:
    return sys.platform.startswith('linux')


def use_clang_cl() -> bool:
    return is_windows() and False


def use_ninja() -> bool:
    return True


def curr_dir() -> str:
    return os.path.abspath(os.path.dirname(__file__))


def atlas_repository_dir() -> str:
    res = os.path.normpath(os.path.join(curr_dir(), '..'))
    assert os.path.exists(res)
    return res


def atlas_util_dir() -> str:
    res = os.path.join(atlas_repository_dir(), 'util')
    assert os.path.exists(res)
    return res


def atlas_src_dir() -> str:
    res = os.path.join(atlas_repository_dir(), 'src')
    assert os.path.exists(res)
    return res


def ext_dir() -> str:
    res = os.path.join(atlas_src_dir(), '3rdparty')
    assert os.path.exists(res)
    return res


def ext_build_dir() -> str:
    res = os.path.join(atlas_src_dir(), '3rdparty', 'build')
    if not os.path.exists(res):
        os.mkdir(res)
    assert os.path.exists(res)
    return res


def ext_conda_build_dir() -> str:
    res = os.path.join(ext_build_dir(), 'conda_build')
    if not os.path.exists(res):
        os.mkdir(res)
    assert os.path.exists(res)
    return res


def atlas_dir() -> str:
    res = os.path.join(atlas_src_dir(), 'atlas')
    assert os.path.exists(res)
    return res


def img_dir() -> str:
    res = os.path.join(atlas_src_dir(), 'img')
    assert os.path.exists(res)
    return res


def python_package_dir() -> str:
    res = os.path.join(atlas_src_dir(), 'python')
    assert os.path.exists(res)
    return res


def resource_dir() -> str:
    res = os.path.join(atlas_dir(), 'Resources')
    assert os.path.exists(res)
    return res


def static_deploy_folder() -> str:
    return os.path.join(os.path.expanduser('~'), 'Dropbox', 'code', 'my', 'proxy', 'static')


def is_my_computer() -> bool:
    return (os.path.exists(static_deploy_folder()) and
            os.path.exists(os.path.join(static_deploy_folder(), 'atlas_deps')) and
            os.path.exists(os.path.join(static_deploy_folder(), 'atlas_test_data')))


def static_src_package_dir() -> str:
    res = os.path.join(static_deploy_folder(), 'atlas_deps')
    assert os.path.exists(res)
    return res


def src_package_dir() -> str:
    if is_my_computer():
        res = static_src_package_dir()
    else:
        res = os.path.join(atlas_repository_dir(), 'atlas_deps')
        assert os.path.exists(res)
    return res


def static_atlas_test_data_dir() -> str:
    res = os.path.join(static_deploy_folder(), 'atlas_test_data')
    assert os.path.exists(res)
    return res


def atlas_test_data_dir() -> str:
    if is_my_computer():
        res = static_atlas_test_data_dir()
    else:
        res = os.path.join(atlas_repository_dir(), 'atlas_test_data')
        assert os.path.exists(res)
    return res


def atlas_build_dir(arm64: bool = False) -> str:
    if use_ninja():
        res = os.path.join(atlas_repository_dir(), 'cmake-build-release-ninja')
    else:
        res = os.path.join(atlas_repository_dir(), 'cmake-build-release')
    if arm64:
        res += '-arm64'
    if not os.path.exists(res):
        os.mkdir(res)
    assert os.path.exists(res)
    return res


def python_package_build_dir() -> str:
    if use_ninja():
        res = os.path.join(atlas_repository_dir(), 'cmake-build-python-ninja')
    else:
        res = os.path.join(atlas_repository_dir(), 'cmake-build-python')
    if not os.path.exists(res):
        os.mkdir(res)
    assert os.path.exists(res)
    return res


def atlas_binary_dir(arm64: bool = False) -> str:
    res = os.path.join(atlas_build_dir(arm64=arm64), 'src', 'atlas')
    if not use_ninja() and sys.platform.startswith('win32'):
        res = os.path.join(atlas_build_dir(arm64=arm64), 'src', 'atlas', 'Release')
    assert os.path.exists(res)
    return res


def deploy_target_dir() -> str:
    return os.path.join(atlas_repository_dir(), 'deploy')


def qt_install_dir() -> str:
    if is_windows():
        res = os.path.join('C:', os.sep, 'Qt')
        if not os.path.exists(res):
            res = os.path.join(os.path.expanduser('~'), 'Qt')
    elif is_mac():
        res = os.path.join(os.path.expanduser('~'), 'Qt')
    else:
        res = os.path.join(os.path.expanduser('~'), 'Qt')
    assert os.path.exists(res)
    return res


def qt_compiler_name() -> str:
    if is_windows():
        return 'msvc2022_64'
    elif is_mac():
        return 'macos'
    else:
        return 'gcc_64'


def qmake_bin_name() -> str:
    if sys.platform.startswith('win32'):
        return 'qmake.exe'
    else:
        return 'qmake'


def qt_ver() -> str:
    vers = [fd for fd in os.listdir(qt_install_dir()) if
            os.path.exists(os.path.join(qt_install_dir(), fd, qt_compiler_name(), 'bin', qmake_bin_name()))]
    assert vers, "No valid QT versions found."
    vers = sorted(vers, key=version.parse)
    ver = vers[-1]
    return ver


def qt_base_dir() -> str:
    return os.path.join(qt_install_dir(), qt_ver(), qt_compiler_name())


def qt_bin_dir() -> str:
    return os.path.join(qt_base_dir(), 'bin')


def qmake_bin() -> str:
    return os.path.join(qt_bin_dir(), qmake_bin_name())


def qt_installer_framework_ver() -> str:
    folder = os.path.join(qt_install_dir(), 'Tools', 'QtInstallerFramework')
    vers = [fd for fd in os.listdir(folder) if
            os.path.exists(os.path.join(folder, fd, 'bin'))]
    assert vers
    vers = sorted(vers, key=version.parse)
    ver = vers[-1]
    return ver


def qt_installer_framework_bin_dir() -> str:
    folder = os.path.join(qt_install_dir(), 'Tools', 'QtInstallerFramework')
    return os.path.join(folder, qt_installer_framework_ver(), 'bin')


def vulkan_SDK_dir() -> str:
    if is_windows():
        res = os.path.join('C:', os.sep, 'VulkanSDK')
        if not os.path.exists(res):
            res = os.path.join(os.path.expanduser('~'), 'VulkanSDK')
    elif is_mac():
        res = os.path.join(os.path.expanduser('~'), 'VulkanSDK')
    else:
        res = os.path.join(os.path.expanduser('~'), 'VulkanSDK')
    assert os.path.exists(res)
    return res


def _vulkan_SDK_bin_folder_name() -> str:
    if is_windows():
        return 'Bin'
    elif is_mac():
        return 'macOS/bin'
    else:
        return 'x86_64/bin'


def vulkan_SDK_ver() -> str:
    vers = [fd for fd in os.listdir(vulkan_SDK_dir()) if
            os.path.exists(os.path.join(vulkan_SDK_dir(), fd, _vulkan_SDK_bin_folder_name()))]
    assert vers, "No valid vulkan SDK versions found."
    vers = sorted(vers, key=version.parse)
    ver = vers[-1]
    return ver


def _vulkan_SDK_env_folder_name() -> str:
    if is_windows():
        return ''
    elif is_mac():
        return 'macOS'
    else:
        return 'x86_64'


def vulkan_SDK_env_dir() -> str:
    res = os.path.join(vulkan_SDK_dir(), vulkan_SDK_ver(), _vulkan_SDK_env_folder_name())
    assert os.path.exists(res)
    return res


def vulkan_SDK_bin_dir() -> str:
    res = os.path.join(vulkan_SDK_dir(), vulkan_SDK_ver(), _vulkan_SDK_bin_folder_name())
    assert os.path.exists(res)
    return res


def vs_install_dir() -> str:
    assert sys.platform.startswith('win32')

    vsinstalldir = r'C:\Program Files\Microsoft Visual Studio\2022\Community'
    if not os.path.exists(vsinstalldir):
        vsinstalldir = r'C:\Program Files\Microsoft Visual Studio\2022\Enterprise'
    assert os.path.exists(vsinstalldir)

    return vsinstalldir


def vc_redist_dir() -> str:
    assert sys.platform.startswith('win32')

    vc_redist_version_filename = os.path.join(vs_install_dir(), 'VC', 'Auxiliary', 'Build',
                                              'Microsoft.VCRedistVersion.default.txt')
    assert os.path.exists(vc_redist_version_filename)
    with open(vc_redist_version_filename, mode='r', encoding='utf-8') as f:
        vc_redist_version = f.readline().rstrip()

    res = os.path.join(vs_install_dir(), 'VC', 'Redist', 'MSVC', vc_redist_version)
    assert os.path.exists(res)
    return res


def vc_CRT_redist_dir() -> str:
    assert sys.platform.startswith('win32')

    res = os.path.join(vc_redist_dir(), 'x64', 'Microsoft.VC143.CRT')
    assert os.path.exists(res)
    return res


def vc_CXXAMP_redist_dir() -> str:
    assert sys.platform.startswith('win32')

    res = os.path.join(vc_redist_dir(), 'x64', 'Microsoft.VC143.CXXAMP')
    assert os.path.exists(res)
    return res


def vc_OpenMP_redist_dir() -> str:
    assert sys.platform.startswith('win32')

    res = os.path.join(vc_redist_dir(), 'x64', 'Microsoft.VC143.OpenMP')
    assert os.path.exists(res)
    return res


def intel_sw_dir() -> str:
    if sys.platform.startswith('win32'):
        # res = os.path.join('C:', os.sep, 'Program Files (x86)', 'IntelSWTools', 'compilers_and_libraries', 'windows')
        res = os.path.join('C:', os.sep, 'Program Files (x86)', 'Intel', 'oneAPI')
    else:
        res = os.path.join(os.sep, 'opt', 'intel', 'oneapi')
    if not os.path.exists(res):
        res = os.path.join(os.path.expanduser('~'), 'oneapi')
    assert os.path.exists(res)
    return res


def tbb_dir() -> str:
    if is_mac():
        return ext_build_dir()
    else:
        return intel_sw_dir() + '/tbb/latest/lib/cmake/tbb'


def tbb_redist_dir() -> str:
    assert sys.platform.startswith('win32')

    res = os.path.join(intel_sw_dir(), 'tbb', 'latest', 'bin')
    assert os.path.exists(res)
    return res


# def assimp_redist_dir() -> str:
#     assert sys.platform.startswith('win32')
#
#     res = os.path.join(ext_build_dir(), 'bin')
#     assert os.path.exists(res)
#     return res


def freeimage_redist_dir() -> str:
    assert sys.platform.startswith('win32')

    res = os.path.join(ext_build_dir(), 'freeimage')
    assert os.path.exists(res)
    return res


def find_src_package_with_glob(files: str):
    file_list = glob.glob(files)
    if len(file_list) == 1:
        return file_list[0]
    elif len(file_list) == 0:
        raise Exception("Can not find matching package with pattern: " + files)
    else:
        raise Exception("Find more than one matching packages with pattern: " + files)


def remove_old_src_folder_with_glob(folder: str):
    folder_list = glob.glob(folder)
    if len(folder_list) == 1:
        shutil.rmtree(folder_list[0], ignore_errors=False)
    elif len(folder_list) > 1:
        raise Exception("Find more than one matching folders with pattern: " + folder)


def get_7za_binary() -> str:
    if is_windows():
        return os.path.join(atlas_repository_dir(), 'util', '7za.exe')
    else:
        return '7za'


def get_package_top_level_folder(file: str):
    res = ''
    if file.lower().endswith('.zip'):
        with zipfile.ZipFile(file, mode='r') as zf:
            logger.info(zf.namelist())
            res = os.path.commonpath(
                [nm for nm in zf.namelist() if not nm.endswith('/') and not nm.startswith('__MACOSX')])
    elif file.lower().endswith('.tar.gz') or file.lower().endswith('.tar.bz2') or file.lower().endswith('.tar.xz') \
            or file.lower().endswith('.tgz'):
        with tarfile.open(file, mode='r|*') as tf:
            names = [nm for nm in tf.getnames() if not nm == '.']
            res = os.path.commonpath(names)
    elif file.lower().endswith('.7z'):
        cp = subprocess.run([get_7za_binary(), 'l', '-slt', '-sccUTF-8', file],
                            stdout=subprocess.PIPE, encoding='utf-8')
        started = False
        filenames = []
        for line in cp.stdout.splitlines():
            if started:
                if line.startswith('Path = '):
                    filenames.append(line.replace('Path = ', ''))
            else:
                if line.startswith('-------'):
                    started = True
        res = os.path.commonpath(filenames)

    if res.endswith('/') or res.endswith('\\'):
        res = res[:-1]
    return res


def unpack_file_to_folder(file: str, folder: str):
    logger.info(f'unpacking {file}')
    if file.lower().endswith('.zip'):
        with zipfile.ZipFile(file, mode='r') as zf:
            zf.extractall(path=folder)
    elif file.lower().endswith('.tar.gz') or file.lower().endswith('.tar.bz2') or file.lower().endswith('.tar.xz') \
            or file.lower().endswith('.tgz'):
        with tarfile.open(file, mode='r|*') as tf:
            tf.extractall(path=folder)
    elif file.lower().endswith('.7z'):
        subprocess.run([get_7za_binary(), 'x', '-y', '-o' + folder, file], shell=False, check=True)


def unpack_tool_to_target_dir(tool_package_folder: str, tool_package_glob_name: str,
                              tool_folder_glob_name=None, *, target_dir: str = ext_build_dir()) -> str:
    if tool_folder_glob_name is None:
        tool_folder_glob_name = tool_package_glob_name
    package_name = find_src_package_with_glob(os.path.join(tool_package_folder, tool_package_glob_name))
    package_folder = get_package_top_level_folder(package_name)
    if not package_folder:
        package_unpack_folder = os.path.join(target_dir, os.path.splitext(os.path.basename(package_name))[0])
    else:
        package_unpack_folder = os.path.join(target_dir, package_folder)
    logger.info(f'package unpack folder: {package_unpack_folder}')
    if not os.path.exists(package_unpack_folder):
        remove_old_src_folder_with_glob(os.path.join(target_dir, tool_folder_glob_name))
        unpack_file_to_folder(package_name, target_dir if package_folder else package_unpack_folder)
        assert os.path.exists(package_unpack_folder)
    return package_unpack_folder


def install_cmake():
    if is_windows():
        unpack_tool_to_target_dir(src_package_dir(), 'cmake*windows*')
    elif is_linux():
        unpack_tool_to_target_dir(src_package_dir(), 'cmake*linux*')
    else:
        unpack_tool_to_target_dir(src_package_dir(), 'cmake*macos*')


# to install new version of ninja, delete existing binary in atlas/src/3rdparty/build/ninja
def install_ninja():
    target_dir = ext_build_dir()
    if os.path.exists(os.path.join(target_dir, 'ninja')) or os.path.exists(os.path.join(target_dir, 'ninja.exe')):
        return
    if is_windows():
        unpack_file_to_folder(os.path.join(src_package_dir(), 'ninja-win.zip'), target_dir)
    elif is_linux():
        unpack_file_to_folder(os.path.join(src_package_dir(), 'ninja-linux.zip'), target_dir)
        os.chmod(os.path.join(target_dir, 'ninja'), stat.S_IRWXU or stat.S_IXGRP or stat.S_IRGRP or stat.S_IROTH)
    else:
        unpack_file_to_folder(os.path.join(src_package_dir(), 'ninja-mac.zip'), target_dir)
        os.chmod(os.path.join(target_dir, 'ninja'), stat.S_IRWXU or stat.S_IXGRP or stat.S_IRGRP or stat.S_IROTH)


def install_ffmpeg():
    if is_windows():
        unpack_tool_to_target_dir(src_package_dir(), 'ffmpeg*win*')
    elif is_linux():
        unpack_tool_to_target_dir(src_package_dir(), 'ffmpeg*linux*')
    else:
        if os.path.lexists(os.path.join(ext_build_dir(), 'ffmpeg')):
            os.remove(os.path.join(ext_build_dir(), 'ffmpeg'))
        unpack_tool_to_target_dir(src_package_dir(), 'ffmpeg*intel*')
        os.rename(os.path.join(ext_build_dir(), 'ffmpeg'), os.path.join(ext_build_dir(), 'ffmpeg-intel'))
        unpack_tool_to_target_dir(src_package_dir(), 'ffmpeg*arm*')
        os.rename(os.path.join(ext_build_dir(), 'ffmpeg'), os.path.join(ext_build_dir(), 'ffmpeg-arm'))
        subprocess.run(['lipo', '-create', os.path.join(ext_build_dir(), 'ffmpeg-intel'),
                        os.path.join(ext_build_dir(), 'ffmpeg-arm'), '-output',
                        os.path.join(ext_build_dir(), 'ffmpeg')], cwd=ext_build_dir(), shell=False, check=True)
        os.remove(os.path.join(ext_build_dir(), 'ffmpeg-intel'))
        os.remove(os.path.join(ext_build_dir(), 'ffmpeg-arm'))
        subprocess.run(['xattr', '-cr', os.path.join(ext_build_dir(), 'ffmpeg')], cwd=ext_build_dir(), shell=False,
                       check=True)
        subprocess.run(['codesign', '--force', '--deep', '--sign', '-', os.path.join(ext_build_dir(), 'ffmpeg')],
                       cwd=ext_build_dir(),
                       shell=False,
                       check=True)
        os.chmod(os.path.join(ext_build_dir(), 'ffmpeg'), stat.S_IRWXU or stat.S_IXGRP or stat.S_IRGRP or stat.S_IROTH)
        if is_my_computer():
            # from https://evermeet.cx/ffmpeg/
            if os.path.lexists('/usr/local/bin/ffmpeg'):
                os.remove('/usr/local/bin/ffmpeg')
            os.symlink(os.path.join(ext_build_dir(), 'ffmpeg'), '/usr/local/bin/ffmpeg')


def get_cmake_binary() -> str:
    if is_windows():
        cmake_folder = find_src_package_with_glob(os.path.join(ext_build_dir(), 'cmake-*windows*'))
        return os.path.join(cmake_folder, 'bin', 'cmake')
    elif is_linux():
        cmake_folder = find_src_package_with_glob(os.path.join(ext_build_dir(), 'cmake-*linux*_64'))
        return os.path.join(cmake_folder, 'bin', 'cmake')
    else:
        cmake_folder = find_src_package_with_glob(os.path.join(ext_build_dir(), 'cmake-*macos*'))
        return os.path.join(cmake_folder, 'CMake.app', 'Contents', 'bin', 'cmake')


def get_ctest_binary() -> str:
    if is_windows():
        cmake_folder = find_src_package_with_glob(os.path.join(ext_build_dir(), 'cmake-*windows*'))
        return os.path.join(cmake_folder, 'bin', 'ctest')
    elif is_linux():
        cmake_folder = find_src_package_with_glob(os.path.join(ext_build_dir(), 'cmake-*linux*_64'))
        return os.path.join(cmake_folder, 'bin', 'ctest')
    else:
        cmake_folder = find_src_package_with_glob(os.path.join(ext_build_dir(), 'cmake-*macos*'))
        return os.path.join(cmake_folder, 'CMake.app', 'Contents', 'bin', 'ctest')


def get_ninja_binary() -> str:
    if is_windows():
        return os.path.join(ext_build_dir(), 'ninja.exe')
    else:
        return os.path.join(ext_build_dir(), 'ninja')


def get_ffmpeg_binary() -> str:
    if is_windows():
        folder = find_src_package_with_glob(os.path.join(ext_build_dir(), 'ffmpeg*win*'))
        return os.path.join(folder, 'bin', 'ffmpeg.exe')
    elif is_linux():
        folder = find_src_package_with_glob(os.path.join(ext_build_dir(), 'ffmpeg*linux*'))
        return os.path.join(folder, 'bin', 'ffmpeg')
    else:
        folder = find_src_package_with_glob(os.path.join(ext_build_dir(), 'ffmpeg'))
        return folder


def install_gperf():
    if is_windows():
        unpack_tool_to_target_dir(src_package_dir(), 'gperf*-bin*')
    else:
        assert False


def get_gperf_dir() -> str:
    if is_windows():
        folder = find_src_package_with_glob(os.path.join(ext_build_dir(), 'gperf*-bin*'))
        return os.path.join(folder, 'bin')
    else:
        assert False


def handleRemoveReadonly(func, path, excinfo):
    """
    Helper for shutil.rmtree error handling on Python 3.12+ using the
    ``onexc`` callback (func, path, excinfo).

    Clears readonly bits when the failure is a permissions error, then
    retries the removal once; otherwise re-raises the original exception.
    """
    if isinstance(excinfo, OSError) and excinfo.errno in (errno.EACCES, errno.EPERM):
        os.chmod(path, stat.S_IRWXU | stat.S_IRWXG | stat.S_IRWXO)  # 0777
        if func is os.open:
            fd = os.open(path, os.O_RDONLY)
            os.close(fd)
        elif func is os.scandir:
            with os.scandir(path) as it:
                list(it)
        else:
            func(path)
        return

    raise excinfo


def rm_tree(path: str, attempts: int = 3):
    if not os.path.exists(path):
        return
    for _ in range(attempts):
        try:
            shutil.rmtree(path, onexc=handleRemoveReadonly)
            return
        except OSError:
            time.sleep(0.5)
    shutil.rmtree(path, ignore_errors=True)


if __name__ == "__main__":
    print(vulkan_SDK_ver())
    print('done')
