import os
import sys
import shutil
import tarfile
import zipfile
from pathlib import Path
import subprocess
import difflib

if sys.version_info[0] < 3 or sys.version_info[1] < 5:
    sys.stderr.write('Error: need python 3.5 or higher\n')
    sys.exit(1)


def unpack_file_to_folder(file: str, folder: str):
    print('unpacking', file)
    if file.lower().endswith('.zip'):
        with zipfile.ZipFile(file, mode='r') as zf:
            zf.extractall(path=folder)
    elif file.lower().endswith('.tar.gz') or file.lower().endswith('.tar.bz2') or file.lower().endswith('.tar.xz'):
        with tarfile.open(file, mode='r|*') as tf:
            tf.extractall(path=folder)
    elif file.lower().endswith('.7z'):
        shell = sys.platform.startswith('win')
        subprocess.run(['7z', 'x', '-y', '-o' + folder, file],
                       shell=shell, check=True)


def update_or_clone_git_repository(repository_folder: str, repository_url: str):
    shell = sys.platform.startswith('win')
    if os.path.exists(repository_folder):
        print('git', 'pull', Path(repository_folder).name)
        subprocess.run(['git', 'pull'], cwd=repository_folder, shell=shell, check=True)
    else:
        subprocess.run(['git', 'clone', repository_url, repository_folder], shell=shell, check=True)


def export_git_repository(repository_folder: str, target_folder: str, tag: str=''):
    shell = sys.platform.startswith('win')
    if not tag:
        tag = 'master'
    shutil.rmtree(target_folder, ignore_errors=True)
    subprocess.run(['git', 'clone', '--shared', '--branch', tag, repository_folder, target_folder],
                   shell=shell, check=True)
    shutil.rmtree(os.path.join(target_folder, '.git'), ignore_errors=False)


def create_build_dir(src_dir: str):
    build_dir = os.path.join(src_dir, '..', '__' + Path(src_dir).name + '-build')
    shutil.rmtree(build_dir, ignore_errors=True)
    os.makedirs(build_dir, exist_ok=False)
    return build_dir


def get_bak_file_name(orig_file: str):
    return orig_file + '.bak'


def get_vcvars_environment():
    """
    Returns a dictionary containing the environment variables set up by vcvarsall.bat amd64
    """

    comntools_var_names = ['VS140COMNTOOLS']

    vscomntools = None
    for var_name in comntools_var_names:
        vscomntools = os.getenv(var_name)
        if vscomntools is not None:
            break

    if vscomntools is None:
        sys.stderr.write('Could not find COMNTOOLS environment variable\n')
        sys.exit(1)

    vcvars = os.path.join(vscomntools, '..', '..', 'VC', 'vcvarsall.bat')
    python = sys.executable
    process = subprocess.Popen(
        '("{0}" amd64>nul)&&"{1}" -c "import os; print repr(os.environ)"'.format(vcvars, python),
        stdout=subprocess.PIPE, shell=True)
    stdout, _ = process.communicate()
    exitcode = process.wait()
    if exitcode != 0:
        raise Exception("Got error code {0} from subprocess.".format(exitcode))
    return eval(stdout.strip())


def get_cmake_cmd_common_part(install_dir: str):
    if sys.platform.startswith('win'):
        print('not yet')
        return []
    else:
        osx_sysroot = r'/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/' \
                      r'MacOSX10.12.sdk'
        if not os.path.exists(osx_sysroot):
            osx_sysroot = r'/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/' \
                          r'MacOSX10.11.sdk'
        if not os.path.exists(osx_sysroot):
            osx_sysroot = r'/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/' \
                          r'MacOSX10.10.sdk'
        assert os.path.exists(osx_sysroot)

        return ['cmake',  # '-E', 'echo',
                '-DCMAKE_BUILD_TYPE=Release',
                '-DCMAKE_INSTALL_PREFIX=' + install_dir,
                '-DCMAKE_OSX_DEPLOYMENT_TARGET=10.8',
                '-DCMAKE_OSX_SYSROOT=' + osx_sysroot,
                '-DCMAKE_CXX_FLAGS:STRING=-stdlib=libc++ -std=c++14'
                ]


def build_gflags(src_dir: str, install_dir: str, curr_dir: str):
    build_dir = create_build_dir(src_dir)
    shutil.rmtree(install_dir, ignore_errors=True)
    shell = sys.platform.startswith('win')

    cmakecmd = get_cmake_cmd_common_part(install_dir)
    cmakecmd.extend([src_dir])
    subprocess.run(cmakecmd, cwd=build_dir, shell=shell, check=True)
    subprocess.run(['make', '-j' + str(os.cpu_count()), 'install'],
                   cwd=build_dir, shell=shell, check=True)

    shutil.rmtree(build_dir, ignore_errors=False)


