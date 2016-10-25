import os
import sys
import shutil
import tarfile
import zipfile
from pathlib import Path
import subprocess

if sys.version_info[0] < 3 or sys.version_info[1] < 5:
    sys.stderr.write("Error: need python 3.5 or higher")
    sys.exit(-1)


def unpack_file_to_folder(file: str, folder: str):
    print('unpacking', file)
    if file.lower().endswith('.zip'):
        with zipfile.ZipFile(file, mode='r') as zf:
            zf.extractall(path=folder)
    elif file.lower().endswith('.tar.gz') or file.lower().endswith('.tar.bz2') or file.lower().endswith('.tar.xz'):
        with tarfile.open(file, mode='r|*') as tf:
            tf.extractall(path=folder)
    elif file.lower().endswith('.7z'):
        if sys.platform.startswith('win'):
            shell = True
        else:
            shell = False
        subprocess.run(['7z', 'x', '-y', '-o' + folder, file],
                       shell=shell, check=True)


def update_or_clone_git_repository(repository_folder: str, repository_url: str):
    if sys.platform.startswith('win'):
        shell = True
    else:
        shell = False
    if os.path.exists(repository_folder):
        print('git', 'pull', Path(repository_folder).name)
        subprocess.run(['git', 'pull'], cwd=repository_folder, shell=shell, check=True)
    else:
        subprocess.run(['git', 'clone', repository_url, repository_folder], shell=shell, check=True)


def export_git_repository(repository_folder: str, target_folder: str, tag: str=''):
    if sys.platform.startswith('win'):
        shell = True
    else:
        shell = False
    if not tag:
        tag = 'master'
    shutil.rmtree(target_folder, ignore_errors=True)
    subprocess.run(['git', 'clone', '--shared', '--branch', tag, repository_folder, target_folder],
                   shell=shell, check=True)
    shutil.rmtree(os.path.join(target_folder, '.git'), ignore_errors=False)


