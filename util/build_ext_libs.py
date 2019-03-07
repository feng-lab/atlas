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
import mmap

from common_dirs import *


def macos_min_version():
    return '10.12'


def update_or_clone_git_repository(repository_folder: str, repository_url: str):
    if os.path.exists(repository_folder):
        print('git', 'pull', Path(repository_folder).name)
        subprocess.run(['git', 'pull'], cwd=repository_folder, shell=False, check=False)
    else:
        subprocess.run(['git', 'clone', repository_url, repository_folder], shell=False, check=True)


def update_or_clone_git_repository_with_submodules(repository_folder: str, repository_url: str):
    if os.path.exists(repository_folder):
        print('git', 'pull', Path(repository_folder).name)
        subprocess.run(['git', 'pull'], cwd=repository_folder, shell=False, check=False)
        subprocess.run(['git', 'submodule', 'update', '--init'], cwd=repository_folder, shell=False, check=False)
    else:
        subprocess.run(['git', 'clone', '--recursive', repository_url, repository_folder], shell=False, check=True)


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


def get_vcvars_environment(remove_conda_from_path: bool = True):
    """
    Returns a dictionary containing the environment variables set up by vcvarsall.bat amd64
    """

    vcvars = os.path.normpath(os.path.join(vs_install_dir(), 'VC', 'Auxiliary', 'Build', 'vcvarsall.bat'))
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
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, shell=False, universal_newlines=True,
                                   env=start_env)
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
    remove_path_contains('mingw', env)
    return env


def get_tbb_env():
    if is_windows():
        env = get_enviroment_from_shell_script(os.path.join(intel_sw_dir(), 'tbb', 'bin',
                                                            'tbbvars.bat'),
                                               para='intel64 vs2017',
                                               start_env=get_vcvars_environment())
    else:
        if is_linux():
            env = get_enviroment_from_shell_script(os.path.join(intel_sw_dir(), 'tbb', 'bin',
                                                                'tbbvars.sh'), 'intel64')
        else:
            env = get_enviroment_from_shell_script(os.path.join(intel_sw_dir(), 'tbb', 'bin',
                                                                'tbbvars.sh'))
    return env


def get_cmake_cmd_common_part(install_dir: str):
    if is_windows():
        if use_ninja():
            return [get_cmake_binary(),  # '-E', 'echo',
                    '-DCMAKE_BUILD_TYPE=Release',
                    '-G', 'Ninja', '-DCMAKE_MAKE_PROGRAM=' + get_ninja_binary(),
                    '-DCMAKE_INSTALL_PREFIX=' + install_dir
                    ]
        else:
            return [get_cmake_binary(),  # '-E', 'echo',
                    '-G', 'Visual Studio 15 2017 Win64', '-T', 'host=x64',
                    '-DCMAKE_INSTALL_PREFIX=' + install_dir
                    ]
    elif is_linux():
        res = [get_cmake_binary(),  # '-E', 'echo',
               '-DCMAKE_BUILD_TYPE=Release',
               '-DCMAKE_INSTALL_PREFIX=' + install_dir,
               '-DCMAKE_C_FLAGS:STRING=-fPIC',
               '-DCMAKE_CXX_FLAGS:STRING=-std=c++14 -fPIC'
               ]
        if use_ninja():
            res.extend(['-G', 'Ninja', '-DCMAKE_MAKE_PROGRAM=' + get_ninja_binary()])
        return res
    else:
        osx_sysroot = r'/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk'
        assert os.path.exists(osx_sysroot)

        res = [get_cmake_binary(),  # '-E', 'echo',
               '-DCMAKE_BUILD_TYPE=Release',
               '-DCMAKE_INSTALL_PREFIX=' + install_dir,
               '-DCMAKE_OSX_DEPLOYMENT_TARGET=' + macos_min_version(),
               '-DCMAKE_OSX_SYSROOT=' + osx_sysroot,
               '-DCMAKE_CXX_FLAGS:STRING=-stdlib=libc++ -std=c++14'
               ]
        if use_ninja():
            res.extend(['-G', 'Ninja', '-DCMAKE_MAKE_PROGRAM=' + get_ninja_binary()])
        return res


def build_cmakecmd(cmakecmd, build_dir: str, env=None):
    if is_windows():
        if env is None:
            env = get_vcvars_environment()
        subprocess.run(cmakecmd, cwd=build_dir, shell=False, check=True, env=env)
        if use_ninja():
            subprocess.run([get_ninja_binary()],
                           cwd=build_dir, shell=False, check=True, env=env)
        else:
            subprocess.run(['MSBuild', 'ALL_BUILD.vcxproj', '/property:Configuration=Release', '/maxcpucount'],
                           cwd=build_dir, shell=True, check=True, env=env)
    else:
        if use_ninja():
            if env is None:
                subprocess.run(cmakecmd, cwd=build_dir, shell=False, check=True)
                subprocess.run([get_ninja_binary()],
                               cwd=build_dir, shell=False, check=True)
            else:
                subprocess.run(cmakecmd, cwd=build_dir, shell=False, check=True, env=env)
                subprocess.run([get_ninja_binary()],
                               cwd=build_dir, shell=False, check=True, env=env)
        else:
            if env is None:
                subprocess.run(cmakecmd, cwd=build_dir, shell=False, check=True)
                subprocess.run(['make', '-j' + str(os.cpu_count())],
                               cwd=build_dir, shell=False, check=True)
            else:
                subprocess.run(cmakecmd, cwd=build_dir, shell=False, check=True, env=env)
                subprocess.run(['make', '-j' + str(os.cpu_count())],
                               cwd=build_dir, shell=False, check=True, env=env)


def build_and_install_cmakecmd(cmakecmd, build_dir: str, env=None):
    if is_windows():
        if env is None:
            env = get_vcvars_environment()
        subprocess.run(cmakecmd, cwd=build_dir, shell=False, check=True, env=env)
        if use_ninja():
            subprocess.run([get_ninja_binary(), 'install'],
                           cwd=build_dir, shell=False, check=True, env=env)
        else:
            subprocess.run(['MSBuild', 'INSTALL.vcxproj', '/property:Configuration=Release', '/maxcpucount'],
                           cwd=build_dir, shell=True, check=True, env=env)
    else:
        if use_ninja():
            if env is None:
                subprocess.run(cmakecmd, cwd=build_dir, shell=False, check=True)
                subprocess.run([get_ninja_binary(), 'install'],
                               cwd=build_dir, shell=False, check=True)
            else:
                subprocess.run(cmakecmd, cwd=build_dir, shell=False, check=True, env=env)
                subprocess.run([get_ninja_binary(), 'install'],
                               cwd=build_dir, shell=False, check=True, env=env)
        else:
            if env is None:
                subprocess.run(cmakecmd, cwd=build_dir, shell=False, check=True)
                subprocess.run(['make', '-j' + str(os.cpu_count()), 'install'],
                               cwd=build_dir, shell=False, check=True)
            else:
                subprocess.run(cmakecmd, cwd=build_dir, shell=False, check=True, env=env)
                subprocess.run(['make', '-j' + str(os.cpu_count()), 'install'],
                               cwd=build_dir, shell=False, check=True, env=env)


def patch_file(orig_file: str, from_texts: list, to_texts: list, keep_bak_file: bool=True) -> str:
    assert len(from_texts) == len(to_texts)
    bak_file = get_bak_file_name(orig_file)
    os.rename(orig_file, bak_file)
    with open(bak_file, mode='r', encoding='utf-8') as f:
        from_lines = f.readlines()
    with open(orig_file, mode='w', encoding='utf-8') as f:
        to_lines = []
        for line in from_lines:
            for from_text, to_text in zip(from_texts, to_texts):
                line = line.replace(from_text, to_text)
            f.write(line)
            to_lines.append(line)
    print(''.join(list(difflib.unified_diff(from_lines, to_lines, fromfile=orig_file, tofile='<new>'))))
    if not keep_bak_file:
        os.remove(bak_file)
    return bak_file