def build_glog(src_dir: str, install_dir: str, curr_dir: str):
    build_dir = create_build_dir(src_dir)
    shutil.rmtree(install_dir, ignore_errors=True)
    shell = sys.platform.startswith('win')

    try:
        subprocess.run(['git', 'apply', os.path.join(curr_dir, 'glog_patch.txt')],
                       cwd=src_dir, shell=shell, check=True)

        cmakecmd = get_cmake_cmd_common_part(install_dir)
        cmakecmd.extend(['-Dgflags_DIR:PATH={0}/gflags/lib/cmake/gflags'.format(curr_dir),
                         src_dir])
        subprocess.run(cmakecmd, cwd=build_dir, shell=shell, check=True)
        subprocess.run(['make', '-j' + str(os.cpu_count()), 'install'],
                       cwd=build_dir, shell=shell, check=True)
    finally:
        subprocess.run(['git', 'reset', '--hard', 'HEAD'],
                       cwd=src_dir, shell=shell, check=True)

    shutil.rmtree(build_dir, ignore_errors=False)


def build_benchmark(src_dir: str, install_dir: str, curr_dir: str):
    build_dir = create_build_dir(src_dir)
    shutil.rmtree(install_dir, ignore_errors=True)
    shell = sys.platform.startswith('win')

    cmakecmd = get_cmake_cmd_common_part(install_dir)
    cmakecmd.extend(['-DBENCHMARK_USE_LIBCXX:BOOL=ON',
                    src_dir])
    subprocess.run(cmakecmd, cwd=build_dir, shell=shell, check=True)
    subprocess.run(['make', '-j' + str(os.cpu_count()), 'install'],
                   cwd=build_dir, shell=shell, check=True)

    shutil.rmtree(build_dir, ignore_errors=False)


def build_glbinding(src_dir: str, install_dir: str, curr_dir: str):
    build_dir = create_build_dir(src_dir)
    shutil.rmtree(install_dir, ignore_errors=True)
    shell = sys.platform.startswith('win')

    cmakecmd = get_cmake_cmd_common_part(install_dir)
    cmakecmd.extend(['-DOPTION_BUILD_GPU_TESTS:BOOL=OFF',
                     '-DBUILD_SHARED_LIBS:BOOL=OFF',
                     '-DOPTION_BUILD_TESTS:BOOL=OFF',
                    src_dir])
    subprocess.run(cmakecmd, cwd=build_dir, shell=shell, check=True)
    subprocess.run(['make', '-j' + str(os.cpu_count()), 'install'],
                   cwd=build_dir, shell=shell, check=True)

    shutil.rmtree(build_dir, ignore_errors=False)


def build_libjpeg(src_dir: str, install_dir: str, curr_dir: str):
    build_dir = create_build_dir(src_dir)
    shutil.rmtree(install_dir, ignore_errors=True)
    shell = sys.platform.startswith('win')

    subprocess.run(['sh', src_dir + '/configure', '--host', 'x86_64-apple-darwin', 'NASM=' + curr_dir + '/nasm',
                    '--enable-static', '--disable-shared', 'CFLAGS=-mmacosx-version-min=10.8 -O3',
                    'LDFLAGS=-mmacosx-version-min=10.8'],
                   cwd=build_dir, shell=shell, check=True)
    subprocess.run(['make', '-j' + str(os.cpu_count())],
                   cwd=build_dir, shell=shell, check=True)
    subprocess.run(['make', 'install', 'prefix=' + install_dir, 'libdir=' + install_dir + '/lib'],
                   cwd=build_dir, shell=shell, check=True)

    shutil.rmtree(build_dir, ignore_errors=False)


def build_libpng(src_dir: str, install_dir: str, curr_dir: str):
    build_dir = create_build_dir(src_dir)
    shutil.rmtree(install_dir, ignore_errors=True)
    shell = sys.platform.startswith('win')

    orig_file = os.path.join(src_dir, 'pngread.c')
    bak_file = get_bak_file_name(orig_file)
    try:
        os.rename(orig_file, bak_file)
        with open(bak_file, mode='r', encoding='utf-8') as f:
            from_lines = f.readlines()
        with open(orig_file, mode='w', encoding='utf-8') as f:
            to_lines = []
            for line in from_lines:
                line = line.replace(r'                || (png_ptr->mode & PNG_HAVE_CHUNK_AFTER_IDAT) != 0)',
                                    r'                && 0)')
                f.write(line)
                to_lines.append(line)
        print(''.join(list(difflib.unified_diff(from_lines, to_lines, fromfile=orig_file, tofile='<new>'))))

        cmakecmd = get_cmake_cmd_common_part(install_dir)
        cmakecmd.extend(['-DPNG_TESTS:BOOL=OFF',
                         '-DPNG_SHARED:BOOL=OFF',
                        src_dir])
        subprocess.run(cmakecmd, cwd=build_dir, shell=shell, check=True)
        subprocess.run(['make', '-j' + str(os.cpu_count()), 'install'],
                       cwd=build_dir, shell=shell, check=True)
    finally:
        os.replace(bak_file, orig_file)

    shutil.rmtree(build_dir, ignore_errors=False)


