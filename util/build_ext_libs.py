import argparse
import difflib
import json
import mmap
import os.path
from collections import OrderedDict
from pathlib import Path

from common_dirs import *


def macos_min_version():
    return '11.0'


def cpp_standard() -> int:
    return 20


def use_clang_in_linux() -> bool:
    return is_linux()


def get_clang_in_linux() -> str:
    return 'clang-18'


def get_clangplus_in_linux() -> str:
    return 'clang++-18'


def update_or_clone_git_repository(repository_folder: str, repository_url: str):
    if os.path.exists(repository_folder):
        print('git', 'pull', Path(repository_folder).name)
        subprocess.run(['git', 'pull'], cwd=repository_folder, shell=False, check=False)
    else:
        subprocess.run(['git', 'clone', repository_url, repository_folder], shell=False, check=True)


def update_or_clone_git_repository_with_submodules(repository_folder: str, repository_url: str):
    if os.path.exists(repository_folder):
        print(f'git pull {Path(repository_folder).name}')
        subprocess.run(['git', 'pull'], cwd=repository_folder, shell=False, check=False)
        subprocess.run(['git', 'submodule', 'update', '--init', '--recursive'],
                       cwd=repository_folder, shell=False, check=False)
    else:
        subprocess.run(['git', 'clone', '--recursive', repository_url, repository_folder],
                       shell=False, check=True)


# do not use, might cause error when delete the .git folder
# def export_git_repository(repository_folder: str, target_folder: str, branch: str = '', tag: str = ''):
#     if not branch:
#         branch = 'master'
#     shutil.rmtree(target_folder, ignore_errors=True)
#     subprocess.run(['git', 'clone', '--shared', '--branch', branch, repository_folder, target_folder],
#                    shell=False, check=True)
#     if tag:
#         subprocess.run(['git', 'checkout', tag], cwd=target_folder, shell=False, check=True)
#     shutil.rmtree(os.path.join(target_folder, '.git'), ignore_errors=False)


def update_git_submodule(target_folder: str, tag: str = None):
    assert os.path.exists(target_folder)
    print('update', 'submodule', Path(target_folder).name)
    subprocess.run(['git', 'submodule', 'update', '--init', '--remote', '--',
                    f'src/3rdparty/{Path(target_folder).name}'],
                   cwd=atlas_repository_dir(), shell=False, check=True)
    if tag is not None:
        subprocess.run(['git', 'checkout', tag], cwd=target_folder, shell=False, check=True)


def cleanup_git_submodule(target_folder: str):
    assert os.path.isfile(os.path.join(target_folder, '.git')), f'{target_folder} is not git submodule'
    subprocess.run(['git', 'reset', '--hard'],
                   cwd=target_folder, shell=False, check=True)
    subprocess.run(['git', 'clean', '-dff'],
                   cwd=target_folder, shell=False, check=True)


def create_build_dir(src_dir: str):
    build_dir = os.path.normpath(os.path.join(ext_build_dir(), '__' + Path(src_dir).name))
    if src_dir.endswith('ITK'):
        build_dir = os.path.normpath(os.path.join(ext_build_dir(), '_I'))  # ITK windows build dir length limit
    if os.path.exists(build_dir):
        shutil.rmtree(build_dir, ignore_errors=False, onerror=handleRemoveReadonly)
    os.mkdir(build_dir)
    return build_dir


def create_arm64_install_dir(src_dir: str):
    install_dir = os.path.normpath(os.path.join(ext_build_dir(), '__arm64_' + Path(src_dir).name))
    if os.path.exists(install_dir):
        shutil.rmtree(install_dir, ignore_errors=False, onerror=handleRemoveReadonly)
    os.mkdir(install_dir)
    return install_dir


def create_universal_binaries(arm64_install_dir, final_install_dir, remove_dylib: bool = False):
    assert is_mac()
    print(f'{arm64_install_dir} to {final_install_dir}')
    for root, dirs, files in os.walk(arm64_install_dir):
        for name in files:
            filename = os.path.join(root, name)
            if Path(filename).is_symlink():
                if remove_dylib and filename.endswith('.dylib'):
                    target_filename = filename.replace(arm64_install_dir, final_install_dir)
                    print(f'deleting {target_filename}')
                    os.unlink(target_filename)
                continue
            if filename.endswith('.a') or filename.endswith('.dylib') or root.endswith('bin'):
                if name.endswith('.sh'):  # text file
                    continue
                if name == 'c_rehash':  # text file
                    continue
                if name == 'libpng16-config':
                    continue
                if name == 'xzdiff' or name == 'xzgrep' or name == 'xzless' or name == 'xzmore':  # text file
                    continue
                target_filename = filename.replace(arm64_install_dir, final_install_dir)
                if name.startswith('libtegra_hal.a'):
                    print(f'copy {filename} to {target_filename}')
                    shutil.copyfile(filename, target_filename)
                    continue
                if remove_dylib and filename.endswith('.dylib'):
                    print(f'deleting {target_filename}')
                    os.unlink(target_filename)
                elif not os.path.exists(target_filename):
                    print(f'copy {filename} to {target_filename}')
                    shutil.copyfile(filename, target_filename)
                else:
                    print(f'merge {filename} to {target_filename}')
                    subprocess.run(['lipo', '-create', filename, target_filename, '-output', target_filename],
                                   shell=False, check=True)


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


def glob_remove(files: str):
    for file in glob.iglob(files):
        if os.path.isdir(file):
            shutil.rmtree(file, ignore_errors=True)
        else:
            os.remove(file)
        print(f'{file} removed')


def remove_installed_dynamic_library(install_dir: str, libnames: list):
    for libname in libnames:
        if is_linux():
            glob_remove(os.path.join(install_dir, 'lib', f'lib{libname}.so*'))
        elif is_windows():
            filename = os.path.join(install_dir, 'lib', f'{libname}.lib')
            if os.path.exists(filename):
                print(f'deleting {filename}')
                os.unlink(filename)
            filename = os.path.join(install_dir, 'bin', f'{libname}.dll')
            if os.path.exists(filename):
                print(f'deleting {filename}')
                os.unlink(filename)
        else:
            glob_remove(os.path.join(install_dir, 'lib', f'lib{libname}.*dylib'))


def get_vcvars_environment(remove_conda_from_path: bool = True,
                           remove_scoop_from_path: bool = True):
    """
    Returns a dictionary containing the environment variables set up by vcvarsall.bat amd64
    """

    vcvars = os.path.normpath(os.path.join(vs_install_dir(), 'VC', 'Auxiliary', 'Build', 'vcvarsall.bat'))
    return get_enviroment_from_shell_script(vcvars, 'x64', remove_conda_from_path=remove_conda_from_path,
                                            remove_scoop_from_path=remove_scoop_from_path)


def get_enviroment_from_shell_script(script: str, para: str = '', start_env=os.environ,
                                     remove_conda_from_path: bool = True,
                                     remove_scoop_from_path: bool = True):
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
    # print(stdout.strip())
    env = json.loads(stdout.strip())
    if remove_conda_from_path:
        remove_path_contains('miniconda', env)
        remove_path_contains('anaconda', env)
    if remove_scoop_from_path:
        remove_path_contains('scoop', env)
    remove_path_contains('mingw', env)
    env['PATH'] += r';C:\Program Files\LLVM\bin'
    return env


def get_tbb_env():
    env = {}
    env['TBBROOT'] = ext_build_dir() if is_mac() else os.path.join(intel_sw_dir(), 'tbb', 'latest')
    env['TBB_ROOT'] = env['TBBROOT']
    return env


def get_common_build_flags(cpp_standard: int = cpp_standard(), with_optimization: bool = False, universal: bool = False,
                           arm64_only: bool = False, no_hidden_visibility: bool = False):
    res = {}
    optimization = ' -O3' if with_optimization else ''
    if is_mac():
        osx_sysroot = r'/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk'
        assert os.path.exists(osx_sysroot)

        arch_flag = ' -mavx '
        if universal:
            arch_flag = ' -mavx -mcpu=apple-m1 '
        elif arm64_only:
            arch_flag = ' -mcpu=apple-m1 '

        res['CC'] = 'clang'
        res['CFLAGS'] = f'-isysroot {osx_sysroot} -mmacosx-version-min={macos_min_version()} ' \
                        f'-fPIC {"" if no_hidden_visibility else "-fvisibility=hidden"} {arch_flag}' + optimization
        res['LDFLAGS'] = '-stdlib=libc++'
        res['CXX'] = 'clang++'
        res['CXXFLAGS'] = f'-stdlib=libc++ -std=c++{cpp_standard} ' \
                          f'-isysroot {osx_sysroot} -mmacosx-version-min={macos_min_version()} ' \
                          f'{"" if no_hidden_visibility else "-fvisibility=hidden -fvisibility-inlines-hidden"} ' \
                          f'-fPIC {arch_flag}' + optimization
        res['ASMFLAGS'] = f'-isysroot {osx_sysroot} -mmacosx-version-min={macos_min_version()}'
    elif is_linux():
        if use_clang_in_linux():
            res['CC'] = get_clang_in_linux()
            res['CFLAGS'] = f'-fPIC {"" if no_hidden_visibility else "-fvisibility=hidden"} -mavx' + optimization
            res['CXX'] = get_clangplus_in_linux()
            res['CXXFLAGS'] = f'-std=c++{cpp_standard} -fPIC ' \
                              f'{"" if no_hidden_visibility else "-fvisibility=hidden -fvisibility-inlines-hidden"} ' \
                              f'-mavx' + optimization
        else:
            res['CFLAGS'] = f'-fPIC {"" if no_hidden_visibility else "-fvisibility=hidden"} -mavx' + optimization
            res['CXXFLAGS'] = f'-std=c++{cpp_standard} -fPIC ' \
                              f'{"" if no_hidden_visibility else "-fvisibility=hidden -fvisibility-inlines-hidden"} ' \
                              f'-mavx' + optimization
    elif is_windows():
        optimization = ' /O2' if with_optimization else ''
        res['CFLAGS'] = f'/utf-8' + optimization
        res['CXXFLAGS'] = f'/utf-8 /std:c++{cpp_standard} /EHsc /D_SILENCE_ALL_CXX17_DEPRECATION_WARNINGS ' \
                          f'/DNOMINMAX /arch:AVX' + optimization
    return res


def get_env_for_config_make(cpp_standard: int = cpp_standard(),
                            remove_conda_from_path: bool = True,
                            remove_scoop_from_path: bool = True,
                            with_optimization: bool = True,
                            universal: bool = False,
                            arm64_only: bool = False,
                            no_hidden_visibility: bool = False
                            ):
    env = get_vcvars_environment(remove_conda_from_path=remove_conda_from_path,
                                 remove_scoop_from_path=remove_scoop_from_path) if is_windows() else os.environ.copy()
    cbf = get_common_build_flags(cpp_standard=cpp_standard, with_optimization=with_optimization, universal=universal,
                                 arm64_only=arm64_only, no_hidden_visibility=no_hidden_visibility)
    if is_mac():
        env['CC'] = cbf['CC']
        env['CFLAGS'] = cbf['CFLAGS']
        env['LDFLAGS'] = cbf['LDFLAGS']
        env['CXX'] = cbf['CXX']
        env['CXXFLAGS'] = cbf['CXXFLAGS']
    elif is_linux():
        if use_clang_in_linux():
            env['CC'] = cbf['CC']
            env['CFLAGS'] = cbf['CFLAGS']
            env['CXX'] = cbf['CXX']
            env['CXXFLAGS'] = cbf['CXXFLAGS']
        else:
            env['CFLAGS'] = cbf['CFLAGS']
            env['CXXFLAGS'] = cbf['CXXFLAGS']
    elif is_windows():
        env['CFLAGS'] = cbf['CFLAGS']
        env['CXXFLAGS'] = cbf['CXXFLAGS']
    return env


def get_cmake_cmd_common_part(install_dir: str, *, use_ninja: bool = use_ninja(), cpp_standard: int = cpp_standard(),
                              cpp_extention: bool = False, universal: bool = False, arm64_only: bool = False,
                              no_hidden_visibility: bool = False):
    cbf = get_common_build_flags(cpp_standard=cpp_standard, with_optimization=False, universal=universal,
                                 arm64_only=arm64_only, no_hidden_visibility=no_hidden_visibility)
    if is_windows():
        res = [get_cmake_binary(),  # '-E', 'echo',
               '-DCMAKE_BUILD_TYPE=Release',
               '-DCMAKE_PREFIX_PATH=' + ext_build_dir(),
               # '-DCMAKE_MODULE_PATH=' + ext_build_dir(),
               '-G', 'Ninja', '-DCMAKE_MAKE_PROGRAM=' + get_ninja_binary(),
               '-DCMAKE_INSTALL_PREFIX=' + install_dir,
               '' if no_hidden_visibility else '-DCMAKE_VISIBILITY_INLINES_HIDDEN=ON',
               '' if no_hidden_visibility else '-DCMAKE_CXX_VISIBILITY_PRESET=hidden',
               '' if no_hidden_visibility else '-DCMAKE_C_VISIBILITY_PRESET=hidden',
               f'-DCMAKE_CXX_STANDARD={cpp_standard}',
               '-DCMAKE_CXX_STANDARD_REQUIRED=ON',
               '-DCMAKE_CXX_EXTENSIONS=OFF',
               f'-DCMAKE_C_FLAGS:STRING={cbf["CFLAGS"]}',
               f'-DCMAKE_CXX_FLAGS:STRING={cbf["CXXFLAGS"]}',
               '-DCMAKE_LIBRARY_ARCHITECTURE=x86_64',
               ]
        if use_clang_cl():
            res.extend(['-DCMAKE_CXX_COMPILER=clang-cl',
                        '-DCMAKE_C_COMPILER=clang-cl',
                        ])
        if use_ninja:
            res.extend(['-G', 'Ninja', '-DCMAKE_MAKE_PROGRAM=' + get_ninja_binary()])
        else:
            res.extend(['-G', 'Visual Studio 17 2022', '-A', 'x64', '-T', 'host=x64'])
        return res
    elif is_linux():
        res = [get_cmake_binary(),  # '-E', 'echo',
               '-DCMAKE_BUILD_TYPE=Release',
               '-DCMAKE_PREFIX_PATH=' + ext_build_dir(),
               '-DCMAKE_IGNORE_PREFIX_PATH=/usr/local',
               '-DCMAKE_IGNORE_PATH=/usr/local/bin',
               '-DCMAKE_MODULE_PATH=' + ext_build_dir(),
               '-DCMAKE_INSTALL_PREFIX=' + install_dir,
               '' if no_hidden_visibility else '-DCMAKE_VISIBILITY_INLINES_HIDDEN=ON',
               '' if no_hidden_visibility else '-DCMAKE_CXX_VISIBILITY_PRESET=hidden',
               '' if no_hidden_visibility else '-DCMAKE_C_VISIBILITY_PRESET=hidden',
               f'-DCMAKE_CXX_STANDARD={cpp_standard}',
               '-DCMAKE_CXX_STANDARD_REQUIRED=ON',
               '-DCMAKE_CXX_EXTENSIONS=' + ('ON' if cpp_extention else 'OFF'),
               f'-DCMAKE_C_FLAGS:STRING={cbf["CFLAGS"]}',
               f'-DCMAKE_CXX_FLAGS:STRING={cbf["CXXFLAGS"]}',
               '-DCMAKE_LIBRARY_ARCHITECTURE=x86_64',
               ]
        if use_ninja:
            res.extend(['-G', 'Ninja', '-DCMAKE_MAKE_PROGRAM=' + get_ninja_binary()])
        return res
    else:
        osx_sysroot = r'/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk'
        assert os.path.exists(osx_sysroot)

        arch = 'x86_64'
        if universal:
            arch = 'x86_64;arm64'
        elif arm64_only:
            arch = 'arm64'

        res = [get_cmake_binary(),  # '-E', 'echo',
               '-DCMAKE_BUILD_TYPE=Release',
               '' if not arm64_only else '-DCMAKE_SYSTEM_NAME=Darwin',
               # '' if universal else f'-DCMAKE_SYSTEM_PROCESSOR={arch}',
               '' if not arm64_only else f'-DCMAKE_SYSTEM_PROCESSOR={arch}',
               '-DCMAKE_PREFIX_PATH=' + ext_build_dir(),
               '-DCMAKE_IGNORE_PREFIX_PATH=/usr/local',
               '-DCMAKE_IGNORE_PATH=/usr/local/bin',
               '-DCMAKE_MODULE_PATH=' + ext_build_dir(),
               '-DCMAKE_INSTALL_PREFIX=' + install_dir,
               '' if no_hidden_visibility else '-DCMAKE_VISIBILITY_INLINES_HIDDEN=ON',
               '' if no_hidden_visibility else '-DCMAKE_CXX_VISIBILITY_PRESET=hidden',
               '' if no_hidden_visibility else '-DCMAKE_C_VISIBILITY_PRESET=hidden',
               f'-DCMAKE_CXX_STANDARD={cpp_standard}',
               '-DCMAKE_CXX_STANDARD_REQUIRED=ON',
               '-DCMAKE_CXX_EXTENSIONS=OFF',
               '-DCMAKE_OSX_DEPLOYMENT_TARGET=' + macos_min_version(),
               '-DCMAKE_OSX_SYSROOT=' + osx_sysroot,
               '' if (not arm64_only and not universal) else f'-DCMAKE_OSX_ARCHITECTURES={arch}',
               f'-DCMAKE_C_FLAGS:STRING={cbf["CFLAGS"]}',
               f'-DCMAKE_CXX_FLAGS:STRING={cbf["CXXFLAGS"]}'
               ]
        if use_ninja:
            res.extend(['-G', 'Ninja', '-DCMAKE_MAKE_PROGRAM=' + get_ninja_binary()])
        return res


# def build_cmakecmd(cmakecmd, build_dir: str, *, env=None, use_ninja=use_ninja()):
#     if is_windows():
#         if env is None:
#             env = get_vcvars_environment()
#         subprocess.run(cmakecmd, cwd=build_dir, shell=False, check=True, env=env)
#         if use_ninja:
#             subprocess.run([get_ninja_binary()],
#                            cwd=build_dir, shell=False, check=True, env=env)
#         else:
#             subprocess.run(['MSBuild', 'ALL_BUILD.vcxproj', '/property:Configuration=Release', '/maxcpucount'],
#                            cwd=build_dir, shell=True, check=True, env=env)
#     else:
#         if use_ninja:
#             if env is None:
#                 subprocess.run(cmakecmd, cwd=build_dir, shell=False, check=True)
#                 subprocess.run([get_ninja_binary()],
#                                cwd=build_dir, shell=False, check=True)
#             else:
#                 subprocess.run(cmakecmd, cwd=build_dir, shell=False, check=True, env=env)
#                 subprocess.run([get_ninja_binary()],
#                                cwd=build_dir, shell=False, check=True, env=env)
#         else:
#             if env is None:
#                 subprocess.run(cmakecmd, cwd=build_dir, shell=False, check=True)
#                 subprocess.run(['make', '-j' + str(os.cpu_count())],
#                                cwd=build_dir, shell=False, check=True)
#             else:
#                 subprocess.run(cmakecmd, cwd=build_dir, shell=False, check=True, env=env)
#                 subprocess.run(['make', '-j' + str(os.cpu_count())],
#                                cwd=build_dir, shell=False, check=True, env=env)


def build_and_install_cmakecmd(cmakecmd, build_dir: str, *, additional_env=None, use_ninja=use_ninja(), use_cmake=False,
                               ninja_para: str = 'install'):
    cmakecmd[:] = [x for x in cmakecmd if x]
    if is_windows():
        env = get_vcvars_environment()
        if additional_env is not None:
            env.update(additional_env)
        subprocess.run(cmakecmd, cwd=build_dir, shell=False, check=True, env=env)
        if use_cmake:
            subprocess.run([get_cmake_binary(), '--build', '.'],
                           cwd=build_dir, shell=False, check=True, env=env)
        elif use_ninja:
            subprocess.run([get_ninja_binary(), ninja_para],
                           cwd=build_dir, shell=False, check=True, env=env)
        else:
            subprocess.run(['MSBuild', 'INSTALL.vcxproj', '/property:Configuration=Release', '/maxcpucount'],
                           cwd=build_dir, shell=True, check=True, env=env)
    else:
        env = get_env_for_config_make(with_optimization=False)
        if additional_env is not None:
            env.update(additional_env)
        if use_cmake:
            subprocess.run(cmakecmd, cwd=build_dir, shell=False, check=True, env=env)
            subprocess.run([get_cmake_binary(), '--build', '.'],
                           cwd=build_dir, shell=False, check=True, env=env)
        elif use_ninja:
            subprocess.run(cmakecmd, cwd=build_dir, shell=False, check=True, env=env)
            subprocess.run([get_ninja_binary(), ninja_para],
                           cwd=build_dir, shell=False, check=True, env=env)
        else:
            subprocess.run(cmakecmd, cwd=build_dir, shell=False, check=True, env=env)
            subprocess.run(['make', '-j' + str(os.cpu_count()), 'install'],
                           cwd=build_dir, shell=False, check=True, env=env)


def patch_file(orig_file: str, from_texts: list, to_texts: list, keep_bak_file: bool = True) -> str:
    assert len(from_texts) == len(to_texts)
    bak_file = get_bak_file_name(orig_file)
    if os.path.exists(bak_file):
        os.remove(bak_file)
    os.rename(orig_file, bak_file)

    txt = Path(bak_file).read_text(errors='ignore')
    with open(orig_file, mode='w', encoding='utf-8') as f:
        for from_text, to_text in zip(from_texts, to_texts):
            txt = txt.replace(from_text, to_text)
        f.write(txt)
    with open(bak_file, mode='r', encoding='utf-8', errors='ignore') as f:
        from_lines = f.readlines()
    with open(orig_file, mode='r', encoding='utf-8') as f:
        to_lines = f.readlines()
    print(''.join(list(difflib.unified_diff(from_lines, to_lines, fromfile=orig_file, tofile='<new>'))))
    if not keep_bak_file:
        os.remove(bak_file)
    return bak_file


