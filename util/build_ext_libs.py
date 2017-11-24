import os
import stat
import sys
import shutil
import tarfile
import zipfile
from pathlib import Path
import subprocess
import difflib
import json
import distutils.dir_util
import glob

import common_dirs


def is_windows() -> bool:
    return sys.platform.startswith('win')


def is_mac() -> bool:
    return sys.platform.startswith('darwin')


def is_linux() -> bool:
    return sys.platform.startswith('linux')


def macos_min_version():
    return '10.10'


def get_package_top_level_folder(file: str, folder: str):
    res = ''
    if file.lower().endswith('.zip'):
        with zipfile.ZipFile(file, mode='r') as zf:
            res = os.path.join(folder, os.path.commonpath(zf.namelist()))
    elif file.lower().endswith('.tar.gz') or file.lower().endswith('.tar.bz2') or file.lower().endswith('.tar.xz') \
            or file.lower().endswith('.tgz'):
        with tarfile.open(file, mode='r|*') as tf:
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

    if res.endswith('/'):
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
                           shell=False, check=True, cwd=common_dirs.curr_dir())
        else:
            subprocess.run(['7za', 'x', '-y', '-o' + folder, file],
                           shell=False, check=True)


def update_or_clone_git_repository(repository_folder: str, repository_url: str):
    if os.path.exists(repository_folder):
        print('git', 'pull', Path(repository_folder).name)
        subprocess.run(['git', 'pull'], cwd=repository_folder, shell=False, check=False)
    else:
        subprocess.run(['git', 'clone', repository_url, repository_folder], shell=False, check=True)


def export_git_repository(repository_folder: str, target_folder: str, branch: str = '', tag: str = ''):
    if not branch:
        branch = 'master'
    shutil.rmtree(target_folder, ignore_errors=True)
    subprocess.run(['git', 'clone', '--shared', '--branch', branch, repository_folder, target_folder],
                   shell=False, check=True)
    if tag:
        subprocess.run(['git', 'checkout', tag], cwd=target_folder, shell=False, check=True)
    shutil.rmtree(os.path.join(target_folder, '.git'), ignore_errors=False)


def create_build_dir(src_dir: str):
    build_dir = os.path.normpath(os.path.join(src_dir, '..', '__' + Path(src_dir).name + '-build'))
    shutil.rmtree(build_dir, ignore_errors=True)
    os.makedirs(build_dir, exist_ok=False)
    return build_dir


def get_bak_file_name(orig_file: str):
    return orig_file + '.bak'


def remove_path_contains(path: str, env=os.environ):
    env['PATH'] = os.pathsep.join([x for x in env['PATH'].split(os.pathsep) if path.lower() not in x.lower()])


def glob_copy(files: str, dst: str):
    if not os.path.exists(dst):
        os.makedirs(dst)
    assert os.path.isdir(dst)
    for file in glob.glob(files):
        shutil.copy2(file, dst)


def find_src_package_with_glob(files: str):
    file_list = glob.glob(files)
    if len(file_list) == 1:
        return file_list[0]
    elif len(file_list) == 0:
        raise Exception("Can not find matching package with pattern: " + files)
    else:
        raise Exception("Find more than one matching packages with pattern: " + files)


def get_vcvars_environment(remove_conda_from_path: bool = True):
    """
    Returns a dictionary containing the environment variables set up by vcvarsall.bat amd64
    """

    vsinstalldir_var_names = ['VS2017INSTALLDIR']

    vsinstalldir = None
    for var_name in vsinstalldir_var_names:
        vsinstalldir = os.getenv(var_name)
        if vsinstalldir is not None:
            break

    if vsinstalldir is None:
        raise OSError('could not find COMNTOOLS environment variable')

    vcvars = os.path.normpath(os.path.join(vsinstalldir, 'VC', 'Auxiliary', 'Build', 'vcvarsall.bat'))
    return get_enviroment_from_shell_script(vcvars, 'x64', remove_conda_from_path=remove_conda_from_path)


def get_enviroment_from_shell_script(script: str, para: str = '', start_env=os.environ,
                                     remove_conda_from_path: bool = True):
    python = sys.executable
    if is_windows():
        process = subprocess.Popen(
            '"{}" {} >nul && "{}" -c "import os, json; print(json.dumps(dict(os.environ)))"'.format(
                script, para, python),
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, shell=False, universal_newlines=True, env=start_env)
    else:
        source = '. "{}" {}'.format(script, para)
        dump = '"{}" -c "import os, json; print(json.dumps(dict(os.environ)))"'.format(python)
        process = subprocess.Popen(['/bin/bash', '-c', '{} && {}'.format(source, dump)],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, shell=False, universal_newlines=True, env=start_env)
    stdout, stderr = process.communicate()
    exitcode = process.wait()
    if exitcode != 0:
        print(stdout, flush=True)
        print(stderr, flush=True)
        raise Exception("Got error code {} from subprocess.".format(exitcode))
    env = json.loads(stdout.strip())
    if remove_conda_from_path:
        remove_path_contains('miniconda', env)
        remove_path_contains('anaconda', env)
    return env


def get_cmake_cmd() -> str:
    if is_windows():
        cmake_folder = find_src_package_with_glob(os.path.join(common_dirs.software_dir(), 'cmake-*win*-x64'))
        return os.path.join(cmake_folder, 'bin', 'cmake')
    elif is_linux():
        cmake_folder = find_src_package_with_glob(os.path.join(common_dirs.software_dir(), 'cmake-*Linux*_64'))
        return os.path.join(cmake_folder, 'bin', 'cmake')
    else:
        cmake_folder = find_src_package_with_glob(os.path.join(common_dirs.software_dir(), 'cmake-*Darwin*_64'))
        return os.path.join(cmake_folder, 'CMake.app', 'Contents', 'bin', 'cmake')


def get_cmake_cmd_common_part(install_dir: str):
    if is_windows():
        return [get_cmake_cmd(),  # '-E', 'echo',
                '-G', 'Visual Studio 15 2017 Win64',
                '-DCMAKE_INSTALL_PREFIX=' + install_dir
                ]
    elif is_linux():
        return [get_cmake_cmd(),  # '-E', 'echo',
                '-DCMAKE_BUILD_TYPE=Release',
                '-DCMAKE_INSTALL_PREFIX=' + install_dir,
                '-DCMAKE_CXX_FLAGS:STRING=-std=c++14'
                ]
    else:
        osx_sysroot = r'/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk'
        assert os.path.exists(osx_sysroot)

        return [get_cmake_cmd(),  # '-E', 'echo',
                '-DCMAKE_BUILD_TYPE=Release',
                '-DCMAKE_INSTALL_PREFIX=' + install_dir,
                '-DCMAKE_OSX_DEPLOYMENT_TARGET=' + macos_min_version(),
                '-DCMAKE_OSX_SYSROOT=' + osx_sysroot,
                '-DCMAKE_CXX_FLAGS:STRING=-stdlib=libc++ -std=c++14'
                ]


def build_gflags(src_dir: str, install_dir: str, ext_dir: str):
    del ext_dir
    build_dir = create_build_dir(src_dir)
    shutil.rmtree(install_dir, ignore_errors=True)

    try:
        cmakecmd = get_cmake_cmd_common_part(install_dir)
        cmakecmd.extend([src_dir])

        if is_windows():
            env = get_vcvars_environment()
            subprocess.run(cmakecmd,
                           cwd=build_dir, shell=False, check=True, env=env)
            subprocess.run(['MSBuild', 'INSTALL.vcxproj', '/property:Configuration=Release', '/maxcpucount'],
                           cwd=build_dir, shell=True, check=True, env=env)
        else:
            subprocess.run(cmakecmd, cwd=build_dir, shell=False, check=True)
            subprocess.run(['make', '-j' + str(os.cpu_count()), 'install'],
                           cwd=build_dir, shell=False, check=True)
    finally:
        shutil.rmtree(build_dir, ignore_errors=False)


def build_glog(src_dir: str, install_dir: str, ext_dir: str):
    build_dir = create_build_dir(src_dir)
    shutil.rmtree(install_dir, ignore_errors=True)

    try:
        subprocess.run(['git', 'apply', os.path.join(ext_dir, 'glog_patch.txt')],
                       cwd=src_dir, shell=False, check=True)

        cmakecmd = get_cmake_cmd_common_part(install_dir)

        if is_windows():
            orig_file = os.path.join(src_dir, 'src', 'logging_unittest.cc')
            bak_file = get_bak_file_name(orig_file)
            os.rename(orig_file, bak_file)
            with open(bak_file, mode='r', encoding='utf-8') as f:
                from_lines = f.readlines()
            with open(orig_file, mode='w', encoding='utf-8') as f:
                to_lines = []
                for line in from_lines:
                    line = line.replace(r'google::ERROR',
                                        r'GLOG_ERROR')
                    f.write(line)
                    to_lines.append(line)
            print(''.join(list(difflib.unified_diff(from_lines, to_lines, fromfile=orig_file, tofile='<new>'))))
            os.remove(bak_file)

            cmakecmd.extend(['-Dgflags_DIR:PATH={}/gflags/CMake'.format(ext_dir),
                             src_dir])
            env = get_vcvars_environment()
            subprocess.run(cmakecmd, cwd=build_dir, shell=False, check=True, env=env)
            subprocess.run(['MSBuild', 'INSTALL.vcxproj', '/property:Configuration=Release', '/maxcpucount'],
                           cwd=build_dir, shell=True, check=True, env=env)
        else:
            cmakecmd.extend(['-Dgflags_DIR:PATH={}/gflags/lib/cmake/gflags'.format(ext_dir),
                             src_dir])
            subprocess.run(cmakecmd, cwd=build_dir, shell=False, check=True)
            subprocess.run(['make', '-j' + str(os.cpu_count()), 'install'],
                           cwd=build_dir, shell=False, check=True)
    finally:
        subprocess.run(['git', 'reset', '--hard', 'HEAD'],
                       cwd=src_dir, shell=False, check=True)
        shutil.rmtree(build_dir, ignore_errors=False)