def build_jxrlib(src_dir: str, install_dir: str, curr_dir: str):
    shutil.rmtree(install_dir, ignore_errors=True)
    shell = sys.platform.startswith('win')

    orig_file = os.path.join(src_dir, 'Makefile')
    bak_file = get_bak_file_name(orig_file)
    try:
        os.rename(orig_file, bak_file)
        with open(bak_file, mode='r', encoding='utf-8') as f:
            from_lines = f.readlines()
        with open(orig_file, mode='w', encoding='utf-8') as f:
            to_lines = []
            for line in from_lines:
                line = line.replace(r'CFLAGS=-I. -Icommon/include -I$(DIR_SYS) '
                                    r'$(ENDIANFLAG) -D__ANSI__ -DDISABLE_PERF_MEASUREMENT -w $(PICFLAG) -O',
                                    r'CFLAGS=-mmacosx-version-min=10.8 -I. -Icommon/include -I$(DIR_SYS) '
                                    r'$(ENDIANFLAG) -D__ANSI__ -DDISABLE_PERF_MEASUREMENT -w $(PICFLAG) -O3')
                f.write(line)
                to_lines.append(line)
        print(''.join(list(difflib.unified_diff(from_lines, to_lines, fromfile=orig_file, tofile='<new>'))))

        subprocess.run(['make', '-j' + str(os.cpu_count()), 'install', 'DIR_INSTALL=' + install_dir],
                       cwd=src_dir, shell=shell, check=True)
    finally:
        shutil.rmtree(os.path.join(src_dir, 'build'), ignore_errors=True)
        os.replace(bak_file, orig_file)


def build_geometrictools(src_dir: str, install_dir: str, curr_dir: str):
    shutil.rmtree(install_dir, ignore_errors=True)
    shell = sys.platform.startswith('win')

    orig_file = os.path.join(src_dir, 'Include', 'GTEngine.h')
    bak_file = get_bak_file_name(orig_file)
    try:
        os.rename(orig_file, bak_file)
        with open(bak_file, mode='r', encoding='utf-8') as f:
            from_lines = f.readlines()
        with open(orig_file, mode='w', encoding='utf-8') as f:
            to_lines = []
            for line in from_lines:
                line = line.replace(r'#include <GTGraphics.h>', r'')
                line = line.replace(r'#include <GTPhysics.h>', r'')
                line = line.replace(r'#include <GTApplications.h>', r'')
                f.write(line)
                to_lines.append(line)
        print(''.join(list(difflib.unified_diff(from_lines, to_lines, fromfile=orig_file, tofile='<new>'))))

        subprocess.run(['xcodebuild', '-project', 'GTEngine.xcodeproj', '-configuration', 'Default',
                        '-target', 'Release Static', 'build', '-arch', 'x86_64',
                        'MACOSX_DEPLOYMENT_TARGET=10.8', 'CLANG_CXX_LANGUAGE_STANDARD=c++14'],
                       cwd=src_dir, shell=shell, check=True)
        shutil.copytree(os.path.join(src_dir, 'build', 'Default'), os.path.join(install_dir, 'lib'))
        shutil.copytree(os.path.join(src_dir, 'Include'), os.path.join(install_dir, 'include'))
    finally:
        shutil.rmtree(os.path.join(src_dir, 'build'), ignore_errors=True)
        os.replace(bak_file, orig_file)


def build_assimp(src_dir: str, install_dir: str, curr_dir: str):
    build_dir = create_build_dir(src_dir)
    shutil.rmtree(install_dir, ignore_errors=True)
    shell = sys.platform.startswith('win')

    orig_file = os.path.join(src_dir, 'include', 'assimp', 'defs.h')
    bak_file = get_bak_file_name(orig_file)
    try:
        os.rename(orig_file, bak_file)
        with open(bak_file, mode='r', encoding='utf-8') as f:
            from_lines = f.readlines()
        with open(orig_file, mode='w', encoding='utf-8') as f:
            to_lines = []
            for line in from_lines:
                line = line.replace(r'#define AI_MAX_ALLOC(type) ((256U * 1024 * 1024) / sizeof(type))',
                                    r'#define AI_MAX_ALLOC(type) ((size_t(256) * 1024 * 1024 * 1024) / sizeof(type))')
                f.write(line)
                to_lines.append(line)
        print(''.join(list(difflib.unified_diff(from_lines, to_lines, fromfile=orig_file, tofile='<new>'))))

        cmakecmd = get_cmake_cmd_common_part(install_dir)
        cmakecmd.extend(['-DASSIMP_BUILD_ASSIMP_TOOLS:BOOL=OFF',
                         '-DASSIMP_BUILD_TESTS:BOOL=OFF',
                        src_dir])
        subprocess.run(cmakecmd, cwd=build_dir, shell=shell, check=True)
        subprocess.run(['make', '-j' + str(os.cpu_count()), 'install'],
                       cwd=build_dir, shell=shell, check=True)
    finally:
        os.replace(bak_file, orig_file)

    shutil.rmtree(build_dir, ignore_errors=False)


