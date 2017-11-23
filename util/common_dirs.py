import os
import sys
import xml.etree.ElementTree as etree


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
    tree = etree.parse(component_file)
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


def write_cmake_file_with_qt_info():
    with open(os.path.join(repository_dir(), 'cmake', 'QtInfo.cmake'), mode='w', encoding='utf-8') as f:
        f.write('# Set Qt related variables\n')
        f.write(f'set(QT_VERSION {qt_ver()})\n')
        f.write(f'set(QT_PATHS {qt_base_dir()})\n')


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