def build_benchmark(src_dir: str, install_dir: str, ext_dir: str):
    del ext_dir
    build_dir = create_build_dir(src_dir)
    shutil.rmtree(install_dir, ignore_errors=True)

    try:
        cmakecmd = get_cmake_cmd_common_part(install_dir)

        if is_windows():
            cmakecmd.extend(['-DBENCHMARK_ENABLE_LTO:BOOL=ON',
                             src_dir])
            env = get_vcvars_environment()
            subprocess.run(cmakecmd, cwd=build_dir, shell=False, check=True, env=env)
            subprocess.run(['MSBuild', 'INSTALL.vcxproj', '/property:Configuration=Release', '/maxcpucount'],
                           cwd=build_dir, shell=True, check=True, env=env)
        else:
            if is_linux():
                cmakecmd.extend([src_dir])
            else:
                cmakecmd.extend(['-DBENCHMARK_USE_LIBCXX:BOOL=ON',
                                 src_dir])
            subprocess.run(cmakecmd, cwd=build_dir, shell=False, check=True)
            subprocess.run(['make', '-j' + str(os.cpu_count()), 'install'],
                           cwd=build_dir, shell=False, check=True)
    finally:
        shutil.rmtree(build_dir, ignore_errors=False)


def build_glbinding(src_dir: str, install_dir: str, ext_dir: str):
    del ext_dir
    build_dir = create_build_dir(src_dir)
    shutil.rmtree(install_dir, ignore_errors=True)

    try:
        cmakecmd = get_cmake_cmd_common_part(install_dir)

        if is_windows():
            cmakecmd.extend(['-DOPTION_BUILD_TOOLS:BOOL=OFF',
                             '-DBUILD_SHARED_LIBS:BOOL=OFF',
                             '-DOPTION_BUILD_TESTS:BOOL=OFF',
                             '-DOPTION_BUILD_GPU_TESTS:BOOL=OFF',
                             src_dir])
            env = get_vcvars_environment()
            subprocess.run(cmakecmd, cwd=build_dir, shell=False, check=True, env=env)
            subprocess.run(['MSBuild', 'INSTALL.vcxproj', '/property:Configuration=Release', '/maxcpucount'],
                           cwd=build_dir, shell=True, check=True, env=env)
        else:
            cmakecmd.extend(['-DOPTION_BUILD_TOOLS:BOOL=OFF',
                             '-DBUILD_SHARED_LIBS:BOOL=OFF',
                             '-DOPTION_BUILD_TESTS:BOOL=OFF',
                             '-DOPTION_BUILD_GPU_TESTS:BOOL=OFF',
                             src_dir])
            subprocess.run(cmakecmd, cwd=build_dir, shell=False, check=True)
            subprocess.run(['make', '-j' + str(os.cpu_count()), 'install'],
                           cwd=build_dir, shell=False, check=True)
    finally:
        shutil.rmtree(build_dir, ignore_errors=False)


def build_libjpeg(src_dir: str, install_dir: str, ext_dir: str, nasm_dir: str):
    build_dir = create_build_dir(src_dir)
    shutil.rmtree(install_dir, ignore_errors=True)

    orig_file = os.path.join(src_dir, 'CMakeLists.txt')
    bak_file = get_bak_file_name(orig_file)
    try:
        if is_windows():
            os.rename(orig_file, bak_file)
            with open(bak_file, mode='r', encoding='utf-8') as f:
                from_lines = f.readlines()
            with open(orig_file, mode='w', encoding='utf-8') as f:
                to_lines = []
                for line in from_lines:
                    line = line.replace(r'${CMAKE_CURRENT_BINARY_DIR}/tjbench-static.exe',
                                        r'${CMAKE_CURRENT_BINARY_DIR}/Release/tjbench-static.exe')
                    line = line.replace(r'${CMAKE_CURRENT_BINARY_DIR}/cjpeg-static.exe',
                                        r'${CMAKE_CURRENT_BINARY_DIR}/Release/cjpeg-static.exe')
                    line = line.replace(r'${CMAKE_CURRENT_BINARY_DIR}/djpeg-static.exe',
                                        r'${CMAKE_CURRENT_BINARY_DIR}/Release/djpeg-static.exe')
                    line = line.replace(r'${CMAKE_CURRENT_BINARY_DIR}/jpegtran-static.exe',
                                        r'${CMAKE_CURRENT_BINARY_DIR}/Release/jpegtran-static.exe')
                    f.write(line)
                    to_lines.append(line)
            print(''.join(list(difflib.unified_diff(from_lines, to_lines, fromfile=orig_file, tofile='<new>'))))

            cmakecmd = get_cmake_cmd_common_part(install_dir)
            cmakecmd.extend(['-DENABLE_SHARED:BOOL=OFF',
                             '-DNASM:FILEPATH=' + nasm_dir + '\\nasm.exe',
                             '-DWITH_CRT_DLL:BOOL=ON',
                             src_dir])
            env = get_vcvars_environment()
            subprocess.run(cmakecmd, cwd=build_dir, shell=False, check=True, env=env)
            # /property:ForceImportBeforeCppTargets=%currDIR%\runtime_md.props
            subprocess.run(['MSBuild', 'INSTALL.vcxproj', '/property:Configuration=Release', '/maxcpucount'],
                           cwd=build_dir, shell=True, check=True, env=env)
        else:
            if is_linux():
                subprocess.run(['sh', src_dir + '/configure',
                                'NASM=nasm',
                                '--enable-static', '--disable-shared'],
                               cwd=build_dir, shell=False, check=True)
            else:
                subprocess.run(['sh', src_dir + '/configure', '--host', 'x86_64-apple-darwin',
                                'NASM=' + nasm_dir + '/nasm',
                                '--enable-static', '--disable-shared',
                                'CFLAGS=-mmacosx-version-min=' + macos_min_version() + ' -O3',
                                'LDFLAGS=-mmacosx-version-min=' + macos_min_version()],
                               cwd=build_dir, shell=False, check=True)
            subprocess.run(['make', '-j' + str(os.cpu_count())],
                           cwd=build_dir, shell=False, check=True)
            subprocess.run(['make', 'install', 'prefix=' + install_dir, 'libdir=' + install_dir + '/lib'],
                           cwd=build_dir, shell=False, check=True)
    finally:
        shutil.rmtree(build_dir, ignore_errors=False)
        if is_windows():
            os.replace(bak_file, orig_file)


def build_zlib(src_dir: str, install_dir: str, ext_dir: str):
    del ext_dir
    build_dir = create_build_dir(src_dir)
    shutil.rmtree(install_dir, ignore_errors=True)

    try:
        cmakecmd = get_cmake_cmd_common_part(install_dir)

        if is_windows():
            cmakecmd.extend([src_dir])
            env = get_vcvars_environment()
            subprocess.run(cmakecmd, cwd=build_dir, shell=False, check=True, env=env)
            subprocess.run(['MSBuild', 'INSTALL.vcxproj', '/property:Configuration=Release', '/maxcpucount'],
                           cwd=build_dir, shell=True, check=True, env=env)
        else:
            cmakecmd.extend(['-DAMD64:BOOL=ON',
                             src_dir])
            subprocess.run(cmakecmd, cwd=build_dir, shell=False, check=True)
            subprocess.run(['make', '-j' + str(os.cpu_count()), 'install'],
                           cwd=build_dir, shell=False, check=True)
    finally:
        shutil.rmtree(build_dir, ignore_errors=False)


def build_folly(src_dir: str, install_dir: str):
    del src_dir
    orig_file = os.path.join(install_dir, 'folly', 'ScopeGuard.h')
    bak_file = get_bak_file_name(orig_file)
    try:
        os.rename(orig_file, bak_file)
        with open(bak_file, mode='r', encoding='utf-8') as f:
            from_lines = f.readlines()
        with open(orig_file, mode='w', encoding='utf-8') as f:
            to_lines = []
            for line in from_lines:
                line = line.replace(r'#include <folly/Portability.h>',
                                    r'#include <folly/CPortability.h>')
                line = line.replace(r'static void warnAboutToCrash() noexcept;',
                                    r'inline static void warnAboutToCrash() noexcept {}')
                f.write(line)
                to_lines.append(line)
        print(''.join(list(difflib.unified_diff(from_lines, to_lines, fromfile=orig_file, tofile='<new>'))))
    finally:
        print('')


