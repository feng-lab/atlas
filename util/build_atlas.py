import argparse

from common_dirs import *
import build_ext_libs


def get_cmake_cmd_common_part():
    if is_windows():
        if use_ninja():
            res = [get_cmake_binary(),  # '-E', 'echo',
                   '-DCMAKE_BUILD_TYPE=Release',
                   '-G', 'Ninja', '-DCMAKE_MAKE_PROGRAM=' + get_ninja_binary(),
                   ]
            if use_clang_cl():
                res.extend(['-DCMAKE_CXX_COMPILER=clang-cl',
                            '-DCMAKE_C_COMPILER=clang-cl',
                            ])
            return res
        else:
            return [get_cmake_binary(),  # '-E', 'echo',
                    '-G', 'Visual Studio 17 2022', '-A', 'x64', '-T', 'host=x64'
                    ]
    elif is_linux():
        if use_ninja():
            return [get_cmake_binary(),  # '-E', 'echo',
                    '-DCMAKE_BUILD_TYPE=Release',
                    '-DCMAKE_EXPORT_COMPILE_COMMANDS:BOOL=ON',
                    '-G', 'Ninja', '-DCMAKE_MAKE_PROGRAM=' + get_ninja_binary()
                    ]
        else:
            return [get_cmake_binary(),  # '-E', 'echo',
                    '-DCMAKE_BUILD_TYPE=Release',
                    '-DCMAKE_EXPORT_COMPILE_COMMANDS:BOOL=ON',
                    ]
    elif is_mac():
        if use_ninja():
            return [get_cmake_binary(),  # '-E', 'echo',
                    '-DCMAKE_BUILD_TYPE=Release',
                    '-DCMAKE_EXPORT_COMPILE_COMMANDS:BOOL=ON',
                    '-G', 'Ninja', '-DCMAKE_MAKE_PROGRAM=' + get_ninja_binary()
                    ]
        else:
            return [get_cmake_binary(),  # '-E', 'echo',
                    '-DCMAKE_BUILD_TYPE=Release',
                    '-DCMAKE_EXPORT_COMPILE_COMMANDS:BOOL=ON',
                    ]


def build_atlas(use_asan: bool = False, skip_test: bool = False):
    print('srcDIR:', atlas_repository_dir())
    print('buildDIR:', atlas_build_dir())
    print('useNinja:', use_ninja())

    cmakecmd = get_cmake_cmd_common_part()
    if use_asan:
        cmakecmd.extend(['-DATLAS_SANITIZE_ADDRESS:BOOL=ON',
                         ])
    cmakecmd.extend([atlas_repository_dir()])

    if is_windows():
        env = build_ext_libs.get_vcvars_environment()
        env['caexcludepath'] = ';'.join([os.path.join(atlas_repository_dir(), 'src', '3rdparty'),
                                         intel_sw_dir(),
                                         r'C:\Program Files (x86)\Windows Kits',
                                         os.path.join(atlas_repository_dir(), 'test'),
                                         ])
        env['PATH'] = f'{env["PATH"]};{tbb_redist_dir()};{qt_bin_dir()};{freeimage_redist_dir()}'
        print(env['PATH'])
        subprocess.run(cmakecmd,
                       cwd=atlas_build_dir(), shell=False, check=True, env=env)
        if use_ninja():
            subprocess.run([build_ext_libs.get_ninja_binary()],
                           cwd=atlas_build_dir(), shell=False, check=True, env=env)
        else:
            subprocess.run(['MSBuild', 'ALL_BUILD.vcxproj', '/property:Configuration=Release', '/maxcpucount'],
                           cwd=atlas_build_dir(), shell=True, check=True, env=env)

        if not skip_test and not use_asan:
            env['CTEST_PARALLEL_LEVEL'] = str(os.cpu_count())
            subprocess.run([get_ctest_binary(), '--extra-verbose'],
                           cwd=atlas_build_dir(), shell=False, check=True, env=env)
    else:
        env = os.environ.copy()
        if is_linux() and build_ext_libs.use_clang_in_linux():
            env['CC'] = 'clang'
            env['CXX'] = 'clang++'
        subprocess.run(cmakecmd, cwd=atlas_build_dir(), shell=False, check=True, env=env)
        if use_ninja():
            subprocess.run([build_ext_libs.get_ninja_binary()],
                           cwd=atlas_build_dir(), shell=False, check=True, env=env)
        else:
            subprocess.run(['make', '-j' + str(os.cpu_count())],
                           cwd=atlas_build_dir(), shell=False, check=True, env=env)

        if not skip_test and not use_asan:
            env['CTEST_PARALLEL_LEVEL'] = str(os.cpu_count())
            subprocess.run([get_ctest_binary(), '--extra-verbose'],
                           cwd=atlas_build_dir(), shell=False, check=True, env=env)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        epilog=f"""
Examples:

python build_atlas.py [--use-asan] [--skip-test]
""",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--use-asan", action='store_true', help="use sanitizers")
    parser.add_argument("--skip-test", action='store_true', help="skip test")
    args = parser.parse_args()

    build_atlas(use_asan=args.use_asan, skip_test=args.skip_test)