def build_zlib(src_dir: str, install_dir: str):
    build_dir = create_build_dir(src_dir)

    orig_file = bak_file = None
    try:
        orig_file = os.path.join(src_dir, 'CMakeLists.txt')
        bak_file = patch_file(orig_file,
                              from_texts=[r'install(TARGETS zlib zlibstatic',
                                          r'target_link_libraries(example zlib)',
                                          r'target_link_libraries(minigzip zlib)',
                                          r'target_link_libraries(example64 zlib)',
                                          r'target_link_libraries(minigzip64 zlib)',
                                          ],
                              to_texts=[r'install(TARGETS zlibstatic',
                                        r'target_link_libraries(example zlibstatic)',
                                        r'target_link_libraries(minigzip zlibstatic)',
                                        r'target_link_libraries(example64 zlibstatic)',
                                        r'target_link_libraries(minigzip64 zlibstatic)',
                                        ])

        cmakecmd = get_cmake_cmd_common_part(install_dir, universal=True)

        cmakecmd.extend([src_dir])
        build_and_install_cmakecmd(cmakecmd, build_dir)
    finally:
        print('done')
        shutil.rmtree(build_dir, ignore_errors=False)
        os.replace(bak_file, orig_file)


def build_boost(src_dir: str, install_dir: str):
    try:
        shutil.rmtree(os.path.join(src_dir, 'bin.v2'), ignore_errors=True)
        if is_windows():
            cbf = get_common_build_flags(with_optimization=True)
            env = get_vcvars_environment()
            subprocess.run(['bootstrap'],
                           cwd=src_dir, shell=True, check=True, env=env)
            subprocess.run(['.\\b2',
                            '--prefix=' + install_dir,
                            f'cxxflags={cbf["CXXFLAGS"]}',
                            '--with-headers',
                            '--with-context',
                            '--with-filesystem',
                            '--with-program_options',
                            '--with-regex',
                            '--with-thread',
                            '--with-system',
                            'address-model=64',
                            'variant=release', 'link=static', 'threading=multi', 'runtime-link=shared',
                            'install',
                            ],
                           cwd=src_dir, shell=True, check=True, env=env)
        elif is_mac():
            arm64_install_dir = create_arm64_install_dir(src_dir)
            try:
                cbf = get_common_build_flags(with_optimization=True)
                env = get_env_for_config_make()
                subprocess.run(['./bootstrap.sh',
                                '--with-libraries=headers,context,filesystem,program_options,regex,thread,system',
                                '--without-icu',
                                '--prefix=' + install_dir],
                               cwd=src_dir, shell=False, check=True, env=env)
                subprocess.run(['./b2',
                                '--disable-icu',
                                'variant=release', 'link=static', 'threading=multi', 'runtime-link=shared',
                                f'cxxflags={cbf["CXXFLAGS"]}',
                                f'linkflags={cbf["LDFLAGS"]}',
                                f'cflags={cbf["CFLAGS"]}',
                                f'asmflags={cbf["ASMFLAGS"]}',
                                # 'target-os=darwin', 'architecture=x86', 'abi=sysv',
                                # f'cxxflags={cbf["CXXFLAGS"]} -arch x86_64',
                                # f'linkflags={cbf["LDFLAGS"]} -arch x86_64',
                                # f'cflags={cbf["CFLAGS"]} -arch x86_64',
                                # f'asmflags={cbf["ASMFLAGS"]} -arch x86_64',
                                'install',
                                ],
                               cwd=src_dir, shell=False, check=True, env=env)

                cbf = get_common_build_flags(with_optimization=True, arm64_only=True)
                env = get_env_for_config_make(arm64_only=True)
                subprocess.run(['./bootstrap.sh',
                                '--with-libraries=headers,context,filesystem,program_options,regex,thread,system',
                                '--without-icu',
                                '--prefix=' + arm64_install_dir],
                               cwd=src_dir, shell=False, check=True, env=env)
                subprocess.run(['./b2',
                                '--disable-icu',
                                'variant=release', 'link=static', 'threading=multi', 'runtime-link=shared',
                                'target-os=darwin', 'architecture=arm', 'abi=aapcs',
                                f'cxxflags={cbf["CXXFLAGS"]} -arch arm64',
                                f'linkflags={cbf["LDFLAGS"]} -arch arm64',
                                f'cflags={cbf["CFLAGS"]} -arch arm64',
                                f'asmflags={cbf["ASMFLAGS"]} -arch arm64',
                                'install',
                                ],
                               cwd=src_dir, shell=False, check=True, env=env)
                create_universal_binaries(arm64_install_dir, install_dir)
            finally:
                shutil.rmtree(arm64_install_dir, ignore_errors=False)
        else:
            cbf = get_common_build_flags(with_optimization=True)
            env = get_env_for_config_make()
            subprocess.run(['./bootstrap.sh',
                            '--with-toolset=clang' if use_clang_in_linux() else '',
                            '--with-libraries=headers,context,filesystem,program_options,regex,thread,system',
                            '--without-icu',
                            '--prefix=' + install_dir],
                           cwd=src_dir, shell=False, check=True, env=env)

            subprocess.run(['./b2',
                            '--disable-icu',
                            'toolset=clang' if use_clang_in_linux() else '',
                            'address-model=64',
                            'variant=release', 'link=static', 'threading=multi', 'runtime-link=shared',
                            f'cxxflags={cbf["CXXFLAGS"]}',
                            'install',
                            ],
                           cwd=src_dir, shell=False, check=True, env=env)
    finally:
        print('done')


def clean_boost(install_dir: str):
    shutil.rmtree(os.path.join(install_dir, 'include', 'boost'), ignore_errors=True)
    glob_remove(os.path.join(install_dir, 'lib', 'cmake', 'boost*'))
    glob_remove(os.path.join(install_dir, 'lib', 'cmake', 'Boost*'))
    glob_remove(os.path.join(install_dir, 'lib', 'libboost*'))


def build_tbb(src_dir: str, install_dir: str):
    build_dir = create_build_dir(src_dir)

    try:
        cmakecmd = get_cmake_cmd_common_part(install_dir, universal=True, no_hidden_visibility=True)
        cmakecmd.extend(['-DTBB_TEST:BOOL=OFF',
                         '-DTBB_STRICT:BOOL=OFF'])

        cmakecmd.extend([src_dir])
        build_and_install_cmakecmd(cmakecmd, build_dir)
    finally:
        shutil.rmtree(build_dir, ignore_errors=False)
        print()


def build_cpuinfo(src_dir: str, install_dir: str):
    build_dir = create_build_dir(src_dir)
    cmake_options = ['-DBUILD_GMOCK:BOOL=OFF',
                     '-DCPUINFO_BUILD_MOCK_TESTS:BOOL=OFF',
                     '-DCPUINFO_BUILD_BENCHMARKS:BOOL=OFF',
                     '-DCPUINFO_BUILD_UNIT_TESTS:BOOL=OFF',
                     '-DCPUINFO_BUILD_TOOLS:BOOL=ON']

    try:
        cmakecmd = get_cmake_cmd_common_part(install_dir)
        cmakecmd.extend(cmake_options)

        cmakecmd.extend([src_dir])
        build_and_install_cmakecmd(cmakecmd, build_dir)
    finally:
        shutil.rmtree(build_dir, ignore_errors=False)

    if is_mac():
        build_dir = create_build_dir(src_dir)
        arm64_install_dir = create_arm64_install_dir(src_dir)

        try:
            cmakecmd = get_cmake_cmd_common_part(arm64_install_dir, arm64_only=True)
            cmakecmd.extend(cmake_options)

            cmakecmd.extend([src_dir])
            build_and_install_cmakecmd(cmakecmd, build_dir)

            create_universal_binaries(arm64_install_dir, install_dir)
        finally:
            shutil.rmtree(build_dir, ignore_errors=False)
            shutil.rmtree(arm64_install_dir, ignore_errors=False)


def build_gflags(src_dir: str, install_dir: str):
    build_dir = create_build_dir(src_dir)

    bak_file = orig_file = None
    try:
        if is_mac():
            orig_file = os.path.join(src_dir, 'src', 'gflags.cc')
            bak_file = patch_file(orig_file,
                                  from_texts=[r'ReportError(DIE, "ERROR: something wrong with'],
                                  to_texts=[r'ReportError(DO_NOT_DIE, "ERROR: something wrong with'])

        cmakecmd = get_cmake_cmd_common_part(install_dir, universal=True)
        cmakecmd.extend(['-DGFLAGS_NAMESPACE=gflags',
                         ])

        cmakecmd.extend([src_dir])
        build_and_install_cmakecmd(cmakecmd, build_dir)
    finally:
        shutil.rmtree(build_dir, ignore_errors=False)
        if is_mac():
            os.replace(bak_file, orig_file)


def build_glog(src_dir: str, install_dir: str):
    build_dir = create_build_dir(src_dir)

    bak_file1 = orig_file1 = None
    try:
        orig_file1 = os.path.join(src_dir, 'src', 'logging.cc')
        bak_file1 = patch_file(orig_file1,
                               from_texts=[r'ColoredWriteToStderr(severity, message, message_len);',
                                           ],
                               to_texts=[r'ColoredWriteToStdout(severity, message, message_len);'
                                         ])

        cmakecmd = get_cmake_cmd_common_part(install_dir, universal=True)
        cmakecmd.extend(['-DBUILD_TESTING:BOOL=OFF',
                         '-DBUILD_SHARED_LIBS:BOOL=OFF',
                         '-DGFLAGS_USE_TARGET_NAMESPACE:BOOL=ON',
                         ])

        cmakecmd.extend([src_dir])
        build_and_install_cmakecmd(cmakecmd, build_dir)

        # patch_file(os.path.join(ext_build_dir(), 'include', 'glog', 'export.h'),
        #            from_texts=[r'#define GOOGLE_GLOG_DLL_DECL_H',
        #                        ],
        #            to_texts=['#define GOOGLE_GLOG_DLL_DECL_H\n'
        #                      '#define GLOG_STATIC_DEFINE\n'
        #                      '#define HAVE_CXX11_ATOMIC\n',
        #                      ])
    finally:
        shutil.rmtree(build_dir, ignore_errors=False)
        os.replace(bak_file1, orig_file1)


def build_benchmark(src_dir: str, install_dir: str):
    build_dir = create_build_dir(src_dir)

    bak_file = orig_file = None
    try:
        orig_file = os.path.join(src_dir, 'src', 'CMakeLists.txt')
        bak_file = patch_file(orig_file,
                              from_texts=[r'target_compile_definitions(benchmark PRIVATE -DBENCHMARK_STATIC_DEFINE)'],
                              to_texts=[r'target_compile_definitions(benchmark PUBLIC -DBENCHMARK_STATIC_DEFINE)'],
                              )

        cmakecmd = get_cmake_cmd_common_part(install_dir, universal=True)
        cmakecmd.extend(['-DBENCHMARK_ENABLE_TESTING:BOOL=OFF',
                         '-DBENCHMARK_ENABLE_GTEST_TESTS:BOOL=OFF'])

        if is_windows() and not use_clang_cl():
            cmakecmd.extend(['-DBENCHMARK_ENABLE_LTO:BOOL=ON'])
        elif is_mac():
            cmakecmd.extend(['-DBENCHMARK_USE_LIBCXX:BOOL=ON'])

        cmakecmd.extend([src_dir])
        build_and_install_cmakecmd(cmakecmd, build_dir)
    finally:
        shutil.rmtree(build_dir, ignore_errors=False)
        os.replace(bak_file, orig_file)


def build_openssl(src_dir: str, install_dir: str, nasm_dir: str):
    try:
        if is_linux():
            env = get_env_for_config_make()
            subprocess.run(['perl', './Configure',
                            'linux-x86_64',
                            'enable-ec_nistp_64_gcc_128',
                            'no-shared',
                            'no-tests',
                            'no-ui-console',
                            # 'no-legacy',
                            '--prefix=' + install_dir,
                            '--openssldir=' + os.path.join(install_dir, 'ssl')],
                           cwd=src_dir, shell=False, check=True, env=env)
            subprocess.run(['make', 'install_sw'],
                           cwd=src_dir, shell=False, check=True, env=env)
        elif is_mac():
            env = get_env_for_config_make()
            subprocess.run(['perl', './Configure',
                            'darwin64-x86_64',
                            'enable-ec_nistp_64_gcc_128',
                            'no-shared',
                            'no-tests',
                            'no-ui-console',
                            # 'no-legacy',
                            '--prefix=' + install_dir,
                            '--openssldir=' + os.path.join(install_dir, 'ssl')],
                           cwd=src_dir, shell=False, check=True, env=env)
            subprocess.run(['make', 'install_sw'],
                           cwd=src_dir, shell=False, check=True, env=env)
            subprocess.run(['make', 'clean', 'distclean'],
                           cwd=src_dir, shell=False, check=True, env=env)

            env = get_env_for_config_make(arm64_only=True)
            arm64_install_dir = create_arm64_install_dir(src_dir)

            try:
                subprocess.run(['perl', './Configure',
                                'darwin64-arm64',
                                'enable-ec_nistp_64_gcc_128',
                                'no-shared',
                                'no-tests',
                                'no-ui-console',
                                # 'no-legacy',
                                '--prefix=' + arm64_install_dir,
                                '--openssldir=' + os.path.join(arm64_install_dir, 'ssl')],
                               cwd=src_dir, shell=False, check=True, env=env)
                subprocess.run(['make', 'install_sw'],
                               cwd=src_dir, shell=False, check=True, env=env)
                subprocess.run(['make', 'clean', 'distclean'],
                               cwd=src_dir, shell=False, check=True, env=env)

                create_universal_binaries(arm64_install_dir, install_dir)
            finally:
                print()
                shutil.rmtree(arm64_install_dir, ignore_errors=False)
        elif is_windows():
            env = get_env_for_config_make(remove_scoop_from_path=False)
            env['PATH'] = f'{env["PATH"]};{nasm_dir}'
            subprocess.run(['perl', './Configure',
                            'VC-WIN64A',
                            'no-shared',
                            'no-tests',
                            'no-ui-console',
                            # 'no-legacy',
                            '--prefix=' + install_dir,
                            '--openssldir=' + os.path.join(install_dir, 'ssl')],
                           cwd=src_dir, shell=True, check=True, env=env)
            subprocess.run(['nmake', 'install_sw'],
                           cwd=src_dir, shell=True, check=True, env=env)
    finally:
        print('done')


def build_grpc(src_dir: str, install_dir: str, nasm_dir: str):
    print(nasm_dir)
    # ssl_src_dir = os.path.join(src_dir, 'third_party', 'boringssl-with-bazel')
    # ssl_install_dir = ext_build_dir()
    # ssl_build_dir = create_build_dir(src_dir)
    # try:
    #     cmakecmd = get_cmake_cmd_common_part(ssl_install_dir)
    #     if is_windows():
    #         cmakecmd.extend(['-DCMAKE_ASM_NASM_COMPILER:FILEPATH=' + nasm_dir + '\\nasm.exe',
    #                          ssl_src_dir])
    #     else:
    #         cmakecmd.extend([ssl_src_dir])
    #     build_cmakecmd(cmakecmd, ssl_build_dir)
    #     distutils.dir_util.copy_tree(os.path.join(ssl_src_dir, 'src', 'include'),
    #                                  os.path.join(ssl_install_dir, 'include'))
    #     if is_windows():
    #         glob_copy(os.path.join(ssl_build_dir, '*.lib'), os.path.join(ssl_install_dir, 'lib'))
    #         glob_copy(os.path.join(ssl_build_dir, 'decrepit', '*.lib'), os.path.join(ssl_install_dir, 'lib'))
    #         glob_copy(os.path.join(ssl_build_dir, 'crypto', '*.lib'), os.path.join(ssl_install_dir, 'lib'))
    #         glob_copy(os.path.join(ssl_build_dir, 'ssl', '*.lib'), os.path.join(ssl_install_dir, 'lib'))
    #     else:
    #         glob_copy(os.path.join(ssl_build_dir, 'lib*.a'), os.path.join(ssl_install_dir, 'lib'))
    #         glob_copy(os.path.join(ssl_build_dir, 'decrepit', 'lib*.a'), os.path.join(ssl_install_dir, 'lib'))
    #         glob_copy(os.path.join(ssl_build_dir, 'crypto', 'lib*.a'), os.path.join(ssl_install_dir, 'lib'))
    #         glob_copy(os.path.join(ssl_build_dir, 'ssl', 'lib*.a'), os.path.join(ssl_install_dir, 'lib'))
    # finally:
    #     shutil.rmtree(ssl_build_dir, ignore_errors=False)

    # sub_src_dir = os.path.join(src_dir, 'third_party', 'cares', 'cares')
    # sub_install_dir = ext_build_dir()
    # sub_build_dir = create_build_dir(src_dir)
    # try:
    #     cmakecmd = get_cmake_cmd_common_part(sub_install_dir)
    #     cmakecmd.extend(['-DCARES_SHARED:BOOL=OFF',
    #                      '-DCARES_STATIC:BOOL=ON',
    #                      '-DCARES_STATIC_PIC:BOOL=ON',
    #                      sub_src_dir])
    #     build_and_install_cmakecmd(cmakecmd, sub_build_dir)
    # finally:
    #     shutil.rmtree(sub_build_dir, ignore_errors=False)

    sub_src_dir = os.path.join(src_dir, 'third_party', 'abseil-cpp')
    sub_install_dir = ext_build_dir()
    sub_build_dir = create_build_dir(src_dir)
    try:
        cmakecmd = get_cmake_cmd_common_part(sub_install_dir, universal=True)
        cmakecmd.extend(['-DABSL_USE_EXTERNAL_GOOGLETEST:BOOL=ON',
                         '-DCMAKE_POSITION_INDEPENDENT_CODE=TRUE', ])

        cmakecmd.extend([sub_src_dir])
        build_and_install_cmakecmd(cmakecmd, sub_build_dir)
    finally:
        shutil.rmtree(sub_build_dir, ignore_errors=False)

    sub_src_dir = os.path.join(src_dir, 'third_party', 'protobuf')
    sub_install_dir = ext_build_dir()
    sub_build_dir = create_build_dir(src_dir)
    try:
        cmakecmd = get_cmake_cmd_common_part(sub_install_dir, universal=True)
        cmakecmd.extend(['-Dprotobuf_BUILD_TESTS:BOOL=OFF',
                         '-Dprotobuf_WITH_ZLIB:BOOL=ON',
                         '-Dprotobuf_MSVC_STATIC_RUNTIME:BOOL=OFF',
                         '-Dprotobuf_BUILD_SHARED_LIBS:BOOL=OFF',
                         '-Dprotobuf_ABSL_PROVIDER=package'])

        cmakecmd.extend([sub_src_dir])
        build_and_install_cmakecmd(cmakecmd, sub_build_dir)

        # orig_file_2 = os.path.join(sub_install_dir, 'lib', 'cmake', 'protobuf', 'protobuf-config.cmake')
        # if is_windows():
        #     orig_file_2 = os.path.join(sub_install_dir, 'cmake', 'protobuf-config.cmake')
        # patch_file(orig_file_2,
        #            from_texts=[r'${protobuf_generate_PROTOC_OUT_DIR}/${_rel_dir}/${_basename}${_ext}'],
        #            to_texts=[r'${protobuf_generate_PROTOC_OUT_DIR}/${_basename}${_ext}'])
    finally:
        shutil.rmtree(sub_build_dir, ignore_errors=False)

    build_dir = create_build_dir(src_dir)
    orig_file = bak_file = None
    orig_file1 = bak_file1 = None
    try:
        # if is_linux() and not use_clang_in_linux():
        #     orig_file = os.path.join(src_dir, 'src', 'core', 'ext', 'gcp', 'metadata_query.cc')
        #     bak_file = patch_file(orig_file,
        #                           from_texts=[r'constexpr const char MetadataQuery',
        #                                       ],
        #                           to_texts=[r'const char MetadataQuery',
        #                                     ])
        if is_mac():
            orig_file = os.path.join(src_dir, 'cmake', 'gRPCConfig.cmake.in')
            bak_file = patch_file(orig_file,
                                  from_texts=[r'if(NOT CMAKE_CROSSCOMPILING)',
                                              ],
                                  to_texts=[r'if(1)',
                                            ])
        orig_file1 = os.path.join(src_dir, 'src', 'core', 'lib', 'promise', 'detail', 'promise_like.h')
        bak_file1 = patch_file(orig_file1,
                               from_texts=[r'typename std::result_of<F()>::type>::value>> {',
                                           ],
                               to_texts=[r"""#if (defined(__cpp_lib_is_invocable) && __cpp_lib_is_invocable >= 201703L) || \
    (defined(_MSVC_LANG) && _MSVC_LANG >= 201703L)
                         std::invoke_result_t<F>
#else
                         typename std::result_of<F()>::type
#endif
                         >::value>> {""",
                                         ])

        cmakecmd = get_cmake_cmd_common_part(install_dir, universal=True)
        cmakecmd.extend(['-DgRPC_INSTALL:BOOL=ON',
                         '-DgRPC_BUILD_TESTS:BOOL=OFF',
                         '-DgRPC_MSVC_STATIC_RUNTIME:BOOL=OFF' if is_windows() else '',
                         '-DgRPC_ZLIB_PROVIDER:STRING=package',
                         '-DgRPC_PROTOBUF_PROVIDER=package',
                         '-DgRPC_CARES_PROVIDER=module',
                         '-DgRPC_SSL_PROVIDER=package',
                         f'-DOPENSSL_ROOT_DIR:PATH={install_dir}',
                         '-DgRPC_BENCHMARK_PROVIDER:STRING=package',
                         '-DgRPC_ABSL_PROVIDER:STRING=package',
                         '-DgRPC_RE2_PROVIDER:STRING=module',
                         ])

        cmakecmd.extend([src_dir])
        build_and_install_cmakecmd(cmakecmd, build_dir)
    finally:
        shutil.rmtree(build_dir, ignore_errors=False)
        # if is_linux() and not use_clang_in_linux():
        #     os.replace(bak_file, orig_file)
        if is_mac():
            os.replace(bak_file, orig_file)
        os.replace(bak_file1, orig_file1)