def build_libpng(src_dir: str, install_dir: str, ext_dir: str):
    build_dir = create_build_dir(src_dir)
    shutil.rmtree(install_dir, ignore_errors=True)

    orig_file = os.path.join(src_dir, 'pngread.c')
    bak_file = get_bak_file_name(orig_file)
    orig_file1 = os.path.join(src_dir, 'pngpriv.h')
    bak_file1 = get_bak_file_name(orig_file1)
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

        if is_windows():
            cmakecmd.extend(['-DPNG_TESTS:BOOL=OFF',
                             '-DPNG_SHARED:BOOL=OFF',
                             '-DZLIB_INCLUDE_DIR:PATH=' + ext_dir + '\\zlib\\include',
                             '-DZLIB_LIBRARY_RELEASE:FILEPATH=' + ext_dir + '\\zlib\\lib\\zlibstatic.lib',
                             src_dir])
            env = get_vcvars_environment()
            subprocess.run(cmakecmd, cwd=build_dir, shell=False, check=True, env=env)
            subprocess.run(['MSBuild', 'INSTALL.vcxproj', '/property:Configuration=Release', '/maxcpucount'],
                           cwd=build_dir, shell=True, check=True, env=env)
        else:
            if is_mac() and os.path.exists('/usr/include'):
                # todo: still need to fix the unsettling warning about function only exists in macOS 10.13
                os.rename(orig_file1, bak_file1)
                with open(bak_file1, mode='r', encoding='utf-8') as f:
                    from_lines = f.readlines()
                with open(orig_file1, mode='w', encoding='utf-8') as f:
                    to_lines = []
                    for line in from_lines:
                        line = line.replace(r'#if PNG_ZLIB_VERNUM != 0 && PNG_ZLIB_VERNUM != ZLIB_VERNUM',
                                            r'#if 0')
                        f.write(line)
                        to_lines.append(line)
                print(''.join(list(difflib.unified_diff(from_lines, to_lines, fromfile=orig_file1, tofile='<new>'))))

            cmakecmd.extend(['-DPNG_TESTS:BOOL=OFF',
                             '-DPNG_SHARED:BOOL=OFF',
                             src_dir])
            subprocess.run(cmakecmd, cwd=build_dir, shell=False, check=True)
            subprocess.run(['make', '-j' + str(os.cpu_count()), 'install'],
                           cwd=build_dir, shell=False, check=True)
    finally:
        os.replace(bak_file, orig_file)
        if is_mac() and os.path.exists('/usr/include'):
            os.replace(bak_file1, orig_file1)
        shutil.rmtree(build_dir, ignore_errors=False)


def build_jxrlib(src_dir: str, install_dir: str, ext_dir: str):
    shutil.rmtree(install_dir, ignore_errors=True)

    orig_file = os.path.join(src_dir, 'Makefile')
    bak_file = get_bak_file_name(orig_file)
    try:
        if is_windows():
            env = get_vcvars_environment()
            subprocess.run(['MSBuild', 'JXR_vc14.sln', '/target:JXRDecApp', '/property:Platform=x64',
                            '/ToolsVersion:15.0', '/property:PlatformToolset=v141',
                            '/property:WindowsTargetPlatformVersion=10.0.14393.0',
                            '/property:ForceImportBeforeCppTargets=' + ext_dir + '\\runtime_md.props',
                            '/property:Configuration=Release', '/maxcpucount'],
                           cwd=os.path.join(src_dir, 'jxrencoderdecoder'), shell=True, check=True, env=env)
            glob_copy(os.path.join(src_dir, 'common', 'include', '*.h'),
                      os.path.join(install_dir, 'include', 'libjxr', 'common'))
            glob_copy(os.path.join(src_dir, 'image', 'x86', '*.h'),
                      os.path.join(install_dir, 'include', 'libjxr', 'image', 'x86'))
            glob_copy(os.path.join(src_dir, 'image', 'sys', '*.h'),
                      os.path.join(install_dir, 'include', 'libjxr', 'image'))
            glob_copy(os.path.join(src_dir, 'image', 'encode', '*.h'),
                      os.path.join(install_dir, 'include', 'libjxr', 'image'))
            glob_copy(os.path.join(src_dir, 'image', 'decode', '*.h'),
                      os.path.join(install_dir, 'include', 'libjxr', 'image'))
            glob_copy(os.path.join(src_dir, 'jxrgluelib', '*.h'),
                      os.path.join(install_dir, 'include', 'libjxr', 'glue'))

            glob_copy(os.path.join(src_dir, 'jxrgluelib', 'Release', 'JXRGlueLib', 'x64', '*.lib'),
                      os.path.join(install_dir, 'lib'))
            glob_copy(os.path.join(src_dir, 'image', 'vc14projects', 'Release', 'JXRCommonLib', 'x64', '*.lib'),
                      os.path.join(install_dir, 'lib'))
            glob_copy(os.path.join(src_dir, 'image', 'vc14projects', 'Release', 'JXRDecodeLib', 'x64', '*.lib'),
                      os.path.join(install_dir, 'lib'))
            glob_copy(os.path.join(src_dir, 'image', 'vc14projects', 'Release', 'JXREncodeLib', 'x64', '*.lib'),
                      os.path.join(install_dir, 'lib'))
        else:
            os.rename(orig_file, bak_file)
            with open(bak_file, mode='r', encoding='utf-8') as f:
                from_lines = f.readlines()
            with open(orig_file, mode='w', encoding='utf-8') as f:
                to_lines = []
                for line in from_lines:
                    if is_linux():
                        line = line.replace(r'CFLAGS=-I. -Icommon/include -I$(DIR_SYS) '
                                            r'$(ENDIANFLAG) -D__ANSI__ -DDISABLE_PERF_MEASUREMENT -w $(PICFLAG) -O',
                                            r'CFLAGS=-I. -Icommon/include -I$(DIR_SYS) '
                                            r'$(ENDIANFLAG) -D__ANSI__ -DDISABLE_PERF_MEASUREMENT -w $(PICFLAG) -O3')
                    else:
                        line = line.replace(r'CFLAGS=-I. -Icommon/include -I$(DIR_SYS) '
                                            r'$(ENDIANFLAG) -D__ANSI__ -DDISABLE_PERF_MEASUREMENT -w $(PICFLAG) -O',
                                            r'CFLAGS=-I. -Icommon/include -I$(DIR_SYS) '
                                            r'$(ENDIANFLAG) -D__ANSI__ -DDISABLE_PERF_MEASUREMENT -w $(PICFLAG) -O3 '
                                            r'-mmacosx-version-min={0}'.format(macos_min_version()))
                    f.write(line)
                    to_lines.append(line)
            print(''.join(list(difflib.unified_diff(from_lines, to_lines, fromfile=orig_file, tofile='<new>'))))

            subprocess.run(['make', '-j' + str(os.cpu_count()), 'install', 'DIR_INSTALL=' + install_dir],
                           cwd=src_dir, shell=False, check=True)
    finally:
        if not is_windows():
            shutil.rmtree(os.path.join(src_dir, 'build'), ignore_errors=True)
            os.replace(bak_file, orig_file)


