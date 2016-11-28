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


def src_package_dir():
    res = os.path.join(base_dir(), 'atlas_others')
    if not os.path.exists(res):
        if sys.platform.startswith('win'):
            res = os.path.join('z:', os.sep, 'Google Drive', 'code', 'my')
        else:
            res = os.path.join(os.path.expanduser('~'), 'Google Drive', 'code', 'my')
    assert os.path.exists(res)
    return res


def binary_dir():
    res = os.path.join(repository_dir(), 'cmake-build-release', 'src', 'atlas')
    assert os.path.exists(res)
    return res


def deploy_target_dir():
    res = os.path.join(os.path.expanduser('~'), 'Google Drive', "jinny'lab", 'software')
    assert os.path.exists(res)
    return res


def qt_bin_dir():
    res = os.path.join(os.path.expanduser('~'), 'Qt', "5.7", 'clang_64', 'bin')
    assert os.path.exists(res)
    return res