def build_hdf5(src_dir: str, install_dir: str, curr_dir: str):
    build_dir = create_build_dir(src_dir)
    shutil.rmtree(install_dir, ignore_errors=True)
    shell = sys.platform.startswith('win')

    cmakecmd = get_cmake_cmd_common_part(install_dir)
    cmakecmd.extend(['-DBUILD_TESTING:BOOL=OFF',
                     '-DHDF5_ENABLE_DEPRECATED_SYMBOLS:BOOL=OFF',
                     '-DHDF5_ENABLE_Z_LIB_SUPPORT:BOOL=ON',
                     '-DHDF5_ENABLE_THREADSAFE:BOOL=OFF',
                     '-DHDF5_BUILD_EXAMPLES:BOOL=OFF',
                    src_dir])
    subprocess.run(cmakecmd, cwd=build_dir, shell=shell, check=True)
    subprocess.run(['make', '-j' + str(os.cpu_count()), 'install'],
                   cwd=build_dir, shell=shell, check=True)

    shutil.rmtree(build_dir, ignore_errors=False)


def build_freeimage(src_dir: str, install_dir: str, curr_dir: str):
    shutil.rmtree(install_dir, ignore_errors=True)
    shell = sys.platform.startswith('win')

    orig_file = os.path.join(src_dir, 'Source', 'LibRawLite', 'internal', 'dcraw_common.cpp')
    bak_file = get_bak_file_name(orig_file)
    orig_file_2 = os.path.join(src_dir, 'Source', 'LibRawLite', 'internal', 'libraw_x3f.cpp')
    bak_file_2 = get_bak_file_name(orig_file_2)
    try:
        os.rename(orig_file, bak_file)
        with open(bak_file, mode='r', encoding='utf-8') as f:
            from_lines = f.readlines()
        with open(orig_file, mode='w', encoding='utf-8') as f:
            to_lines = []
            for line in from_lines:
                line = line.replace(r',0,0x80,', r',0,static_cast<signed char>(0x80),')
                line = line.replace(r',1,0x80,', r',1,static_cast<signed char>(0x80),')
                line = line.replace(r',0,0x88,', r',0,static_cast<signed char>(0x88),')
                line = line.replace(r',1,0x88,', r',1,static_cast<signed char>(0x88),')
                line = line.replace(r'DCRAW_VERSION', r' DCRAW_VERSION')
                f.write(line)
                to_lines.append(line)
        print(''.join(list(difflib.unified_diff(from_lines, to_lines, fromfile=orig_file, tofile='<new>'))))

        os.rename(orig_file_2, bak_file_2)
        with open(bak_file_2, mode='r', encoding='utf-8') as f:
            from_lines = f.readlines()
        with open(orig_file_2, mode='w', encoding='utf-8') as f:
            to_lines = []
            for line in from_lines:
                line = line.replace(r'offset,offset,offset',
                                    r'static_cast<int16_t>(offset),static_cast<int16_t>(offset),'
                                    r'static_cast<int16_t>(offset)')
                f.write(line)
                to_lines.append(line)
        print(''.join(list(difflib.unified_diff(from_lines, to_lines, fromfile=orig_file_2, tofile='<new>'))))

        shutil.copy2(os.path.join(curr_dir, 'freeimage-makefiles', 'Makefile_gnu'), src_dir)
        shutil.copy2(os.path.join(curr_dir, 'freeimage-makefiles', 'Makefile_fip'), src_dir)
        subprocess.run(['make', '-f', 'Makefile_gnu', '-j' + str(os.cpu_count())],
                       cwd=src_dir, shell=shell, check=True)
        subprocess.run(['make', '-f', 'Makefile_gnu', '-j' + str(os.cpu_count()), 'install', 'PREFIX=' + install_dir],
                       cwd=src_dir, shell=shell, check=True)
        subprocess.run(['make', '-f', 'Makefile_gnu', 'clean'],
                       cwd=src_dir, shell=shell, check=True)
        subprocess.run(['make', '-f', 'Makefile_fip', '-j' + str(os.cpu_count())],
                       cwd=src_dir, shell=shell, check=True)
        subprocess.run(['make', '-f', 'Makefile_fip', '-j' + str(os.cpu_count()), 'install', 'PREFIX=' + install_dir],
                       cwd=src_dir, shell=shell, check=True)
        subprocess.run(['make', '-f', 'Makefile_fip', 'clean'],
                       cwd=src_dir, shell=shell, check=True)

        subprocess.run(['install_name_tool', '-id',
                        install_dir + '/lib/libfreeimage.dylib',
                        install_dir + '/lib/libfreeimage.dylib'],
                       shell=shell, check=True)
        subprocess.run(['install_name_tool', '-id',
                        install_dir + '/lib/libfreeimageplus.dylib',
                        install_dir + '/lib/libfreeimageplus.dylib'],
                       shell=shell, check=True)
    finally:
        os.remove(os.path.join(src_dir, 'Makefile_gnu'))
        os.remove(os.path.join(src_dir, 'Makefile_fip'))
        os.replace(bak_file, orig_file)
        os.replace(bak_file_2, orig_file_2)