def build_geometrictools(src_dir: str, install_dir: str, ext_dir: str):
    shutil.rmtree(install_dir, ignore_errors=True)

    if is_linux():
        orig_file = os.path.join(src_dir, 'makeengine.gte')
        bak_file = get_bak_file_name(orig_file)
    else:
        orig_file = os.path.join(src_dir, 'Source', 'Mathematics', 'GteGenerateMeshUV.cpp')
        bak_file = get_bak_file_name(orig_file)

    try:
        if is_windows():
            env = get_vcvars_environment()
            subprocess.run(['MSBuild', 'GTEngine.v15.vcxproj', '/property:Platform=x64',
                            '/property:Configuration=Release', '/maxcpucount',
                            '/property:ForceImportBeforeCppTargets=' + ext_dir + '\\no_warning_as_error.props'],
                           cwd=src_dir, shell=True, check=True, env=env)
            glob_copy(os.path.join(src_dir, '_Output', 'v141', 'x64', 'Release', 'GTEngine.v15.lib'),
                      os.path.join(install_dir, 'lib'))
            glob_copy(os.path.join(src_dir, '_Output', 'v141', 'x64', 'Release', 'GTEngine.v15.pch'),
                      os.path.join(install_dir, 'lib'))
            glob_copy(os.path.join(src_dir, '_Output', 'v141', 'x64', 'Release', 'GTEngine.v15.pdb'),
                      os.path.join(install_dir, 'lib'))
        elif is_linux():
            os.rename(orig_file, bak_file)
            with open(bak_file, mode='r', encoding='utf-8') as f:
                from_lines = f.readlines()
            with open(orig_file, mode='w', encoding='utf-8') as f:
                to_lines = []
                for line in from_lines:
                    line = line.replace(r'$(SRC_APPLICATIONS_GLX)/*.cpp',
                                        r'$(SRC_APPLICATIONS_GLX)/*.nonono')
                    line = line.replace(r'$(SRC_APPLICATIONS_GLX)/%.cpp',
                                        r'$(SRC_APPLICATIONS_GLX)/%.nonono')
                    f.write(line)
                    to_lines.append(line)
            print(''.join(list(difflib.unified_diff(from_lines, to_lines, fromfile=orig_file, tofile='<new>'))))

            subprocess.run(['make', '-j' + str(os.cpu_count()), 'CFG=Release', '-f', 'makeengine.gte'],
                           cwd=src_dir, shell=False, check=True)
            shutil.copytree(os.path.join(src_dir, 'lib', 'Release'), os.path.join(install_dir, 'lib'))
        else:
            os.rename(orig_file, bak_file)
            with open(bak_file, mode='r', encoding='utf-8') as f:
                from_lines = f.readlines()
            with open(orig_file, mode='w', encoding='utf-8') as f:
                to_lines = []
                for line in from_lines:
                    line = line.replace(r'#include <Mathematics/GteGenerateMeshUV.h>',
                                        '#include <Mathematics/GteGenerateMeshUV.h>\n#include <string>')
                    f.write(line)
                    to_lines.append(line)
            print(''.join(list(difflib.unified_diff(from_lines, to_lines, fromfile=orig_file, tofile='<new>'))))

            shutil.copy2(os.path.join(ext_dir, 'makeengine.macos.gte'), src_dir)
            subprocess.run(['make', '-j' + str(os.cpu_count()), 'CFG=Release', '-f', 'makeengine.macos.gte'],
                           cwd=src_dir, shell=False, check=True)
            shutil.copytree(os.path.join(src_dir, 'lib', 'Release'), os.path.join(install_dir, 'lib'))

        shutil.copytree(os.path.join(src_dir, 'Include'), os.path.join(install_dir, 'include'))
    finally:
        shutil.rmtree(os.path.join(src_dir, 'lib'), ignore_errors=True)  # macOS/linux
        shutil.rmtree(os.path.join(src_dir, 'obj'), ignore_errors=True)  # macOS/linux
        shutil.rmtree(os.path.join(src_dir, '_Output'), ignore_errors=True)  # win
        if is_linux() or is_mac():
            os.replace(bak_file, orig_file)


def build_ospray(src_dir: str, install_dir: str, ext_dir: str, ispc_dir: str, embree_dir: str):
    del ext_dir
    build_dir = create_build_dir(src_dir)
    shutil.rmtree(install_dir, ignore_errors=True)

    try:
        cmakecmd = get_cmake_cmd_common_part(install_dir)

        if is_windows():
            intel_sw_dir = 'C:\\Program Files (x86)\\IntelSWTools\\compilers_and_libraries\\windows\\'
            env = get_enviroment_from_shell_script(intel_sw_dir + 'tbb\\bin\\tbbvars.bat',
                                                   para='intel64 vs2017',
                                                   start_env=get_vcvars_environment())
            print('TBBROOT:', env['TBBROOT'])

            cmakecmd.extend(['-DTBB_ROOT:PATH=' + env['TBBROOT'],
                             '-DOSPRAY_USE_EXTERNAL_EMBREE:BOOL=ON',
                             '-DOSPRAY_USE_EMBREE_STREAMS:BOOL=ON',
                             '-DOSPRAY_USE_HIGH_QUALITY_BVH:BOOL=ON',
                             '-Dembree_DIR:PATH=' + embree_dir,
                             '-DISPC_EXECUTABLE:FILEPATH=' + ispc_dir + '\\ispc.exe',
                             '-DOSPRAY_APPS_GLUTVIEWER:BOOL=OFF',
                             '-DOSPRAY_APPS_QTVIEWER:BOOL=OFF',
                             '-DOSPRAY_APPS_VOLUMEVIEWER:BOOL=OFF',
                             src_dir])
            env = get_vcvars_environment()
            subprocess.run(cmakecmd, cwd=build_dir, shell=False, check=True, env=env)
            subprocess.run(['MSBuild', 'INSTALL.vcxproj', '/property:Configuration=Release', '/maxcpucount'],
                           cwd=build_dir, shell=True, check=True, env=env)
        else:
            cmakecmd.extend(['-DTBB_ROOT:PATH=/opt/intel/tbb',
                             '-DOSPRAY_USE_EXTERNAL_EMBREE:BOOL=ON',
                             '-DOSPRAY_USE_EMBREE_STREAMS:BOOL=ON',
                             '-DOSPRAY_USE_HIGH_QUALITY_BVH:BOOL=ON',
                             '-Dembree_DIR:PATH=' + embree_dir,
                             '-DISPC_EXECUTABLE:FILEPATH=' + ispc_dir + '/ispc',
                             '-DOSPRAY_APPS_GLUTVIEWER:BOOL=OFF',
                             '-DOSPRAY_APPS_QTVIEWER:BOOL=OFF',
                             '-DOSPRAY_APPS_VOLUMEVIEWER:BOOL=OFF',
                             src_dir])
            subprocess.run(cmakecmd, cwd=build_dir, shell=False, check=True)
            subprocess.run(['make', '-j' + str(os.cpu_count()), 'install'],
                           cwd=build_dir, shell=False, check=True)
    finally:
        shutil.rmtree(build_dir, ignore_errors=False)


def build_assimp(src_dir: str, install_dir: str, ext_dir: str):
    build_dir = create_build_dir(src_dir)
    shutil.rmtree(install_dir, ignore_errors=True)

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

        if is_windows():
            cmakecmd.extend(['-DASSIMP_BUILD_ASSIMP_TOOLS:BOOL=OFF',
                             '-DASSIMP_BUILD_TESTS:BOOL=OFF',
                             '-DZLIB_INCLUDE_DIR:PATH=' + ext_dir + '\\zlib\\include',
                             '-DZLIB_LIBRARY_REL:FILEPATH=' + ext_dir + '\\zlib\\lib\\zlibstatic.lib',
                             src_dir])
            env = get_vcvars_environment()
            subprocess.run(cmakecmd, cwd=build_dir, shell=False, check=True, env=env)
            subprocess.run(['MSBuild', 'INSTALL.vcxproj', '/property:Configuration=Release', '/maxcpucount'],
                           cwd=build_dir, shell=True, check=True, env=env)
        else:
            cmakecmd.extend(['-DASSIMP_BUILD_ASSIMP_TOOLS:BOOL=OFF',
                             '-DASSIMP_BUILD_TESTS:BOOL=OFF',
                             src_dir])
            subprocess.run(cmakecmd, cwd=build_dir, shell=False, check=True)
            subprocess.run(['make', '-j' + str(os.cpu_count()), 'install'],
                           cwd=build_dir, shell=False, check=True)
    finally:
        os.replace(bak_file, orig_file)
        shutil.rmtree(build_dir, ignore_errors=False)


def build_hdf5(src_dir: str, install_dir: str, ext_dir: str):
    build_dir = create_build_dir(src_dir)
    shutil.rmtree(install_dir, ignore_errors=True)

    try:
        cmakecmd = get_cmake_cmd_common_part(install_dir)

        if is_windows():
            cmakecmd.extend(['-DBUILD_TESTING:BOOL=OFF',
                             '-DBUILD_SHARED_LIBS:BOOL=OFF',
                             '-DHDF5_ENABLE_DEPRECATED_SYMBOLS:BOOL=OFF',
                             '-DHDF5_ENABLE_Z_LIB_SUPPORT:BOOL=ON',
                             '-DZLIB_INCLUDE_DIR:PATH=' + ext_dir + '\\zlib\\include',
                             '-DZLIB_LIBRARY_RELEASE:FILEPATH=' + ext_dir + '\\zlib\\lib\\zlibstatic.lib',
                             '-DHDF5_ENABLE_THREADSAFE:BOOL=OFF',
                             '-DHDF5_BUILD_EXAMPLES:BOOL=OFF',
                             src_dir])
            env = get_vcvars_environment()
            subprocess.run(cmakecmd, cwd=build_dir, shell=False, check=True, env=env)
            subprocess.run(['MSBuild', 'INSTALL.vcxproj', '/property:Configuration=Release', '/maxcpucount'],
                           cwd=build_dir, shell=True, check=True, env=env)
        else:
            cmakecmd.extend(['-DBUILD_TESTING:BOOL=OFF',
                             '-DBUILD_SHARED_LIBS:BOOL=OFF',
                             '-DHDF5_ENABLE_DEPRECATED_SYMBOLS:BOOL=OFF',
                             '-DHDF5_ENABLE_Z_LIB_SUPPORT:BOOL=ON',
                             '-DHDF5_ENABLE_THREADSAFE:BOOL=OFF',
                             '-DHDF5_BUILD_EXAMPLES:BOOL=OFF',
                             src_dir])
            subprocess.run(cmakecmd, cwd=build_dir, shell=False, check=True)
            subprocess.run(['make', '-j' + str(os.cpu_count()), 'install'],
                           cwd=build_dir, shell=False, check=True)
    finally:
        shutil.rmtree(build_dir, ignore_errors=False)