def build_bzip2(src_dir: str, install_dir: str):
    build_dir = create_build_dir(src_dir)

    try:
        cmakecmd = get_cmake_cmd_common_part(install_dir, universal=True)
        cmakecmd.extend(['-DENABLE_DEBUG=OFF',
                         '-DENABLE_APP=ON',
                         '-DENABLE_DOCS=OFF',
                         '-DENABLE_EXAMPLES=OFF',
                         '-DENABLE_STATIC_LIB=ON',
                         '-DENABLE_SHARED_LIB=OFF',
                         src_dir])
        build_and_install_cmakecmd(cmakecmd, build_dir)
        if not is_windows():
            shutil.copy2(os.path.join(install_dir, 'lib', 'libbz2_static.a'),
                         os.path.join(install_dir, 'lib', 'libbz2.a'))
        else:
            shutil.copy2(os.path.join(install_dir, 'lib', 'bz2_static.lib'),
                         os.path.join(install_dir, 'lib', 'bz2.lib'))
    finally:
        shutil.rmtree(build_dir, ignore_errors=False)


def build_double_conversion(src_dir: str, install_dir: str):
    build_dir = create_build_dir(src_dir)

    try:
        cmakecmd = get_cmake_cmd_common_part(install_dir, universal=True)
        cmakecmd.extend(['-DBUILD_TESTING:BOOL=OFF',
                         src_dir])
        build_and_install_cmakecmd(cmakecmd, build_dir)
    finally:
        shutil.rmtree(build_dir, ignore_errors=False)


def build_fmt(src_dir: str, install_dir: str):
    build_dir = create_build_dir(src_dir)

    try:
        cmakecmd = get_cmake_cmd_common_part(install_dir, universal=True)
        cmakecmd.extend(['-DFMT_DOC:BOOL=OFF',
                         '-DFMT_TEST:BOOL=OFF',
                         src_dir])
        build_and_install_cmakecmd(cmakecmd, build_dir)
    finally:
        shutil.rmtree(build_dir, ignore_errors=False)


def build_libevent(src_dir: str, install_dir: str):
    build_dir = create_build_dir(src_dir)

    orig_file = bak_file = None
    try:
        orig_file = os.path.join(src_dir, 'cmake', 'LibeventConfig.cmake.in')
        bak_file = patch_file(orig_file,
                              from_texts=[
                                  r'if (${CMAKE_VERSION} VERSION_LESS "3.15.0" AND ${LIBEVENT_STATIC_LINK} AND ${OPENSSL_FOUND} AND ${Threads_FOUND})',
                              ],
                              to_texts=[r'if (0)',
                                        ])

        cmakecmd = get_cmake_cmd_common_part(install_dir, universal=True)
        cmakecmd.extend(['-DEVENT__DISABLE_DEBUG_MODE:BOOL=ON',
                         '-DEVENT__DISABLE_OPENSSL:BOOL=ON',
                         '-DEVENT__DISABLE_BENCHMARK:BOOL=ON',
                         '-DEVENT__DISABLE_TESTS:BOOL=ON',
                         '-DEVENT__DISABLE_REGRESS:BOOL=ON',
                         '-DEVENT__DISABLE_SAMPLES:BOOL=ON',
                         '-DEVENT__DISABLE_MBEDTLS:BOOL=ON',
                         '-DEVENT__MSVC_STATIC_RUNTIME:BOOL=OFF',
                         '-DEVENT__DOXYGEN:BOOL=OFF',
                         '-DEVENT__LIBRARY_TYPE=STATIC',
                         src_dir])
        build_and_install_cmakecmd(cmakecmd, build_dir)
    finally:
        shutil.rmtree(build_dir, ignore_errors=False)
        os.replace(bak_file, orig_file)


def build_lz4(src_dir: str, install_dir: str):
    build_dir = create_build_dir(src_dir)

    try:
        cmakecmd = get_cmake_cmd_common_part(install_dir, universal=True)
        cmakecmd.extend(['-DLZ4_BUNDLED_MODE:BOOL=OFF',
                         '-DBUILD_SHARED_LIBS:BOOL=OFF',
                         '-DBUILD_STATIC_LIBS:BOOL=ON',
                         '-DLZ4_BUILD_LEGACY_LZ4C:BOOL=OFF',
                         '-DLZ4_BUILD_CLI:BOOL=OFF',
                         os.path.join(src_dir, 'build', 'cmake')])
        build_and_install_cmakecmd(cmakecmd, build_dir)
    finally:
        shutil.rmtree(build_dir, ignore_errors=False)


def build_snappy(src_dir: str, install_dir: str):
    build_dir = create_build_dir(src_dir)

    orig_file = bak_file = None
    arm64_install_dir = None
    try:
        orig_file = os.path.join(src_dir, 'CMakeLists.txt')
        # no-rtti cause link error
        bak_file = patch_file(orig_file,
                              from_texts=[r'NOT CMAKE_CXX_FLAGS MATCHES "-Werror"',
                                          r'string(REGEX REPLACE "/GR" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")',
                                          r'set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /GR-")',
                                          r'string(REGEX REPLACE "-frtti" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")',
                                          r'set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-rtti")',
                                          ],
                              to_texts=[r'OFF',
                                        r'',
                                        r'',
                                        r'',
                                        r'',
                                        ])

        cmakecmd = get_cmake_cmd_common_part(install_dir)
        cmakecmd.extend(['-DBUILD_SHARED_LIBS:BOOL=OFF',
                         '-DSNAPPY_BUILD_TESTS:BOOL=OFF',
                         '-DSNAPPY_BUILD_BENCHMARKS:BOOL=OFF',
                         '-DSNAPPY_REQUIRE_AVX:BOOL=ON',
                         '-DSNAPPY_REQUIRE_AVX2:BOOL=OFF',
                         src_dir])
        build_and_install_cmakecmd(cmakecmd, build_dir)

        if is_mac():
            build_dir = create_build_dir(src_dir)
            arm64_install_dir = create_arm64_install_dir(src_dir)

            try:
                cmakecmd = get_cmake_cmd_common_part(arm64_install_dir, arm64_only=True)
                cmakecmd.extend(['-DBUILD_SHARED_LIBS:BOOL=OFF',
                                 '-DSNAPPY_BUILD_TESTS:BOOL=OFF',
                                 '-DSNAPPY_BUILD_BENCHMARKS:BOOL=OFF',
                                 '-DSNAPPY_REQUIRE_AVX:BOOL=OFF',
                                 '-DSNAPPY_REQUIRE_AVX2:BOOL=OFF',
                                 src_dir])
                build_and_install_cmakecmd(cmakecmd, build_dir)
                create_universal_binaries(arm64_install_dir, install_dir)
            finally:
                shutil.rmtree(arm64_install_dir, ignore_errors=False)
    finally:
        shutil.rmtree(build_dir, ignore_errors=False)
        os.replace(bak_file, orig_file)


def build_xz(src_dir: str, install_dir: str):
    build_dir = create_build_dir(src_dir)
    arm64_install_dir = None

    bak_file = orig_file = None
    try:
        orig_file = os.path.join(src_dir, 'src', 'liblzma', 'api', 'lzma.h')
        bak_file = patch_file(orig_file,
                              from_texts=[r'#ifndef LZMA_API_IMPORT',
                                          ],
                              to_texts=['#ifndef LZMA_API_STATIC\n'
                                        '#define LZMA_API_STATIC\n'
                                        '#endif\n'
                                        '#ifndef LZMA_API_IMPORT\n',
                                        ])

        cmakecmd = get_cmake_cmd_common_part(install_dir)
        cmakecmd.extend(['-DBUILD_SHARED_LIBS:BOOL=OFF',
                         '-DCREATE_XZ_SYMLINKS:BOOL=OFF',
                         '-DCREATE_LZMA_SYMLINKS:BOOL=OFF',
                         '-DENABLE_SMALL:BOOL=OFF',
                         src_dir])
        build_and_install_cmakecmd(cmakecmd, build_dir)

        if is_mac():
            build_dir = create_build_dir(src_dir)
            arm64_install_dir = create_arm64_install_dir(src_dir)

            try:
                cmakecmd = get_cmake_cmd_common_part(arm64_install_dir, arm64_only=True)
                cmakecmd.extend(['-DBUILD_SHARED_LIBS:BOOL=OFF',
                                 '-DCREATE_XZ_SYMLINKS:BOOL=OFF',
                                 '-DCREATE_LZMA_SYMLINKS:BOOL=OFF',
                                 '-DENABLE_SMALL:BOOL=OFF',
                                 src_dir])
                build_and_install_cmakecmd(cmakecmd, build_dir)
                create_universal_binaries(arm64_install_dir, install_dir)
            finally:
                shutil.rmtree(arm64_install_dir, ignore_errors=False)
    finally:
        shutil.rmtree(build_dir, ignore_errors=False)
        os.replace(bak_file, orig_file)


def build_zstd(src_dir: str, install_dir: str):
    build_dir = create_build_dir(src_dir)

    bak_file = orig_file = None
    try:
        if is_mac():
            orig_file = os.path.join(src_dir, 'lib', 'zstd.h')
            bak_file = patch_file(orig_file,
                                  from_texts=[r'#ifdef ZSTD_DISABLE_DEPRECATE_WARNINGS',
                                              ],
                                  to_texts=['#define ZSTD_DISABLE_DEPRECATE_WARNINGS\n'
                                            '#ifdef ZSTD_DISABLE_DEPRECATE_WARNINGS',
                                            ])

        cmakecmd = get_cmake_cmd_common_part(install_dir, universal=True)
        cmakecmd.extend(['-DZSTD_USE_STATIC_RUNTIME:BOOL=OFF',
                         '-DZSTD_BUILD_SHARED:BOOL=OFF',
                         '-DZSTD_BUILD_STATIC:BOOL=ON',
                         os.path.join(src_dir, 'build', 'cmake')])
        build_and_install_cmakecmd(cmakecmd, build_dir)
    finally:
        shutil.rmtree(build_dir, ignore_errors=False)
        if is_mac():
            os.replace(bak_file, orig_file)


def build_libsodium(src_dir: str, install_dir: str):
    try:
        if is_windows():
            env = get_vcvars_environment()
            subprocess.run(['devenv', 'libsodium.sln', '/Upgrade'],
                           cwd=os.path.join(src_dir, 'builds', 'msvc', 'vs2019'), shell=True, check=True, env=env)
            subprocess.run(['MSBuild', 'libsodium.sln', '/target:libsodium', '/property:Platform=x64',
                            '/property:Configuration=StaticRelease', '/maxcpucount'],
                           cwd=os.path.join(src_dir, 'builds', 'msvc', 'vs2019'), shell=True, check=True, env=env)
            glob_copy(os.path.join(src_dir, 'src', 'libsodium', 'include', '*.h'),
                      os.path.join(install_dir, 'include'))
            shutil.rmtree(os.path.join(install_dir, 'include', 'sodium'), ignore_errors=True)
            shutil.copytree(os.path.join(src_dir, 'src', 'libsodium', 'include', 'sodium'),
                            os.path.join(install_dir, 'include', 'sodium'), dirs_exist_ok=True)

            glob_copy(os.path.join(src_dir, 'bin', 'x64', 'Release', 'v143', 'static', '*.lib'),
                      os.path.join(install_dir, 'lib'))

            orig_file = os.path.join(install_dir, 'include', 'sodium', 'export.h')
            patch_file(orig_file,
                       from_texts=[r'#ifdef SODIUM_STATIC'],
                       to_texts=['#define SODIUM_STATIC\n'
                                 '#ifdef SODIUM_STATIC'])
        else:
            env = get_env_for_config_make()
            # if is_mac():
            #     env['CFLAGS'] += ' -arch x86_64'
            #     env['CXXFLAGS'] += ' -arch x86_64'
            subprocess.run(['./configure',
                            '--enable-shared=no',
                            '--enable-static=yes',
                            '--prefix=' + install_dir],
                           cwd=src_dir, shell=False, check=True, env=env)
            subprocess.run(['make', '-j' + str(os.cpu_count()), 'install'],
                           cwd=src_dir, shell=False, check=True, env=env)
            subprocess.run(['make', 'clean', 'distclean'],
                           cwd=src_dir, shell=False, check=True, env=env)

            if is_mac():
                env = get_env_for_config_make(arm64_only=True)
                env['CFLAGS'] += ' -arch arm64'
                env['CXXFLAGS'] += ' -arch arm64'
                arm64_install_dir = create_arm64_install_dir(src_dir)

                try:
                    subprocess.run(['./configure',
                                    '--host=aarch64-apple-darwin20.6.0',
                                    '--enable-shared=no',
                                    '--enable-static=yes',
                                    '--prefix=' + arm64_install_dir],
                                   cwd=src_dir, shell=False, check=True, env=env)
                    subprocess.run(['make', '-j' + str(os.cpu_count()), 'install'],
                                   cwd=src_dir, shell=False, check=True, env=env)
                    subprocess.run(['make', 'clean', 'distclean'],
                                   cwd=src_dir, shell=False, check=True, env=env)

                    create_universal_binaries(arm64_install_dir, install_dir)
                finally:
                    print()
                    shutil.rmtree(arm64_install_dir, ignore_errors=False)
    finally:
        print('done')


def build_folly(src_dir: str, install_dir: str, use_asan: bool = False):
    build_dir = create_build_dir(src_dir)

    orig_file1 = bak_file1 = None
    orig_file5 = bak_file5 = None
    orig_file2 = bak_file2 = None
    orig_file3 = bak_file3 = None
    orig_file4 = bak_file4 = None
    orig_file6 = bak_file6 = None
    orig_file7 = bak_file7 = None
    try:
        if is_mac() and macos_min_version().startswith('10.'):
            # preadv and pwritev are only available after macOS 11.0
            orig_file5 = os.path.join(src_dir, 'CMake', 'FollyConfigChecks.cmake')
            bak_file5 = patch_file(orig_file5,
                                   from_texts=[r'check_symbol_exists(preadv',
                                               r'check_symbol_exists(pwritev'],
                                   to_texts=[r'#check_symbol_exists(preadv',
                                             r'#check_symbol_exists(pwritev'])

        orig_file1 = os.path.join(src_dir, 'CMake', 'folly-config.cmake.in')
        bak_file1 = patch_file(orig_file1,
                               from_texts=[r'find_dependency(fmt)'],
                               to_texts=['find_dependency(fmt)\n'
                                         'find_dependency(gflags CONFIG)\n'
                                         'find_dependency(glog CONFIG)'])

        # orig_file7 = os.path.join(src_dir, 'CMake', 'folly-config.h.cmake')
        # bak_file7 = patch_file(orig_file7,
        #                        from_texts=[r'#cmakedefine FOLLY_USE_JEMALLOC 1'],
        #                        to_texts=['#cmakedefine FOLLY_USE_JEMALLOC 1\n'
        #                                  '#define FOLLY_ASSUME_NO_JEMALLOC\n'
        #                                  '#define FOLLY_ASSUME_NO_TCMALLOC'])

        orig_file2 = os.path.join(src_dir, 'CMake', 'folly-deps.cmake')
        bak_file2 = patch_file(orig_file2,
                               from_texts=[r'${ZLIB_INCLUDE_DIRS}',
                                           r'${BZIP2_INCLUDE_DIRS}',
                                           r'find_package(OpenSSL MODULE REQUIRED)',
                                           r'find_package(BZip2 MODULE)',
                                           r'find_package(LibLZMA MODULE)',
                                           r'find_package(LZ4 MODULE)',
                                           r'find_package(Zstd MODULE)',
                                           r'find_package(Snappy MODULE)',
                                           r'find_package(Libsodium)',
                                           r'find_package(Gflags MODULE)',
                                           r'list(APPEND FOLLY_LINK_LIBRARIES ${LIBGFLAGS_LIBRARY})',
                                           r'find_package(Glog MODULE)',
                                           r'set(FOLLY_HAVE_LIBGLOG ${GLOG_FOUND})',
                                           r'list(APPEND FOLLY_LINK_LIBRARIES ${GLOG_LIBRARY})',
                                           r'find_package(LibDwarf)' + ('' if is_mac() else '_NONONO'),
                                           r'find_package(Libiberty)' + ('' if is_mac() else '_NONONO'),
                                           r'find_package(LibAIO)' + ('' if is_mac() else '_NONONO'),
                                           r'find_package(LibUring)' + ('' if is_mac() else '_NONONO'),
                                           r'find_package(LibUnwind)' + ('' if is_mac() else '_NONONO'),
                                           r'set(FOLLY_USE_SYMBOLIZER ON)',
                                           ],
                               to_texts=[r'',
                                         r'',
                                         'find_package(OpenSSL MODULE REQUIRED)\n'
                                         'if (WIN32)\n'
                                         'list(APPEND OPENSSL_LIBRARIES ${OPENSSL_LIBRARIES} Bcrypt.lib Crypt32.lib Ws2_32.lib)\n'
                                         'endif (WIN32)\n',
                                         r'find_package(BZip2 MODULE REQUIRED)',
                                         r'find_package(LibLZMA MODULE REQUIRED)',
                                         r'find_package(LZ4 MODULE REQUIRED)',
                                         f'find_package({"ZSTD" if is_mac() else "Zstd"} MODULE REQUIRED)',
                                         f'find_package({"SNAPPY" if is_mac() else "Snappy"} MODULE REQUIRED)',
                                         f'find_package({"LIBSODIUM" if is_mac() else "Libsodium"} REQUIRED)',
                                         r'find_package(Gflags MODULE REQUIRED)',
                                         r'#list(APPEND FOLLY_LINK_LIBRARIES ${LIBGFLAGS_LIBRARY})',
                                         r'find_package(Glog CONFIG REQUIRED)',
                                         r'set(FOLLY_HAVE_LIBGLOG ON)',
                                         r'list(APPEND FOLLY_LINK_LIBRARIES glog::glog)',
                                         r'find_package(LIBDWARF)',
                                         r'find_package(LIBIBERTY)',
                                         r'find_package(LIBAIO)',
                                         r'find_package(LIBURING)',
                                         r'find_package(LIBUNWIND)',
                                         r'set(FOLLY_USE_SYMBOLIZER OFF)',
                                         ])

        orig_file3 = os.path.join(src_dir, 'CMake', 'FollyCompilerMSVC.cmake')
        bak_file3 = patch_file(orig_file3,
                               from_texts=[r'list(APPEND FOLLY_LINK_LIBRARIES Iphlpapi.lib Ws2_32.lib)',
                                           r'/std:${MSVC_LANGUAGE_VERSION}',
                                           r'/EHs #',
                                           ],
                               to_texts=[
                                   r'list(APPEND FOLLY_LINK_LIBRARIES Iphlpapi.lib Ws2_32.lib Bcrypt.lib Crypt32.lib)',
                                   r'/DGLOG_NO_ABBREVIATED_SEVERITIES #/std:${MSVC_LANGUAGE_VERSION}',
                                   r'/EHsc #',
                               ])

        orig_file4 = os.path.join(src_dir, 'folly', 'experimental', 'symbolizer', 'Elf.cpp')
        bak_file4 = patch_file(orig_file4,
                               from_texts=[r'#include <folly/portability/SysMman.h>'],
                               to_texts=['#include <folly/portability/SysMman.h>\n'
                                         '#include <folly/portability/Unistd.h>'])

        if is_windows():
            orig_file6 = os.path.join(src_dir, 'CMake', 'FindLibsodium.cmake')
            bak_file6 = patch_file(orig_file6,
                                   from_texts=[r'find_library(LIBSODIUM_LIBRARY NAMES sodium)',
                                               ],
                                   to_texts=[
                                       r'find_library(LIBSODIUM_LIBRARY NAMES sodium libsodium)',
                                   ])

            orig_file7 = os.path.join(src_dir, 'folly', 'memory', 'UninitializedMemoryHacks.h')
            bak_file7 = patch_file(orig_file7,
                                   from_texts=[r'template void std::basic_string<TYPE>::_Eos(std::size_t);',
                                               ],
                                   to_texts=[
                                       r'template void std::basic_string<TYPE>::_Eos(std::size_t) noexcept;',
                                   ])

        os.remove(os.path.join(src_dir, 'folly', 'logging', 'BridgeFromGoogleLogging.cpp'))

        cmakecmd_options = ['-DBUILD_SHARED_LIBS:BOOL=OFF',
                            '-DPYTHON_EXTENSIONS:BOOL=OFF',
                            '-DBUILD_TESTS:BOOL=OFF',
                            '-DBOOST_LINK_STATIC=ON',
                            '-DGFLAGS_USE_TARGET_NAMESPACE:BOOL=ON',
                            '-DFOLLY_LIBRARY_SANITIZE_ADDRESS:BOOL=' + ('ON' if use_asan else 'OFF'),
                            src_dir]

        cmakecmd = get_cmake_cmd_common_part(install_dir, cpp_extention=True, universal=True)
        cmakecmd.extend(cmakecmd_options)
        build_and_install_cmakecmd(cmakecmd, build_dir)

        # if is_mac():
        #     build_dir = create_build_dir(src_dir)
        #     arm64_install_dir = create_arm64_install_dir(src_dir)
        #     try:
        #         cmakecmd = get_cmake_cmd_common_part(arm64_install_dir, cpp_extention=True, arm64_only=True)
        #         cmakecmd_options.extend(['-DFOLLY_HAVE_UNALIGNED_ACCESS_EXITCODE=0',
        #                                  '-DFOLLY_HAVE_LINUX_VDSO_EXITCODE=1',
        #                                  '-DFOLLY_HAVE_WCHAR_SUPPORT_EXITCODE=0',
        #                                  '-DHAVE_VSNPRINTF_ERRORS_EXITCODE=1',
        #                                  ])
        #         cmakecmd.extend(cmakecmd_options)
        #         build_and_install_cmakecmd(cmakecmd, build_dir)
        #         create_universal_binaries(arm64_install_dir, install_dir)
        #     finally:
        #         shutil.rmtree(arm64_install_dir, ignore_errors=False)
    finally:
        shutil.rmtree(build_dir, ignore_errors=False)
        os.replace(bak_file1, orig_file1)
        if is_mac() and macos_min_version().startswith('10.'):
            os.replace(bak_file5, orig_file5)
        os.replace(bak_file2, orig_file2)
        os.replace(bak_file3, orig_file3)
        os.replace(bak_file4, orig_file4)
        if is_windows():
            os.replace(bak_file6, orig_file6)
            os.replace(bak_file7, orig_file7)
        cleanup_git_submodule(src_dir)