def build_cpuinfo(src_dir: str, install_dir: str):
    build_dir = create_build_dir(src_dir)
    shutil.rmtree(install_dir, ignore_errors=True)

    try:
        cmakecmd = get_cmake_cmd_common_part(install_dir)
        cmakecmd.extend(['-DBUILD_GMOCK:BOOL=OFF',
                         '-DCPUINFO_BUILD_MOCK_TESTS:BOOL=OFF',
                         '-DCPUINFO_BUILD_BENCHMARKS:BOOL=OFF',
                         '-DCPUINFO_BUILD_UNIT_TESTS:BOOL=OFF',
                         '-DCPUINFO_BUILD_TOOLS:BOOL=ON'])

        cmakecmd.extend([src_dir])
        build_and_install_cmakecmd(cmakecmd, build_dir)
    finally:
        shutil.rmtree(build_dir, ignore_errors=False)


def build_gflags(src_dir: str, install_dir: str):
    build_dir = create_build_dir(src_dir)
    shutil.rmtree(install_dir, ignore_errors=True)

    try:
        cmakecmd = get_cmake_cmd_common_part(install_dir)
        cmakecmd.extend([src_dir])
        build_and_install_cmakecmd(cmakecmd, build_dir)
    finally:
        shutil.rmtree(build_dir, ignore_errors=False)


def build_glog(src_dir: str, install_dir: str):
    build_dir = create_build_dir(src_dir)
    shutil.rmtree(install_dir, ignore_errors=True)

    try:
        subprocess.run(['git', 'apply', '--stat', '--apply', os.path.join(ext_dir(), 'glog_patch.txt')],
                       cwd=src_dir, shell=False, check=True)

        cmakecmd = get_cmake_cmd_common_part(install_dir)

        if is_windows():
            patch_file(os.path.join(src_dir, 'src', 'logging_unittest.cc'),
                       from_texts=[r'google::ERROR'],
                       to_texts=[r'GLOG_ERROR'],
                       keep_bak_file=False)

            cmakecmd.extend([f'-Dgflags_DIR:PATH={ext_dir()}/gflags/CMake'])
        else:
            cmakecmd.extend([f'-Dgflags_DIR:PATH={ext_dir()}/gflags/lib/cmake/gflags'])

        cmakecmd.extend([src_dir])
        build_and_install_cmakecmd(cmakecmd, build_dir)
    finally:
        subprocess.run(['git', 'reset', '--hard', 'HEAD'],
                       cwd=src_dir, shell=False, check=True)
        shutil.rmtree(build_dir, ignore_errors=False)


def build_benchmark(src_dir: str, install_dir: str):
    build_dir = create_build_dir(src_dir)
    shutil.rmtree(install_dir, ignore_errors=True)

    try:
        cmakecmd = get_cmake_cmd_common_part(install_dir)
        cmakecmd.extend(['-DBENCHMARK_ENABLE_TESTING:BOOL=OFF',
                         '-DBENCHMARK_ENABLE_GTEST_TESTS:BOOL=OFF'])

        if is_windows():
            cmakecmd.extend(['-DBENCHMARK_ENABLE_LTO:BOOL=ON'])
        elif is_mac():
            cmakecmd.extend(['-DBENCHMARK_USE_LIBCXX:BOOL=ON'])

        cmakecmd.extend([src_dir])
        build_and_install_cmakecmd(cmakecmd, build_dir)
    finally:
        shutil.rmtree(build_dir, ignore_errors=False)


def build_grpc(src_dir: str, install_dir: str, nasm_dir: str):
    ssl_src_dir = os.path.join(src_dir, 'third_party', 'boringssl')
    ssl_install_dir = f'{ext_dir()}/boringssl'
    ssl_build_dir = os.path.join(ssl_src_dir, 'build')
    shutil.rmtree(ssl_build_dir, ignore_errors=True)
    os.makedirs(ssl_build_dir, exist_ok=False)
    shutil.rmtree(ssl_install_dir, ignore_errors=True)
    try:
        cmakecmd = get_cmake_cmd_common_part(ssl_install_dir)
        if is_windows():
            cmakecmd.extend(['-DCMAKE_ASM_NASM_COMPILER:FILEPATH=' + nasm_dir + '\\nasm.exe',
                             ssl_src_dir])
        else:
            cmakecmd.extend([ssl_src_dir])
        build_cmakecmd(cmakecmd, ssl_build_dir)
        shutil.copytree(os.path.join(ssl_src_dir, 'include'), os.path.join(ssl_install_dir, 'include'))
        if is_windows():
            glob_copy(os.path.join(ssl_build_dir, '*.lib'), os.path.join(ssl_install_dir, 'lib'))
            glob_copy(os.path.join(ssl_build_dir, 'decrepit', '*.lib'), os.path.join(ssl_install_dir, 'lib'))
            glob_copy(os.path.join(ssl_build_dir, 'crypto', '*.lib'), os.path.join(ssl_install_dir, 'lib'))
            glob_copy(os.path.join(ssl_build_dir, 'ssl', '*.lib'), os.path.join(ssl_install_dir, 'lib'))
        else:
            glob_copy(os.path.join(ssl_build_dir, 'lib*.a'), os.path.join(ssl_install_dir, 'lib'))
            glob_copy(os.path.join(ssl_build_dir, 'decrepit', 'lib*.a'), os.path.join(ssl_install_dir, 'lib'))
            glob_copy(os.path.join(ssl_build_dir, 'crypto', 'lib*.a'), os.path.join(ssl_install_dir, 'lib'))
            glob_copy(os.path.join(ssl_build_dir, 'ssl', 'lib*.a'), os.path.join(ssl_install_dir, 'lib'))
    finally:
        shutil.rmtree(ssl_build_dir, ignore_errors=False)

    sub_src_dir = os.path.join(src_dir, 'third_party', 'cares', 'cares')
    sub_install_dir = f'{ext_dir()}/c-ares'
    sub_build_dir = create_build_dir(sub_src_dir)
    shutil.rmtree(sub_install_dir, ignore_errors=True)
    try:
        cmakecmd = get_cmake_cmd_common_part(sub_install_dir)
        cmakecmd.extend(['-DCARES_SHARED:BOOL=OFF',
                         '-DCARES_STATIC:BOOL=ON',
                         '-DCARES_STATIC_PIC:BOOL=ON',
                         sub_src_dir])
        build_and_install_cmakecmd(cmakecmd, sub_build_dir)
    finally:
        shutil.rmtree(sub_build_dir, ignore_errors=False)

    sub_src_dir = os.path.join(src_dir, 'third_party', 'protobuf', 'cmake')
    sub_install_dir = f'{ext_dir()}/protobuf'
    sub_build_dir = create_build_dir(sub_src_dir)
    shutil.rmtree(sub_install_dir, ignore_errors=True)
    try:
        cmakecmd = get_cmake_cmd_common_part(sub_install_dir)
        cmakecmd.extend(['-Dprotobuf_BUILD_TESTS:BOOL=OFF',
                         '-Dprotobuf_WITH_ZLIB:BOOL=ON',
                         '-Dprotobuf_MSVC_STATIC_RUNTIME:BOOL=OFF'])
        if is_windows():
            cmakecmd.extend([f'-DZLIB_ROOT:PATH={ext_dir()}/zlib'])
        cmakecmd.extend([sub_src_dir])
        build_and_install_cmakecmd(cmakecmd, sub_build_dir)
    finally:
        shutil.rmtree(sub_build_dir, ignore_errors=False)

    build_dir = create_build_dir(src_dir)
    shutil.rmtree(install_dir, ignore_errors=True)
    try:
        cmakecmd = get_cmake_cmd_common_part(install_dir)
        cmakecmd.extend(['-DgRPC_INSTALL:BOOL=ON',
                         '-DgRPC_BUILD_TESTS:BOOL=OFF',
                         '-DgRPC_ZLIB_PROVIDER:STRING=package',
                         '-DgRPC_PROTOBUF_PROVIDER=package',
                         '-DgRPC_PROTOBUF_PACKAGE_TYPE:STRING=CONFIG',
                         '-DgRPC_CARES_PROVIDER=package',
                         '-DgRPC_SSL_PROVIDER=package',
                         f'-DOPENSSL_ROOT_DIR:PATH={ssl_install_dir}',
                         '-DgRPC_GFLAGS_PROVIDER:STRING=package',
                         '-DgRPC_BENCHMARK_PROVIDER:STRING=package'])
        if is_windows():
            cmakecmd.extend([f'-DZLIB_ROOT:PATH={ext_dir()}/zlib',
                             f'-Dgflags_DIR:PATH={ext_dir()}/gflags/lib/cmake/gflags',
                             f'-Dbenchmark_DIR:PATH={ext_dir()}/benchmark/lib/cmake/benchmark',
                             f'-DProtobuf_DIR:PATH={ext_dir()}/protobuf/cmake',
                             f'-Dc-ares_DIR:PATH={ext_dir()}/c-ares/lib/cmake/c-ares'])
        else:
            cmakecmd.extend([f'-Dgflags_DIR:PATH={ext_dir()}/gflags/lib/cmake/gflags',
                             f'-Dbenchmark_DIR:PATH={ext_dir()}/benchmark/lib/cmake/benchmark',
                             f'-DProtobuf_DIR:PATH={ext_dir()}/protobuf/lib/cmake/protobuf',
                             f'-Dc-ares_DIR:PATH={ext_dir()}/c-ares/lib/cmake/c-ares'])
        cmakecmd.extend([src_dir])
        build_and_install_cmakecmd(cmakecmd, build_dir)
    finally:
        shutil.rmtree(build_dir, ignore_errors=False)