def build_freeimage(src_dir: str, install_dir: str, ext_dir: str):
    shutil.rmtree(install_dir, ignore_errors=True)

    orig_file = os.path.join(src_dir, 'Source', 'LibRawLite', 'internal', 'dcraw_common.cpp')
    bak_file = get_bak_file_name(orig_file)
    orig_file_2 = os.path.join(src_dir, 'Source', 'LibRawLite', 'internal', 'libraw_x3f.cpp')
    bak_file_2 = get_bak_file_name(orig_file_2)
    orig_file_3 = os.path.join(src_dir, 'Makefile.gnu')
    bak_file_3 = get_bak_file_name(orig_file_3)
    orig_file_4 = os.path.join(src_dir, 'Makefile.fip')
    bak_file_4 = get_bak_file_name(orig_file_4)
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
                line = line.replace(r'size_t strnlen',
                                    r'size_t nononononostrnlen')
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

        if is_windows():
            env = get_vcvars_environment()
            subprocess.run(['devenv', 'FreeImage.2013.sln', '/Upgrade'],
                           cwd=src_dir, shell=True, check=True, env=env)
            subprocess.run(['MSBuild', 'FreeImage.2013.sln', '/target:FreeImagePlus', '/property:Platform=x64',
                            '/property:Configuration=Release', '/maxcpucount',
                            '/property:WindowsTargetPlatformVersion=10.0.14393.0'],
                           cwd=src_dir, shell=True, check=True, env=env)
            distutils.dir_util.copy_tree(os.path.join(src_dir, 'Dist', 'x64'),
                                         install_dir)
            distutils.dir_util.copy_tree(os.path.join(src_dir, 'Wrapper', 'FreeImagePlus', 'dist', 'x64'),
                                         install_dir)
        elif is_linux():
            os.rename(orig_file_3, bak_file_3)
            with open(bak_file_3, mode='r', encoding='utf-8') as f:
                from_lines = f.readlines()
            with open(orig_file_3, mode='w', encoding='utf-8') as f:
                to_lines = []
                for line in from_lines:
                    line = line.replace(r'INCDIR ?= $(DESTDIR)/usr/include',
                                        r'INCDIR ?= $(DESTDIR)$(PREFIX)/include')
                    line = line.replace(r'INSTALLDIR ?= $(DESTDIR)/usr/lib',
                                        r'INSTALLDIR ?= $(DESTDIR)$(PREFIX)/lib')
                    line = line.replace(r' -o root -g root ',
                                        r' ')
                    f.write(line)
                    to_lines.append(line)
            print(''.join(list(difflib.unified_diff(from_lines, to_lines, fromfile=orig_file_3, tofile='<new>'))))

            os.rename(orig_file_4, bak_file_4)
            with open(bak_file_4, mode='r', encoding='utf-8') as f:
                from_lines = f.readlines()
            with open(orig_file_4, mode='w', encoding='utf-8') as f:
                to_lines = []
                for line in from_lines:
                    line = line.replace(r'INCDIR ?= $(DESTDIR)/usr/include',
                                        r'INCDIR ?= $(DESTDIR)$(PREFIX)/include')
                    line = line.replace(r'INSTALLDIR ?= $(DESTDIR)/usr/lib',
                                        r'INSTALLDIR ?= $(DESTDIR)$(PREFIX)/lib')
                    line = line.replace(r' -o root -g root ',
                                        r' ')
                    f.write(line)
                    to_lines.append(line)
            print(''.join(list(difflib.unified_diff(from_lines, to_lines, fromfile=orig_file_4, tofile='<new>'))))


            subprocess.run(['make', '-f', 'Makefile.gnu', '-j' + str(os.cpu_count())],
                           cwd=src_dir, shell=False, check=True)
            subprocess.run(['make', '-f', 'Makefile.gnu', '-j' + str(os.cpu_count()), 'install',
                            'PREFIX=' + install_dir],
                           cwd=src_dir, shell=False, check=True)
            subprocess.run(['make', '-f', 'Makefile.gnu', 'clean'],
                           cwd=src_dir, shell=False, check=True)
            subprocess.run(['make', '-f', 'Makefile.fip', '-j' + str(os.cpu_count())],
                           cwd=src_dir, shell=False, check=True)
            subprocess.run(['make', '-f', 'Makefile.fip', '-j' + str(os.cpu_count()), 'install',
                            'PREFIX=' + install_dir],
                           cwd=src_dir, shell=False, check=True)
            subprocess.run(['make', '-f', 'Makefile.fip', 'clean'],
                           cwd=src_dir, shell=False, check=True)
        else:
            shutil.copy2(os.path.join(ext_dir, 'freeimage-makefiles', 'Makefile_gnu'), src_dir)
            shutil.copy2(os.path.join(ext_dir, 'freeimage-makefiles', 'Makefile_fip'), src_dir)
            subprocess.run(['make', '-f', 'Makefile_gnu', '-j' + str(os.cpu_count())],
                           cwd=src_dir, shell=False, check=True)
            subprocess.run(['make', '-f', 'Makefile_gnu', '-j' + str(os.cpu_count()), 'install',
                            'PREFIX=' + install_dir],
                           cwd=src_dir, shell=False, check=True)
            subprocess.run(['make', '-f', 'Makefile_gnu', 'clean'],
                           cwd=src_dir, shell=False, check=True)
            subprocess.run(['make', '-f', 'Makefile_fip', '-j' + str(os.cpu_count())],
                           cwd=src_dir, shell=False, check=True)
            subprocess.run(['make', '-f', 'Makefile_fip', '-j' + str(os.cpu_count()), 'install',
                            'PREFIX=' + install_dir],
                           cwd=src_dir, shell=False, check=True)
            subprocess.run(['make', '-f', 'Makefile_fip', 'clean'],
                           cwd=src_dir, shell=False, check=True)
    finally:
        os.replace(bak_file, orig_file)
        os.replace(bak_file_2, orig_file_2)
        if is_mac():
            os.remove(os.path.join(src_dir, 'Makefile_gnu'))
            os.remove(os.path.join(src_dir, 'Makefile_fip'))
        elif is_linux():
            os.replace(bak_file_3, orig_file_3)
            os.replace(bak_file_4, orig_file_4)


def build_botan(src_dir: str, install_dir: str, ext_dir: str):
    shutil.rmtree(install_dir, ignore_errors=True)

    try:
        python = sys.executable

        if is_windows():
            env = get_vcvars_environment()
            subprocess.run([python, src_dir + '/configure.py', '--cc=msvc',
                            '--prefix=' + install_dir],
                           cwd=src_dir, shell=False, check=True, env=env)
            subprocess.run(['nmake'],
                           cwd=src_dir, shell=True, check=True, env=env)
            subprocess.run(['botan-test'],
                           cwd=src_dir, shell=True, check=True, env=env)
            subprocess.run(['nmake', 'install'],
                           cwd=src_dir, shell=False, check=True, env=env)  # todo: install not working
        elif is_linux():
            subprocess.run([python, src_dir + '/configure.py', '--with-zlib',
                            '--prefix=' + install_dir],
                           cwd=src_dir, shell=False, check=True)
            subprocess.run(['make', '-j' + str(os.cpu_count()), 'install'],
                           cwd=src_dir, shell=False, check=True)
            subprocess.run(['make', 'clean'],
                           cwd=src_dir, shell=False, check=True)
        else:
            subprocess.run([python, src_dir + '/configure.py', '--cc=clang', '--with-zlib',
                            '--cc-abi-flags=-mmacosx-version-min=' + macos_min_version(),
                            '--prefix=' + install_dir],
                           cwd=src_dir, shell=False, check=True)
            subprocess.run(['make', '-j' + str(os.cpu_count()), 'install'],
                           cwd=src_dir, shell=False, check=True)
            subprocess.run(['make', 'clean'],
                           cwd=src_dir, shell=False, check=True)
    finally:
        shutil.rmtree(os.path.join(src_dir, 'build'), ignore_errors=True)
        os.remove(os.path.join(src_dir, 'Makefile'))


