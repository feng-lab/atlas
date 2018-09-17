import os
import sys
import xml.etree.ElementTree as eTree
import difflib
from pkg_resources import parse_version
import shutil
import glob
import tarfile
import zipfile
import stat
import subprocess


def use_ninja() -> bool:
    return True


def curr_dir() -> str:
    return os.path.abspath(os.path.dirname(__file__))


def repository_dir() -> str:
    res = os.path.normpath(os.path.join(curr_dir(), '..'))
    assert os.path.exists(res)
    return res


def base_dir() -> str:
    res = os.path.normpath(os.path.join(repository_dir(), '..'))
    assert os.path.exists(res)
    return res


def src_dir() -> str:
    res = os.path.join(repository_dir(), 'src')
    assert os.path.exists(res)
    return res


def ext_dir() -> str:
    res = os.path.join(src_dir(), '3rdparty')
    assert os.path.exists(res)
    return res


def atlas_dir() -> str:
    res = os.path.join(src_dir(), 'atlas')
    assert os.path.exists(res)
    return res


def img_dir() -> str:
    res = os.path.join(src_dir(), 'img')
    assert os.path.exists(res)
    return res


def resource_dir() -> str:
    res = os.path.join(atlas_dir(), 'Resources')
    assert os.path.exists(res)
    return res


def src_package_dir() -> str:
    res = os.path.join(base_dir(), 'atlas_others')
    if not os.path.exists(res):
        if sys.platform.startswith('win'):
            res = os.path.join('Z:', os.sep, 'Google Drive', 'code', 'my', 'atlas_others')
        else:
            res = os.path.join(os.path.expanduser('~'), 'Google Drive', 'code', 'my', 'atlas_others')
    assert os.path.exists(res)
    return res


def build_dir() -> str:
    if use_ninja():
        res = os.path.join(repository_dir(), 'cmake-build-release-ninja')
    else:
        res = os.path.join(repository_dir(), 'cmake-build-release')
    if not os.path.exists(res):
        os.mkdir(res)
    assert os.path.exists(res)
    return res


def binary_dir() -> str:
    res = os.path.join(build_dir(), 'src', 'atlas')
    if not use_ninja() and sys.platform.startswith('win32'):
        res = os.path.join(build_dir(), 'src', 'atlas', 'Release')
    assert os.path.exists(res)
    return res


def deploy_target_dir() -> str:
    return repository_dir()


def qt_install_dir() -> str:
    if sys.platform.startswith('win32'):
        res = os.path.join('C:', os.sep, 'Qt')
    elif sys.platform.startswith('darwin'):
        res = os.path.join(os.path.expanduser('~'), 'Qt')
    else:
        res = os.path.join(os.path.expanduser('~'), 'Qt')
    assert os.path.exists(res)
    return res


def qt_compiler_name() -> str:
    if sys.platform.startswith('win32'):
        return 'msvc2017_64'
    elif sys.platform.startswith('darwin'):
        return 'clang_64'
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
    assert vers
    vers = sorted(vers, key=parse_version)
    ver = vers[-1]
    return ver


def qt_base_dir() -> str:
    return os.path.join(qt_install_dir(), qt_ver(), qt_compiler_name())


def qt_bin_dir() -> str:
    return os.path.join(qt_base_dir(), 'bin')


def qmake_bin() -> str:
    return os.path.join(qt_bin_dir(), qmake_bin_name())


def vs_install_dir() -> str:
    assert sys.platform.startswith('win32')

    vsinstalldir_var_names = ['VS2017INSTALLDIR']

    vsinstalldir = None
    for var_name in vsinstalldir_var_names:
        vsinstalldir = os.getenv(var_name)
        if vsinstalldir is not None:
            break

    if vsinstalldir is None:
        raise OSError('could not find VS2017INSTALLDIR environment variable')
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

    res = os.path.join(vc_redist_dir(), 'x64', 'Microsoft.VC141.CRT')
    assert os.path.exists(res)
    return res


def vc_CXXAMP_redist_dir() -> str:
    assert sys.platform.startswith('win32')

    res = os.path.join(vc_redist_dir(), 'x64', 'Microsoft.VC141.CXXAMP')
    assert os.path.exists(res)
    return res


def vc_OpenMP_redist_dir() -> str:
    assert sys.platform.startswith('win32')

    res = os.path.join(vc_redist_dir(), 'x64', 'Microsoft.VC141.OpenMP')
    assert os.path.exists(res)
    return res