def build_itk(src_dir: str, install_dir: str, curr_dir: str):
    build_dir = create_build_dir(src_dir)
    shutil.rmtree(install_dir, ignore_errors=True)
    shell = sys.platform.startswith('win')

    cmakecmd = get_cmake_cmd_common_part(install_dir)
    cmakecmd.extend(['-DBUILD_EXAMPLES:BOOL=OFF',
                     '-DBUILD_TESTING:BOOL=OFF',
                     '-DITK_USE_64BITS_IDS:BOOL=ON',
                     '-DITK_LEGACY_REMOVE:BOOL=ON',
                     '-DITK_USE_GPU:BOOL=ON',
                     '-DITK_DOXYGEN_HTML:BOOL=OFF',
                     '-DITK_USE_STRICT_CONCEPT_CHECKING:BOOL=ON',
                     '-DModule_ITKReview:BOOL=ON',
                     '-DITK_USE_SYSTEM_ZLIB:BOOL=ON',
                     '-DVNL_CONFIG_LEGACY_METHODS:BOOL=OFF',
                    src_dir])
    subprocess.run(cmakecmd, cwd=build_dir, shell=shell, check=True)
    subprocess.run(['make', '-j' + str(os.cpu_count()), 'install'],
                   cwd=build_dir, shell=shell, check=True)

    shutil.rmtree(build_dir, ignore_errors=False)


def build_vtk(src_dir: str, install_dir: str, curr_dir: str):
    build_dir = create_build_dir(src_dir)
    shutil.rmtree(install_dir, ignore_errors=True)
    shell = sys.platform.startswith('win')

    cmakecmd = get_cmake_cmd_common_part(install_dir)
    cmakecmd.extend(['-DBUILD_EXAMPLES:BOOL=OFF',
                     '-DBUILD_TESTING:BOOL=OFF',
                     '-DBUILD_SHARED_LIBS:BOOL=OFF',
                     '-DVTK_USE_SYSTEM_ZLIB:BOOL=ON',
                     '-DVTK_LEGACY_REMOVE:BOOL=ON',
                     '-DVTK_USE_SYSTEM_LIBXML2:BOOL=ON',
                    src_dir])
    subprocess.run(cmakecmd, cwd=build_dir, shell=shell, check=True)
    subprocess.run(['make', '-j' + str(os.cpu_count()), 'install'],
                   cwd=build_dir, shell=shell, check=True)

    shutil.rmtree(build_dir, ignore_errors=False)