def build_glbinding(src_dir: str, install_dir: str):
    build_dir = create_build_dir(src_dir)

    try:
        cmakecmd = get_cmake_cmd_common_part(install_dir, universal=True)
        cmakecmd.extend(['-DOPTION_BUILD_TOOLS:BOOL=OFF',
                         '-DBUILD_SHARED_LIBS:BOOL=OFF',
                         '-DOPTION_BUILD_TESTS:BOOL=OFF',
                         '-DOPTION_BUILD_DOCS:BOOL=OFF',
                         '-DOPTION_BUILD_EXAMPLES:BOOL=OFF',
                         '-DOPTION_BUILD_OWN_KHR_HEADERS:BOOL=ON',
                         src_dir])
        build_and_install_cmakecmd(cmakecmd, build_dir)
    finally:
        shutil.rmtree(build_dir, ignore_errors=False)


def build_libjpeg(src_dir: str, install_dir: str, nasm_dir: str):
    build_dir = create_build_dir(src_dir)

    try:
        cmakecmd = get_cmake_cmd_common_part(install_dir)
        if is_windows():
            cmakecmd.extend(['-DENABLE_SHARED:BOOL=OFF',
                             '-DCMAKE_ASM_NASM_COMPILER:FILEPATH=' + nasm_dir + '\\nasm.exe',
                             '-DWITH_CRT_DLL:BOOL=ON',
                             src_dir])
        elif is_linux():
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

    if is_mac():
        build_dir = create_build_dir(src_dir)
        arm64_install_dir = create_arm64_install_dir(src_dir)

        try:
            cmakecmd = get_cmake_cmd_common_part(arm64_install_dir, arm64_only=True)
            cmakecmd.extend(['-DENABLE_SHARED:BOOL=OFF',
                             '-DCMAKE_ASM_NASM_COMPILER:FILEPATH=' + nasm_dir + '/nasm',
                             src_dir])
            build_and_install_cmakecmd(cmakecmd, build_dir)
            create_universal_binaries(arm64_install_dir=arm64_install_dir, final_install_dir=install_dir)
        finally:
            shutil.rmtree(build_dir, ignore_errors=False)
            shutil.rmtree(arm64_install_dir, ignore_errors=False)


def build_eigen(src_dir: str, install_dir: str):
    build_dir = create_build_dir(src_dir)

    orig_file = bak_file = None
    try:
        orig_file = os.path.join(src_dir, 'CMakeLists.txt')
        bak_file = patch_file(orig_file,
                              from_texts=[r'add_subdirectory(blas',
                                          r'add_subdirectory(lapack',
                                          ],
                              to_texts=[r'set(blas',
                                        r'set(lapack',
                                        ])

        cmakecmd = get_cmake_cmd_common_part(install_dir, universal=True)

        cmakecmd.extend(['-DBUILD_TESTING:BOOL=OFF',
                         '-DEIGEN_BUILD_DOC:BOOL=OFF',
                         ])

        cmakecmd.extend([src_dir])
        build_and_install_cmakecmd(cmakecmd, build_dir)
    finally:
        shutil.rmtree(build_dir, ignore_errors=False)
        os.replace(bak_file, orig_file)


def build_glm(src_dir: str, install_dir: str):
    build_dir = create_build_dir(src_dir)

    try:
        cmakecmd = get_cmake_cmd_common_part(install_dir, universal=True)

        cmakecmd.extend(['-DGLM_BUILD_LIBRARY:BOOL=OFF',
                         '-DGLM_BUILD_TESTS:BOOL=OFF',
                         ])

        cmakecmd.extend([src_dir])
        build_and_install_cmakecmd(cmakecmd, build_dir)

    #     orig_file = os.path.join(install_dir, 'include', 'glm', 'detail', 'type_vec_simd.inl')
    #     patch_file(orig_file,
    #                from_texts=[r"""template<qualifier Q, int E0, int E1, int E2, int E3>
    # struct _swizzle_base1<2, float, Q, E0, E1, E2, E3, true> : public _swizzle_base1<2, float, Q, E0, E1, E2, E3, false> {};
    #
    # template<qualifier Q, int E0, int E1, int E2, int E3>
    # struct _swizzle_base1<2, int, Q, E0, E1, E2, E3, true> : public _swizzle_base1<2, int, Q, E0, E1, E2, E3, false> {};""",
    #                            r'return !compute_vec_equal<float, Q, false, 32, true>::call(v1, v2);',
    #                            r'return !compute_vec_equal<uint, Q, false, 32, true>::call(v1, v2);',
    #                            r'return !compute_vec_equal<int, Q, false, 32, true>::call(v1, v2);',
    #                            ],
    #                to_texts=[r'',
    #                          r'return !compute_vec_equal<L, float, Q, false, 32, true>::call(v1, v2);',
    #                          r'return !compute_vec_equal<L, uint, Q, false, 32, true>::call(v1, v2);',
    #                          r'return !compute_vec_equal<L, int, Q, false, 32, true>::call(v1, v2);',
    #                          ])
    finally:
        shutil.rmtree(build_dir, ignore_errors=False)


def build_pocketfft(src_dir: str, install_dir: str):
    shutil.copy2(os.path.join(src_dir, 'pocketfft_hdronly.h'), os.path.join(install_dir, 'include'))


def build_suitesparse(src_dir: str, install_dir: str):
    build_dir = create_build_dir(src_dir)

    orig_file = bak_file = None
    # orig_file1 = bak_file1 = None
    try:
        orig_file = os.path.join(src_dir, 'SuiteSparse_config', 'cmake_modules', 'SuiteSparseBLAS.cmake')
        bak_file = patch_file(orig_file,
                              from_texts=["""set ( BLA_VENDOR Intel10_64lp )
set ( BLA_SIZEOF_INTEGER 4 )
find_package ( BLAS )""",
                                          ],
                              to_texts=["""set ( BLA_VENDOR Intel10_64lp )
set ( BLA_SIZEOF_INTEGER 4 )
# find_package ( BLAS )
include(libs)""",
                                        ])

        # orig_file1 = os.path.join(src_dir, 'SuiteSparse_config', 'cmake_modules', 'SuiteSparseLAPACK.cmake')
        # bak_file1 = patch_file(orig_file1,
        #                        from_texts=[r'find_package ( LAPACK REQUIRED )',
        #                                    ],
        #                        to_texts=[r'# find_package ( LAPACK REQUIRED )',
        #                                  ])

        shutil.copy2(os.path.join(ext_dir(), 'suitesparse-cmake', 'libs.cmake'),
                     os.path.join(src_dir, 'SuiteSparse_config', 'cmake_modules'))

        cmakecmd = get_cmake_cmd_common_part(install_dir, no_hidden_visibility=True)
        cmakecmd_options = ['-DSUITESPARSE_ENABLE_PROJECTS=suitesparse_config;amd;camd;ccolamd;colamd;cholmod;spqr',
                            '-DGRAPHBLAS_BUILD_STATIC_LIBS:BOOL=ON',
                            '-DSUITESPARSE_USE_OPENMP:BOOL=OFF',
                            '-DSUITESPARSE_USE_CUDA:BOOL=OFF',
                            '-DSUITESPARSE_USE_STRICT:BOOL=ON',
                            '-DBUILD_SHARED_LIBS:BOOL=OFF',
                            '-DBUILD_STATIC_LIBS:BOOL=ON',
                            '-DBLA_STATIC:BOOL=ON',
                            '-DSUITESPARSE_USE_64BIT_BLAS:BOOL=OFF',
                            '-DSUITESPARSE_USE_FORTRAN:BOOL=OFF',
                            ]

        cmakecmd.extend(cmakecmd_options)
        cmakecmd.extend([src_dir])

        build_and_install_cmakecmd(cmakecmd, build_dir)

        if is_mac():
            build_dir = create_build_dir(src_dir)
            arm64_install_dir = create_arm64_install_dir(src_dir)

            try:
                cmakecmd = get_cmake_cmd_common_part(arm64_install_dir, arm64_only=True, no_hidden_visibility=True)

                cmakecmd.extend(cmakecmd_options)
                cmakecmd.extend([src_dir])

                build_and_install_cmakecmd(cmakecmd, build_dir)
                create_universal_binaries(arm64_install_dir, install_dir)
            finally:
                shutil.rmtree(arm64_install_dir, ignore_errors=False)

        # cmakecmd_options = ['-DBLA_STATIC:BOOL=ON',
        #                     '-DNFORTRAN:BOOL=ON',
        #                     '-DNOPENMP:BOOL=ON',
        #                     '-DENABLE_CUDA:BOOL=OFF',
        #                     '-DNSTATIC:BOOL=OFF',
        #                     '-DSUITESPARSE_USE_FORTRAN:BOOL=OFF',
        #                     '-DSUITESPARSE_USE_CUDA:BOOL=OFF',
        #                     '-DSUITESPARSE_USE_OPENMP:BOOL=OFF',
        #                     ]
        #
        # for module in ['SuiteSparse_config', 'COLAMD', 'AMD', 'CCOLAMD', 'CAMD', 'CHOLMOD', 'SPQR']:
        #     module_src_dir = os.path.join(src_dir, module)
        #     build_dir = create_build_dir(module_src_dir)
        #     try:
        #         cmakecmd = get_cmake_cmd_common_part(install_dir, no_hidden_visibility=True)
        #         cmakecmd.extend(cmakecmd_options)
        #         cmakecmd.extend([module_src_dir])
        #         build_and_install_cmakecmd(cmakecmd, build_dir)
        #
        #         if is_mac():
        #             build_dir = create_build_dir(module_src_dir)
        #             arm64_install_dir = create_arm64_install_dir(module_src_dir)
        #             try:
        #                 cmakecmd = get_cmake_cmd_common_part(arm64_install_dir, arm64_only=True,
        #                                                      no_hidden_visibility=True)
        #                 cmakecmd.extend(cmakecmd_options)
        #                 cmakecmd.extend([module_src_dir])
        #                 build_and_install_cmakecmd(cmakecmd, build_dir)
        #                 create_universal_binaries(arm64_install_dir, install_dir, remove_dylib=False)
        #             finally:
        #                 shutil.rmtree(arm64_install_dir, ignore_errors=False)
        #     finally:
        #         shutil.rmtree(build_dir, ignore_errors=False)
    finally:
        shutil.rmtree(build_dir, ignore_errors=False)
        os.replace(bak_file, orig_file)
        # os.replace(bak_file1, orig_file1)
        os.unlink(os.path.join(src_dir, 'SuiteSparse_config', 'cmake_modules', 'libs.cmake'))

        # remove_installed_dynamic_library(install_dir,
        #                                  ['amd', 'camd', 'ccolamd', 'cholmod_cuda', 'cholmod', 'colamd', 'spqr_cuda',
        #                                   'spqr', 'suitesparseconfig'])
        # if is_windows():
        #     cleanup_git_submodule(src_dir)


def build_ceres_solver(src_dir: str, install_dir: str):
    build_dir = create_build_dir(src_dir)

    orig_file = bak_file = None
    orig_file1 = bak_file1 = None
    orig_file2 = bak_file2 = None
    orig_file3 = bak_file3 = None
    orig_file4 = bak_file4 = None
    orig_file5 = bak_file5 = None
    try:
        orig_file = os.path.join(src_dir, 'CMakeLists.txt')
        bak_file = patch_file(orig_file,
                              from_texts=[r'if (HOMEBREW_EXECUTABLE)',
                                          r'EIGEN3_FOUND',
                                          r'if (TARGET gflags)'],
                              to_texts=[r'if (FALSE)',
                                        r'Eigen3_FOUND',
                                        r'if (TARGET gflags::gflags)'])
        orig_file1 = os.path.join(src_dir, 'cmake', 'FindSuiteSparse.cmake')
        bak_file1 = patch_file(orig_file1,
                               from_texts=[r'if (HOMEBREW_EXECUTABLE)',
                                           r'${LAPACK_LIBRARIES}',
                                           r'${BLAS_LIBRARIES}',
                                           r'find_package(BLAS QUIET)',
                                           r'find_package(LAPACK QUIET)',
                                           r'find_package (METIS)',
                                           r'check_symbol_exists (cholmod_metis cholmod.h SuiteSparse_CHOLMOD_USES_METIS)',
                                           r'set(CMAKE_FIND_LIBRARY_PREFIXES "lib" "" "${CMAKE_FIND_LIBRARY_PREFIXES}")',
                                           ],
                               to_texts=[r'if (FALSE)',
                                         r' ',
                                         r' ',
                                         r'set(BLAS_FOUND ON CACHE BOOL "")',
                                         r'set(LAPACK_FOUND ON CACHE BOOL "")',
                                         r'add_library (METIS::METIS IMPORTED INTERFACE)',
                                         r'set(SuiteSparse_CHOLMOD_USES_METIS 1)',
                                         'set(CMAKE_FIND_LIBRARY_PREFIXES "lib" "" "${CMAKE_FIND_LIBRARY_PREFIXES}")\n'
                                         r'set(CMAKE_FIND_LIBRARY_SUFFIXES "_static.lib" "${CMAKE_FIND_LIBRARY_SUFFIXES}")',
                                         ]
                               )
        orig_file2 = os.path.join(src_dir, 'cmake', 'FindGlog.cmake')
        bak_file2 = patch_file(orig_file2,
                               from_texts=[r'if (HOMEBREW_EXECUTABLE)'],
                               to_texts=[r'if (FALSE)'])
        # we build ceres as static lib, so no point to hard link lapack now as we might link to mkl later
        orig_file4 = os.path.join(src_dir, 'internal', 'ceres', 'CMakeLists.txt')
        bak_file4 = patch_file(orig_file4,
                               from_texts=[r' ${LAPACK_LIBRARIES}',
                                           r'add_definitions(-DCERES_SUITESPARSE_VERSION="${SuiteSparse_VERSION}")',
                                           r'list(APPEND CERES_LIBRARY_PUBLIC_DEPENDENCIES gflags)',
                                           ],
                               to_texts=[r' ',
                                         'add_definitions(-DCERES_SUITESPARSE_VERSION="${SuiteSparse_VERSION}")\n'
                                         'add_definitions(-DCERES_METIS_VERSION="${METIS_VERSION}")',
                                         r'list(APPEND CERES_LIBRARY_PUBLIC_DEPENDENCIES gflags::gflags)',
                                         ])
        orig_file5 = os.path.join(src_dir, 'cmake', 'CeresConfig.cmake.in')
        bak_file5 = patch_file(orig_file5,
                               from_texts=[r'EIGEN3_FOUND',
                                           r'if (gflags_FOUND AND TARGET gflags)'],
                               to_texts=[r'Eigen3_FOUND',
                                         r'if (gflags_FOUND AND TARGET gflags::gflags)'])

        cmakecmd = get_cmake_cmd_common_part(install_dir)
        cmakecmd_options = ['-DBUILD_TESTING:BOOL=OFF',
                            '-DSUITESPARSE:BOOL=ON',
                            '-DACCELERATESPARSE:BOOL=' + ('ON' if is_mac() else 'OFF'),
                            '-DUSE_CUDA:BOOL=OFF',
                            '-DEIGENMETIS:BOOL=OFF',
                            '-DBUILD_EXAMPLES:BOOL=OFF',
                            '-DBUILD_BENCHMARKS:BOOL=OFF',
                            '-DBUILD_SHARED_LIBS:BOOL=OFF',
                            '-DGFLAGS_USE_TARGET_NAMESPACE:BOOL=ON',
                            ]

        cmakecmd.extend(cmakecmd_options)
        cmakecmd.extend([src_dir])

        env = {}
        env['MKLROOT'] = os.path.join(intel_sw_dir(), 'mkl', 'latest')
        build_and_install_cmakecmd(cmakecmd, build_dir, additional_env=env)

        if is_mac():
            build_dir = create_build_dir(src_dir)
            arm64_install_dir = create_arm64_install_dir(src_dir)

            try:
                cmakecmd = get_cmake_cmd_common_part(arm64_install_dir, arm64_only=True)

                cmakecmd.extend(cmakecmd_options)
                cmakecmd.extend([src_dir])

                build_and_install_cmakecmd(cmakecmd, build_dir)
                create_universal_binaries(arm64_install_dir, install_dir)
            finally:
                shutil.rmtree(arm64_install_dir, ignore_errors=False)

        # on linux, cmake complains about could not find the fake target SuiteSparse::Partition
        orig_file3 = os.path.join(install_dir, 'lib', 'cmake', 'Ceres', 'CeresTargets.cmake')
        patch_file(orig_file3,
                   from_texts=[r';\$<LINK_ONLY:SuiteSparse::Partition>;\$<LINK_ONLY:METIS::METIS>',
                               ],
                   to_texts=[r'',
                             ]
                   )
    finally:
        shutil.rmtree(build_dir, ignore_errors=False)
        os.replace(bak_file, orig_file)
        os.replace(bak_file1, orig_file1)
        os.replace(bak_file2, orig_file2)
        os.replace(bak_file4, orig_file4)
        os.replace(bak_file5, orig_file5)
        cleanup_git_submodule(src_dir)


def build_libpng(src_dir: str, install_dir: str):
    build_dir = create_build_dir(src_dir)

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
                         '-DPNG_SHARED:BOOL=OFF',
                         '-DPNG_FRAMEWORK:BOOL=OFF'])

        if is_windows():
            print('')
            # cmakecmd.extend(['-DZLIB_INCLUDE_DIR:PATH=' + ext_dir() + '\\zlib\\include',
            #                  '-DZLIB_LIBRARY_RELEASE:FILEPATH=' + ext_dir() + '\\zlib\\lib\\zlibstatic.lib'])
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

        if is_mac():
            build_dir = create_build_dir(src_dir)
            arm64_install_dir = create_arm64_install_dir(src_dir)

            try:
                cmakecmd = get_cmake_cmd_common_part(arm64_install_dir, arm64_only=True)

                cmakecmd.extend(['-DPNG_TESTS:BOOL=OFF',
                                 '-DPNG_SHARED:BOOL=OFF',
                                 '-DPNG_FRAMEWORK:BOOL=OFF'])
                cmakecmd.extend([src_dir])

                build_and_install_cmakecmd(cmakecmd, build_dir)
                create_universal_binaries(arm64_install_dir, install_dir)
            finally:
                shutil.rmtree(arm64_install_dir, ignore_errors=False)
    finally:
        os.replace(bak_file, orig_file)
        if is_mac() and os.path.exists('/usr/include'):
            os.replace(bak_file1, orig_file1)
        if is_mac():
            os.replace(bak_file2, orig_file2)
        shutil.rmtree(build_dir, ignore_errors=False)


def build_openjpeg(src_dir: str, install_dir: str):
    build_dir = create_build_dir(src_dir)

    bak_file = orig_file = None
    try:
        orig_file = os.path.join(src_dir, 'src', 'lib', 'openjp2', 'openjpeg.h')
        bak_file = patch_file(orig_file,
                              from_texts=[r'#define OPENJPEG_H'],
                              to_texts=['#define OPENJPEG_H\n'
                                        '#ifndef OPJ_STATIC\n'
                                        '#define OPJ_STATIC\n'
                                        '#endif\n'])

        cmakecmd = get_cmake_cmd_common_part(install_dir, universal=True)

        cmakecmd.extend(['-DBUILD_STATIC_LIBS:BOOL=ON',
                         '-DBUILD_DOC:BOOL=OFF',
                         '-DBUILD_SHARED_LIBS:BOOL=OFF',
                         '-DBUILD_CODEC:BOOL=OFF',
                         ])

        cmakecmd.extend([src_dir])
        build_and_install_cmakecmd(cmakecmd, build_dir)
    finally:
        shutil.rmtree(build_dir, ignore_errors=False)
        os.replace(bak_file, orig_file)


def build_libwebp(src_dir: str, install_dir: str):
    cmakecmd_options = ['-DWEBP_BUILD_ANIM_UTILS:BOOL=OFF',
                        '-DWEBP_BUILD_CWEBP:BOOL=OFF',
                        '-DWEBP_BUILD_DWEBP:BOOL=OFF',
                        '-DWEBP_BUILD_GIF2WEBP:BOOL=OFF',
                        '-DWEBP_BUILD_IMG2WEBP:BOOL=OFF',
                        '-DWEBP_BUILD_VWEBP:BOOL=OFF',
                        '-DWEBP_BUILD_WEBPINFO:BOOL=OFF',
                        '-DWEBP_BUILD_WEBPMUX:BOOL=OFF',
                        '-DWEBP_BUILD_EXTRAS:BOOL=OFF',
                        '-DWEBP_BUILD_WEBP_JS:BOOL=OFF',
                        ]

    build_dir = create_build_dir(src_dir)
    try:
        cmakecmd = get_cmake_cmd_common_part(install_dir)
        cmakecmd.extend(cmakecmd_options)
        cmakecmd.extend([src_dir])
        build_and_install_cmakecmd(cmakecmd, build_dir)
    finally:
        print('done')
        shutil.rmtree(build_dir, ignore_errors=False)

    if is_mac():
        build_dir = create_build_dir(src_dir)
        arm64_install_dir = create_arm64_install_dir(src_dir)
        try:
            cmakecmd = get_cmake_cmd_common_part(arm64_install_dir, arm64_only=True)
            cmakecmd.extend(cmakecmd_options)
            cmakecmd.extend([src_dir])
            build_and_install_cmakecmd(cmakecmd, build_dir)
            create_universal_binaries(arm64_install_dir, install_dir)
        finally:
            print('done')
            shutil.rmtree(build_dir, ignore_errors=False)
            shutil.rmtree(arm64_install_dir, ignore_errors=False)

    orig_file = os.path.join(install_dir, 'share', 'WebP', 'cmake', 'WebPConfig.cmake')
    patch_file(orig_file,
               from_texts=[r'check_required_components(WebP)',
                           ],
               to_texts=[r'#check_required_components(WebP)',
                         ])