def build_itk(src_dir: str, install_dir: str, ext_dir: str):
    build_dir = create_build_dir(src_dir)
    shutil.rmtree(install_dir, ignore_errors=True)

    try:
        cmakecmd = get_cmake_cmd_common_part(install_dir)

        if is_windows():
            cmakecmd.extend(['-DBUILD_EXAMPLES:BOOL=OFF',
                             '-DBUILD_TESTING:BOOL=OFF',
                             '-DITK_USE_64BITS_IDS:BOOL=ON',
                             '-DITK_LEGACY_REMOVE:BOOL=ON',
                             '-DITK_USE_GPU:BOOL=OFF',
                             '-DITK_DOXYGEN_HTML:BOOL=OFF',
                             '-DITK_USE_STRICT_CONCEPT_CHECKING:BOOL=ON',
                             '-DModule_ITKReview:BOOL=ON',
                             '-DITK_USE_SYSTEM_ZLIB:BOOL=ON',
                             '-DVNL_CONFIG_LEGACY_METHODS:BOOL=OFF',
                             '-DZLIB_INCLUDE_DIR:PATH=' + ext_dir + '\\zlib\\include',
                             '-DZLIB_LIBRARY_RELEASE:FILEPATH=' + ext_dir + '\\zlib\\lib\\zlibstatic.lib',
                             src_dir])
            env = get_vcvars_environment()
            subprocess.run(cmakecmd, cwd=build_dir, shell=False, check=True, env=env)
            subprocess.run(['MSBuild', 'INSTALL.vcxproj', '/property:Configuration=Release', '/maxcpucount',
                            '/property:ForceImportBeforeCppTargets=' + ext_dir + '\\no_warning_as_error.props'],
                           cwd=build_dir, shell=True, check=True, env=env)
        else:
            cmakecmd.extend(['-DBUILD_EXAMPLES:BOOL=OFF',
                             '-DBUILD_TESTING:BOOL=OFF',
                             '-DITK_USE_64BITS_IDS:BOOL=ON',
                             '-DITK_LEGACY_REMOVE:BOOL=ON',
                             '-DITK_USE_GPU:BOOL=OFF',
                             '-DITK_DOXYGEN_HTML:BOOL=OFF',
                             '-DITK_USE_STRICT_CONCEPT_CHECKING:BOOL=ON',
                             '-DModule_ITKReview:BOOL=ON',
                             '-DITK_USE_SYSTEM_ZLIB:BOOL=ON',
                             '-DVNL_CONFIG_LEGACY_METHODS:BOOL=OFF',
                             src_dir])
            subprocess.run(cmakecmd, cwd=build_dir, shell=False, check=True)
            subprocess.run(['make', '-j' + str(os.cpu_count()), 'install'],
                           cwd=build_dir, shell=False, check=True)
    finally:
        shutil.rmtree(build_dir, ignore_errors=False)


def build_vtk(src_dir: str, install_dir: str, ext_dir: str):
    build_dir = create_build_dir(src_dir)
    shutil.rmtree(install_dir, ignore_errors=True)

    try:
        cmakecmd = get_cmake_cmd_common_part(install_dir)

        if is_windows():
            cmakecmd.extend(['-DBUILD_EXAMPLES:BOOL=OFF',
                             '-DBUILD_TESTING:BOOL=OFF',
                             '-DBUILD_SHARED_LIBS:BOOL=OFF',
                             '-DVTK_USE_SYSTEM_ZLIB:BOOL=ON',
                             '-DVTK_LEGACY_REMOVE:BOOL=ON',
                             '-DVTK_USE_SYSTEM_LIBXML2:BOOL=OFF',
                             '-DZLIB_INCLUDE_DIR:PATH=' + ext_dir + '\\zlib\\include',
                             '-DZLIB_LIBRARY_RELEASE:FILEPATH=' + ext_dir + '\\zlib\\lib\\zlibstatic.lib',
                             src_dir])
            env = get_vcvars_environment()
            subprocess.run(cmakecmd, cwd=build_dir, shell=False, check=True, env=env)
            subprocess.run(['MSBuild', 'INSTALL.vcxproj', '/property:Configuration=Release', '/maxcpucount'],
                           cwd=build_dir, shell=True, check=True, env=env)
        else:
            if is_linux():
                cmakecmd.extend(['-DBUILD_EXAMPLES:BOOL=OFF',
                                 '-DBUILD_TESTING:BOOL=OFF',
                                 '-DBUILD_SHARED_LIBS:BOOL=OFF',
                                 '-DVTK_USE_SYSTEM_ZLIB:BOOL=ON',
                                 '-DVTK_LEGACY_REMOVE:BOOL=ON',
                                 '-DVTK_USE_SYSTEM_LIBXML2:BOOL=OFF',
                                 src_dir])
            else:
                cmakecmd.extend(['-DBUILD_EXAMPLES:BOOL=OFF',
                                 '-DBUILD_TESTING:BOOL=OFF',
                                 '-DBUILD_SHARED_LIBS:BOOL=OFF',
                                 '-DVTK_USE_SYSTEM_ZLIB:BOOL=ON',
                                 '-DVTK_LEGACY_REMOVE:BOOL=ON',
                                 '-DVTK_USE_SYSTEM_LIBXML2:BOOL=ON',
                                 src_dir])
            subprocess.run(cmakecmd, cwd=build_dir, shell=False, check=True)
            subprocess.run(['make', '-j' + str(os.cpu_count()), 'install'],
                           cwd=build_dir, shell=False, check=True)
    finally:
        shutil.rmtree(build_dir, ignore_errors=False)