def build_glbinding(src_dir: str, install_dir: str):
    build_dir = create_build_dir(src_dir)
    shutil.rmtree(install_dir, ignore_errors=True)

    try:
        cmakecmd = get_cmake_cmd_common_part(install_dir)
        cmakecmd.extend(['-DOPTION_BUILD_TOOLS:BOOL=OFF',
                         '-DBUILD_SHARED_LIBS:BOOL=OFF',
                         '-DOPTION_BUILD_TESTS:BOOL=OFF',
                         '-DOPTION_BUILD_GPU_TESTS:BOOL=OFF',
                         src_dir])
        build_and_install_cmakecmd(cmakecmd, build_dir)
    finally:
        shutil.rmtree(build_dir, ignore_errors=False)


def build_libjpeg(src_dir: str, install_dir: str, nasm_dir: str):
    build_dir = create_build_dir(src_dir)
    shutil.rmtree(install_dir, ignore_errors=True)

    try:
        cmakecmd = get_cmake_cmd_common_part(install_dir)
        if is_windows():
            cmakecmd.extend(['-DENABLE_SHARED:BOOL=OFF',
                             '-DCMAKE_ASM_NASM_COMPILER:FILEPATH=' + nasm_dir + '\\nasm.exe',
                             '-DWITH_CRT_DLL:BOOL=ON',
                             src_dir])
        else:
            if is_linux():
                cmakecmd.extend(['-DENABLE_SHARED:BOOL=OFF',
                                 '-DCMAKE_ASM_NASM_COMPILER:FILEPATH=nasm',
                                 src_dir])
            else:
                cmakecmd.extend(['-DENABLE_SHARED:BOOL=OFF',
                                 '-DCMAKE_ASM_NASM_COMPILER:FILEPATH=' + nasm_dir + '/nasm',
                                 src_dir])
        build_and_install_cmakecmd(cmakecmd, build_dir)
    finally:
        shutil.rmtree(build_dir, ignore_errors=False)


def build_zlib(src_dir: str, install_dir: str):
    build_dir = create_build_dir(src_dir)
    shutil.rmtree(install_dir, ignore_errors=True)

    try:
        cmakecmd = get_cmake_cmd_common_part(install_dir)

        if not is_windows():
            cmakecmd.extend(['-DAMD64:BOOL=ON'])

        cmakecmd.extend([src_dir])
        build_and_install_cmakecmd(cmakecmd, build_dir)
    finally:
        shutil.rmtree(build_dir, ignore_errors=False)


def build_folly(src_dir: str, install_dir: str):
    del src_dir
    try:
        if is_windows():
            shutil.copy2(os.path.join(ext_dir(), 'folly-configs', 'folly-config-win.h'),
                         os.path.join(install_dir, 'folly', 'folly-config.h'))
        elif is_mac():
            shutil.copy2(os.path.join(ext_dir(), 'folly-configs', 'folly-config-macos.h'),
                         os.path.join(install_dir, 'folly', 'folly-config.h'))
        else:
            shutil.copy2(os.path.join(ext_dir(), 'folly-configs', 'folly-config-linux.h'),
                         os.path.join(install_dir, 'folly', 'folly-config.h'))

        orig_file = os.path.join(install_dir, 'folly', 'ScopeGuard.h')
        patch_file(orig_file,
                   from_texts=[r'static void warnAboutToCrash() noexcept;'],
                   to_texts=[r'inline static void warnAboutToCrash() noexcept {}'])
    finally:
        print('')


def build_libpng(src_dir: str, install_dir: str):
    build_dir = create_build_dir(src_dir)
    shutil.rmtree(install_dir, ignore_errors=True)

    orig_file = None
    bak_file = None
    orig_file1 = None
    bak_file1 = None
    orig_file2 = None
    bak_file2 = None
    try:
        orig_file = os.path.join(src_dir, 'pngread.c')
        bak_file = patch_file(orig_file,
                              from_texts=[r'                || (png_ptr->mode & PNG_HAVE_CHUNK_AFTER_IDAT) != 0)'],
                              to_texts=[r'                && 0)'])

        cmakecmd = get_cmake_cmd_common_part(install_dir)
        cmakecmd.extend(['-DPNG_TESTS:BOOL=OFF',
                         '-DPNG_SHARED:BOOL=OFF'])

        if is_windows():
            cmakecmd.extend(['-DZLIB_INCLUDE_DIR:PATH=' + ext_dir() + '\\zlib\\include',
                             '-DZLIB_LIBRARY_RELEASE:FILEPATH=' + ext_dir() + '\\zlib\\lib\\zlibstatic.lib'])
        else:
            if is_mac() and os.path.exists('/usr/include'):
                orig_file1 = os.path.join(src_dir, 'pngpriv.h')
                bak_file1 = patch_file(orig_file1,
                                       from_texts=[r'#if PNG_ZLIB_VERNUM != 0 && PNG_ZLIB_VERNUM != ZLIB_VERNUM'],
                                       to_texts=[r'#if 0'])

            if is_mac():
                orig_file2 = os.path.join(src_dir, 'pngrutil.c')
                bak_file2 = patch_file(orig_file2,
                                       from_texts=[r'#if ZLIB_VERNUM >= 0x1290'],
                                       to_texts=[r'#if 0'])

        cmakecmd.extend([src_dir])
        build_and_install_cmakecmd(cmakecmd, build_dir)
    finally:
        os.replace(bak_file, orig_file)
        if is_mac() and os.path.exists('/usr/include'):
            os.replace(bak_file1, orig_file1)
        if is_mac():
            os.replace(bak_file2, orig_file2)
        shutil.rmtree(build_dir, ignore_errors=False)