def intel_sw_dir() -> str:
    if sys.platform.startswith('win32'):
        res = os.path.join('C:', os.sep, 'Program Files (x86)', 'IntelSWTools', 'compilers_and_libraries', 'windows')
    else:
        res = os.path.join(os.sep, 'opt', 'intel')
    assert os.path.exists(res)
    return res


def tbb_redist_dir() -> str:
    assert sys.platform.startswith('win32')

    res = os.path.join('C:', os.sep, 'Program Files (x86)', 'IntelSWTools', 'compilers_and_libraries',
                       'windows', 'redist', 'intel64', 'tbb', 'vc14')
    assert os.path.exists(res)
    return res


def assimp_redist_dir() -> str:
    assert sys.platform.startswith('win32')

    res = os.path.join(ext_dir(), 'assimp', 'bin')
    assert os.path.exists(res)
    return res


def freeimage_redist_dir() -> str:
    assert sys.platform.startswith('win32')

    res = os.path.join(ext_dir(), 'freeimage')
    assert os.path.exists(res)
    return res


def write_cmake_file_with_qt_info():
    with open(os.path.join(repository_dir(), 'cmake', 'QtInfo.cmake'), mode='w', encoding='utf-8') as file:
        file.write('# Set Qt related variables\n')
        file.write(f'set(QT_VERSION {qt_ver()})\n')
        if sys.platform.startswith('win32'):
            file.write('set(QT_PATHS {0})\n'.format(qt_base_dir().replace("\\", "/")))
            # also need to patch Qt
            orig_file = os.path.join(qt_base_dir(), 'include', 'QtCore', 'qglobal.h')
            bak_file = os.path.join(qt_base_dir(), 'include', 'QtCore', 'qglobal.h.bak')
            if not os.path.exists(bak_file):
                os.rename(orig_file, bak_file)
                with open(bak_file, mode='r', encoding='utf-8') as f:
                    from_lines = f.readlines()
                with open(orig_file, mode='w', encoding='utf-8') as f:
                    to_lines = []
                    for line in from_lines:
                        line = line.replace(
                            r'#if defined(__cpp_variable_templates) && __cpp_variable_templates >= 201304 // C++14',
                            r'#if defined(_MSC_VER) || '
                            r'defined(__cpp_variable_templates) && __cpp_variable_templates >= 201304 // C++14')
                        f.write(line)
                        to_lines.append(line)
                print(''.join(list(difflib.unified_diff(from_lines, to_lines, fromfile=orig_file, tofile='<new>'))))
        else:
            file.write(f'set(QT_PATHS {qt_base_dir()})\n')


def software_dir() -> str:
    res = os.path.join(os.path.expanduser('~'), 'software')
    if not os.path.exists(res):
        os.mkdir(res)
    assert os.path.exists(res)
    return res


def is_windows() -> bool:
    return sys.platform.startswith('win')


def is_mac() -> bool:
    return sys.platform.startswith('darwin')


def is_linux() -> bool:
    return sys.platform.startswith('linux')


def find_src_package_with_glob(files: str):
    file_list = glob.glob(files)
    if len(file_list) == 1:
        return file_list[0]
    elif len(file_list) == 0:
        raise Exception("Can not find matching package with pattern: " + files)
    else:
        raise Exception("Find more than one matching packages with pattern: " + files)


def get_package_top_level_folder(file: str, folder: str):
    res = ''
    if file.lower().endswith('.zip'):
        with zipfile.ZipFile(file, mode='r') as zf:
            # print(zf.namelist())
            res = os.path.join(folder, os.path.commonpath([nm for nm in zf.namelist() if not nm.endswith('/')]))
    elif file.lower().endswith('.tar.gz') or file.lower().endswith('.tar.bz2') or file.lower().endswith('.tar.xz') \
            or file.lower().endswith('.tgz'):
        with tarfile.open(file, mode='r|*') as tf:
            # print(tf.getnames())
            res = os.path.join(folder, os.path.commonpath(tf.getnames()))
    elif file.lower().endswith('.7z'):
        cp = subprocess.run(['7za', 'l', '-slt', file], stdout=subprocess.PIPE, encoding='utf-8')
        started = False
        filenames = []
        for line in cp.stdout.splitlines():
            if started:
                if line.startswith('Path = '):
                    filenames.append(line.replace('Path = ', ''))
            else:
                if line.startswith('-------'):
                    started = True
        res = os.path.join(folder, os.path.commonpath(filenames))

    if res.endswith('/') or res.endswith('\\'):
        res = res[:-1]
    return res