def build_jxrlib(src_dir: str, install_dir: str):
    try:
        orig_file = os.path.join(src_dir, 'Makefile')
        from_texts = [r'CFLAGS=-I. -Icommon/include -I$(DIR_SYS) '
                      r'$(ENDIANFLAG) -D__ANSI__ -DDISABLE_PERF_MEASUREMENT -w $(PICFLAG) -O',
                      r'@python -c ']
        if is_linux():
            to_texts = [r'CFLAGS=-I. -Icommon/include -I$(DIR_SYS) '
                        r'$(ENDIANFLAG) -D__ANSI__ -DDISABLE_PERF_MEASUREMENT -w $(PICFLAG) -O3 -fPIC -mavx '
                        r'-Wno-error=implicit-function-declaration ',
                        r'cp $< $@ # @python -c ']
        else:
            to_texts = [r'CFLAGS=-arch x86_64 -arch arm64 -I. -Icommon/include -I$(DIR_SYS) '
                        r'$(ENDIANFLAG) -D__ANSI__ -DDISABLE_PERF_MEASUREMENT -w $(PICFLAG) -O3 -mavx -mcpu=apple-m1 '
                        r'-Wno-error=implicit-function-declaration '
                        r'-mmacosx-version-min={0}'.format(macos_min_version()),
                        r'cp $< $@ # @python -c ']
        patch_file(orig_file, from_texts=from_texts, to_texts=to_texts)

        orig_file = os.path.join(src_dir, 'image', 'sys', 'strcodec.c')
        patch_file(orig_file,
                   from_texts=[r'ERR CloseWS_File(struct WMPStream** ppWS)'],
                   to_texts=[r"""ERR CreateWS_FileTemp(struct WMPStream** ppWS, char* szFilename, const char* szMode)
{
#ifdef WIN32
    ERR err = WMP_errFileIO;
#else
    ERR err = WMP_errSuccess;
    struct WMPStream* pWS = NULL;

    Call(WMPAlloc((void** )ppWS, sizeof(**ppWS)));
    pWS = *ppWS;

    pWS->Close = CloseWS_File;
    pWS->EOS = EOSWS_File;

    pWS->Read = ReadWS_File;
    pWS->Write = WriteWS_File;
    //pWS->GetLine = GetLineWS_File;

    pWS->SetPos = SetPosWS_File;
    pWS->GetPos = GetPosWS_File;

    int fd = mkstemp(szFilename);
    FailIf(-1 == fd, WMP_errFileIO);
    pWS->state.file.pFile = fdopen(fd, szMode);
    FailIf(NULL == pWS->state.file.pFile, WMP_errFileIO);
#endif

Cleanup:
    return err;
}

ERR CloseWS_File(struct WMPStream** ppWS)"""])

        orig_file = os.path.join(src_dir, 'image', 'encode', 'strenc.c')
        patch_file(orig_file,
                   from_texts=[r"""#else //DPK needs to support ANSI 
                pSC->ppTempFile[i] = (char *)malloc(FILENAME_MAX * sizeof(char));
                if(pSC->ppTempFile[i] == NULL) return ICERR_ERROR;

                if ((pFilename = tmpnam(NULL)) == NULL)
                    return ICERR_ERROR;                
                strcpy(pSC->ppTempFile[i], pFilename);
#endif
                if(CreateWS_File(pSC->ppWStream + i, pFilename, "w+b") != ICERR_OK) return ICERR_ERROR;"""],
                   to_texts=[r"""                if(CreateWS_File(pSC->ppWStream + i, pFilename, "w+b") != ICERR_OK) return ICERR_ERROR;

#else //DPK needs to support ANSI 
                pSC->ppTempFile[i] = (char *)malloc(FILENAME_MAX * sizeof(char));
                if(pSC->ppTempFile[i] == NULL) return ICERR_ERROR;
                pFilename = NULL;
                snprintf(pSC->ppTempFile[i], L_tmpnam, "%s/tmp.XXXXXXXXXX", P_tmpdir);
                if(CreateWS_FileTemp(pSC->ppWStream + i, pSC->ppTempFile[i], "w+b") != ICERR_OK) return ICERR_ERROR;
#endif"""])

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
            subprocess.run(['make', '-j' + str(os.cpu_count()), 'install', 'DIR_INSTALL=' + install_dir],
                           cwd=src_dir, shell=False, check=True)
    finally:
        # if not is_windows():
        #     shutil.rmtree(os.path.join(src_dir, 'build'), ignore_errors=True)
        #     os.replace(bak_file, orig_file)
        cleanup_git_submodule(src_dir)


def build_assimp(src_dir: str, install_dir: str):
    build_dir = create_build_dir(src_dir)

    orig_file = bak_file = None
    orig_file2 = bak_file2 = None
    orig_file3 = bak_file3 = None
    # orig_file_3 = bak_file_3 = None
    # orig_file_4 = bak_file_4 = None
    try:
        orig_file = os.path.join(src_dir, 'include', 'assimp', 'defs.h')
        from_texts = [r'#define AI_MAX_ALLOC(type) ((256U * 1024 * 1024) / sizeof(type))']
        to_texts = [r'#define AI_MAX_ALLOC(type) ((size_t(256) * 1024 * 1024 * 1024) / sizeof(type))']
        bak_file = patch_file(orig_file, from_texts=from_texts, to_texts=to_texts)

        orig_file2 = os.path.join(src_dir, 'code', 'CMakeLists.txt')
        from_texts = [r'-Werror',
                      r'/WX']
        to_texts = [r' ',
                    r' ']
        bak_file2 = patch_file(orig_file2, from_texts=from_texts, to_texts=to_texts)

        orig_file3 = os.path.join(src_dir, 'CMakeLists.txt')
        from_texts = [r'SET (ASSIMP_SOVERSION 5)',
                      r' -lz',
                      r' /WX',
                      ]
        to_texts = [r'SET (ASSIMP_SOVERSION ${ASSIMP_VERSION_MAJOR})',
                    r' ZLIB::ZLIB',
                    r' ',
                    ]
        bak_file3 = patch_file(orig_file3, from_texts=from_texts, to_texts=to_texts)

        os.remove(os.path.join(src_dir, 'cmake-modules', 'FindZLIB.cmake'))

        # if is_mac():
        #     orig_file_3 = os.path.join(src_dir, 'assimpTargets-release.cmake.in')
        #     from_texts = [r'libassimp${ASSIMP_LIBRARY_SUFFIX}@CMAKE_SHARED_LIBRARY_SUFFIX@.@ASSIMP_VERSION_MAJOR@']
        #     to_texts = [r'libassimp${ASSIMP_LIBRARY_SUFFIX}.@ASSIMP_VERSION_MAJOR@@CMAKE_SHARED_LIBRARY_SUFFIX@']
        #     bak_file_3 = patch_file(orig_file_3, from_texts=from_texts, to_texts=to_texts)
        #
        #     orig_file_4 = os.path.join(src_dir, 'assimpTargets-debug.cmake.in')
        #     from_texts = [
        #         r'libassimp${ASSIMP_LIBRARY_SUFFIX}@CMAKE_DEBUG_POSTFIX@@CMAKE_SHARED_LIBRARY_SUFFIX@.'
        #         r'@ASSIMP_VERSION_MAJOR@']
        #     to_texts = [
        #         r'libassimp${ASSIMP_LIBRARY_SUFFIX}@CMAKE_DEBUG_POSTFIX@.@ASSIMP_VERSION_MAJOR@'
        #         r'@CMAKE_SHARED_LIBRARY_SUFFIX@']
        #     bak_file_4 = patch_file(orig_file_4, from_texts=from_texts, to_texts=to_texts)

        cmakecmd = get_cmake_cmd_common_part(install_dir, universal=True)
        cmakecmd.extend(['-DASSIMP_BUILD_ASSIMP_TOOLS:BOOL=OFF',
                         '-DBUILD_SHARED_LIBS:BOOL=OFF',
                         '-DASSIMP_BUILD_FRAMEWORK:BOOL=OFF',
                         '-DASSIMP_BUILD_TESTS:BOOL=OFF',
                         '-DASSIMP_BUILD_ZLIB:BOOL=OFF',
                         '-DASSIMP_HUNTER_ENABLED:BOOL=OFF'])

        # if is_windows():
        #     cmakecmd.extend(['-DZLIB_INCLUDE_DIR:PATH=' + ext_dir() + '\\zlib\\include',
        #                      '-DZLIB_LIBRARY_REL:FILEPATH=' + ext_dir() + '\\zlib\\lib\\zlibstatic.lib'])

        cmakecmd.extend([src_dir])
        build_and_install_cmakecmd(cmakecmd, build_dir)

        # if is_mac():
        #     subprocess.run(['install_name_tool', '-id', '@rpath/libIrrXML.dylib', 'lib/libIrrXML.dylib'],
        #                    cwd=install_dir, shell=False, check=True)
        #     subprocess.run(['install_name_tool', '-change', 'libIrrXML.dylib', '@loader_path/libIrrXML.dylib',
        #                     'lib/libassimp.dylib'],
        #                    cwd=install_dir, shell=False, check=True)

        orig_file_5 = os.path.join(install_dir, 'include', 'assimp', 'types.h')
        patch_file(orig_file_5,
                   from_texts=[r'#   include "../contrib/utf8cpp/source/utf8.h"'],
                   to_texts=[r''])

    finally:
        os.replace(bak_file, orig_file)
        os.replace(bak_file2, orig_file2)
        os.replace(bak_file3, orig_file3)
        # if is_mac():
        #     os.replace(bak_file_3, orig_file_3)
        #     os.replace(bak_file_4, orig_file_4)
        shutil.rmtree(build_dir, ignore_errors=False)
        cleanup_git_submodule(src_dir)


def build_hdf5(src_dir: str, install_dir: str):
    build_dir = create_build_dir(src_dir)

    try:
        cmakecmd = get_cmake_cmd_common_part(install_dir, universal=True)
        cmakecmd.extend(['-DBUILD_TESTING:BOOL=OFF',
                         '-DBUILD_SHARED_LIBS:BOOL=OFF',
                         '-DHDF5_ENABLE_DEPRECATED_SYMBOLS:BOOL=ON',
                         '-DHDF5_ENABLE_Z_LIB_SUPPORT:BOOL=ON',
                         # '-DHDF5_ENABLE_SZIP_SUPPORT:BOOL=1',
                         '-DHDF5_ENABLE_THREADSAFE:BOOL=OFF',
                         '-DHDF5_BUILD_EXAMPLES:BOOL=OFF',
                         '-DHDF5_BUILD_CPP_LIB:BOOL=1',
                         ])

        # if is_windows():
        #     cmakecmd.extend(['-DZLIB_INCLUDE_DIR:PATH=' + ext_dir() + '\\zlib\\include',
        #                      '-DZLIB_LIBRARY_RELEASE:FILEPATH=' + ext_dir() + '\\zlib\\lib\\zlibstatic.lib'])

        cmakecmd.extend([src_dir])
        build_and_install_cmakecmd(cmakecmd, build_dir)
    finally:
        shutil.rmtree(build_dir, ignore_errors=False)


def build_freeimage(src_dir: str, install_dir: str):
    orig_file_3 = None
    bak_file_3 = None
    orig_file_4 = None
    bak_file_4 = None
    orig_file = bak_file = None
    orig_file5 = bak_file5 = None
    orig_file2 = bak_file2 = None
    try:
        orig_file = os.path.join(src_dir, 'fipMakefile.srcs')
        from_texts = [r'Source/LibTIFF4/tif_dir.c ']
        to_texts = [r'Source/LibTIFF4/tif_dir.c Source/LibTIFF4/tif_hash_set.c ']
        bak_file = patch_file(orig_file, from_texts=from_texts, to_texts=to_texts)

        orig_file5 = os.path.join(src_dir, 'Wrapper', 'FreeImagePlus', 'FreeImagePlus.h')
        from_texts = [r'#define WIN32_LEAN_AND_MEAN']
        to_texts = ['#ifndef WIN32_LEAN_AND_MEAN\n#define WIN32_LEAN_AND_MEAN\n#endif']
        bak_file5 = patch_file(orig_file5, from_texts=from_texts, to_texts=to_texts)

        orig_file2 = os.path.join(src_dir, 'Source', 'OpenEXR', 'IlmImf', 'ImfAttribute.cpp')
        from_texts = [r': std::binary_function <const char *, const char *, bool>']
        to_texts = ['']
        bak_file2 = patch_file(orig_file2, from_texts=from_texts, to_texts=to_texts)

        if os.path.exists(os.path.join(src_dir, 'Source', 'LibTIFF4', 'VERSION')):
            os.rename(os.path.join(src_dir, 'Source', 'LibTIFF4', 'VERSION'),
                      os.path.join(src_dir, 'Source', 'LibTIFF4', '__VERSION'))

        if is_windows():
            env = get_vcvars_environment()
            subprocess.run(['MSBuild', 'FreeImage.2017.sln', '/target:FreeImagePlus', '/property:Platform=x64',
                            '/property:Configuration=Release', '/maxcpucount',
                            '/property:PlatformToolset=v143',
                            '/property:WindowsTargetPlatformVersion=' + env['UCRTVERSION']  # like 10.0.16299.0
                            ],
                           cwd=src_dir, shell=True, check=True, env=env)
            shutil.copytree(os.path.join(src_dir, 'Dist', 'x64'),
                            os.path.join(install_dir, 'freeimage'), dirs_exist_ok=True)
            shutil.copytree(os.path.join(src_dir, 'Wrapper', 'FreeImagePlus', 'dist', 'x64'),
                            os.path.join(install_dir, 'freeimage'), dirs_exist_ok=True)
        elif is_linux():
            if use_clang_in_linux():
                env = get_env_for_config_make()
                env.pop('CFLAGS')
                env.pop('CXXFLAGS')
                shutil.copy2(os.path.join(ext_dir(), 'freeimage-makefiles', 'Makefile_fip_clang_linux'), src_dir)
                subprocess.run(['make', '-f', 'Makefile_fip_clang_linux', '-j' + str(os.cpu_count())],
                               cwd=src_dir, shell=False, check=True, env=env)
                subprocess.run(['make', '-f', 'Makefile_fip_clang_linux', '-j' + str(os.cpu_count()), 'install',
                                'PREFIX=' + install_dir],
                               cwd=src_dir, shell=False, check=True, env=env)
                subprocess.run(['make', '-f', 'Makefile_fip_clang_linux', 'clean'],
                               cwd=src_dir, shell=False, check=True, env=env)
            else:
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

                # subprocess.run(['make', '-f', 'Makefile.gnu', '-j' + str(os.cpu_count())],
                #                cwd=src_dir, shell=False, check=True)
                # subprocess.run(['make', '-f', 'Makefile.gnu', '-j' + str(os.cpu_count()), 'install',
                #                 'PREFIX=' + install_dir],
                #                cwd=src_dir, shell=False, check=True)
                # subprocess.run(['make', '-f', 'Makefile.gnu', 'clean'],
                #                cwd=src_dir, shell=False, check=True)
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
            # subprocess.run(['make', '-f', 'Makefile_gnu', '-j' + str(os.cpu_count())],
            #                cwd=src_dir, shell=False, check=True)
            # subprocess.run(['make', '-f', 'Makefile_gnu', '-j' + str(os.cpu_count()), 'install',
            #                 'PREFIX=' + install_dir],
            #                cwd=src_dir, shell=False, check=True)
            # subprocess.run(['make', '-f', 'Makefile_gnu', 'clean'],
            #                cwd=src_dir, shell=False, check=True)
            subprocess.run(['make', '-f', 'Makefile_fip', '-j' + str(os.cpu_count())],
                           cwd=src_dir, shell=False, check=True)
            subprocess.run(['make', '-f', 'Makefile_fip', '-j' + str(os.cpu_count()), 'install',
                            'PREFIX=' + install_dir],
                           cwd=src_dir, shell=False, check=True)
            subprocess.run(['make', '-f', 'Makefile_fip', 'clean'],
                           cwd=src_dir, shell=False, check=True)
    finally:
        os.replace(bak_file, orig_file)
        os.replace(bak_file5, orig_file5)
        os.replace(bak_file2, orig_file2)
        if is_mac():
            os.remove(os.path.join(src_dir, 'Makefile_gnu'))
            os.remove(os.path.join(src_dir, 'Makefile_fip'))
        elif is_linux():
            if use_clang_in_linux():
                os.remove(os.path.join(src_dir, 'Makefile_fip_clang_linux'))
            else:
                os.replace(bak_file_3, orig_file_3)
                os.replace(bak_file_4, orig_file_4)


def build_itk(src_dir: str, install_dir: str):
    build_dir = create_build_dir(src_dir)

    orig_file = bak_file = None
    # orig_file_1 = bak_file_1 = None
    # orig_file4 = bak_file4 = None
    try:
        orig_file = os.path.join(src_dir, 'Modules', 'ThirdParty', 'NIFTI', 'src', 'nifti', 'niftilib', 'nifti1_io.c')
        bak_file = patch_file(orig_file,
                              from_texts=[r'#include <limits.h>'],
                              to_texts=['#include <limits.h>\n#include <stdint.h>'])

        # orig_file = os.path.join(src_dir, 'Modules', 'ThirdParty', 'MetaIO', 'src', 'MetaIO', 'src', 'CMakeLists.txt')
        # bak_file = patch_file(orig_file, from_texts=[r'install(FILES ${headers}'],
        #                       to_texts=['file(GLOB __files "${CMAKE_CURRENT_SOURCE_DIR}/*.h")\n'
        #                                 'set(headers ${headers} ${__files})\n'
        #                                 'install(FILES ${headers}'])

        # orig_file_1 = os.path.join(src_dir, 'Modules', 'ThirdParty', 'VNL', 'src', 'vxl', 'vcl', 'CMakeLists.txt')
        # if is_windows():
        #     bak_file_1 = patch_file(orig_file_1, from_texts=[r'vcl_legacy_aliases.h'],
        #                             to_texts=[r'vcl_legacy_aliases.h vcl_msvc_warnings.h'])

        # orig_file4 = os.path.join(src_dir, 'Modules', 'ThirdParty', 'GDCM', 'src', 'gdcm', 'CMakeLists.txt')
        # bak_file4 = patch_file(orig_file4, from_texts=[r'find_package(OpenJPEG 2.0.0 REQUIRED)'],
        #                        to_texts=[r'find_package(OpenJPEG REQUIRED)'])

        cmakecmd = get_cmake_cmd_common_part(install_dir, universal=True)
        cmakecmd.extend(['-DBUILD_EXAMPLES:BOOL=OFF',
                         '-DBUILD_TESTING:BOOL=OFF',
                         '-DITK_USE_64BITS_IDS:BOOL=ON',
                         '-DITK_FUTURE_LEGACY_REMOVE:BOOL=ON',
                         '-DITK_LEGACY_REMOVE:BOOL=ON',
                         '-DITK_USE_GPU:BOOL=OFF',
                         '-DITK_DOXYGEN_HTML:BOOL=OFF',
                         '-DModule_ITKReview:BOOL=ON',
                         '-DModule_ITKTBB:BOOL=ON',
                         '-DTBB_DIR:PATH=' + tbb_dir(),
                         '-DITK_USE_SYSTEM_DOUBLECONVERSION:BOOL=ON',
                         '-DITK_USE_SYSTEM_EIGEN:BOOL=ON',
                         '-DITK_USE_SYSTEM_HDF5:BOOL=ON',
                         '-DITK_USE_SYSTEM_JPEG:BOOL=ON',
                         '-DITK_USE_SYSTEM_PNG:BOOL=ON',
                         '-DITK_USE_SYSTEM_ZLIB:BOOL=ON',
                         # '-DGDCM_USE_SYSTEM_OPENJPEG:BOOL=ON',

                         '-DModule_MorphologicalContourInterpolation=OFF',  # example how to turn on a remote module
                         ],
                        )

        if is_windows():
            cmakecmd.extend([
                # '-DZLIB_INCLUDE_DIR:PATH=' + ext_dir() + '\\zlib\\include',
                # '-DZLIB_LIBRARY_RELEASE:FILEPATH=' + ext_dir() + '\\zlib\\lib\\zlibstatic.lib',
                '-DHDF5_DIR:PATH=' + ext_build_dir() + '/share/cmake',
            ])
        # else:
        #     cmakecmd.extend(['-DHDF5_DIR:PATH=' + ext_dir() + '/hdf5/share/cmake/hdf5',
        #                      ])

        cmakecmd.extend([src_dir])
        build_and_install_cmakecmd(cmakecmd, build_dir)

        # duplicated call to find_package cause cmake error
        # remove tbb from itk interface to make it work with conda tbb
        orig_file_2 = os.path.join(install_dir, 'lib', 'cmake', 'ITK-5.4', 'Modules', 'ITKTBB.cmake')
        patch_file(orig_file_2,
                   from_texts=[r'find_package(TBB REQUIRED CONFIG)',
                               r'set(ITKTBB_INCLUDE_DIRS',
                               r'set(ITKTBB_LIBRARIES',
                               r'set(TBB_DIR'],
                   to_texts=[r'#find_package(TBB REQUIRED CONFIG)',
                             r'#set(ITKTBB_INCLUDE_DIRS',
                             r'#set(ITKTBB_LIBRARIES',
                             r'#set(TBB_DIR'])

        # ITKZLIB_INCLUDE_DIRS includes
        # /Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk/usr/include
        # which cause conda compilation errors
        # orig_file_3 = os.path.join(install_dir, 'lib', 'cmake', 'ITK-5.3', 'Modules', 'ITKZLIB.cmake')
        # patch_file(orig_file_3,
        #            from_texts=[r'set(ITKZLIB_INCLUDE_DIRS',
        #                        ],
        #            to_texts=[r'#set(ITKZLIB_INCLUDE_DIRS',
        #                      ])

        # shutil.copy2(os.path.join(src_dir, 'CMake', 'ITKInitializeCXXStandard.cmake'),
        #              os.path.join(install_dir, 'lib', 'cmake', 'ITK-5.3', 'ITKInitializeCXXStandard.cmake'))
    finally:
        shutil.rmtree(build_dir, ignore_errors=False)
        os.replace(bak_file, orig_file)
        # # os.replace(bak_file4, orig_file4)
        # if is_windows():
        #     os.replace(bak_file_1, orig_file_1)