def update_ext_libs():
    curr_dir = os.path.abspath(os.path.dirname(__file__))
    print('currDIR:', curr_dir)

    src_package_dir = os.path.normpath(os.path.join(curr_dir, '..', '..', '..', 'atlas_others'))
    if sys.platform.startswith('win'):
        if not os.path.exists(src_package_dir):
            src_package_dir = os.path.join('z:', os.sep, 'Google Drive', 'code', 'my')
    else:
        if not os.path.exists(src_package_dir):
            src_package_dir = os.path.join(os.path.expanduser('~/'), 'Google Drive', 'code', 'my')

    assert os.path.exists(src_package_dir)
    base_dir = os.path.normpath(os.path.join(curr_dir, '..', '..', '..'))
    assert os.path.exists(base_dir)
    print('srcPackageDIR:', src_package_dir)
    print('baseDIR:', base_dir)

    shutil.rmtree(os.path.join(base_dir, 'FreeImage'), ignore_errors=True)
    unpack_file_to_folder(os.path.join(src_package_dir, 'freeimage-FreeImage.tar.gz'),
                          base_dir)

    shutil.rmtree(os.path.join(base_dir, 'libjpeg-turbo-1.5.1'), ignore_errors=True)
    unpack_file_to_folder(os.path.join(src_package_dir, 'libjpeg-turbo-1.5.1.tar.gz'),
                          base_dir)

    shutil.rmtree(os.path.join(base_dir, 'libpng-1.6.26'), ignore_errors=True)
    unpack_file_to_folder(os.path.join(src_package_dir, 'libpng-1.6.26.tar.gz'),
                          base_dir)

    shutil.rmtree(os.path.join(base_dir, 'hdf5-1.10.0-patch1'), ignore_errors=True)
    unpack_file_to_folder(os.path.join(src_package_dir, 'hdf5-1.10.0-patch1.tar.bz2'),
                          base_dir)

    shutil.rmtree(os.path.join(base_dir, 'GeometricTools'), ignore_errors=True)
    unpack_file_to_folder(os.path.join(src_package_dir, 'GeometricTools', 'GeometricToolsEngine3p3.zip'),
                          base_dir)

    if sys.platform.startswith('win32'):
        shutil.rmtree(os.path.join(base_dir, 'ispc-v1.9.1-windows-vs2015'), ignore_errors=True)
        unpack_file_to_folder(os.path.join(src_package_dir, 'ispc-v1.9.1-windows-vs2015.zip'),
                              base_dir)

        shutil.rmtree(os.path.join(base_dir, 'zlib-1.2.8'), ignore_errors=True)
        unpack_file_to_folder(os.path.join(src_package_dir, 'zlib128.zip'),
                              base_dir)
    else:
        shutil.rmtree(os.path.join(base_dir, 'ispc-v1.9.1-osx'), ignore_errors=True)
        unpack_file_to_folder(os.path.join(src_package_dir, 'ispc-v1.9.1-osx.tar.gz'),
                              base_dir)

        unpack_file_to_folder(os.path.join(src_package_dir, 'ffmpeg-3.1.5.7z'),
                              os.path.join(base_dir, 'atlas'))

    if not os.path.exists(os.path.join(curr_dir, 'boost')):
        unpack_file_to_folder(os.path.join(src_package_dir, 'boost_1_62_0.tar.bz2'),
                              curr_dir)
        os.rename(os.path.join(curr_dir, 'boost_1_62_0'), os.path.join(curr_dir, 'boost'))

    if not os.path.exists(os.path.join(curr_dir, 'eigen')):
        unpack_file_to_folder(os.path.join(src_package_dir, 'eigen-eigen-3c986dbcba0c.zip'),
                              curr_dir)
        os.rename(os.path.join(curr_dir, 'eigen-eigen-3c986dbcba0c'), os.path.join(curr_dir, 'eigen'))

    update_or_clone_git_repository(os.path.join(base_dir, 'glm'), 'git@github.com:g-truc/glm.git')
    export_git_repository(os.path.join(base_dir, 'glm'), os.path.join(curr_dir, 'glm'))

    update_or_clone_git_repository(os.path.join(base_dir, 'googletest'), 'git@github.com:google/googletest.git')
    shutil.rmtree(os.path.join(curr_dir, 'googletest'), ignore_errors=True)
    shutil.copytree(os.path.join(base_dir, 'googletest', 'googletest'), os.path.join(curr_dir, 'googletest'))

    update_or_clone_git_repository(os.path.join(base_dir, 'folly'), 'git@github.com:facebook/folly.git')
    export_git_repository(os.path.join(base_dir, 'folly'), os.path.join(curr_dir, 'folly'))

    update_or_clone_git_repository(os.path.join(base_dir, 'benchmark'), 'git@github.com:google/benchmark.git')

    update_or_clone_git_repository(os.path.join(base_dir, 'glog'), 'git@github.com:google/glog.git')

    update_or_clone_git_repository(os.path.join(base_dir, 'gflags'), 'git@github.com:gflags/gflags.git')

    update_or_clone_git_repository(os.path.join(base_dir, 'glbinding'), 'git@github.com:cginternals/glbinding.git')

    update_or_clone_git_repository(os.path.join(base_dir, 'jxrlib'), 'https://git01.codeplex.com/jxrlib')

    update_or_clone_git_repository(os.path.join(base_dir, 'OSPRay'), 'git@github.com:ospray/OSPRay.git')

    update_or_clone_git_repository(os.path.join(base_dir, 'assimp'), 'git@github.com:assimp/assimp.git')

    update_or_clone_git_repository(os.path.join(base_dir, 'botan'), 'git@github.com:randombit/botan.git')

    update_or_clone_git_repository(os.path.join(base_dir, 'ITK'), 'git://itk.org/ITK.git')

    update_or_clone_git_repository(os.path.join(base_dir, 'VTK'), 'https://gitlab.kitware.com/vtk/vtk.git')

    update_or_clone_git_repository(os.path.join(base_dir, 'opencv'), 'git@github.com:Itseez/opencv.git')

    update_or_clone_git_repository(os.path.join(base_dir, 'opencv_contrib'),
                                   'git@github.com:Itseez/opencv_contrib.git')


if __name__ == "__main__":
    assert len(sys.argv) == 1
    update_ext_libs()
