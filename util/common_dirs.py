import os
import sys
import xml.etree.ElementTree as eTree
import difflib


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
    res = os.path.join(repository_dir(), 'cmake-build-release')
    if not os.path.exists(res):
        os.mkdir(res)
    assert os.path.exists(res)
    return res


def binary_dir() -> str:
    if sys.platform.startswith('win32'):
        res = os.path.join(build_dir(), 'src', 'atlas', 'Release')
    else:
        res = os.path.join(build_dir(), 'src', 'atlas')
    assert os.path.exists(res)
    return res


def deploy_target_dir() -> str:
    return repository_dir()


def qt_install_dir() -> str:
    if sys.platform.startswith('win32'):
        res = os.path.join('C:', os.sep, 'Qt', 'Qt')
    elif sys.platform.startswith('darwin'):
        res = os.path.join(os.path.expanduser('~'), 'Qt')
    else:
        res = os.path.join(os.path.expanduser('~'), 'Qt')
    assert os.path.exists(res)
    return res


def qt_ver() -> str:
    component_file = os.path.join(qt_install_dir(), 'components.xml')
    assert os.path.exists(component_file)
    tree = eTree.parse(component_file)
    root = tree.getroot()
    ver = None
    for child in root:
        if child.tag == 'ApplicationName' and child.text.startswith('Qt '):
            ver = child.text[3:]
    assert ver is not None
    return ver


def qt_base_dir() -> str:
    if sys.platform.startswith('win32'):
        res = os.path.join(qt_install_dir(), qt_ver(), 'msvc2017_64')
    elif sys.platform.startswith('darwin'):
        res = os.path.join(qt_install_dir(), qt_ver(), 'clang_64')
    else:
        res = os.path.join(qt_install_dir(), qt_ver(), 'gcc_64')
    assert os.path.exists(res)
    return res


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
        vc_redist_version = f.readline().splitlines()[0]

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


def qt_bin_dir() -> str:
    res = os.path.join(qt_base_dir(), 'bin')
    assert os.path.exists(res)
    return res


if __name__ == "__main__":
    print(f'Qt {qt_ver()} in {qt_base_dir()}')
    write_cmake_file_with_qt_info()