def build_opencv(src_dir: str, src_contrib_dir: str, install_dir: str, curr_dir: str):
    build_dir = create_build_dir(src_dir)
    shutil.rmtree(install_dir, ignore_errors=True)
    shell = sys.platform.startswith('win')

    orig_file = os.path.join(src_dir, 'cmake', 'OpenCVCompilerOptions.cmake')
    bak_file = get_bak_file_name(orig_file)
    try:
        os.rename(orig_file, bak_file)
        with open(bak_file, mode='r', encoding='utf-8') as f:
            from_lines = f.readlines()
        with open(orig_file, mode='w', encoding='utf-8') as f:
            to_lines = []
            for line in from_lines:
                line = line.replace(r'stdc++',
                                    r'')
                f.write(line)
                to_lines.append(line)
        print(''.join(list(difflib.unified_diff(from_lines, to_lines, fromfile=orig_file, tofile='<new>'))))

        cmakecmd = get_cmake_cmd_common_part(install_dir)
        cmakecmd.extend(['-DIPPROOT=/opt/intel/ipp',
                         '-DBUILD_WITH_DYNAMIC_IPP:BOOL=OFF',
                         '-DWITH_PTHREADS_PF:BOOL=OFF',
                         '-DBUILD_opencv_videoio:BOOL=OFF',
                         '-DBUILD_SHARED_LIBS:BOOL=OFF',
                         '-DBUILD_opencv_python2:BOOL=OFF',
                         '-DBUILD_opencv_videostab:BOOL=ON',
                         '-DEIGEN_INCLUDE_PATH:PATH=' + curr_dir + '/eigen',
                         '-DBUILD_opencv_world:BOOL=OFF',
                         '-DWITH_TBB:BOOL=ON',
                         '-DWITH_VTK:BOOL=OFF',
                         '-DBUILD_PERF_TESTS:BOOL=OFF',
                         '-DWITH_PNG:BOOL=OFF',
                         '-DWITH_FFMPEG:BOOL=OFF',
                         '-DWITH_QUICKTIME:BOOL=OFF',
                         '-DWITH_1394:BOOL=OFF',
                         '-DWITH_GSTREAMER:BOOL=OFF',
                         '-DBUILD_DOCS:BOOL=OFF',
                         '-DBUILD_PNG:BOOL=OFF',
                         '-DWITH_GPHOTO2:BOOL=OFF',
                         '-DBUILD_ZLIB:BOOL=OFF',
                         '-DWITH_CUDA:BOOL=OFF',
                         '-DWITH_PVAPI:BOOL=OFF',
                         '-DBUILD_JASPER:BOOL=OFF',
                         '-DOPENCV_EXTRA_MODULES_PATH:PATH=' + src_contrib_dir + '/modules',
                         '-DBUILD_TESTS:BOOL=OFF',
                         '-DBUILD_JPEG:BOOL=OFF',
                         '-DBUILD_TIFF:BOOL=OFF',
                         '-DWITH_LIBV4L:BOOL=OFF',
                         '-DWITH_GIGEAPI:BOOL=OFF',
                         '-DBUILD_opencv_video:BOOL=ON',
                         '-DWITH_V4L:BOOL=OFF',
                         '-DWITH_WEBP:BOOL=OFF',
                         '-DWITH_TIFF:BOOL=OFF',
                         '-DBUILD_FAT_JAVA_LIB:BOOL=OFF',
                         '-DBUILD_OPENEXR:BOOL=OFF',
                         '-DWITH_CUFFT:BOOL=ON',
                         '-DWITH_JPEG:BOOL=OFF',
                         '-DWITH_OPENEXR:BOOL=OFF',
                         '-DBUILD_PACKAGE:BOOL=OFF',
                         '-DWITH_JASPER:BOOL=OFF',
                         '-DBUILD_WITH_DEBUG_INFO:BOOL=OFF',
                         '-DBUILD_opencv_apps:BOOL=OFF',
                         '-DBUILD_opencv_matlab:BOOL=OFF',
                        src_dir])
        subprocess.run(cmakecmd, cwd=build_dir, shell=shell, check=True)
        subprocess.run(['make', '-j' + str(os.cpu_count()), 'install'],
                       cwd=build_dir, shell=shell, check=True)

        orig_file_2 = os.path.join(install_dir, 'share', 'OpenCV', 'OpenCVModules-release.cmake')
        bak_file_2 = get_bak_file_name(orig_file_2)
        os.rename(orig_file_2, bak_file_2)
        with open(bak_file_2, mode='r', encoding='utf-8') as f:
            from_lines = f.readlines()
        with open(orig_file_2, mode='w', encoding='utf-8') as f:
            to_lines = []
            for line in from_lines:
                line = line.replace(r';libtbb.dylib;ippcore;ipps;ippi;ippcc;ippcv;ippvm;'
                                    r'/opt/intel/compilers_and_libraries_2017.0.102/mac/compiler/lib/libirc.dylib;'
                                    r'/opt/intel/compilers_and_libraries_2017.0.102/mac/compiler/lib/libimf.dylib;'
                                    r'/opt/intel/compilers_and_libraries_2017.0.102/mac/compiler/lib/libsvml.dylib',
                                    r'')
                f.write(line)
                to_lines.append(line)
        print(''.join(list(difflib.unified_diff(from_lines, to_lines, fromfile=orig_file_2, tofile='<new>'))))
    finally:
        os.replace(bak_file, orig_file)

    shutil.rmtree(build_dir, ignore_errors=False)


