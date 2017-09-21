import os
import sys


def curr_dir():
    return os.path.abspath(os.path.dirname(__file__))


def repository_dir():
    res = os.path.normpath(os.path.join(curr_dir(), '..'))
    assert os.path.exists(res)
    return res


def base_dir():
    res = os.path.normpath(os.path.join(repository_dir(), '..'))
    assert os.path.exists(res)
    return res


def src_dir():
    res = os.path.join(repository_dir(), 'src')
    assert os.path.exists(res)
    return res


def ext_dir():
    res = os.path.join(src_dir(), '3rdparty')
    assert os.path.exists(res)
    return res


def atlas_dir():
    res = os.path.join(src_dir(), 'atlas')
    assert os.path.exists(res)
    return res


def img_dir():
    res = os.path.join(src_dir(), 'img')
    assert os.path.exists(res)
    return res


def resource_dir():
    res = os.path.join(atlas_dir(), 'Resources')
    assert os.path.exists(res)
    return res


def src_package_dir():
    res = os.path.join(base_dir(), 'atlas_others')
    if not os.path.exists(res):
        if sys.platform.startswith('win'):
            res = os.path.join('Z:', os.sep, 'Google Drive', 'code', 'my', 'atlas_others')
        else:
            res = os.path.join(os.path.expanduser('~'), 'Google Drive', 'code', 'my', 'atlas_others')
    assert os.path.exists(res)
    return res


def build_dir():
    res = os.path.join(repository_dir(), 'cmake-build-release')
    assert os.path.exists(res)
    return res


def binary_dir():
    if sys.platform.startswith('win32'):
        res = os.path.join(build_dir(), 'src', 'atlas', 'Release')
    else:
        res = os.path.join(build_dir(), 'src', 'atlas')
    assert os.path.exists(res)
    return res


def deploy_target_dir():
    return repository_dir()


def qt_ver():
    return '5.9.1'


def qt_base_dir():
    if sys.platform.startswith('win32'):
        res = os.path.join('C:', os.sep, 'Qt', 'Qt', qt_ver(), 'msvc2017_64')
    elif sys.platform.startswith('darwin'):
        res = os.path.join(os.path.expanduser('~'), 'Qt', qt_ver(), 'clang_64')
    else:
        res = os.path.join(os.path.expanduser('~'), 'Qt', qt_ver(), 'gcc_64')
    assert os.path.exists(res)
    return res


def software_dir() -> str:
    res = os.path.join(os.path.expanduser('~'), 'software')
    if not os.path.exists(res):
        os.mkdir(res)
    assert os.path.exists(res)
    return res


def qt_bin_dir():
    res = os.path.join(qt_base_dir(), 'bin')
    assert os.path.exists(res)
    return res