def build_jxrlib(src_dir: str, install_dir: str):
    shutil.rmtree(install_dir, ignore_errors=True)

    orig_file = None
    bak_file = None
    try:
        if is_windows():
            env = get_vcvars_environment()
            subprocess.run(['devenv', 'JXR_vc14.sln', '/Upgrade'],
                           cwd=os.path.join(src_dir, 'jxrencoderdecoder'), shell=True, check=True, env=env)
            subprocess.run(['MSBuild', 'JXR_vc14.sln', '/target:JXRDecApp', '/property:Platform=x64',
                            '/property:WindowsTargetPlatformVersion=' + env['UCRTVERSION'],  # like 10.0.16299.0
                            '/property:ForceImportBeforeCppTargets=' + ext_dir() + '\\runtime_md.props',
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
            orig_file = os.path.join(src_dir, 'Makefile')
            from_texts = [r'CFLAGS=-I. -Icommon/include -I$(DIR_SYS) '
                          r'$(ENDIANFLAG) -D__ANSI__ -DDISABLE_PERF_MEASUREMENT -w $(PICFLAG) -O']
            if is_linux():
                to_texts = [r'CFLAGS=-I. -Icommon/include -I$(DIR_SYS) '
                            r'$(ENDIANFLAG) -D__ANSI__ -DDISABLE_PERF_MEASUREMENT -w $(PICFLAG) -O3 -fPIC']
            else:
                to_texts = [r'CFLAGS=-I. -Icommon/include -I$(DIR_SYS) '
                            r'$(ENDIANFLAG) -D__ANSI__ -DDISABLE_PERF_MEASUREMENT -w $(PICFLAG) -O3 '
                            r'-mmacosx-version-min={0}'.format(macos_min_version())]

            bak_file = patch_file(orig_file, from_texts=from_texts, to_texts=to_texts)

            subprocess.run(['make', '-j' + str(os.cpu_count()), 'install', 'DIR_INSTALL=' + install_dir],
                           cwd=src_dir, shell=False, check=True)
    finally:
        if not is_windows():
            shutil.rmtree(os.path.join(src_dir, 'build'), ignore_errors=True)
            os.replace(bak_file, orig_file)
        subprocess.run(['git', 'reset', '--hard', 'HEAD'],
                       cwd=src_dir, shell=False, check=True)


def build_geometrictools(src_dir: str, install_dir: str):
    shutil.rmtree(install_dir, ignore_errors=True)

    orig_file = None
    bak_file = None
    orig_file2 = None
    bak_file2 = None
    orig_file3 = None
    bak_file3 = None
    try:
        if is_windows():
            env = get_vcvars_environment()
            subprocess.run(['MSBuild', 'GTEngine.v15.vcxproj', '/property:Platform=x64',
                            '/property:Configuration=Release', '/maxcpucount',
                            '/property:ForceImportBeforeCppTargets=' + ext_dir() + '\\no_warning_as_error.props'],
                           cwd=src_dir, shell=True, check=True, env=env)
            glob_copy(os.path.join(src_dir, '_Output', 'v141', 'x64', 'Release', 'GTEngine.v15.lib'),
                      os.path.join(install_dir, 'lib'))
            glob_copy(os.path.join(src_dir, '_Output', 'v141', 'x64', 'Release', 'GTEngine.v15.pch'),
                      os.path.join(install_dir, 'lib'))
            glob_copy(os.path.join(src_dir, '_Output', 'v141', 'x64', 'Release', 'GTEngine.v15.pdb'),
                      os.path.join(install_dir, 'lib'))
        elif is_linux():
            orig_file = os.path.join(src_dir, 'makeengine.gte')
            bak_file = patch_file(orig_file,
                                  from_texts=[r'$(SRC_APPLICATIONS_GLX)/*.cpp',
                                              r'$(SRC_APPLICATIONS_GLX)/%.cpp',
                                              r'$(SRC_GRAPHICS_GLX)/*.cpp',
                                              r'$(SRC_GRAPHICS_GLX)/%.cpp'
                                              ],
                                  to_texts=[r'$(SRC_APPLICATIONS_GLX)/*.nonono',
                                            r'$(SRC_APPLICATIONS_GLX)/%.nonono',
                                            r'$(SRC_GRAPHICS_GLX)/*.nonono',
                                            r'$(SRC_GRAPHICS_GLX)/%.nonono'
                                            ])

            subprocess.run(['make', '-j' + str(os.cpu_count()), 'CFG=Release', '-f', 'makeengine.gte'],
                           cwd=src_dir, shell=False, check=True)
            shutil.copytree(os.path.join(src_dir, 'lib', 'Release'), os.path.join(install_dir, 'lib'))
        else:
            orig_file = os.path.join(src_dir, 'Source', 'Mathematics', 'GteGenerateMeshUV.cpp')
            bak_file = patch_file(orig_file,
                                  from_texts=[r'#include <Mathematics/GteGenerateMeshUV.h>'],
                                  to_texts=['#include <Mathematics/GteGenerateMeshUV.h>\n#include <string>'])
            orig_file2 = os.path.join(src_dir, 'Source', 'Mathematics', 'GteIEEEBinary16.cpp')
            bak_file2 = patch_file(orig_file2, from_texts=[r'_Float16'], to_texts=[r'___Float16'])
            orig_file3 = os.path.join(src_dir, 'Include', 'Mathematics', 'GteIEEEBinary16.h')
            bak_file3 = patch_file(orig_file3, from_texts=[r'_Float16'], to_texts=[r'___Float16'])

            shutil.copy2(os.path.join(ext_dir(), 'makeengine.macos.gte'), src_dir)
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
        if is_mac():
            os.replace(bak_file2, orig_file2)
            os.replace(bak_file3, orig_file3)


def build_ospray(src_dir: str, install_dir: str, ispc_dir: str, embree_dir: str):
    build_dir = create_build_dir(src_dir)
    shutil.rmtree(install_dir, ignore_errors=True)

    try:
        cmakecmd = get_cmake_cmd_common_part(install_dir)
        cmakecmd.extend(['-DOSPRAY_USE_EMBREE_STREAMS:BOOL=ON',
                         '-DOSPRAY_MODULE_BILINEAR_PATCH:BOOL=ON',
                         '-Dembree_DIR:PATH=' + embree_dir])

        env = get_tbb_env()
        print('TBBROOT:', env['TBBROOT'])
        if is_windows():
            cmakecmd.extend(['-DTBB_ROOT:PATH=' + env['TBBROOT'],
                             '-DISPC_EXECUTABLE:FILEPATH=' + ispc_dir + '\\ispc.exe'])
        else:
            cmakecmd.extend(['-DTBB_ROOT:PATH=' + env['TBBROOT'],
                             '-DISPC_EXECUTABLE:FILEPATH=' + ispc_dir + '/ispc'])

        cmakecmd.extend([src_dir])
        build_and_install_cmakecmd(cmakecmd, build_dir)
    finally:
        shutil.rmtree(build_dir, ignore_errors=False)


def build_assimp(src_dir: str, install_dir: str):
    build_dir = create_build_dir(src_dir)
    shutil.rmtree(install_dir, ignore_errors=True)

    orig_file = None
    bak_file = None
    orig_file_3 = None
    bak_file_3 = None
    orig_file_4 = None
    bak_file_4 = None
    try:
        orig_file = os.path.join(src_dir, 'include', 'assimp', 'defs.h')
        from_texts = [r'#define AI_MAX_ALLOC(type) ((256U * 1024 * 1024) / sizeof(type))']
        to_texts = [r'#define AI_MAX_ALLOC(type) ((size_t(256) * 1024 * 1024 * 1024) / sizeof(type))']
        bak_file = patch_file(orig_file, from_texts=from_texts, to_texts=to_texts)

        if is_mac():
            orig_file_3 = os.path.join(src_dir, 'assimpTargets-release.cmake.in')
            from_texts = [r'libassimp${ASSIMP_LIBRARY_SUFFIX}@CMAKE_SHARED_LIBRARY_SUFFIX@.@ASSIMP_VERSION_MAJOR@']
            to_texts = [r'libassimp${ASSIMP_LIBRARY_SUFFIX}.@ASSIMP_VERSION_MAJOR@@CMAKE_SHARED_LIBRARY_SUFFIX@']
            bak_file_3 = patch_file(orig_file_3, from_texts=from_texts, to_texts=to_texts)

            orig_file_4 = os.path.join(src_dir, 'assimpTargets-debug.cmake.in')
            from_texts = [r'libassimp${ASSIMP_LIBRARY_SUFFIX}@CMAKE_DEBUG_POSTFIX@@CMAKE_SHARED_LIBRARY_SUFFIX@.@ASSIMP_VERSION_MAJOR@']
            to_texts = [r'libassimp${ASSIMP_LIBRARY_SUFFIX}@CMAKE_DEBUG_POSTFIX@.@ASSIMP_VERSION_MAJOR@@CMAKE_SHARED_LIBRARY_SUFFIX@']
            bak_file_4 = patch_file(orig_file_4, from_texts=from_texts, to_texts=to_texts)

        cmakecmd = get_cmake_cmd_common_part(install_dir)
        cmakecmd.extend(['-DASSIMP_BUILD_ASSIMP_TOOLS:BOOL=OFF',
                         '-DASSIMP_BUILD_TESTS:BOOL=OFF'])

        if is_windows():
            cmakecmd.extend(['-DZLIB_INCLUDE_DIR:PATH=' + ext_dir() + '\\zlib\\include',
                             '-DZLIB_LIBRARY_REL:FILEPATH=' + ext_dir() + '\\zlib\\lib\\zlibstatic.lib'])

        cmakecmd.extend([src_dir])
        build_and_install_cmakecmd(cmakecmd, build_dir)
    finally:
        os.replace(bak_file, orig_file)
        if is_mac():
            os.replace(bak_file_3, orig_file_3)
            os.replace(bak_file_4, orig_file_4)
        shutil.rmtree(build_dir, ignore_errors=False)


def build_hdf5(src_dir: str, install_dir: str):
    build_dir = create_build_dir(src_dir)
    shutil.rmtree(install_dir, ignore_errors=True)

    try:
        cmakecmd = get_cmake_cmd_common_part(install_dir)
        cmakecmd.extend(['-DBUILD_TESTING:BOOL=OFF',
                         '-DBUILD_SHARED_LIBS:BOOL=OFF',
                         '-DHDF5_ENABLE_DEPRECATED_SYMBOLS:BOOL=OFF',
                         '-DHDF5_ENABLE_Z_LIB_SUPPORT:BOOL=ON',
                         '-DHDF5_ENABLE_THREADSAFE:BOOL=OFF',
                         '-DHDF5_BUILD_EXAMPLES:BOOL=OFF'])

        if is_windows():
            cmakecmd.extend(['-DZLIB_INCLUDE_DIR:PATH=' + ext_dir() + '\\zlib\\include',
                             '-DZLIB_LIBRARY_RELEASE:FILEPATH=' + ext_dir() + '\\zlib\\lib\\zlibstatic.lib'])

        cmakecmd.extend([src_dir])
        build_and_install_cmakecmd(cmakecmd, build_dir)
    finally:
        shutil.rmtree(build_dir, ignore_errors=False)


def build_freeimage(src_dir: str, install_dir: str):
    shutil.rmtree(install_dir, ignore_errors=True)

    orig_file = None
    bak_file = None
    orig_file_3 = None
    bak_file_3 = None
    orig_file_4 = None
    bak_file_4 = None
    try:
        orig_file = os.path.join(src_dir, 'Source', 'LibRawLite', 'internal', 'dcraw_common.cpp')
        from_texts = [r'DCRAW_VERSION']
        to_texts = [r' DCRAW_VERSION']
        bak_file = patch_file(orig_file, from_texts=from_texts, to_texts=to_texts)

        if is_windows():
            env = get_vcvars_environment()
            subprocess.run(['MSBuild', 'FreeImage.2017.sln', '/target:FreeImagePlus', '/property:Platform=x64',
                            '/property:Configuration=Release', '/maxcpucount',
                            '/property:WindowsTargetPlatformVersion=' + env['UCRTVERSION']  # like 10.0.16299.0
                            ],
                           cwd=src_dir, shell=True, check=True, env=env)
            distutils.dir_util.copy_tree(os.path.join(src_dir, 'Dist', 'x64'),
                                         install_dir)
            distutils.dir_util.copy_tree(os.path.join(src_dir, 'Wrapper', 'FreeImagePlus', 'dist', 'x64'),
                                         install_dir)
        elif is_linux():
            orig_file_3 = os.path.join(src_dir, 'Makefile.gnu')
            from_texts = [r'INCDIR ?= $(DESTDIR)/usr/include',
                          r'INSTALLDIR ?= $(DESTDIR)/usr/lib',
                          r' -o root -g root ']
            to_texts = [r'INCDIR ?= $(DESTDIR)$(PREFIX)/include',
                        r'INSTALLDIR ?= $(DESTDIR)$(PREFIX)/lib',
                        r' ']
            bak_file_3 = patch_file(orig_file_3, from_texts=from_texts, to_texts=to_texts)

            orig_file_4 = os.path.join(src_dir, 'Makefile.fip')
            bak_file_4 = patch_file(orig_file_4, from_texts=from_texts, to_texts=to_texts)

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
            shutil.copy2(os.path.join(ext_dir(), 'freeimage-makefiles', 'Makefile_gnu'), src_dir)
            shutil.copy2(os.path.join(ext_dir(), 'freeimage-makefiles', 'Makefile_fip'), src_dir)
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
        if is_mac():
            os.remove(os.path.join(src_dir, 'Makefile_gnu'))
            os.remove(os.path.join(src_dir, 'Makefile_fip'))
        elif is_linux():
            os.replace(bak_file_3, orig_file_3)
            os.replace(bak_file_4, orig_file_4)


def build_botan(src_dir: str, install_dir: str):
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


def build_itk(src_dir: str, install_dir: str):
    build_dir = create_build_dir(src_dir)
    shutil.rmtree(install_dir, ignore_errors=True)

    orig_file = None
    bak_file = None
    orig_file_1 = None
    bak_file_1 = None
    try:
        orig_file = os.path.join(src_dir, 'Modules', 'ThirdParty', 'MetaIO', 'src', 'MetaIO', 'src', 'CMakeLists.txt')
        bak_file = patch_file(orig_file, from_texts=[r'install(FILES ${headers}'],
                              to_texts=['file(GLOB __files "${CMAKE_CURRENT_SOURCE_DIR}/*.h")\n'
                                        'set(headers ${headers} ${__files})\n'
                                        'install(FILES ${headers}'])

        orig_file_1 = os.path.join(src_dir, 'Modules', 'ThirdParty', 'VNL', 'src', 'vxl', 'vcl', 'CMakeLists.txt')
        if is_windows():
            bak_file_1 = patch_file(orig_file_1, from_texts=[r'vcl_legacy_aliases.h'],
                                    to_texts=[r'vcl_legacy_aliases.h vcl_msvc_warnings.h'])

        cmakecmd = get_cmake_cmd_common_part(install_dir)
        cmakecmd.extend(['-DBUILD_EXAMPLES:BOOL=OFF',
                         '-DBUILD_TESTING:BOOL=OFF',
                         '-DITK_USE_64BITS_IDS:BOOL=ON',
                         '-DITK_FUTURE_LEGACY_REMOVE:BOOL=ON',
                         '-DITK_LEGACY_REMOVE:BOOL=ON',
                         '-DITK_USE_GPU:BOOL=OFF',
                         '-DITK_DOXYGEN_HTML:BOOL=OFF',
                         '-DModule_ITKReview:BOOL=ON',
                         '-DITK_USE_SYSTEM_ZLIB:BOOL=ON',
                         '-DModule_ITKTBB:BOOL=ON',
                         '-DTBB_DIR:PATH=' + atlas_repository_dir() + '/src/cmake'])

        if is_windows():
            cmakecmd.extend(['-DZLIB_INCLUDE_DIR:PATH=' + ext_dir() + '\\zlib\\include',
                             '-DZLIB_LIBRARY_RELEASE:FILEPATH=' + ext_dir() + '\\zlib\\lib\\zlibstatic.lib'])

        cmakecmd.extend([src_dir])
        build_and_install_cmakecmd(cmakecmd, build_dir)

        # duplicated call to find_package cause cmake error
        # remove tbb from itk interface to make it work with conda tbb
        orig_file_2 = os.path.join(install_dir, 'lib', 'cmake', 'ITK-5.0', 'Modules', 'ITKTBB.cmake')
        patch_file(orig_file_2,
                   from_texts=[r'find_package(TBB REQUIRED CONFIG)',
                               r'set(ITKTBB_INCLUDE_DIRS',
                               r'set(ITKTBB_LIBRARIES',
                               r'set(TBB_DIR'],
                   to_texts=[r'#find_package(TBB REQUIRED CONFIG)',
                             r'#set(ITKTBB_INCLUDE_DIRS',
                             r'#set(ITKTBB_LIBRARIES',
                             r'#set(TBB_DIR'])
    finally:
        shutil.rmtree(build_dir, ignore_errors=False)
        os.replace(bak_file, orig_file)
        if is_windows():
            os.replace(bak_file_1, orig_file_1)


def build_vtk(src_dir: str, install_dir: str):
    build_dir = create_build_dir(src_dir)
    shutil.rmtree(install_dir, ignore_errors=True)

    try:
        cmakecmd = get_cmake_cmd_common_part(install_dir)
        cmakecmd.extend(['-DVTK_BUILD_EXAMPLES:BOOL=OFF',
                         '-DBUILD_TESTING:BOOL=OFF',
                         '-DBUILD_SHARED_LIBS:BOOL=OFF',
                         '-DVTK_BUILD_TESTING:STRING=OFF',
                         '-DVTK_DATA_EXCLUDE_FROM_ALL:BOOL=OFF',
                         '-DBUILD_SHARED_LIBS:BOOL=OFF',
                         '-DVTK_MODULE_USE_EXTERNAL_VTK_zlib:BOOL=ON',
                         '-DVTK_LEGACY_REMOVE:BOOL=ON'])

        if is_windows():
            cmakecmd.extend(['-DVTK_MODULE_USE_EXTERNAL_VTK_libxml2:BOOL=OFF',
                             '-DZLIB_INCLUDE_DIR:PATH=' + ext_dir() + '\\zlib\\include',
                             '-DZLIB_LIBRARY_RELEASE:FILEPATH=' + ext_dir() + '\\zlib\\lib\\zlibstatic.lib'])
        else:
            if is_mac():
                cmakecmd.extend(['-DVTK_MODULE_USE_EXTERNAL_VTK_libxml2:BOOL=ON'])
            else:
                cmakecmd.extend(['-DVTK_MODULE_USE_EXTERNAL_VTK_libxml2:BOOL=OFF'])

        cmakecmd.extend([src_dir])
        build_and_install_cmakecmd(cmakecmd, build_dir)
    finally:
        shutil.rmtree(build_dir, ignore_errors=False)


def build_opencv(src_dir: str, src_contrib_dir: str, install_dir: str):
    build_dir = create_build_dir(src_dir)
    shutil.rmtree(install_dir, ignore_errors=True)

    orig_file_3 = None
    bak_file_3 = None
    try:
        cmakecmd = get_cmake_cmd_common_part(install_dir)
        cmakecmd.extend(['-DBUILD_opencv_videoio:BOOL=OFF',
                         '-DBUILD_SHARED_LIBS:BOOL=OFF',
                         '-DBUILD_PROTOBUF:BOOL=OFF',
                         '-DBUILD_opencv_python2:BOOL=OFF',
                         '-DBUILD_opencv_videostab:BOOL=ON',
                         '-DBUILD_opencv_hdf:BOOL=OFF',
                         '-DBUILD_opencv_sfm:BOOL=OFF',
                         '-DBUILD_opencv_ts:BOOL=OFF',
                         '-DBUILD_opencv_xfeatures2d:BOOL=OFF',
                         '-DBUILD_opencv_dnn:BOOL=OFF',
                         '-DBUILD_opencv_dnn_objdetect:BOOL=OFF',
                         '-DBUILD_opencv_world:BOOL=OFF',
                         '-DBUILD_opencv_python2:BOOL=OFF',
                         '-DBUILD_opencv_python3:BOOL=OFF',
                         '-DBUILD_opencv_java:BOOL=OFF',
                         '-DWITH_TBB:BOOL=ON',
                         '-DWITH_LAPACK:BOOL=OFF',
                         '-DWITH_VTK:BOOL=OFF',
                         '-DBUILD_PERF_TESTS:BOOL=OFF',
                         '-DWITH_PNG:BOOL=OFF',
                         '-DWITH_MATLAB:BOOL=OFF',
                         '-DWITH_FFMPEG:BOOL=OFF',
                         '-DWITH_1394:BOOL=OFF',
                         '-DWITH_GSTREAMER:BOOL=OFF',
                         '-DBUILD_DOCS:BOOL=OFF',
                         '-DBUILD_PNG:BOOL=OFF',
                         '-DWITH_GPHOTO2:BOOL=OFF',
                         '-DBUILD_ZLIB:BOOL=OFF',
                         '-DWITH_CUDA:BOOL=OFF',
                         '-DWITH_CUFFT:BOOL=OFF',
                         '-DWITH_OPENCL:BOOL=OFF',
                         '-DWITH_PVAPI:BOOL=OFF',
                         '-DBUILD_JASPER:BOOL=OFF',
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
                         '-DBUILD_JAVA:BOOL=OFF',
                         '-DBUILD_OPENEXR:BOOL=OFF',
                         '-DWITH_CUFFT:BOOL=ON',
                         '-DWITH_JPEG:BOOL=OFF',
                         '-DWITH_OPENEXR:BOOL=OFF',
                         '-DBUILD_PACKAGE:BOOL=OFF',
                         '-DWITH_JASPER:BOOL=OFF',
                         '-DBUILD_WITH_DEBUG_INFO:BOOL=OFF',
                         '-DBUILD_opencv_apps:BOOL=OFF',
                         '-DBUILD_opencv_matlab:BOOL=OFF',
                         '-DENABLE_PRECOMPILED_HEADERS:BOOL=OFF'])

        if is_windows():
            cmakecmd.extend(['-DBUILD_WITH_STATIC_CRT:BOOL=OFF',
                             '-DEIGEN_INCLUDE_PATH:PATH=' + ext_dir() + '\\eigen',
                             '-DWITH_WIN32UI:BOOL=OFF',
                             '-DOPENCV_EXTRA_MODULES_PATH:PATH=' + src_contrib_dir + '\\modules',
                             '-DZLIB_INCLUDE_DIR:PATH=' + ext_dir() + '\\zlib\\include',
                             '-DZLIB_LIBRARY_RELEASE:FILEPATH=' + ext_dir() + '\\zlib\\lib\\zlibstatic.lib'])
        else:
            cmakecmd.extend(['-DWITH_PTHREADS_PF:BOOL=OFF',
                             '-DEIGEN_INCLUDE_PATH:PATH=' + ext_dir() + '/eigen',
                             '-DWITH_QUICKTIME:BOOL=OFF',
                             '-DOPENCV_EXTRA_MODULES_PATH:PATH=' + src_contrib_dir + '/modules'])

            if is_linux():
                orig_file_3 = os.path.join(src_dir, 'modules', 'core', 'include', 'opencv2', 'core', 'private.hpp')
                bak_file_3 = patch_file(orig_file_3,
                                        from_texts=[r'#define CV_INSTRUMENT_FUN_IPP(FUN, ...) ((FUN)(__VA_ARGS__))'],
                                        to_texts=[r'#define CV_INSTRUMENT_FUN_IPP(FUN, ...) (FUN(__VA_ARGS__))'])

        env = get_tbb_env()
        print('TBBROOT:', env['TBBROOT'])
        cmakecmd.extend([src_dir])
        build_and_install_cmakecmd(cmakecmd, build_dir, env=env)

        if is_windows():
            orig_file_2 = os.path.join(install_dir, 'x64', 'vc15', 'staticlib', 'OpenCVModules.cmake')
        else:
            orig_file_2 = os.path.join(install_dir, 'lib', 'cmake', 'opencv4', 'OpenCVModules.cmake')
        patch_file(orig_file_2,
                   from_texts=[r';\$<LINK_ONLY:tbb>', r'\$<LINK_ONLY:tbb>;'],
                   to_texts=[r'', r''])
    finally:
        if is_linux():
            os.replace(bak_file_3, orig_file_3)
        shutil.rmtree(build_dir, ignore_errors=False)


def build_libs(libs: dict, update_src: bool):
    print('extDIR:', ext_dir())
    print('srcPackageDIR:', src_package_dir())
    print('baseDIR:', base_dir())

    remove_path_contains('miniconda')
    remove_path_contains('anaconda')
    print('PATH:', os.environ['PATH'])

    if is_windows():
        os.environ['HOME'] = os.path.expanduser("~")

    print('HOME:', os.environ['HOME'])

    if libs['cmake']:
        install_cmake()

    if libs['ninja']:
        install_ninja()

    if libs['curl']:
        if is_windows():
            unpack_tool_to_software_dir(src_package_dir(), 'curl*win*')

    if libs['tbb']:
        subprocess.run([get_cmake_binary(), '-P', 'MakeTBBConfigFiles.cmake'],
                       cwd=os.path.join(atlas_repository_dir(), 'src', 'cmake'), shell=False, check=True)

    if libs['qt']:
        print(f'Qt {qt_ver()} in {qt_base_dir()}')
        with open(os.path.join(atlas_src_dir(), 'cmake', 'QtInfo.cmake'), mode='w', encoding='utf-8') as file:
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
        # patch installer framework
        pattern_bytes = b'Mozilla/5.0'
        replace_bytes = b'Mozilla/590'
        file = os.path.join(qt_installer_framework_bin_dir(), 'installerbase')
        if is_windows():
            file += '.exe'
        with open(file, 'r+b') as f:
            with mmap.mmap(f.fileno(), 0) as mm:
                allIndexes = []
                index = mm.find(pattern_bytes)
                while index != -1:
                    allIndexes.append(index)
                    index = mm.find(pattern_bytes, index + len(pattern_bytes))
                if not allIndexes:
                    print("Pattern not found in {0}, skip patching.".format(file))
                else:
                    # make backup
                    shutil.copy2(file, file + '.bak')
                    for index in allIndexes:
                        mm[index:index + len(replace_bytes)] = replace_bytes
                    mm.flush()
                    print("{0} successfully patched at {1} places.".format(file, len(allIndexes)))

    if libs['zlib']:
        if is_windows():
            package_name = find_src_package_with_glob(os.path.join(src_package_dir(), 'zlib*'))
            src_dir = get_package_top_level_folder(package_name, base_dir())
            if update_src:
                shutil.rmtree(src_dir, ignore_errors=True)
                unpack_file_to_folder(package_name, base_dir())
            assert os.path.exists(src_dir)
            build_zlib(src_dir, os.path.join(ext_dir(), 'zlib'))

    if libs['ffmpeg']:
        if is_windows():
            package_name = find_src_package_with_glob(os.path.join(src_package_dir(), 'ffmpeg*win*'))
            package_unpack_folder = get_package_top_level_folder(package_name, ext_dir())
            unpack_file_to_folder(package_name, ext_dir())
            os.replace(os.path.join(package_unpack_folder, 'bin', 'ffmpeg.exe'),
                       os.path.join(ext_dir(), 'ffmpeg.exe'))
            shutil.rmtree(package_unpack_folder, ignore_errors=False)
        elif is_linux():
            package_name = find_src_package_with_glob(os.path.join(src_package_dir(), 'ffmpeg*static.tar.xz'))
            package_unpack_folder = get_package_top_level_folder(package_name, ext_dir())
            unpack_file_to_folder(package_name, ext_dir())
            os.replace(os.path.join(package_unpack_folder, 'ffmpeg'),
                       os.path.join(ext_dir(), 'ffmpeg'))
            shutil.rmtree(package_unpack_folder, ignore_errors=False)
        else:
            # unpack_file_to_folder(find_src_package_with_glob(os.path.join(src_package_dir, 'ffmpeg*7z')),
            #                       ext_dir)
            package_name = find_src_package_with_glob(os.path.join(src_package_dir(), 'ffmpeg*macos*'))
            package_unpack_folder = get_package_top_level_folder(package_name, ext_dir())
            unpack_file_to_folder(package_name, ext_dir())
            os.replace(os.path.join(package_unpack_folder, 'bin', 'ffmpeg'),
                       os.path.join(ext_dir(), 'ffmpeg'))
            shutil.rmtree(package_unpack_folder, ignore_errors=False)
            os.chmod(os.path.join(ext_dir(), 'ffmpeg'), stat.S_IRWXU | stat.S_IRWXG | stat.S_IRWXO)

    if libs['boost']:
        package_name = find_src_package_with_glob(os.path.join(src_package_dir(), 'boost*'))
        src_dir = get_package_top_level_folder(package_name, ext_dir())
        shutil.rmtree(os.path.join(ext_dir(), 'boost'), ignore_errors=True)
        unpack_file_to_folder(package_name, ext_dir())
        os.rename(src_dir, os.path.join(ext_dir(), 'boost'))

    if libs['eigen']:
        src_dir = os.path.join(base_dir(), 'eigen')
        update_or_clone_git_repository(src_dir, 'git@github.com:eigenteam/eigen-git-mirror.git')
        export_git_repository(src_dir, os.path.join(ext_dir(), 'eigen'))

    if libs['pybind11']:
        src_dir = os.path.join(base_dir(), 'pybind11')
        update_or_clone_git_repository(src_dir, 'git@github.com:pybind/pybind11.git')
        export_git_repository(src_dir, os.path.join(ext_dir(), 'pybind11'))

    if libs['glm']:
        src_dir = os.path.join(base_dir(), 'glm')
        update_or_clone_git_repository(src_dir, 'git@github.com:g-truc/glm.git')
        export_git_repository(src_dir, os.path.join(ext_dir(), 'glm'))

    if libs['googletest']:
        src_dir = os.path.join(base_dir(), 'googletest')
        update_or_clone_git_repository(src_dir, 'git@github.com:google/googletest.git')
        shutil.rmtree(os.path.join(ext_dir(), 'googletest'), ignore_errors=True)
        shutil.copytree(os.path.join(src_dir, 'googletest'), os.path.join(ext_dir(), 'googletest'))

    if libs['folly']:
        src_dir = os.path.join(base_dir(), 'folly')
        update_or_clone_git_repository(src_dir, 'git@github.com:facebook/folly.git')
        export_git_repository(src_dir, os.path.join(ext_dir(), 'folly'))
        build_folly(src_dir, os.path.join(ext_dir(), 'folly'))

    if libs['cpuinfo']:
        src_dir = os.path.join(base_dir(), 'cpuinfo')
        if update_src:
            update_or_clone_git_repository(src_dir, 'git@github.com:Maratyszcza/cpuinfo.git')
        assert os.path.exists(src_dir)
        build_cpuinfo(src_dir, os.path.join(ext_dir(), 'cpuinfo'))

    if libs['gflags']:
        gflags_src_dir = os.path.join(base_dir(), 'gflags')
        if update_src:
            update_or_clone_git_repository(gflags_src_dir, 'git@github.com:gflags/gflags.git')
        assert os.path.exists(gflags_src_dir)
        build_gflags(gflags_src_dir, os.path.join(ext_dir(), 'gflags'))

    if libs['glog']:
        src_dir = os.path.join(base_dir(), 'glog')
        if update_src:
            update_or_clone_git_repository(src_dir, 'git@github.com:google/glog.git')
        assert os.path.exists(src_dir)
        build_glog(src_dir, os.path.join(ext_dir(), 'glog'))

    if libs['benchmark']:
        src_dir = os.path.join(base_dir(), 'benchmark')
        if update_src:
            update_or_clone_git_repository(src_dir, 'git@github.com:google/benchmark.git')
        assert os.path.exists(src_dir)
        build_benchmark(src_dir, os.path.join(ext_dir(), 'benchmark'))

    if libs['grpc']:
        src_dir = os.path.join(base_dir(), 'grpc')
        if update_src:
            update_or_clone_git_repository_with_submodules(src_dir, 'git@github.com:grpc/grpc.git')
        assert os.path.exists(src_dir)
        if is_windows():
            nasm_dir = unpack_tool_to_software_dir(src_package_dir(), 'nasm*win64*', 'nasm-*')
        else:
            nasm_dir = ''  # does not need
        build_grpc(src_dir, os.path.join(ext_dir(), 'grpc'), nasm_dir=nasm_dir)

    if libs['glbinding']:
        src_dir = os.path.join(base_dir(), 'glbinding')
        if update_src:
            update_or_clone_git_repository(src_dir, 'git@github.com:cginternals/glbinding.git')
        assert os.path.exists(src_dir)
        build_glbinding(src_dir, os.path.join(ext_dir(), 'glbinding'))

    if libs['libjpeg']:
        if is_windows():
            nasm_dir = unpack_tool_to_software_dir(src_package_dir(), 'nasm*win64*', 'nasm-*')
        elif is_mac():
            nasm_dir = unpack_tool_to_software_dir(src_package_dir(), 'nasm*macosx*', 'nasm-*')
            os.chown(os.path.join(nasm_dir, 'nasm'), os.getuid(), os.getgid())
            os.chmod(os.path.join(nasm_dir, 'nasm'), os.stat(os.path.join(nasm_dir, 'nasm')).st_mode | stat.S_IXUSR)
        else:
            nasm_dir = ''
        package_name = find_src_package_with_glob(os.path.join(src_package_dir(), 'libjpeg*'))
        src_dir = get_package_top_level_folder(package_name, base_dir())
        if update_src:
            shutil.rmtree(src_dir, ignore_errors=True)
            unpack_file_to_folder(package_name, base_dir())
        assert os.path.exists(src_dir)
        build_libjpeg(src_dir, os.path.join(ext_dir(), 'libjpeg-turbo'), nasm_dir=nasm_dir)

    if libs['libpng']:
        package_name = find_src_package_with_glob(os.path.join(src_package_dir(), 'libpng*'))
        src_dir = get_package_top_level_folder(package_name, base_dir())
        if update_src:
            shutil.rmtree(src_dir, ignore_errors=True)
            unpack_file_to_folder(package_name, base_dir())
        assert os.path.exists(src_dir)
        build_libpng(src_dir, os.path.join(ext_dir(), 'libpng'))

    if libs['jxrlib']:
        src_dir = os.path.join(base_dir(), 'jxrlib')
        if update_src:
            update_or_clone_git_repository(src_dir, 'git@github.com:4creators/jxrlib.git')
        assert os.path.exists(src_dir)
        build_jxrlib(src_dir, os.path.join(ext_dir(), 'jxrlib'))

    if libs['geometrictools']:
        package_name = find_src_package_with_glob(os.path.join(src_package_dir(), 'GeometricToolsEngine*'))
        src_dir = get_package_top_level_folder(package_name, base_dir())
        if update_src:
            shutil.rmtree(src_dir, ignore_errors=True)
            unpack_file_to_folder(package_name, base_dir())
        assert os.path.exists(src_dir)
        build_geometrictools(src_dir, os.path.join(ext_dir(), 'geometrictools'))

    if libs['assimp']:
        src_dir = os.path.join(base_dir(), 'assimp')
        if update_src:
            update_or_clone_git_repository(src_dir, 'git@github.com:assimp/assimp.git')
        assert os.path.exists(src_dir)
        build_assimp(src_dir, os.path.join(ext_dir(), 'assimp'))

    if libs['hdf5']:
        package_name = find_src_package_with_glob(os.path.join(src_package_dir(), 'hdf5*'))
        src_dir = get_package_top_level_folder(package_name, base_dir())
        if update_src:
            shutil.rmtree(src_dir, ignore_errors=True)
            unpack_file_to_folder(package_name, base_dir())
        assert os.path.exists(src_dir)
        build_hdf5(src_dir, os.path.join(ext_dir(), 'hdf5'))

    if libs['freeimage']:
        package_name = find_src_package_with_glob(os.path.join(src_package_dir(), 'FreeImage*'))
        src_dir = get_package_top_level_folder(package_name, base_dir())
        if update_src:
            shutil.rmtree(src_dir, ignore_errors=True)
            unpack_file_to_folder(package_name, base_dir())
        assert os.path.exists(src_dir)
        build_freeimage(src_dir, os.path.join(ext_dir(), 'freeimage'))

    if libs['itk']:
        src_dir = os.path.join(base_dir(), 'ITK')
        if update_src:
            update_or_clone_git_repository(src_dir, 'git://itk.org/ITK.git')
        assert os.path.exists(src_dir)
        build_itk(src_dir, os.path.join(ext_dir(), 'itk'))

    if libs['vtk']:
        src_dir = os.path.join(base_dir(), 'VTK')
        if update_src:
            update_or_clone_git_repository(src_dir, 'https://gitlab.kitware.com/vtk/vtk.git')
        assert os.path.exists(src_dir)
        build_vtk(src_dir, os.path.join(ext_dir(), 'vtk'))

    if libs['opencv']:
        src_dir = os.path.join(base_dir(), 'opencv')
        src_contrib_dir = os.path.join(base_dir(), 'opencv_contrib')
        if update_src:
            update_or_clone_git_repository(src_dir, 'git@github.com:Itseez/opencv.git')
            update_or_clone_git_repository(src_contrib_dir, 'git@github.com:Itseez/opencv_contrib.git')
        assert os.path.exists(src_dir)
        assert os.path.exists(src_contrib_dir)
        build_opencv(src_dir, src_contrib_dir, os.path.join(ext_dir(), 'opencv'))

    if libs['botan']:
        src_dir = os.path.join(base_dir(), 'botan')
        if update_src:
            update_or_clone_git_repository(src_dir, 'git@github.com:randombit/botan.git')
        assert os.path.exists(src_dir)
        build_botan(src_dir, os.path.join(ext_dir(), 'botan'))

    if libs['ospray']:
        if is_windows():
            ispc_dir = unpack_tool_to_software_dir(src_package_dir(), 'ispc*win*')
            embree_dir = unpack_tool_to_software_dir(src_package_dir(), 'embree*win*')
        elif is_linux():
            ispc_dir = unpack_tool_to_software_dir(src_package_dir(), 'ispc*linux*')
            embree_dir = unpack_tool_to_software_dir(src_package_dir(), 'embree*linux*')
        else:
            ispc_dir = unpack_tool_to_software_dir(src_package_dir(), 'ispc*osx*')
            embree_dir = unpack_tool_to_software_dir(src_package_dir(), 'embree*osx*')
        src_dir = os.path.join(base_dir(), 'OSPRay')
        if update_src:
            update_or_clone_git_repository(src_dir, 'git@github.com:ospray/OSPRay.git')
        assert os.path.exists(src_dir)
        assert os.path.exists(ispc_dir)
        assert os.path.exists(embree_dir)
        build_ospray(src_dir, os.path.join(ext_dir(), 'ospray'), ispc_dir=ispc_dir, embree_dir=embree_dir)


def parse_inputs(argv: list):
    libs = {'cmake': True,
            'ninja': True,
            'curl': False,
            'tbb': False,
            'qt': False,
            'zlib': False,
            'ffmpeg': False,
            'boost': False,
            'eigen': False,
            'pybind11': False,
            'glm': False,
            'googletest': False,
            'folly': False,
            'cpuinfo': False,
            'gflags': False,
            'glog': False,
            'benchmark': False,
            'grpc': False,
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
                            'zlib': ['libpng', 'assimp', 'hdf5', 'itk', 'vtk', 'opencv', 'grpc'],
                            'gflags': ['glog', 'grpc'],
                            'benchmark': ['grpc']
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
    libs['ninja'] = True
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

    # not used now
    libs['botan'] = False
    libs['ospray'] = False

    return libs, update_src


if __name__ == "__main__":
    build_libs(*parse_inputs(sys.argv))