def build_libs(libs: dict, update_src: bool):
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

    if sys.platform.startswith('win32'):
        if libs['ospray']:
            if update_src:
                shutil.rmtree(os.path.join(base_dir, 'ispc-v1.9.1-windows-vs2015'), ignore_errors=True)
                unpack_file_to_folder(os.path.join(src_package_dir, 'ispc-v1.9.1-windows-vs2015.zip'),
                                      base_dir)

        if libs['zlib']:
            if update_src:
                shutil.rmtree(os.path.join(base_dir, 'zlib-1.2.8'), ignore_errors=True)
                unpack_file_to_folder(os.path.join(src_package_dir, 'zlib128.zip'),
                                      base_dir)
    else:
        if libs['ospray']:
            if update_src:
                shutil.rmtree(os.path.join(base_dir, 'ispc-v1.9.1-osx'), ignore_errors=True)
                unpack_file_to_folder(os.path.join(src_package_dir, 'ispc-v1.9.1-osx.tar.gz'),
                                      base_dir)

        if libs['ffmpeg']:
            unpack_file_to_folder(os.path.join(src_package_dir, 'ffmpeg-3.1.5.7z'),
                                  os.path.join(base_dir, 'atlas'))

    if libs['boost']:
        shutil.rmtree(os.path.join(curr_dir, 'boost'), ignore_errors=True)
        unpack_file_to_folder(os.path.join(src_package_dir, 'boost_1_62_0.tar.bz2'),
                              curr_dir)
        os.rename(os.path.join(curr_dir, 'boost_1_62_0'), os.path.join(curr_dir, 'boost'))

    if libs['eigen']:
        shutil.rmtree(os.path.join(curr_dir, 'eigen'), ignore_errors=True)
        unpack_file_to_folder(os.path.join(src_package_dir, 'eigen-eigen-3c986dbcba0c.zip'),
                              curr_dir)
        os.rename(os.path.join(curr_dir, 'eigen-eigen-3c986dbcba0c'), os.path.join(curr_dir, 'eigen'))

    if libs['glm']:
        update_or_clone_git_repository(os.path.join(base_dir, 'glm'), 'git@github.com:g-truc/glm.git')
        export_git_repository(os.path.join(base_dir, 'glm'), os.path.join(curr_dir, 'glm'))

    if libs['googletest']:
        update_or_clone_git_repository(os.path.join(base_dir, 'googletest'), 'git@github.com:google/googletest.git')
        shutil.rmtree(os.path.join(curr_dir, 'googletest'), ignore_errors=True)
        shutil.copytree(os.path.join(base_dir, 'googletest', 'googletest'), os.path.join(curr_dir, 'googletest'))

    if libs['folly']:
        update_or_clone_git_repository(os.path.join(base_dir, 'folly'), 'git@github.com:facebook/folly.git')
        export_git_repository(os.path.join(base_dir, 'folly'), os.path.join(curr_dir, 'folly'))

    if libs['glog']:
        if update_src:
            update_or_clone_git_repository(os.path.join(base_dir, 'gflags'), 'git@github.com:gflags/gflags.git')
            update_or_clone_git_repository(os.path.join(base_dir, 'glog'), 'git@github.com:google/glog.git')
        build_gflags(os.path.join(base_dir, 'gflags'), os.path.join(curr_dir, 'gflags'), curr_dir)
        build_glog(os.path.join(base_dir, 'glog'), os.path.join(curr_dir, 'glog'), curr_dir)

    if libs['benchmark']:
        if update_src:
            update_or_clone_git_repository(os.path.join(base_dir, 'benchmark'), 'git@github.com:google/benchmark.git')
        build_benchmark(os.path.join(base_dir, 'benchmark'), os.path.join(curr_dir, 'benchmark'), curr_dir)

    if libs['glbinding']:
        if update_src:
            update_or_clone_git_repository(os.path.join(base_dir, 'glbinding'),
                                           'git@github.com:cginternals/glbinding.git')
        build_glbinding(os.path.join(base_dir, 'glbinding'), os.path.join(curr_dir, 'glbinding'), curr_dir)

    if libs['libjpeg']:
        if update_src:
            shutil.rmtree(os.path.join(base_dir, 'libjpeg-turbo-1.5.1'), ignore_errors=True)
            unpack_file_to_folder(os.path.join(src_package_dir, 'libjpeg-turbo-1.5.1.tar.gz'),
                                  base_dir)
        build_libjpeg(os.path.join(base_dir, 'libjpeg-turbo-1.5.1'), os.path.join(curr_dir, 'libjpeg-turbo'), curr_dir)

    if libs['libpng']:
        if update_src:
            shutil.rmtree(os.path.join(base_dir, 'libpng-1.6.26'), ignore_errors=True)
            unpack_file_to_folder(os.path.join(src_package_dir, 'libpng-1.6.26.tar.gz'),
                                  base_dir)
        build_libpng(os.path.join(base_dir, 'libpng-1.6.26'), os.path.join(curr_dir, 'libpng'), curr_dir)

    if libs['jxrlib']:
        if update_src:
            update_or_clone_git_repository(os.path.join(base_dir, 'jxrlib'), 'https://git01.codeplex.com/jxrlib')
        build_jxrlib(os.path.join(base_dir, 'jxrlib'), os.path.join(curr_dir, 'jxrlib'), curr_dir)

    if libs['geometrictools']:
        if update_src:
            shutil.rmtree(os.path.join(base_dir, 'GeometricTools'), ignore_errors=True)
            unpack_file_to_folder(os.path.join(src_package_dir, 'GeometricTools', 'GeometricToolsEngine3p3.zip'),
                                  base_dir)
        build_geometrictools(os.path.join(base_dir, 'GeometricTools', 'GTEngine'),
                             os.path.join(curr_dir, 'wildmagic'), curr_dir)

    if libs['ospray']:
        if update_src:
            update_or_clone_git_repository(os.path.join(base_dir, 'OSPRay'), 'git@github.com:ospray/OSPRay.git')

    if libs['assimp']:
        if update_src:
            update_or_clone_git_repository(os.path.join(base_dir, 'assimp'), 'git@github.com:assimp/assimp.git')
        build_assimp(os.path.join(base_dir, 'assimp'), os.path.join(curr_dir, 'assimp'), curr_dir)

    if libs['hdf5']:
        if update_src:
            shutil.rmtree(os.path.join(base_dir, 'hdf5-1.10.0-patch1'), ignore_errors=True)
            unpack_file_to_folder(os.path.join(src_package_dir, 'hdf5-1.10.0-patch1.tar.bz2'),
                                  base_dir)
        build_hdf5(os.path.join(base_dir, 'hdf5-1.10.0-patch1'), os.path.join(curr_dir, 'hdf5'), curr_dir)

    if libs['freeimage']:
        if update_src:
            shutil.rmtree(os.path.join(base_dir, 'FreeImage'), ignore_errors=True)
            unpack_file_to_folder(os.path.join(src_package_dir, 'freeimage-FreeImage.tar.gz'),
                                  base_dir)
        build_freeimage(os.path.join(base_dir, 'FreeImage'), os.path.join(curr_dir, 'freeimage'), curr_dir)

    if libs['botan']:
        if update_src:
            update_or_clone_git_repository(os.path.join(base_dir, 'botan'), 'git@github.com:randombit/botan.git')

    if libs['itk']:
        if update_src:
            update_or_clone_git_repository(os.path.join(base_dir, 'ITK'), 'git://itk.org/ITK.git')
        build_itk(os.path.join(base_dir, 'ITK'), os.path.join(curr_dir, 'itk'), curr_dir)

    if libs['vtk']:
        if update_src:
            update_or_clone_git_repository(os.path.join(base_dir, 'VTK'), 'https://gitlab.kitware.com/vtk/vtk.git')
        build_vtk(os.path.join(base_dir, 'VTK'), os.path.join(curr_dir, 'vtk'), curr_dir)

    if libs['opencv']:
        if update_src:
            update_or_clone_git_repository(os.path.join(base_dir, 'opencv'),
                                           'git@github.com:Itseez/opencv.git')
            update_or_clone_git_repository(os.path.join(base_dir, 'opencv_contrib'),
                                           'git@github.com:Itseez/opencv_contrib.git')
        build_opencv(os.path.join(base_dir, 'opencv'), os.path.join(base_dir, 'opencv_contrib'),
                     os.path.join(curr_dir, 'opencv'), curr_dir)


if __name__ == "__main__":
    alllibs = {'all': False,
               'zlib': False,
               'glog': False,
               'benchmark': False,
               'glbinding': False,
               'libjpeg': False,
               'libpng': False,
               'jxrlib': False,
               'geometrictools': False,
               'assimp': False,
               'hdf5': False,
               'freeimage': False,
               'itk': False,
               'vtk': False,
               'opencv': False,
               'boost': False,
               'eigen': False,
               'glm': False,
               'googletest': False,
               'folly': False,
               'ffmpeg': False,
               'botan': False,
               'ospray': False
               }
    no_update_src = False

    if len(sys.argv) == 1:
        usage = '~/miniconda3/bin/python3 build.py [noupdatesrc]'
        for lib in alllibs:
            usage += ' [' + lib + ']'
        print(usage)
    else:
        for lib in sys.argv[1:]:
            if lib.lower() in alllibs:
                alllibs[lib.lower()] = True
            elif lib.lower() == 'noupdatesrc':
                no_update_src = True
            else:
                sys.stderr.write("Error: wrong lib name: " + lib + '\n')
                sys.exit(1)

    if alllibs['all']:
        for lib in alllibs:
            alllibs[lib] = True

    build_libs(alllibs, not no_update_src)