def build_vtk(src_dir: str, install_dir: str):
    build_dir = create_build_dir(src_dir)

    bak_file = orig_file = None
    bak_file2 = orig_file2 = None
    bak_file3 = orig_file3 = None
    bak_file4 = orig_file4 = None
    # bak_file5 = orig_file5 = None
    # bak_file6 = orig_file6 = None
    bak_file7 = orig_file7 = None
    try:
        orig_file = os.path.join(src_dir, 'ThirdParty', 'netcdf', 'vtknetcdf', 'CMakeLists.txt')
        bak_file = patch_file(orig_file,
                              from_texts=[r'check_type_size("uint64_t" HAVE_UINT64_T)',
                                          ],
                              to_texts=['check_type_size("uint64_t" HAVE_UINT64_T)\n'
                                        'check_type_size("uintptr_t" HAVE_UINTPTR_T)\n',
                                        ])

        orig_file2 = os.path.join(src_dir, 'ThirdParty', 'eigen', 'vtkeigen', 'CMakeLists.txt')
        bak_file2 = patch_file(orig_file2,
                               from_texts=[r'-std=c++11',
                                           r'set(CMAKE_CXX_STANDARD 11)',
                                           ],
                               to_texts=[f'-std=c++{cpp_standard()}',
                                         f'set(CMAKE_CXX_STANDARD {cpp_standard()})',
                                         ])

        orig_file3 = os.path.join(src_dir, 'CMake', 'vtkCompilerChecks.cmake')
        bak_file3 = patch_file(orig_file3,
                               from_texts=[r'-std=c++11',
                                           r'set(CMAKE_CXX_STANDARD 11)',
                                           ],
                               to_texts=[f'-std=c++{cpp_standard()}',
                                         f'set(CMAKE_CXX_STANDARD {cpp_standard()})',
                                         ])

        orig_file4 = os.path.join(src_dir, 'ThirdParty', 'libproj', 'vtklibproj', 'CMakeLists.txt')
        bak_file4 = patch_file(orig_file4,
                               from_texts=[r'-std=c++11',
                                           r'set(CMAKE_CXX_STANDARD 11)',
                                           r'-Werror -Wall',
                                           ],
                               to_texts=[f'-std=c++{cpp_standard()}',
                                         f'set(CMAKE_CXX_STANDARD {cpp_standard()})',
                                         r'-Wall',
                                         ])

        # if is_windows():
        #     orig_file5 = os.path.join(src_dir, 'Common', 'Core', 'SMP', 'STDThread', 'vtkSMPToolsImpl.txx')
        #     bak_file5 = patch_file(orig_file5,
        #                            from_texts=[r'bool vtkSMPToolsImpl<BackendType::STDThread>::IsParallelScope();', ],
        #                            to_texts=['bool vtkSMPToolsImpl<BackendType::STDThread>::IsParallelScope()\n'
        #                                      '{ return vtkSMPThreadPool::GetInstance().IsParallelScope(); }\n'
        #                                      'template<int> bool __dummyIsParallelScope() { return false; }', ])
        #
        #     orig_file6 = os.path.join(src_dir, 'Common', 'Core', 'SMP', 'STDThread', 'vtkSMPToolsImpl.cxx')
        #     bak_file6 = patch_file(orig_file6,
        #                            from_texts=[r'bool vtkSMPToolsImpl<BackendType::STDThread>::IsParallelScope()', ],
        #                            to_texts=['bool __dummyIsParallelScope<1>()', ])

        if is_linux():
            orig_file7 = os.path.join(src_dir, 'ThirdParty', 'nlohmannjson/vtknlohmannjson/include/vtknlohmann/detail',
                                      'macro_scope.hpp')
            bak_file7 = patch_file(orig_file7,
                                   from_texts=[r'#define JSON_HAS_CPP_17',
                                               ],
                                   to_texts=[r'//#define JSON_HAS_CPP_17',
                                             ])

        cmakecmd = get_cmake_cmd_common_part(install_dir, universal=True)

        cmakecmd.extend(['-DVTK_BUILD_EXAMPLES:BOOL=OFF',
                         '-DBUILD_TESTING:BOOL=OFF',
                         '-DBUILD_SHARED_LIBS:BOOL=OFF',
                         '-DVTK_BUILD_TESTING:STRING=OFF',
                         '-DVTK_DATA_EXCLUDE_FROM_ALL:BOOL=OFF',
                         '-DBUILD_SHARED_LIBS:BOOL=OFF',
                         '-DVTK_MODULE_USE_EXTERNAL_VTK_doubleconversion:BOOL=ON',
                         '-DVTK_MODULE_USE_EXTERNAL_VTK_eigen:BOOL=ON',
                         '-DVTK_MODULE_USE_EXTERNAL_VTK_hdf5:BOOL=' + ('OFF' if is_windows() else 'ON'),
                         '-DVTK_MODULE_USE_EXTERNAL_VTK_jpeg:BOOL=ON',
                         '-DVTK_MODULE_USE_EXTERNAL_VTK_lz4:BOOL=ON',
                         '-DVTK_MODULE_USE_EXTERNAL_VTK_lzma:BOOL=' + ('OFF' if is_windows() else 'ON'),
                         '-DVTK_MODULE_USE_EXTERNAL_VTK_png:BOOL=ON',
                         '-DVTK_MODULE_USE_EXTERNAL_VTK_zlib:BOOL=ON',
                         '-DVTK_LEGACY_REMOVE:BOOL=ON',
                         '-DVTK_MODULE_ENABLE_VTK_IOADIOS2:STRING=NO',
                         '-DVTK_MODULE_ENABLE_VTK_diy2:STRING=NO',
                         '-DVTK_SMP_IMPLEMENTATION_TYPE:STRING=STDThread',
                         '-DTBB_DIR:PATH=' + tbb_dir(),
                         '-DVTK_WRAP_PYTHON:BOOL=OFF',
                         ])

        cmakecmd.extend([src_dir])
        build_and_install_cmakecmd(cmakecmd, build_dir, additional_env=get_tbb_env())

        # orig_file7 = os.path.join(install_dir, 'lib', 'cmake', 'vtk-9.2', 'VTK-targets.cmake')
        # bak_file7 = patch_file(orig_file7,
        #                        from_texts=[r';TBB::tbb',
        #                                    ],
        #                        to_texts=[r'',
        #                                  ])
    finally:
        shutil.rmtree(build_dir, ignore_errors=False)
        os.replace(bak_file, orig_file)
        os.replace(bak_file2, orig_file2)
        os.replace(bak_file3, orig_file3)
        os.replace(bak_file4, orig_file4)
        # if is_windows():
        #     os.replace(bak_file5, orig_file5)
        #     os.replace(bak_file6, orig_file6)
        if is_linux():
            os.replace(bak_file7, orig_file7)


def build_opencv(src_dir: str, src_contrib_dir: str, install_dir: str, conda_build: bool = False):
    build_dir = create_build_dir(src_dir)

    bak_file = orig_file = None
    bak_file2 = orig_file2 = None
    bak_file3 = orig_file3 = None
    bak_file4 = orig_file4 = None
    try:
        orig_file4 = os.path.join(src_dir, 'modules', 'videoio', 'src', 'cap_msmf.cpp')
        bak_file4 = patch_file(orig_file4,
                               from_texts=[r'#include <initguid.h>',
                                           ],
                               to_texts=['#include <initguid.h>\n'
                                         '#include <ks.h>\n',
                                         ])

        if conda_build:
            orig_file = os.path.join(src_dir, 'modules', 'imgcodecs', 'CMakeLists.txt')
            bak_file = patch_file(orig_file,
                                  from_texts=[r'ocv_add_perf_tests()',
                                              ],
                                  to_texts=['include_directories(BEFORE SYSTEM ' + ext_build_dir().encode(
                                      'unicode_escape').decode() + '/include)\n'
                                                                   'ocv_add_perf_tests()\n',
                                            ])

            if is_windows():
                orig_file2 = os.path.join(src_dir, 'cmake', 'OpenCVDetectPython.cmake')
                bak_file2 = patch_file(orig_file2,
                                       from_texts=[r'set(_packages_path "${_path}/Lib/site-packages")'],
                                       to_texts=[r'set(_packages_path "Lib/site-packages")'])

            if is_mac():
                os.rename(os.path.join(ext_build_dir(), 'include', 'tbb'),
                          os.path.join(ext_build_dir(), 'include', '__tbb'))
                os.rename(os.path.join(ext_build_dir(), 'include', 'oneapi'),
                          os.path.join(ext_build_dir(), 'include', '__oneapi'))
        else:
            orig_file = os.path.join(src_dir, 'cmake', 'OpenCVFindMKL.cmake')
            bak_file = patch_file(orig_file,
                                  from_texts=[r'macro(mkl_fail)',
                                              r'set(mkl_lib_find_paths ${MKL_LIB_FIND_PATHS} ${MKL_ROOT_DIR}/lib)',
                                              ],
                                  to_texts=['set(CMAKE_FIND_LIBRARY_SUFFIXES .lib .a ${CMAKE_FIND_LIBRARY_SUFFIXES})\n'
                                            'macro(mkl_fail)\n',
                                            r'set(mkl_lib_find_paths ${MKL_LIB_FIND_PATHS} ${MKL_ROOT_DIR}/lib ${MKL_ROOT_DIR}/../tbb/lib ${MKL_ROOT_DIR}/../tbb/lib/intel64/gcc4.8 ${MKL_ROOT_DIR}/../tbb/lib/intel64/vc14)',
                                            ])

            orig_file2 = os.path.join(src_dir, 'modules', 'calib3d', 'CMakeLists.txt')
            bak_file2 = patch_file(orig_file2,
                                   from_texts=[r'${LAPACK_LIBRARIES}'],
                                   to_texts=[r''])

            orig_file3 = os.path.join(src_dir, 'modules', 'core', 'CMakeLists.txt')
            bak_file3 = patch_file(orig_file3,
                                   from_texts=[r'${LAPACK_LIBRARIES}'],
                                   to_texts=[r''])

        def get_cmakecmd_options(arm64_build: bool = False):
            cmakecmd_options = [
                '-DOPENCV_SKIP_CMAKE_CXX_STANDARD:BOOL=ON',
                '-DHAVE_CXX11:BOOL=ON',
                '-DOPENCV_ENABLE_NONFREE:BOOL=ON',
                '-DOPENCV_FORCE_3RDPARTY_BUILD:BOOL=OFF',
                '-DBUILD_ZLIB:BOOL=OFF',
                '-DBUILD_TIFF:BOOL=OFF',
                '-DBUILD_JASPER:BOOL=OFF',
                '-DBUILD_JPEG:BOOL=OFF',
                '-DBUILD_PNG:BOOL=OFF',
                '-DBUILD_OPENEXR:BOOL=ON',
                '-DBUILD_WEBP:BOOL=OFF',
                '-DBUILD_OPENJPEG:BOOL=OFF',

                '-DBUILD_PROTOBUF:BOOL=OFF',

                '-DWITH_1394:BOOL=OFF',
                '-DWITH_VTK:BOOL=OFF',
                '-DWITH_CUDA:BOOL=OFF',
                '-DWITH_EIGEN:BOOL=ON',
                '-DWITH_FFMPEG:BOOL=ON',
                '-DWITH_GSTREAMER:BOOL=OFF',
                '-DWITH_JASPER:BOOL=OFF',
                '-DWITH_OPENJPEG:BOOL=ON',
                '-DWITH_JPEG:BOOL=ON',
                '-DWITH_WEBP:BOOL=ON',
                '-DWITH_OPENEXR:BOOL=ON',
                '-DWITH_PNG:BOOL=ON',
                '-DWITH_TBB:BOOL=ON',
                '-DWITH_TIFF:BOOL=OFF',
                '-DWITH_OPENCL:BOOL=OFF',
                '-DWITH_OPENCL_SVM:BOOL=OFF',
                '-DWITH_OPENCLAMDFFT:BOOL=OFF',
                '-DWITH_OPENCLAMDBLAS:BOOL=OFF',
                '-DWITH_LAPACK:BOOL=ON',
                # '-DENABLE_NEON:BOOL=' + ('ON' if arm64_build else 'OFF'),
                '-DWITH_IPP:BOOL=' + ('OFF' if arm64_build else 'ON'),
                '-DWITH_MKL:BOOL=' + ('OFF' if arm64_build else 'ON'),
                '-DMKL_WITH_TBB:BOOL=' + ('OFF' if is_windows() or arm64_build else 'ON'),
                # mkl_tbb link with static run lib (/MT)
                '-DMKL_WITH_OPENMP:BOOL=OFF',
                # '-DMKL_LIBRARIES_DONT_HACK:BOOL=' + ('OFF' if conda_build else 'ON'),  # if on lapack check fails
                '-DWITH_PROTOBUF:BOOL=ON',
                '-DWITH_QUIRC:BOOL=OFF',

                '-DBUILD_SHARED_LIBS:BOOL=OFF',
                '-DBUILD_opencv_apps:BOOL=OFF',
                '-DBUILD_opencv_js:BOOL=OFF',
                '-DBUILD_DOCS:BOOL=OFF',
                '-DBUILD_EXAMPLES:BOOL=OFF',
                '-DBUILD_PACKAGE:BOOL=OFF',
                '-DBUILD_PERF_TESTS:BOOL=OFF',
                '-DBUILD_TESTS:BOOL=OFF',
                '-DBUILD_WITH_DEBUG_INFO:BOOL=OFF',
                '-DBUILD_FAT_JAVA_LIB:BOOL=OFF',
                '-DBUILD_JAVA:BOOL=OFF',

                '-DENABLE_PRECOMPILED_HEADERS:BOOL=OFF',

                '-DBUILD_opencv_video:BOOL=ON',
                '-DBUILD_opencv_videoio:BOOL=ON',
                '-DBUILD_opencv_ts:BOOL=OFF',
                '-DBUILD_opencv_dnn:BOOL=OFF',
                '-DBUILD_opencv_world:BOOL=OFF',
                '-DBUILD_opencv_python2:BOOL=OFF',
                '-DBUILD_opencv_python3:BOOL=' + ('ON' if conda_build else 'OFF'),
                '-DPYTHON3_EXECUTABLE=' + sys.executable,
                '-DBUILD_opencv_java:BOOL=OFF',
                '-DBUILD_opencv_calib3d:BOOL=OFF',
                '-DBUILD_opencv_stereo:BOOL=OFF',

                '-DBUILD_opencv_dnn_objdetect:BOOL=OFF',
                '-DBUILD_opencv_hdf:BOOL=OFF',
                '-DBUILD_opencv_matlab:BOOL=OFF',
                '-DBUILD_opencv_sfm:BOOL=OFF',
                '-DBUILD_opencv_videostab:BOOL=ON',
                '-DBUILD_opencv_xfeatures2d:BOOL=ON',
                '-DBUILD_opencv_freetype:BOOL=OFF',

                '-DGFLAGS_USE_TARGET_NAMESPACE:BOOL=ON',
            ]

            if conda_build:
                print('CONDA_PREFIX', os.environ['CONDA_PREFIX'])
                if is_windows():
                    cmakecmd_options.extend([
                        '-DTBB_DIR=' + os.environ['CONDA_PREFIX'] + '/Library/lib/cmake/tbb',
                    ])
                    if not arm64_build:
                        cmakecmd_options.extend([
                            '-DMKL_ROOT_DIR=' + os.environ['CONDA_PREFIX'] + '/Library',
                        ])
                else:
                    cmakecmd_options.extend([
                        '-DTBB_DIR=' + os.environ['CONDA_PREFIX'] + '/lib/cmake/tbb',
                    ])
                    if not arm64_build:
                        cmakecmd_options.extend([
                            '-DMKL_ROOT_DIR=' + os.environ['CONDA_PREFIX'],
                        ])
            else:
                cmakecmd_options.extend([
                    '-DTBB_DIR:PATH=' + tbb_dir(),
                ])
                if not arm64_build:
                    cmakecmd_options.extend([
                        '-DMKL_ROOT_DIR=' + os.path.join(intel_sw_dir(), 'mkl', 'latest'),
                    ])

            if is_windows():
                cmakecmd_options.extend(['-DBUILD_WITH_STATIC_CRT:BOOL=OFF',
                                         '-DWITH_WIN32UI:BOOL=OFF',
                                         '-DOpenJPEG_DIR=' + ext_build_dir() + '\\lib\\openjpeg-2.4',
                                         '-DOPENCV_EXTRA_MODULES_PATH:PATH=' + src_contrib_dir + '\\modules',
                                         ])
            elif is_linux():
                cmakecmd_options.extend(['-DWITH_V4L:BOOL=ON',
                                         '-DWITH_PTHREADS_PF:BOOL=OFF',
                                         '-DOPENCV_EXTRA_MODULES_PATH:PATH=' + src_contrib_dir + '/modules',
                                         ])
            else:
                cmakecmd_options.extend(['-DWITH_PTHREADS_PF:BOOL=OFF',
                                         '-DOPENCV_EXTRA_MODULES_PATH:PATH=' + src_contrib_dir + '/modules',
                                         ])

            return cmakecmd_options

        cmakecmd = get_cmake_cmd_common_part(install_dir)
        if is_windows():
            for idx, cmd in enumerate(cmakecmd):
                if cmd.startswith('-DCMAKE_CXX_FLAGS:'):
                    cmakecmd[idx] = cmd + ' /DWIN32_LEAN_AND_MEAN'
        cmakecmd.extend(get_cmakecmd_options(arm64_build=False))
        # print(cmakecmd)
        cmakecmd.extend([src_dir])

        if conda_build and is_windows():
            env = get_enviroment_from_shell_script(os.path.join(os.environ['CONDA_PREFIX'], 'condabin',
                                                                'conda.bat'),
                                                   para='activate',
                                                   start_env=get_vcvars_environment(),
                                                   remove_conda_from_path=False)
            build_and_install_cmakecmd(cmakecmd, build_dir, additional_env=env)
        else:
            print()
            build_and_install_cmakecmd(cmakecmd, build_dir)

        if is_mac() and not conda_build:
            build_dir = create_build_dir(src_dir)
            arm64_install_dir = create_arm64_install_dir(src_dir)
            try:
                cmakecmd = get_cmake_cmd_common_part(arm64_install_dir, arm64_only=True)
                cmakecmd.extend(get_cmakecmd_options(arm64_build=True))
                # print(cmakecmd)
                cmakecmd.extend([src_dir])
                build_and_install_cmakecmd(cmakecmd, build_dir)
                create_universal_binaries(arm64_install_dir, install_dir)
            finally:
                shutil.rmtree(arm64_install_dir, ignore_errors=False)

        if not conda_build:
            if is_windows():
                orig_file_2 = os.path.join(install_dir, 'x64', 'vc17', 'staticlib', 'OpenCVModules.cmake')
            else:
                orig_file_2 = os.path.join(install_dir, 'lib', 'cmake', 'opencv4', 'OpenCVModules.cmake')

            if is_mac():
                patch_file(orig_file_2,
                           from_texts=[r';\$<LINK_ONLY:tbb>',
                                       r'\$<LINK_ONLY:tbb>;',
                                       r';\$<LINK_ONLY:ittnotify>"',
                                       ],
                           to_texts=[r'',
                                     r'',
                                     r';\$<LINK_ONLY:ittnotify>;${OpenCV_INSTALL_PATH}/lib/opencv4/3rdparty/libtegra_hal.a"',
                                     ])
            else:
                patch_file(orig_file_2,
                           from_texts=[r';\$<LINK_ONLY:tbb>',
                                       r'\$<LINK_ONLY:tbb>;',
                                       ],
                           to_texts=[r'',
                                     r'',
                                     ])
    finally:
        shutil.rmtree(build_dir, ignore_errors=False)
        os.replace(bak_file, orig_file)
        os.replace(bak_file4, orig_file4)
        if conda_build and is_windows():
            os.replace(bak_file2, orig_file2)
        if conda_build and is_mac():
            os.rename(os.path.join(ext_build_dir(), 'include', '__tbb'),
                      os.path.join(ext_build_dir(), 'include', 'tbb'))
            os.rename(os.path.join(ext_build_dir(), 'include', '__oneapi'),
                      os.path.join(ext_build_dir(), 'include', 'oneapi'))
        if not conda_build:
            os.replace(bak_file2, orig_file2)
            os.replace(bak_file3, orig_file3)