def build_opencv(src_dir: str, src_contrib_dir: str, install_dir: str, ext_dir: str):
    build_dir = create_build_dir(src_dir)
    shutil.rmtree(install_dir, ignore_errors=True)

    orig_file = os.path.join(src_dir, 'cmake', 'OpenCVCompilerOptions.cmake')
    bak_file = get_bak_file_name(orig_file)
    orig_file_3 = os.path.join(src_dir, 'modules', 'core', 'include', 'opencv2', 'core', 'private.hpp')
    bak_file_3 = get_bak_file_name(orig_file_3)
    try:
        if is_windows():
            intel_sw_dir = 'C:\\Program Files (x86)\\IntelSWTools\\compilers_and_libraries\\windows\\'
            env = get_enviroment_from_shell_script(intel_sw_dir + 'tbb\\bin\\tbbvars.bat',
                                                   para='intel64 vs2017',
                                                   start_env=get_vcvars_environment())
            print('TBBROOT:', env['TBBROOT'])

            cmakecmd = get_cmake_cmd_common_part(install_dir)
            cmakecmd.extend(['-DBUILD_WITH_STATIC_CRT:BOOL=OFF',
                             '-DBUILD_opencv_videoio:BOOL=OFF',
                             '-DBUILD_SHARED_LIBS:BOOL=OFF',
                             '-DBUILD_PROTOBUF:BOOL=OFF',
                             '-DBUILD_opencv_python2:BOOL=OFF',
                             '-DBUILD_opencv_videostab:BOOL=ON',
                             '-DBUILD_opencv_hdf:BOOL=OFF',
                             '-DBUILD_opencv_sfm:BOOL=OFF',
                             '-DBUILD_opencv_ts:BOOL=OFF',
                             '-DBUILD_opencv_xfeatures2d:BOOL=OFF',
                             '-DBUILD_PROTOBUF:BOOL=OFF',
                             '-DEIGEN_INCLUDE_PATH:PATH=' + ext_dir + '\\eigen',
                             '-DBUILD_opencv_world:BOOL=OFF',
                             '-DWITH_TBB:BOOL=ON',
                             '-DWITH_LAPACK:BOOL=OFF',
                             '-DWITH_VTK:BOOL=OFF',
                             '-DBUILD_PERF_TESTS:BOOL=OFF',
                             '-DWITH_PNG:BOOL=OFF',
                             '-DWITH_MATLAB:BOOL=OFF',
                             '-DWITH_OPENCL:BOOL=OFF',
                             '-DWITH_WIN32UI:BOOL=OFF',
                             '-DWITH_FFMPEG:BOOL=OFF',
                             '-DWITH_1394:BOOL=OFF',
                             '-DWITH_GSTREAMER:BOOL=OFF',
                             '-DBUILD_DOCS:BOOL=OFF',
                             '-DBUILD_PNG:BOOL=OFF',
                             '-DWITH_GPHOTO2:BOOL=OFF',
                             '-DBUILD_ZLIB:BOOL=OFF',
                             '-DWITH_CUDA:BOOL=OFF',
                             '-DWITH_PVAPI:BOOL=OFF',
                             '-DBUILD_JASPER:BOOL=OFF',
                             '-DOPENCV_EXTRA_MODULES_PATH:PATH=' + src_contrib_dir + '\\modules',
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
                             '-DZLIB_INCLUDE_DIR:PATH=' + ext_dir + '\\zlib\\include',
                             '-DZLIB_LIBRARY_RELEASE:FILEPATH=' + ext_dir + '\\zlib\\lib\\zlibstatic.lib',
                             src_dir])
            subprocess.run(cmakecmd, cwd=build_dir, shell=False, check=True, env=env)
            subprocess.run(['MSBuild', 'INSTALL.vcxproj', '/property:Configuration=Release', '/maxcpucount'],
                           cwd=build_dir, shell=True, check=True, env=env)

            orig_file_2 = os.path.join(install_dir, 'x64', 'vc15', 'staticlib', 'OpenCVModules.cmake')
            bak_file_2 = get_bak_file_name(orig_file_2)
            os.rename(orig_file_2, bak_file_2)
            with open(bak_file_2, mode='r', encoding='utf-8') as f:
                from_lines = f.readlines()
            with open(orig_file_2, mode='w', encoding='utf-8') as f:
                to_lines = []
                for line in from_lines:
                    line = line.replace(
                        r';\$<LINK_ONLY:tbb>',
                        r'')
                    line = line.replace(
                        r'\$<LINK_ONLY:tbb>;',
                        r'')
                    f.write(line)
                    to_lines.append(line)
            print(''.join(list(difflib.unified_diff(from_lines, to_lines, fromfile=orig_file_2, tofile='<new>'))))
        else:
            cmakecmd = get_cmake_cmd_common_part(install_dir)
            if is_linux():
                os.rename(orig_file_3, bak_file_3)
                with open(bak_file_3, mode='r', encoding='utf-8') as f:
                    from_lines = f.readlines()
                with open(orig_file_3, mode='w', encoding='utf-8') as f:
                    to_lines = []
                    for line in from_lines:
                        line = line.replace(r'#define CV_INSTRUMENT_FUN_IPP(FUN, ...) ((FUN)(__VA_ARGS__))',
                                            r'#define CV_INSTRUMENT_FUN_IPP(FUN, ...) (FUN(__VA_ARGS__))')
                        f.write(line)
                        to_lines.append(line)
                print(''.join(list(difflib.unified_diff(from_lines, to_lines, fromfile=orig_file_3, tofile='<new>'))))

                env = get_enviroment_from_shell_script('/opt/intel/tbb/bin/tbbvars.sh', 'intel64')
                print('TBBROOT:', env['TBBROOT'])
                cmakecmd.extend(['-DWITH_PTHREADS_PF:BOOL=OFF',
                                 '-DBUILD_opencv_videoio:BOOL=OFF',
                                 '-DBUILD_SHARED_LIBS:BOOL=OFF',
                                 '-DBUILD_opencv_python2:BOOL=OFF',
                                 '-DBUILD_opencv_videostab:BOOL=ON',
                                 '-DBUILD_opencv_hdf:BOOL=OFF',
                                 '-DEIGEN_INCLUDE_PATH:PATH=' + ext_dir + '/eigen',
                                 '-DBUILD_opencv_world:BOOL=OFF',
                                 '-DWITH_TBB:BOOL=ON',
                                 '-DWITH_LAPACK:BOOL=OFF',
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
            else:
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

                env = get_enviroment_from_shell_script('/opt/intel/tbb/bin/tbbvars.sh')
                print('TBBROOT:', env['TBBROOT'])
                cmakecmd.extend(['-DWITH_PTHREADS_PF:BOOL=OFF',
                                 '-DBUILD_opencv_videoio:BOOL=OFF',
                                 '-DBUILD_SHARED_LIBS:BOOL=OFF',
                                 '-DBUILD_opencv_python2:BOOL=OFF',
                                 '-DBUILD_opencv_videostab:BOOL=ON',
                                 '-DBUILD_opencv_hdf:BOOL=OFF',
                                 '-DEIGEN_INCLUDE_PATH:PATH=' + ext_dir + '/eigen',
                                 '-DBUILD_opencv_world:BOOL=OFF',
                                 '-DWITH_TBB:BOOL=ON',
                                 '-DWITH_LAPACK:BOOL=OFF',
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

            subprocess.run(cmakecmd, cwd=build_dir, shell=False, check=True, env=env)
            subprocess.run(['make', '-j' + str(os.cpu_count()), 'install'],
                           cwd=build_dir, shell=False, check=True, env=env)

            orig_file_2 = os.path.join(install_dir, 'share', 'OpenCV', 'OpenCVModules.cmake')
            bak_file_2 = get_bak_file_name(orig_file_2)
            os.rename(orig_file_2, bak_file_2)
            with open(bak_file_2, mode='r', encoding='utf-8') as f:
                from_lines = f.readlines()
            with open(orig_file_2, mode='w', encoding='utf-8') as f:
                to_lines = []
                for line in from_lines:
                    line = line.replace(
                        r';\$<LINK_ONLY:tbb>',
                        r'')
                    line = line.replace(
                        r'\$<LINK_ONLY:tbb>;',
                        r'')
                    f.write(line)
                    to_lines.append(line)
            print(''.join(list(difflib.unified_diff(from_lines, to_lines, fromfile=orig_file_2, tofile='<new>'))))
    finally:
        if is_mac():
            os.replace(bak_file, orig_file)
        elif is_linux():
            os.replace(bak_file_3, orig_file_3)
        shutil.rmtree(build_dir, ignore_errors=False)


def unpack_tool_to_software_dir(tool_package_folder: str, tool_package_glob_name: str,
                                tool_folder_glob_name=None) -> str:
    if tool_folder_glob_name is None:
        tool_folder_glob_name = tool_package_glob_name
    package_name = find_src_package_with_glob(os.path.join(tool_package_folder, tool_package_glob_name))
    package_unpack_folder = get_package_top_level_folder(package_name, common_dirs.software_dir())
    if not os.path.exists(package_unpack_folder):
        folder_list = glob.glob(os.path.join(common_dirs.software_dir(), tool_folder_glob_name))
        if len(folder_list) == 1:
            shutil.rmtree(folder_list[0], ignore_errors=False)
        unpack_file_to_folder(package_name, common_dirs.software_dir())
    return package_unpack_folder


def build_libs(libs: dict, update_src: bool):
    ext_dir = common_dirs.ext_dir()
    src_package_dir = common_dirs.src_package_dir()
    base_dir = common_dirs.base_dir()
    print('extDIR:', ext_dir)
    print('srcPackageDIR:', src_package_dir)
    print('baseDIR:', base_dir)

    remove_path_contains('miniconda')
    remove_path_contains('anaconda')
    print('PATH:', os.environ['PATH'])

    if is_windows():
        os.environ['HOME'] = os.path.expanduser("~")

    print('HOME:', os.environ['HOME'])

    if libs['cmake']:
        if is_windows():
            unpack_tool_to_software_dir(src_package_dir, 'cmake*win64*')
        elif is_linux():
            unpack_tool_to_software_dir(src_package_dir, 'cmake*Linux*')
        else:
            unpack_tool_to_software_dir(src_package_dir, 'cmake*Darwin*')

    if libs['curl']:
        if is_windows():
            unpack_tool_to_software_dir(src_package_dir, 'curl*win*')

    if libs['zlib']:
        if is_windows():
            package_name = find_src_package_with_glob(os.path.join(src_package_dir, 'zlib*'))
            src_dir = get_package_top_level_folder(package_name, base_dir)
            if update_src:
                shutil.rmtree(src_dir, ignore_errors=True)
                unpack_file_to_folder(package_name, base_dir)
            assert os.path.exists(src_dir)
            build_zlib(src_dir, os.path.join(ext_dir, 'zlib'), ext_dir)

    if libs['ffmpeg']:
        if is_windows():
            package_name = find_src_package_with_glob(os.path.join(src_package_dir, 'ffmpeg*win*'))
            package_unpack_folder = get_package_top_level_folder(package_name, ext_dir)
            unpack_file_to_folder(package_name, ext_dir)
            os.replace(os.path.join(package_unpack_folder, 'bin', 'ffmpeg.exe'),
                       os.path.join(ext_dir, 'ffmpeg.exe'))
            shutil.rmtree(package_unpack_folder, ignore_errors=False)
        elif is_linux():
            package_name = find_src_package_with_glob(os.path.join(src_package_dir, 'ffmpeg*static.tar.xz'))
            package_unpack_folder = get_package_top_level_folder(package_name, ext_dir)
            unpack_file_to_folder(package_name, ext_dir)
            os.replace(os.path.join(package_unpack_folder, 'ffmpeg'),
                       os.path.join(ext_dir, 'ffmpeg'))
            shutil.rmtree(package_unpack_folder, ignore_errors=False)
        else:
            unpack_file_to_folder(find_src_package_with_glob(os.path.join(src_package_dir, 'ffmpeg*7z')),
                                  ext_dir)

    if libs['boost']:
        package_name = find_src_package_with_glob(os.path.join(src_package_dir, 'boost*'))
        src_dir = get_package_top_level_folder(package_name, ext_dir)
        shutil.rmtree(os.path.join(ext_dir, 'boost'), ignore_errors=True)
        unpack_file_to_folder(package_name, ext_dir)
        os.rename(src_dir, os.path.join(ext_dir, 'boost'))

    if libs['eigen']:
        package_name = find_src_package_with_glob(os.path.join(src_package_dir, 'eigen*'))
        src_dir = get_package_top_level_folder(package_name, ext_dir)
        shutil.rmtree(os.path.join(ext_dir, 'eigen'), ignore_errors=True)
        unpack_file_to_folder(package_name, ext_dir)
        os.rename(src_dir, os.path.join(ext_dir, 'eigen'))

    if libs['glm']:
        src_dir = os.path.join(base_dir, 'glm')
        update_or_clone_git_repository(src_dir, 'git@github.com:g-truc/glm.git')
        export_git_repository(src_dir, os.path.join(ext_dir, 'glm'))

    if libs['googletest']:
        src_dir = os.path.join(base_dir, 'googletest')
        update_or_clone_git_repository(src_dir, 'git@github.com:google/googletest.git')
        shutil.rmtree(os.path.join(ext_dir, 'googletest'), ignore_errors=True)
        shutil.copytree(os.path.join(src_dir, 'googletest'), os.path.join(ext_dir, 'googletest'))

    if libs['folly']:
        src_dir = os.path.join(base_dir, 'folly')
        update_or_clone_git_repository(src_dir, 'git@github.com:facebook/folly.git')
        export_git_repository(src_dir, os.path.join(ext_dir, 'folly'))
        build_folly(src_dir, os.path.join(ext_dir, 'folly'))

    if libs['glog']:
        src_dir = os.path.join(base_dir, 'glog')
        gflags_src_dir = os.path.join(base_dir, 'gflags')
        if update_src:
            update_or_clone_git_repository(gflags_src_dir, 'git@github.com:gflags/gflags.git')
            update_or_clone_git_repository(src_dir, 'git@github.com:google/glog.git')
        assert os.path.exists(src_dir)
        assert os.path.exists(gflags_src_dir)
        build_gflags(gflags_src_dir, os.path.join(ext_dir, 'gflags'), ext_dir)
        build_glog(src_dir, os.path.join(ext_dir, 'glog'), ext_dir)

    if libs['benchmark']:
        src_dir = os.path.join(base_dir, 'benchmark')
        if update_src:
            update_or_clone_git_repository(src_dir, 'git@github.com:google/benchmark.git')
        assert os.path.exists(src_dir)
        build_benchmark(src_dir, os.path.join(ext_dir, 'benchmark'), ext_dir)

    if libs['glbinding']:
        src_dir = os.path.join(base_dir, 'glbinding')
        if update_src:
            update_or_clone_git_repository(src_dir, 'git@github.com:cginternals/glbinding.git')
        assert os.path.exists(src_dir)
        build_glbinding(src_dir, os.path.join(ext_dir, 'glbinding'), ext_dir)

    if libs['libjpeg']:
        if is_windows():
            nasm_dir = unpack_tool_to_software_dir(src_package_dir, 'nasm*win64*', 'nasm-*')
        elif is_mac():
            nasm_dir = unpack_tool_to_software_dir(src_package_dir, 'nasm*macosx*', 'nasm-*')
            os.chmod(os.path.join(nasm_dir, 'nasm'), stat.S_IXUSR)
        else:
            nasm_dir = ''
        package_name = find_src_package_with_glob(os.path.join(src_package_dir, 'libjpeg*'))
        src_dir = get_package_top_level_folder(package_name, base_dir)
        if update_src:
            shutil.rmtree(src_dir, ignore_errors=True)
            unpack_file_to_folder(package_name, base_dir)
        assert os.path.exists(src_dir)
        build_libjpeg(src_dir, os.path.join(ext_dir, 'libjpeg-turbo'), ext_dir, nasm_dir=nasm_dir)

    if libs['libpng']:
        package_name = find_src_package_with_glob(os.path.join(src_package_dir, 'libpng*'))
        src_dir = get_package_top_level_folder(package_name, base_dir)
        if update_src:
            shutil.rmtree(src_dir, ignore_errors=True)
            unpack_file_to_folder(package_name, base_dir)
        assert os.path.exists(src_dir)
        build_libpng(src_dir, os.path.join(ext_dir, 'libpng'), ext_dir)

    if libs['jxrlib']:
        src_dir = os.path.join(base_dir, 'jxrlib')
        if update_src:
            update_or_clone_git_repository(src_dir, 'https://git01.codeplex.com/jxrlib')
        assert os.path.exists(src_dir)
        build_jxrlib(src_dir, os.path.join(ext_dir, 'jxrlib'), ext_dir)

    if libs['geometrictools']:
        package_name = find_src_package_with_glob(os.path.join(src_package_dir, 'GeometricToolsEngine*'))
        src_dir = get_package_top_level_folder(package_name, base_dir)
        if update_src:
            shutil.rmtree(src_dir, ignore_errors=True)
            unpack_file_to_folder(package_name, base_dir)
        assert os.path.exists(src_dir)
        build_geometrictools(src_dir, os.path.join(ext_dir, 'geometrictools'), ext_dir)

    if libs['assimp']:
        src_dir = os.path.join(base_dir, 'assimp')
        if update_src:
            update_or_clone_git_repository(src_dir, 'git@github.com:assimp/assimp.git')
        assert os.path.exists(src_dir)
        build_assimp(src_dir, os.path.join(ext_dir, 'assimp'), ext_dir)

    if libs['hdf5']:
        package_name = find_src_package_with_glob(os.path.join(src_package_dir, 'hdf5*'))
        src_dir = get_package_top_level_folder(package_name, base_dir)
        if update_src:
            shutil.rmtree(src_dir, ignore_errors=True)
            unpack_file_to_folder(package_name, base_dir)
        assert os.path.exists(src_dir)
        build_hdf5(src_dir, os.path.join(ext_dir, 'hdf5'), ext_dir)

    if libs['freeimage']:
        package_name = find_src_package_with_glob(os.path.join(src_package_dir, 'freeimage*'))
        src_dir = get_package_top_level_folder(package_name, base_dir)
        if update_src:
            shutil.rmtree(src_dir, ignore_errors=True)
            unpack_file_to_folder(package_name, base_dir)
        assert os.path.exists(src_dir)
        build_freeimage(src_dir, os.path.join(ext_dir, 'freeimage'), ext_dir)

    if libs['itk']:
        src_dir = os.path.join(base_dir, 'ITK')
        if update_src:
            update_or_clone_git_repository(src_dir, 'git://itk.org/ITK.git')
        assert os.path.exists(src_dir)
        build_itk(src_dir, os.path.join(ext_dir, 'itk'), ext_dir)

    if libs['vtk']:
        src_dir = os.path.join(base_dir, 'VTK')
        if update_src:
            update_or_clone_git_repository(src_dir, 'https://gitlab.kitware.com/vtk/vtk.git')
        assert os.path.exists(src_dir)
        build_vtk(src_dir, os.path.join(ext_dir, 'vtk'), ext_dir)

    if libs['opencv']:
        src_dir = os.path.join(base_dir, 'opencv')
        src_contrib_dir = os.path.join(base_dir, 'opencv_contrib')
        if update_src:
            update_or_clone_git_repository(src_dir, 'git@github.com:Itseez/opencv.git')
            update_or_clone_git_repository(src_contrib_dir, 'git@github.com:Itseez/opencv_contrib.git')
        assert os.path.exists(src_dir)
        assert os.path.exists(src_contrib_dir)
        build_opencv(src_dir, src_contrib_dir, os.path.join(ext_dir, 'opencv'), ext_dir)

    if libs['botan']:
        src_dir = os.path.join(base_dir, 'botan')
        if update_src:
            update_or_clone_git_repository(src_dir, 'git@github.com:randombit/botan.git')
        assert os.path.exists(src_dir)
        build_botan(src_dir, os.path.join(ext_dir, 'botan'), ext_dir)

    if libs['ospray']:
        if is_windows():
            ispc_dir = unpack_tool_to_software_dir(src_package_dir, 'ispc*win*')
            embree_dir = unpack_tool_to_software_dir(src_package_dir, 'embree*win*')
        elif is_linux():
            ispc_dir = unpack_tool_to_software_dir(src_package_dir, 'ispc*linux*')
            embree_dir = unpack_tool_to_software_dir(src_package_dir, 'embree*linux*')
        else:
            ispc_dir = unpack_tool_to_software_dir(src_package_dir, 'ispc*osx*')
            embree_dir = unpack_tool_to_software_dir(src_package_dir, 'embree*osx*')
        src_dir = os.path.join(base_dir, 'OSPRay')
        if update_src:
            update_or_clone_git_repository(src_dir, 'git@github.com:ospray/OSPRay.git')
        assert os.path.exists(src_dir)
        assert os.path.exists(ispc_dir)
        assert os.path.exists(embree_dir)
        build_ospray(src_dir, os.path.join(ext_dir, 'ospray'), ext_dir, ispc_dir=ispc_dir, embree_dir=embree_dir)


def parse_inputs(argv: list):
    libs = {'cmake': False,
            'curl': False,
            'zlib': False,
            'ffmpeg': False,
            'boost': False,
            'eigen': False,
            'glm': False,
            'googletest': False,
            'folly': False,
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
            'botan': False,
            'ospray': False
            }
    update_src = True
    libs_reverse_depends = {'eigen': ['opencv'],
                            'zlib': ['libpng', 'assimp', 'hdf5', 'itk', 'vtk', 'opencv']
                            }

    print('current interpreter: ' + sys.executable)
    if len(argv) == 1:
        usage = 'usage: python3 build_ext_libs.py [noupdatesrc] [all or components...] [except] [components...]\n' \
                'valid components:'
        for lib in libs:
            usage += ' [' + lib + ']'
        print(usage)
        sys.exit(0)

    state = True
    for lib in argv[1:]:
        if lib.lower() == "all":
            for vlib in libs:
                libs[vlib] = state
        elif lib.lower() == "except":
            state = False
        elif lib.lower() in libs:
            libs[lib.lower()] = state
        elif lib.lower() == 'noupdatesrc':
            update_src = False
        else:
            raise SyntaxError("wrong lib name: " + lib)

    libs['cmake'] = True
    if is_linux():
        libs['zlib'] = False
        libs['curl'] = False
    elif is_mac():
        libs['zlib'] = False
        libs['curl'] = False

    for lib, rev_dep in libs_reverse_depends.items():
        if libs[lib.lower()]:
            for dlib in rev_dep:
                libs[dlib.lower()] = True

    return libs, update_src


if __name__ == "__main__":
    build_libs(*parse_inputs(sys.argv))