def unpack_file_to_folder(file: str, folder: str):
    print('unpacking', file)
    if file.lower().endswith('.zip'):
        with zipfile.ZipFile(file, mode='r') as zf:
            zf.extractall(path=folder)
    elif file.lower().endswith('.tar.gz') or file.lower().endswith('.tar.bz2') or file.lower().endswith('.tar.xz') \
            or file.lower().endswith('.tgz'):
        with tarfile.open(file, mode='r|*') as tf:
            tf.extractall(path=folder)
    elif file.lower().endswith('.7z'):
        if is_windows():
            subprocess.run(['7za', 'x', '-y', '-o' + folder, file],
                           shell=False, check=True, cwd=curr_dir())
        else:
            subprocess.run(['7za', 'x', '-y', '-o' + folder, file],
                           shell=False, check=True)


def unpack_tool_to_software_dir(tool_package_folder: str, tool_package_glob_name: str,
                                tool_folder_glob_name=None) -> str:
    if tool_folder_glob_name is None:
        tool_folder_glob_name = tool_package_glob_name
    package_name = find_src_package_with_glob(os.path.join(tool_package_folder, tool_package_glob_name))
    package_unpack_folder = get_package_top_level_folder(package_name, software_dir())
    if not os.path.exists(package_unpack_folder):
        folder_list = glob.glob(os.path.join(software_dir(), tool_folder_glob_name))
        if len(folder_list) == 1:
            shutil.rmtree(folder_list[0], ignore_errors=False)
        unpack_file_to_folder(package_name, software_dir())
    return package_unpack_folder


def install_cmake():
    if is_windows():
        unpack_tool_to_software_dir(src_package_dir(), 'cmake*win64*')
    elif is_linux():
        unpack_tool_to_software_dir(src_package_dir(), 'cmake*Linux*')
    else:
        unpack_tool_to_software_dir(src_package_dir(), 'cmake*Darwin*')


def install_ninja():
    if is_windows():
        unpack_file_to_folder(os.path.join(src_package_dir(), 'ninja-win.zip'), software_dir())
    elif is_linux():
        if os.path.exists(os.path.join(software_dir(), 'ninja')):
            os.remove(os.path.join(software_dir(), 'ninja'))
        unpack_file_to_folder(os.path.join(src_package_dir(), 'ninja-linux.zip'), software_dir())
        os.chmod(os.path.join(software_dir(), 'ninja'), stat.S_IXUSR)
    else:
        if os.path.exists(os.path.join(software_dir(), 'ninja')):
            os.remove(os.path.join(software_dir(), 'ninja'))
        unpack_file_to_folder(os.path.join(src_package_dir(), 'ninja-mac.zip'), software_dir())
        os.chmod(os.path.join(software_dir(), 'ninja'), stat.S_IXUSR)


def get_cmake_binary() -> str:
    if is_windows():
        cmake_folder = find_src_package_with_glob(os.path.join(software_dir(), 'cmake-*win*-x64'))
        return os.path.join(cmake_folder, 'bin', 'cmake')
    elif is_linux():
        cmake_folder = find_src_package_with_glob(os.path.join(software_dir(), 'cmake-*Linux*_64'))
        return os.path.join(cmake_folder, 'bin', 'cmake')
    else:
        cmake_folder = find_src_package_with_glob(os.path.join(software_dir(), 'cmake-*Darwin*_64'))
        return os.path.join(cmake_folder, 'CMake.app', 'Contents', 'bin', 'cmake')


def get_ninja_binary() -> str:
    if is_windows():
        return os.path.join(software_dir(), 'ninja.exe')
    else:
        return os.path.join(software_dir(), 'ninja')


if __name__ == "__main__":
    print(f'Qt {qt_ver()} in {qt_base_dir()}')
    write_cmake_file_with_qt_info()
    install_cmake()
    subprocess.run([get_cmake_binary(), '-P', 'MakeTBBConfigFiles.cmake'],
                   cwd=os.path.join(repository_dir(), 'cmake'), shell=False, check=True)