def build_rocksdb(src_dir: str, install_dir: str):
    build_dir = create_build_dir(src_dir)

    bak_file = orig_file = None
    try:
        orig_file = os.path.join(src_dir, 'CMakeLists.txt')
        bak_file = patch_file(orig_file,
                              from_texts=[
                                  r'NONONO___CMAKE_EXE_LINKER_FLAGS' if is_linux() else r'set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--copy-dt-needed-entries")',
                                  r'find_package(TBB REQUIRED)',
                                  r'find_package(zstd REQUIRED)',
                              ],
                              to_texts=[
                                  r'#set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--copy-dt-needed-entries")',
                                  'find_package(TBB REQUIRED)\n'
                                  r'add_library(TBB::TBB ALIAS TBB::tbb)',
                                  'find_package(zstd REQUIRED)\n'
                                  r'add_library(zstd::zstd ALIAS zstd::libzstd_static)',
                              ])

        os.rename(os.path.join(src_dir, 'cmake', 'modules', 'FindTBB.cmake'),
                  os.path.join(src_dir, 'cmake', 'modules', '__FindTBB.cmake'))
        os.rename(os.path.join(src_dir, 'cmake', 'modules', 'Findzstd.cmake'),
                  os.path.join(src_dir, 'cmake', 'modules', '__Findzstd.cmake'))

        cmakecmd_options = ['-DWITH_SNAPPY:BOOL=ON',
                            '-DWITH_LZ4:BOOL=ON',
                            '-DWITH_ZSTD:BOOL=ON',
                            '-DROCKSDB_BUILD_SHARED:BOOL=OFF',
                            '-DROCKSDB_SKIP_THIRDPARTY:BOOL=ON',
                            '-DWITH_GFLAGS:BOOL=ON',
                            '-DWITH_TBB:BOOL=ON',
                            '-DUSE_COROUTINES:BOOL=OFF',
                            '-DUSE_FOLLY:BOOL=ON',
                            '-DROCKSDB_INSTALL_ON_WINDOWS:BOOL=ON',
                            '-DGFLAGS_USE_TARGET_NAMESPACE:BOOL=ON',
                            '-DFAIL_ON_WARNINGS:BOOL=OFF',
                            ]

        cmakecmd = get_cmake_cmd_common_part(install_dir)
        cmakecmd.extend(cmakecmd_options)
        cmakecmd.extend(['-DPORTABLE=haswell',
                         ])
        cmakecmd.extend([src_dir])
        build_and_install_cmakecmd(cmakecmd, build_dir)

        if is_mac():
            build_dir = create_build_dir(src_dir)
            arm64_install_dir = create_arm64_install_dir(src_dir)
            try:
                cmakecmd = get_cmake_cmd_common_part(arm64_install_dir, arm64_only=True)
                cmakecmd.extend(cmakecmd_options)
                cmakecmd.extend(['-DPORTABLE=1',
                                 ])
                cmakecmd.extend([src_dir])
                build_and_install_cmakecmd(cmakecmd, build_dir)
                create_universal_binaries(arm64_install_dir, install_dir)
            finally:
                shutil.rmtree(arm64_install_dir, ignore_errors=False)
    finally:
        shutil.rmtree(build_dir, ignore_errors=False, onerror=handleRemoveReadonly)
        os.replace(bak_file, orig_file)
        os.rename(os.path.join(src_dir, 'cmake', 'modules', '__FindTBB.cmake'),
                  os.path.join(src_dir, 'cmake', 'modules', 'FindTBB.cmake'))
        os.rename(os.path.join(src_dir, 'cmake', 'modules', '__Findzstd.cmake'),
                  os.path.join(src_dir, 'cmake', 'modules', 'Findzstd.cmake'))
        print()


def build_llfio(src_dir: str, install_dir: str):
    build_dir = create_build_dir(src_dir)

    # bak_file = orig_file = None
    try:
        # orig_file = os.path.join(src_dir, 'include', 'llfio', 'v2.0', 'detail', 'impl', 'map_handle.ipp')
        # bak_file = patch_file(orig_file,
        #                       from_texts=[
        #                           r'auto *p = *it;',
        #                       ],
        #                       to_texts=[
        #                           r'auto *p = &(*it);',
        #                       ])

        cmakecmd = get_cmake_cmd_common_part(install_dir, universal=True)
        cmakecmd.extend(['-DLLFIO_FORCE_NETWORKING_OFF:BOOL=ON',
                         '-DLLFIO_USE_EXPERIMENTAL_SG14_STATUS_CODE:BOOL=OFF',
                         '-DLLFIO_FORCE_COROUTINES_OFF:BOOL=ON',
                         '-DLLFIO_FORCE_CONCEPTS_OFF:BOOL=ON',
                         '-DLLFIO_FORCE_OPENSSL_OFF:BOOL=ON',
                         '-DPROJECT_IS_DEPENDENCY:BOOL=ON',
                         ])

        cmakecmd.extend([src_dir])
        build_and_install_cmakecmd(cmakecmd, build_dir, ninja_para='install.sl')
        if os.path.exists(os.path.join(build_dir, 'install')):
            shutil.copytree(os.path.join(build_dir, 'install'),
                            os.path.join(ext_build_dir()),
                            dirs_exist_ok=True)
    finally:
        shutil.rmtree(build_dir, ignore_errors=False, onerror=handleRemoveReadonly)
        # os.replace(bak_file, orig_file)
        print()


def build_conda_zimg(src_dir: str, install_dir: str):
    build_dir = create_build_dir(src_dir)

    try:
        if is_mac():
            os.rename(os.path.join(ext_build_dir(), 'include', 'tbb'),
                      os.path.join(ext_build_dir(), 'include', '__tbb'))
            os.rename(os.path.join(ext_build_dir(), 'include', 'oneapi'),
                      os.path.join(ext_build_dir(), 'include', '__oneapi'))

        cmakecmd = get_cmake_cmd_common_part(install_dir)

        if is_windows():
            env = get_enviroment_from_shell_script(os.path.join(os.environ['CONDA_PREFIX'], 'condabin',
                                                                'conda.bat'),
                                                   para='activate',
                                                   start_env=get_vcvars_environment(),
                                                   remove_conda_from_path=False)
        else:
            env = os.environ.copy()
        env['PREFIX'] = env['CONDA_PREFIX']

        cmakecmd.extend([src_dir])
        build_and_install_cmakecmd(cmakecmd, build_dir, additional_env=env)
    finally:
        print('done')
        shutil.rmtree(build_dir, ignore_errors=False)
        if is_mac():
            os.rename(os.path.join(ext_build_dir(), 'include', '__tbb'),
                      os.path.join(ext_build_dir(), 'include', 'tbb'))
            os.rename(os.path.join(ext_build_dir(), 'include', '__oneapi'),
                      os.path.join(ext_build_dir(), 'include', 'oneapi'))


def build_ospray(src_dir: str, install_dir: str):
    build_dir = create_build_dir(src_dir)

    try:
        cmakecmd = get_cmake_cmd_common_part(install_dir, use_ninja=False)
        cmakecmd.extend(['-DDOWNLOAD_ISPC=ON',
                         '-DDOWNLOAD_TBB=OFF',
                         '-DBUILD_EMBREE_FROM_SOURCE=OFF',
                         '-DBUILD_GLFW=ON',
                         '-DBUILD_OIDN_FROM_SOURCE=OFF',
                         ])

        cmakecmd.extend([os.path.join(src_dir, 'scripts', 'superbuild')])
        build_and_install_cmakecmd(cmakecmd, build_dir, use_ninja=False, use_cmake=True)
    finally:
        shutil.rmtree(build_dir, ignore_errors=False)


def build_ants(src_dir: str, install_dir: str):
    build_dir = create_build_dir(src_dir)

    try:
        cmakecmd = get_cmake_cmd_common_part(install_dir, use_ninja=False)

        cmakecmd.extend([src_dir])
        # build_cmakecmd(cmakecmd, build_dir, use_ninja=False)
        subprocess.run(['make', 'install'],
                       cwd=os.path.join(build_dir, 'ANTS-build'), shell=False, check=True)
    finally:
        print('done')
        shutil.rmtree(build_dir, ignore_errors=False)


def build_skia(src_dir: str, install_dir: str):
    try:
        subprocess.run(['python', 'tools/git-sync-deps'],
                       cwd=src_dir, shell=False, check=True)
        subprocess.run(['bin/gn', 'gen', 'out/Static',
                        "--args=is_official_build=true skia_use_libjpeg_turbo_decode=false skia_use_libjpeg_turbo_encode=false skia_use_libpng_decode=false skia_use_libpng_encode=false skia_use_libwebp_decode=false skia_use_libwebp_encode=false skia_use_icu=false skia_use_harfbuzz=false skia_use_fontconfig=false skia_use_expat=false skia_use_freetype=false skia_use_gl=false skia_use_x11=false skia_enable_gpu=false"],
                       cwd=src_dir, shell=False, check=True)
        subprocess.run([get_ninja_binary(), '-C', 'out/Static'],
                       cwd=src_dir, shell=False, check=True)
        skia_include_dir = os.path.join(install_dir, 'include', 'skia', 'include')
        if os.path.exists(skia_include_dir):
            shutil.rmtree(skia_include_dir, ignore_errors=False)
        shutil.copytree(os.path.join(src_dir, 'include'), skia_include_dir)
        skia_lib_dir = os.path.join(install_dir, 'lib', 'skia')
        if os.path.exists(skia_lib_dir):
            shutil.rmtree(skia_lib_dir, ignore_errors=False)
        glob_copy(os.path.join(src_dir, 'out', 'Static', '*.a'), skia_lib_dir)
    finally:
        print('done')


def build_jansson(src_dir: str, install_dir: str):
    build_dir = create_build_dir(src_dir)

    try:
        cmakecmd = get_cmake_cmd_common_part(install_dir, universal=True)
        cmakecmd.extend(['-DJANSSON_STATIC_CRT:BOOL=OFF',
                         '-DJANSSON_EXAMPLES:BOOL=OFF',
                         ])

        cmakecmd.extend([src_dir])
        build_and_install_cmakecmd(cmakecmd, build_dir)
    finally:
        shutil.rmtree(build_dir, ignore_errors=False)


def build_pcre(src_dir: str, install_dir: str):
    build_dir = create_build_dir(src_dir)

    try:
        cmakecmd = get_cmake_cmd_common_part(install_dir, universal=True)
        cmakecmd.extend(['-DBUILD_STATIC_LIBS:BOOL=ON',
                         ])

        cmakecmd.extend([src_dir])
        build_and_install_cmakecmd(cmakecmd, build_dir)
    finally:
        shutil.rmtree(build_dir, ignore_errors=False)


def build_fizz(src_dir: str, install_dir: str):
    build_dir = create_build_dir(src_dir)

    bak_file = orig_file = None
    try:
        orig_file = os.path.join(src_dir, 'CMakeLists.txt')
        bak_file = patch_file(orig_file,
                              from_texts=[
                                  r'list(APPEND FIZZ_SHINY_DEPENDENCIES gflags)',
                              ],
                              to_texts=[
                                  'list(APPEND FIZZ_SHINY_DEPENDENCIES gflags)\n'
                                  'add_library(gflags::gflags ALIAS gflags)',
                              ])

        cmakecmd = get_cmake_cmd_common_part(install_dir, universal=True)
        cmakecmd.extend(['-DFIZZ_BUILD_AEGIS:BOOL=OFF',
                         '-DBUILD_TESTS:BOOL=OFF',
                         ])

        cmakecmd.extend([src_dir])
        build_and_install_cmakecmd(cmakecmd, build_dir)
    finally:
        shutil.rmtree(build_dir, ignore_errors=False)
        os.replace(bak_file, orig_file)
        print()


def build_mvfst(src_dir: str, install_dir: str):
    build_dir = create_build_dir(src_dir)

    bak_file = orig_file = None
    bak_file1 = orig_file1 = None
    try:
        orig_file = os.path.join(src_dir, 'CMakeLists.txt')
        bak_file = patch_file(orig_file,
                              from_texts=[
                                  r'list(APPEND GFLAG_DEPENDENCIES gflags)',
                                  r'iostreams',
                                  r'date_time',
                              ],
                              to_texts=[
                                  'list(APPEND GFLAG_DEPENDENCIES gflags)\n'
                                  'add_library(gflags::gflags ALIAS gflags)',
                                  r'',
                                  r'',
                              ])

        orig_file1 = os.path.join(src_dir, 'cmake', 'mvfst-config.cmake.in')
        bak_file1 = patch_file(orig_file1,
                               from_texts=[
                                   r'find_dependency(Boost COMPONENTS iostreams system thread filesystem regex context)',
                               ],
                               to_texts=[
                                   r'find_dependency(Boost COMPONENTS system thread filesystem regex context)',
                               ])

        cmakecmd = get_cmake_cmd_common_part(install_dir, universal=True)

        cmakecmd.extend([src_dir])
        build_and_install_cmakecmd(cmakecmd, build_dir)
    finally:
        shutil.rmtree(build_dir, ignore_errors=False)
        os.replace(bak_file, orig_file)
        os.replace(bak_file1, orig_file1)
        print()


def build_wangle(src_dir: str, install_dir: str):
    build_dir = create_build_dir(src_dir)

    bak_file = orig_file = None
    try:
        orig_file = os.path.join(src_dir, 'CMakeLists.txt')
        bak_file = patch_file(orig_file,
                              from_texts=[
                                  r'find_package(LibEvent MODULE REQUIRED)',
                              ],
                              to_texts=[
                                  'add_library(gflags::gflags ALIAS gflags)\n'
                                  'find_package(LibEvent MODULE REQUIRED)',
                              ])

        cmakecmd = get_cmake_cmd_common_part(install_dir, universal=True)
        cmakecmd.extend(['-DBUILD_TESTS:BOOL=OFF',
                         ])

        cmakecmd.extend([src_dir])
        build_and_install_cmakecmd(cmakecmd, build_dir)
    finally:
        shutil.rmtree(build_dir, ignore_errors=False)
        os.replace(bak_file, orig_file)
        print()


def build_proxygen(src_dir: str, install_dir: str):
    build_dir = create_build_dir(src_dir)

    bak_file = orig_file = None
    bak_file1 = orig_file1 = None
    bak_file2 = orig_file2 = None
    bak_file3 = orig_file3 = None
    bak_file4 = orig_file4 = None
    bak_file5 = orig_file5 = None
    try:
        orig_file = os.path.join(src_dir, 'CMakeLists.txt')
        bak_file = patch_file(orig_file,
                              from_texts=[
                                  r'list(APPEND GFLAG_DEPENDENCIES gflags)',
                                  r'find_program(PROXYGEN_PYTHON python3)',
                                  r'-Wextra',
                                  r'iostreams',
                              ],
                              to_texts=[
                                  'list(APPEND GFLAG_DEPENDENCIES gflags)\n'
                                  'add_library(gflags::gflags ALIAS gflags)',
                                  r'find_program(PROXYGEN_PYTHON python)' if is_windows() else r'find_program(PROXYGEN_PYTHON python3)',
                                  r'' if is_windows() else r'-Wextra',
                                  r'',
                              ])

        orig_file4 = os.path.join(src_dir, 'proxygen', 'lib', 'CMakeLists.txt')
        bak_file4 = patch_file(orig_file4,
                               from_texts=[
                                   r"""${PROXYGEN_FBCODE_ROOT}
        ${PROXYGEN_GENERATED_ROOT}/proxygen/lib/http""",
                                   r'Boost::boost',
                                   r'Boost::iostreams',
                                   r'-lz',
                               ],
                               to_texts=[
                                   "${PROXYGEN_FBCODE_ROOT}\n${PROXYGEN_GENERATED_ROOT}/proxygen/lib/http\n${PROXYGEN_GPERF}",
                                   r'',
                                   r'',
                                   r'',
                               ])

        if is_windows():
            orig_file1 = os.path.join(src_dir, 'cmake', 'FindZstd.cmake')
            bak_file1 = patch_file(orig_file1,
                                   from_texts=[
                                       r"""find_library(ZSTD_LIBRARIES
   NAMES zstd""",
                                       r'if("${ZSTD_LIBRARIES}" MATCHES ".*.a$")',
                                   ],
                                   to_texts=[
                                       r"""find_library(ZSTD_LIBRARIES
   NAMES zstd zstd_static""",
                                       r'if(TRUE)',
                                   ])

            orig_file2 = os.path.join(src_dir, 'proxygen', 'external', 'CMakeLists.txt')
            bak_file2 = patch_file(orig_file2,
                                   from_texts=[
                                       r'"-Wno-implicit-fallthrough"',
                                   ],
                                   to_texts=[
                                       r'',
                                   ])

            orig_file3 = os.path.join(src_dir, 'proxygen', 'lib', 'services', 'RequestWorkerThread.cpp')
            bak_file3 = patch_file(orig_file3,
                                   from_texts=[
                                       r'sigset_t ss;',
                                       r'PCHECK(pthread_sigmask(SIG_BLOCK, &ss, nullptr) == 0);',
                                   ],
                                   to_texts=[
                                       "#ifndef _MSC_VER\nsigset_t ss;",
                                       "PCHECK(pthread_sigmask(SIG_BLOCK, &ss, nullptr) == 0);\n#endif",
                                   ])

        orig_file5 = os.path.join(src_dir, 'proxygen', 'httpserver', 'samples', 'hq', 'ConnIdLogger.h')
        bak_file5 = patch_file(orig_file5,
                               from_texts=[
                                   r'const struct ::tm* tm_time,',
                                   r'tm_time);',
                               ],
                               to_texts=[
                                   r'const google::LogMessageTime& tm_time,',
                                   "&(tm_time.tm()));",
                               ])

        cmakecmd = get_cmake_cmd_common_part(install_dir, universal=True)
        if is_windows():
            cmakecmd.extend([f'-DCMAKE_PROGRAM_PATH={atlas_util_dir()};{os.path.dirname(sys.executable)}',
                             f'-DBUILD_SAMPLES:BOOL=OFF',
                             ])
        else:
            cmakecmd.extend([f'-DCMAKE_PROGRAM_PATH={os.path.dirname(sys.executable)}',
                             ])

        cmakecmd.extend([src_dir])

        build_and_install_cmakecmd(cmakecmd, build_dir)
    finally:
        shutil.rmtree(build_dir, ignore_errors=False)
        os.replace(bak_file, orig_file)
        os.replace(bak_file4, orig_file4)
        if is_windows():
            os.replace(bak_file1, orig_file1)
            os.replace(bak_file2, orig_file2)
            os.replace(bak_file3, orig_file3)
        os.replace(bak_file5, orig_file5)
        print()


def build_libs(libs: OrderedDict, use_asan: bool):
    # print('extDIR:', ext_dir())
    # print('srcPackageDIR:', src_package_dir())

    remove_path_contains('miniconda')
    remove_path_contains('anaconda')
    print('PATH:', os.environ['PATH'])

    if is_windows():
        os.environ['HOME'] = os.path.expanduser("~")

    print('HOME:', os.environ['HOME'])

    for lib_name, build_lib in libs.items():
        if not build_lib:
            continue

        if lib_name == 'cmake':
            install_cmake()

        if lib_name == 'ninja':
            install_ninja()

        if lib_name == 'curl':
            if is_windows():
                unpack_tool_to_target_dir(src_package_dir(), 'curl*win*')

        if lib_name == 'make-cmake-pathlist':
            with open(os.path.join(ext_build_dir(), 'PathList.cmake'), mode='w', encoding='utf-8') as file:
                file.write('# Set PATH for CMake\n')
                file.write(f'set(QT_VERSION {qt_ver()})\n')
                if is_windows():
                    file.write('set(QT_HOST_PATH "{0}")\n'.format(qt_base_dir().replace("\\", "/")))
                else:
                    file.write(f'set(QT_HOST_PATH {qt_base_dir()})\n')

        if lib_name == 'qt':
            print(f'Qt {qt_ver()} in {qt_base_dir()}')
            # if is_windows():
            #     # patch Qt, not necessary for qt6
            #     orig_file = os.path.join(qt_base_dir(), 'include', 'QtCore', 'qglobal.h')
            #     bak_file = os.path.join(qt_base_dir(), 'include', 'QtCore', 'qglobal.h.bak')
            #     if not os.path.exists(bak_file):
            #         os.rename(orig_file, bak_file)
            #         with open(bak_file, mode='r', encoding='utf-8') as f:
            #             from_lines = f.readlines()
            #         with open(orig_file, mode='w', encoding='utf-8') as f:
            #             to_lines = []
            #             for line in from_lines:
            #                 line = line.replace(
            #                     r'#if defined(__cpp_variable_templates) && __cpp_variable_templates >= 201304 // C++14',
            #                     r'#if defined(_MSC_VER) || '
            #                     r'defined(__cpp_variable_templates) && __cpp_variable_templates >= 201304 // C++14')
            #                 f.write(line)
            #                 to_lines.append(line)
            #         print(''.join(list(difflib.unified_diff(from_lines, to_lines, fromfile=orig_file, tofile='<new>'))))

            # patch installer framework
            pattern_bytes = b'Mozilla/5.0'
            replace_bytes = b'Mozilla/590'
            file = os.path.join(qt_installer_framework_bin_dir(), 'installerbase')
            if is_windows():
                file += '.exe'
            with open(file, 'r+b') as f:
                with mmap.mmap(f.fileno(), 0) as mm:
                    all_indexes = []
                    index = mm.find(pattern_bytes)
                    while index != -1:
                        all_indexes.append(index)
                        index = mm.find(pattern_bytes, index + len(pattern_bytes))
                    if not all_indexes:
                        print("Pattern not found in {0}, skip patching.".format(file))
                    else:
                        # make backup
                        shutil.copy2(file, file + '.bak')
                        for index in all_indexes:
                            mm[index:index + len(replace_bytes)] = replace_bytes
                        mm.flush()
                        print("{0} successfully patched at {1} places.".format(file, len(all_indexes)))

        if lib_name == 'zlib':
            package_name = find_src_package_with_glob(os.path.join(src_package_dir(), 'zlib*'))
            src_dir = get_package_top_level_folder(package_name, ext_dir())
            if not os.path.exists(src_dir):
                remove_old_src_folder_with_glob(os.path.join(ext_dir(), 'zlib*'))
                unpack_file_to_folder(package_name, ext_dir())
                assert os.path.exists(src_dir)
            build_zlib(src_dir, ext_build_dir())

        if lib_name == 'ffmpeg':
            install_ffmpeg()
            if is_windows() or is_linux():
                shutil.copy2(get_ffmpeg_binary(), ext_build_dir())

        if lib_name == 'boost':
            package_name = find_src_package_with_glob(os.path.join(src_package_dir(), 'boost*'))
            src_dir = get_package_top_level_folder(package_name, ext_dir())
            if not os.path.exists(src_dir):
                remove_old_src_folder_with_glob(os.path.join(ext_dir(), 'boost*'))
                clean_boost(ext_build_dir())
                unpack_file_to_folder(package_name, ext_dir())
                assert os.path.exists(src_dir)
            build_boost(src_dir, ext_build_dir())

        if lib_name == 'tbb':
            if is_mac():
                src_dir = os.path.join(ext_dir(), 'oneTBB')
                build_tbb(src_dir, ext_build_dir())

        if lib_name == 'eigen':
            src_dir = os.path.join(ext_dir(), 'eigen')
            build_eigen(src_dir, ext_build_dir())

        if lib_name == 'pybind11':
            print('pybind11')

        if lib_name == 'glm':
            src_dir = os.path.join(ext_dir(), 'glm')
            build_glm(src_dir, ext_build_dir())

        if lib_name == 'magic_enum':
            print('magic_enum')

        if lib_name == 'pocketfft':
            src_dir = os.path.join(ext_dir(), 'pocketfft')
            build_pocketfft(src_dir, ext_build_dir())

        if lib_name == 'googletest':
            print('googletest')

        if lib_name == 'cpuinfo':
            src_dir = os.path.join(ext_dir(), 'cpuinfo')
            build_cpuinfo(src_dir, ext_build_dir())

        if lib_name == 'gflags':
            gflags_src_dir = os.path.join(ext_dir(), 'gflags')
            build_gflags(gflags_src_dir, ext_build_dir())

        if lib_name == 'glog':
            src_dir = os.path.join(ext_dir(), 'glog')
            build_glog(src_dir, ext_build_dir())

        if lib_name == 'benchmark':
            src_dir = os.path.join(ext_dir(), 'benchmark')
            build_benchmark(src_dir, ext_build_dir())

        if lib_name == 'openssl':
            package_name = find_src_package_with_glob(os.path.join(src_package_dir(), 'openssl*'))
            src_dir = get_package_top_level_folder(package_name, ext_dir())
            if not os.path.exists(src_dir):
                remove_old_src_folder_with_glob(os.path.join(ext_dir(), 'openssl*'))
                unpack_file_to_folder(package_name, ext_dir())
                assert os.path.exists(src_dir)
            if is_windows():
                nasm_dir = unpack_tool_to_target_dir(src_package_dir(), 'nasm*win64*', 'nasm-*')
            else:
                nasm_dir = ''  # does not need
            build_openssl(src_dir, ext_build_dir(), nasm_dir=nasm_dir)

        if lib_name == 'grpc':
            src_dir = os.path.join(ext_dir(), 'grpc')
            if is_windows():
                nasm_dir = unpack_tool_to_target_dir(src_package_dir(), 'nasm*win64*', 'nasm-*')
            else:
                nasm_dir = ''  # does not need
            build_grpc(src_dir, ext_build_dir(), nasm_dir=nasm_dir)

        if lib_name == 'double-conversion':
            dc_src_dir = os.path.join(ext_dir(), 'double-conversion')
            build_double_conversion(dc_src_dir, ext_build_dir())

        if lib_name == 'lz4':
            lz4_src_dir = os.path.join(ext_dir(), 'lz4')
            build_lz4(lz4_src_dir, ext_build_dir())

        # lzma
        if lib_name == 'xz':
            xz_src_dir = os.path.join(ext_dir(), 'xz')
            build_xz(xz_src_dir, ext_build_dir())

        if lib_name == 'zstd':
            zstd_src_dir = os.path.join(ext_dir(), 'zstd')
            build_zstd(zstd_src_dir, ext_build_dir())

        if lib_name == 'fmt':
            fmt_src_dir = os.path.join(ext_dir(), 'fmt')
            build_fmt(fmt_src_dir, ext_build_dir())

        if lib_name == 'libevent':
            le_src_dir = os.path.join(ext_dir(), 'libevent')
            build_libevent(le_src_dir, ext_build_dir())

        if lib_name == 'snappy':
            snappy_src_dir = os.path.join(ext_dir(), 'snappy')
            build_snappy(snappy_src_dir, ext_build_dir())

        if lib_name == 'bzip2':
            bz2_src_dir = os.path.join(ext_dir(), 'bzip2')
            build_bzip2(bz2_src_dir, ext_build_dir())

        if lib_name == 'libsodium':
            package_name = find_src_package_with_glob(os.path.join(src_package_dir(), 'libsodium*'))
            src_dir = get_package_top_level_folder(package_name, ext_dir())
            if not os.path.exists(src_dir):
                remove_old_src_folder_with_glob(os.path.join(ext_dir(), 'libsodium*'))
                unpack_file_to_folder(package_name, ext_dir())
                assert os.path.exists(src_dir)
            build_libsodium(src_dir, ext_build_dir())

        if lib_name == 'folly':
            src_dir = os.path.join(ext_dir(), 'folly')
            build_folly(src_dir, ext_build_dir(), use_asan=use_asan)

        if lib_name == 'suitesparse':
            # package_name = find_src_package_with_glob(os.path.join(src_package_dir(), 'SuiteSparse*'))
            # src_dir = get_package_top_level_folder(package_name, ext_dir())
            # if not os.path.exists(src_dir):
            #     remove_old_src_folder_with_glob(os.path.join(ext_dir(), 'SuiteSparse*'))
            #     unpack_file_to_folder(package_name, ext_dir())
            #     assert os.path.exists(src_dir)
            src_dir = os.path.join(ext_dir(), 'SuiteSparse')
            build_suitesparse(src_dir, ext_build_dir())

        if lib_name == 'ceres-solver':
            src_dir = os.path.join(ext_dir(), 'ceres-solver')
            build_ceres_solver(src_dir, ext_build_dir())

        if lib_name == 'glbinding':
            src_dir = os.path.join(ext_dir(), 'glbinding')
            build_glbinding(src_dir, ext_build_dir())

        if lib_name == 'libjpeg':
            if is_windows():
                nasm_dir = unpack_tool_to_target_dir(src_package_dir(), 'nasm*win64*', 'nasm-*')
            elif is_mac():
                nasm_dir = unpack_tool_to_target_dir(src_package_dir(), 'nasm*macosx*', 'nasm-*')
                os.chown(os.path.join(nasm_dir, 'nasm'), os.getuid(), os.getgid())
                os.chmod(os.path.join(nasm_dir, 'nasm'), os.stat(os.path.join(nasm_dir, 'nasm')).st_mode | stat.S_IXUSR)
            else:
                nasm_dir = ''
            package_name = find_src_package_with_glob(os.path.join(src_package_dir(), 'libjpeg*'))
            src_dir = get_package_top_level_folder(package_name, ext_dir())
            if not os.path.exists(src_dir):
                remove_old_src_folder_with_glob(os.path.join(ext_dir(), 'libjpeg*'))
                unpack_file_to_folder(package_name, ext_dir())
                assert os.path.exists(src_dir)
            build_libjpeg(src_dir, ext_build_dir(), nasm_dir=nasm_dir)

        if lib_name == 'libpng':
            package_name = find_src_package_with_glob(os.path.join(src_package_dir(), 'libpng*'))
            src_dir = get_package_top_level_folder(package_name, ext_dir())
            if not os.path.exists(src_dir):
                remove_old_src_folder_with_glob(os.path.join(ext_dir(), 'libpng*'))
                unpack_file_to_folder(package_name, ext_dir())
                assert os.path.exists(src_dir)
            build_libpng(src_dir, ext_build_dir())

        if lib_name == 'openjpeg':
            package_name = find_src_package_with_glob(os.path.join(src_package_dir(), 'openjpeg*'))
            src_dir = get_package_top_level_folder(package_name, ext_dir())
            if not os.path.exists(src_dir):
                remove_old_src_folder_with_glob(os.path.join(ext_dir(), 'openjpeg*'))
                unpack_file_to_folder(package_name, ext_dir())
                assert os.path.exists(src_dir)
            build_openjpeg(src_dir, ext_build_dir())

        if lib_name == 'libwebp':
            package_name = find_src_package_with_glob(os.path.join(src_package_dir(), 'libwebp*'))
            src_dir = get_package_top_level_folder(package_name, ext_dir())
            if not os.path.exists(src_dir):
                remove_old_src_folder_with_glob(os.path.join(ext_dir(), 'libwebp*'))
                unpack_file_to_folder(package_name, ext_dir())
                assert os.path.exists(src_dir)
            build_libwebp(src_dir, ext_build_dir())

        if lib_name == 'jxrlib':
            src_dir = os.path.join(ext_dir(), 'jxrlib')
            build_jxrlib(src_dir, ext_build_dir())

        if lib_name == 'geometrictools':
            print('geometrictools')

        if lib_name == 'assimp':
            src_dir = os.path.join(ext_dir(), 'assimp')
            build_assimp(src_dir, ext_build_dir())

        if lib_name == 'hdf5':
            package_name = find_src_package_with_glob(os.path.join(src_package_dir(), 'hdf5*'))
            src_dir = get_package_top_level_folder(package_name, ext_dir())
            if not os.path.exists(src_dir):
                remove_old_src_folder_with_glob(os.path.join(ext_dir(), 'hdf5*'))
                unpack_file_to_folder(package_name, ext_dir())
                assert os.path.exists(src_dir)
            build_hdf5(src_dir, ext_build_dir())

        if lib_name == 'freeimage':
            package_name = find_src_package_with_glob(os.path.join(src_package_dir(), 'freeimage-svn*'))
            src_dir = get_package_top_level_folder(package_name, ext_dir())
            if not os.path.exists(src_dir):
                remove_old_src_folder_with_glob(os.path.join(ext_dir(), 'freeimage-svn*'))
                unpack_file_to_folder(package_name, ext_dir())
            assert os.path.exists(src_dir)
            build_freeimage(src_dir, ext_build_dir())

        if lib_name == 'itk':
            src_dir = os.path.join(ext_dir(), 'ITK')
            build_itk(src_dir, ext_build_dir())

        if lib_name == 'vtk':
            src_dir = os.path.join(ext_dir(), 'vtk')
            build_vtk(src_dir, ext_build_dir())

        if lib_name == 'opencv':
            src_dir = os.path.join(ext_dir(), 'opencv')
            src_contrib_dir = os.path.join(ext_dir(), 'opencv_contrib')
            build_opencv(src_dir, src_contrib_dir, ext_build_dir())

        # if lib_name == 'botan':
        #     src_dir = os.path.join(ext_dir(), 'botan')
        #     assert os.path.exists(src_dir)
        #     build_botan(src_dir, ext_build_dir())
        #
        # if lib_name == 'ospray':
        #     if is_windows():
        #         ispc_dir = unpack_tool_to_target_dir(src_package_dir(), 'ispc*win*')
        #         embree_dir = unpack_tool_to_target_dir(src_package_dir(), 'embree*win*')
        #     elif is_linux():
        #         ispc_dir = unpack_tool_to_target_dir(src_package_dir(), 'ispc*linux*')
        #         embree_dir = unpack_tool_to_target_dir(src_package_dir(), 'embree*linux*')
        #     else:
        #         ispc_dir = unpack_tool_to_target_dir(src_package_dir(), 'ispc*osx*')
        #         embree_dir = unpack_tool_to_target_dir(src_package_dir(), 'embree*osx*')
        #     src_dir = os.path.join(ext_dir(), 'OSPRay')
        #     assert os.path.exists(src_dir)
        #     assert os.path.exists(ispc_dir)
        #     assert os.path.exists(embree_dir)
        #     build_ospray(src_dir, ext_build_dir(), ispc_dir=ispc_dir, embree_dir=embree_dir)

        if lib_name == 'ospray':
            package_name = find_src_package_with_glob(os.path.join(src_package_dir(), 'ospray*'))
            src_dir = get_package_top_level_folder(package_name, ext_dir())
            if not os.path.exists(src_dir):
                remove_old_src_folder_with_glob(os.path.join(ext_dir(), 'ospray*'))
                unpack_file_to_folder(package_name, ext_dir())
                assert os.path.exists(src_dir)
            build_ospray(src_dir, ext_build_dir())

        if lib_name == 'java':
            shutil.rmtree(os.path.join(ext_build_dir(), 'jars'), ignore_errors=True)
            shutil.copytree(os.path.join(src_package_dir(), 'jars'), os.path.join(ext_build_dir(), 'jars'),
                            dirs_exist_ok=True)

            if is_mac():
                package_name = find_src_package_with_glob(os.path.join(src_package_dir(), '*-jre_x64*mac*'))
            elif is_linux():
                package_name = find_src_package_with_glob(os.path.join(src_package_dir(), '*-jre_x64*linux*'))
            else:
                package_name = find_src_package_with_glob(os.path.join(src_package_dir(), '*-jre_x64*windows*'))
            jre_dir = get_package_top_level_folder(package_name, ext_build_dir())
            print(jre_dir)
            if not os.path.exists(jre_dir):
                if os.path.exists(os.path.join(ext_build_dir(), 'jre')):
                    os.unlink(os.path.join(ext_build_dir(), 'jre'))
                remove_old_src_folder_with_glob(os.path.join(ext_build_dir(), 'jre*'))
                unpack_file_to_folder(package_name, ext_build_dir())
                assert os.path.exists(jre_dir)
                if os.path.lexists(os.path.join(ext_build_dir(), 'jre')):
                    os.unlink(os.path.join(ext_build_dir(), 'jre'))
                    print('link jre')
                    os.symlink(jre_dir, os.path.join(ext_build_dir(), 'jre'))
            if not os.path.lexists(os.path.join(ext_build_dir(), 'jre')):
                print('link jre')
                os.symlink(jre_dir, os.path.join(ext_build_dir(), 'jre'))

            if is_mac():
                package_name = find_src_package_with_glob(os.path.join(src_package_dir(), '*-jre_aarch64*mac*'))
                if not os.path.lexists(os.path.join(ext_build_dir(), 'jrearm')):
                    os.mkdir(os.path.join(ext_build_dir(), 'jrearm'))
                jre_dir = get_package_top_level_folder(package_name, os.path.join(ext_build_dir(), 'jrearm'))
                print(jre_dir)
                if not os.path.exists(jre_dir):
                    if os.path.exists(os.path.join(ext_build_dir(), 'jre-arm')):
                        os.unlink(os.path.join(ext_build_dir(), 'jre-arm'))
                    unpack_file_to_folder(package_name, os.path.join(ext_build_dir(), 'jrearm'))
                    assert os.path.exists(jre_dir)
                    if os.path.lexists(os.path.join(ext_build_dir(), 'jre-arm')):
                        os.unlink(os.path.join(ext_build_dir(), 'jre-arm'))
                        print('link jre-arm')
                        os.symlink(jre_dir, os.path.join(ext_build_dir(), 'jre-arm'))
                if not os.path.lexists(os.path.join(ext_build_dir(), 'jre-arm')):
                    print('link jre-arm')
                    os.symlink(jre_dir, os.path.join(ext_build_dir(), 'jre-arm'))

        if lib_name == 'ants':
            src_dir = os.path.join(atlas_repository_dir(), '..', 'ANTs')
            update_or_clone_git_repository(src_dir, 'git@github.com:ANTsX/ANTs.git')
            if not os.path.exists(src_dir):
                print('no ANTs')
            else:
                build_ants(src_dir, os.path.join(ext_build_dir(), 'ANTs'))

        if lib_name == 'conda-opencv':
            src_dir = os.path.join(ext_dir(), 'opencv')
            src_contrib_dir = os.path.join(ext_dir(), 'opencv_contrib')
            build_opencv(src_dir, src_contrib_dir, ext_conda_build_dir(), conda_build=True)

        if lib_name == 'skia':
            src_dir = os.path.join(atlas_repository_dir(), '..', 'skia')
            update_or_clone_git_repository(src_dir, 'https://github.com/google/skia.git')
            if not os.path.exists(src_dir):
                print('no skia')
            else:
                build_skia(src_dir, ext_build_dir())

        if lib_name == 'neuTube':
            for suffix in ('macOS', 'Linux', 'Windows'):
                shutil.copytree(os.path.join(src_package_dir(), 'packages-' + suffix),
                                os.path.join(ext_build_dir(), 'packages-' + suffix),
                                dirs_exist_ok=True)

        if lib_name == 'rocksdb':
            src_dir = os.path.join(ext_dir(), 'rocksdb')
            build_rocksdb(src_dir, ext_build_dir())

        if lib_name == 'llfio':
            src_dir = os.path.join(ext_dir(), 'llfio')
            build_llfio(src_dir, ext_build_dir())

        if lib_name == 'conda-zimg':
            src_dir = os.path.join(atlas_src_dir(), 'python')
            build_conda_zimg(src_dir, ext_conda_build_dir())

        if lib_name == 'jansson':
            package_name = find_src_package_with_glob(os.path.join(src_package_dir(), 'jansson*'))
            src_dir = get_package_top_level_folder(package_name, ext_dir())
            if not os.path.exists(src_dir):
                remove_old_src_folder_with_glob(os.path.join(ext_dir(), 'jansson*'))
                unpack_file_to_folder(package_name, ext_dir())
                assert os.path.exists(src_dir)
            build_jansson(src_dir, ext_build_dir())

        if lib_name == 'pcre':
            if is_windows():
                package_name = find_src_package_with_glob(os.path.join(src_package_dir(), 'pcre2*'))
                src_dir = get_package_top_level_folder(package_name, ext_dir())
                if not os.path.exists(src_dir):
                    remove_old_src_folder_with_glob(os.path.join(ext_dir(), 'pcre2*'))
                    unpack_file_to_folder(package_name, ext_dir())
                    assert os.path.exists(src_dir)
                build_pcre(src_dir, ext_build_dir())

        if lib_name == 'fizz':
            src_dir = os.path.join(ext_dir(), 'fizz', 'fizz')
            build_fizz(src_dir, ext_build_dir())

        if lib_name == 'mvfst':
            src_dir = os.path.join(ext_dir(), 'mvfst')
            build_mvfst(src_dir, ext_build_dir())

        if lib_name == 'wangle':
            src_dir = os.path.join(ext_dir(), 'wangle', 'wangle')
            build_wangle(src_dir, ext_build_dir())

        if lib_name == 'proxygen':
            src_dir = os.path.join(ext_dir(), 'proxygen')
            build_proxygen(src_dir, ext_build_dir())


def parse_inputs(argv: list):
    lib_list = ['cmake', 'ninja', 'curl', 'make-cmake-pathlist', 'qt', 'zlib', 'ffmpeg', 'boost', 'tbb', 'eigen',
                'pybind11', 'glm', 'magic_enum', 'pocketfft', 'googletest', 'cpuinfo', 'gflags', 'glog', 'benchmark',
                'openssl', 'grpc', 'double-conversion', 'lz4', 'xz', 'zstd', 'fmt', 'libevent', 'snappy', 'bzip2',
                'libsodium', 'folly', 'suitesparse', 'ceres-solver', 'glbinding', 'libjpeg', 'libpng', 'openjpeg',
                'libwebp', 'jxrlib', 'geometrictools', 'assimp', 'hdf5', 'freeimage', 'itk', 'vtk',
                'opencv', 'botan', 'ospray', 'java', 'ants', 'skia',
                'neuTube', 'rocksdb', 'llfio', 'jansson', 'pcre',
                'fizz', 'mvfst', 'wangle', 'proxygen',
                'conda-zimg', 'conda-opencv',
                ]
    libs = OrderedDict([(lib, False) for lib in lib_list])

    # not used now
    lib_skip_list = ['botan', 'ospray', 'ants', 'skia', 'rocksdb', 'conda-opencv', 'llfio']

    libs_reverse_depends = {'eigen': ['opencv', 'ceres-solver', 'itk', 'vtk'],
                            'libpng': ['opencv', 'itk', 'vtk'],
                            'libjpeg': ['opencv', 'itk', 'vtk'],
                            'zlib': ['libpng', 'assimp', 'hdf5', 'itk', 'vtk', 'opencv', 'grpc', 'folly', 'proxygen'],
                            'gflags': ['glog'],
                            'glog': ['ceres-solver', 'folly', 'opencv'],
                            'benchmark': ['grpc'],
                            'openssl': ['grpc', 'folly'],
                            'hdf5': ['itk', 'vtk'],
                            'suitesparse': ['ceres-solver'],
                            'ceres-solver': ['opencv'],  # only if we need opencv sfm
                            'boost': ['folly'],
                            'libevent': ['folly'],
                            'double-conversion': ['folly', 'itk', 'vtk'],
                            'lz4': ['vtk', 'folly', 'rocksdb'],
                            'xz': ['vtk', 'folly'],
                            'zstd': ['folly', 'rocksdb'],
                            'fmt': ['folly'],
                            'openjpeg': ['opencv'],
                            'libwebp': ['opencv'],
                            'snappy': ['folly', 'rocksdb'],
                            'bzip2': ['folly'],
                            'libsodium': ['folly'],
                            'folly': ['rocksdb', 'proxygen', 'wangle', 'fizz', 'mvfst'],
                            'qt': ['make-cmake-pathlist'],
                            'wangle': ['proxygen'],
                            'mvfst': ['proxygen'],
                            'fizz': ['mvfst'],
                            }

    print('current interpreter: ' + sys.executable)

    parser = argparse.ArgumentParser(
        epilog=f"""
Examples:

python build_ext_libs.py [all or libs...] [--exclude-libs] [libs...] [--start-from] [lib] [--use-asan]
""",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("libs", nargs='+', choices=list(libs.keys()) + ['all'],
                        help="all or a list of libs", )
    parser.add_argument("--exclude-libs", nargs='+', choices=list(libs.keys()),
                        help="a list of libs to exclude from building", )
    parser.add_argument("--start-from", choices=list(libs.keys()),
                        help="skip libs before the specified lib")
    parser.add_argument("--use-asan", action='store_true', help="use sanitizers")

    # parse arguments
    args = parser.parse_args(args=None if argv[1:] else ['--help'])

    for lib in args.libs:
        if lib.lower() == "all":
            for vlib in libs:
                if vlib not in lib_skip_list:
                    libs[vlib] = True
        else:
            libs[lib] = True

    if args.exclude_libs is not None:
        for lib in args.exclude_libs:
            libs[lib] = False

    for lib, rev_dep in libs_reverse_depends.items():
        if libs[lib]:
            for dlib in rev_dep:
                if dlib not in lib_skip_list:
                    libs[dlib] = True

    if args.start_from is not None:
        started = False
        for lib in libs:
            if started:
                break
            started = args.start_from.lower() == lib.lower()
            if not started:
                libs[lib] = False

    build_all = True
    for lib in libs:
        if lib not in lib_skip_list:
            build_all = build_all and libs[lib]
    if build_all:
        shutil.rmtree(ext_build_dir(), ignore_errors=True)

    return libs, args.use_asan


if __name__ == "__main__":
    build_libs(*parse_inputs(sys.argv))
