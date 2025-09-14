import argparse
import subprocess
import logging
import os

import common_dirs
import build_ext_libs
from download_atlas_test_data import download_atlas_test_data
from logger import setup_logger

logger = logging.getLogger(__name__)


def get_cmake_cmd_common_part(arm64: bool = False):
    if common_dirs.is_windows():
        if common_dirs.use_ninja():
            res = [common_dirs.get_cmake_binary(),  # '-E', 'echo',
                   '-DCMAKE_BUILD_TYPE=Release',
                   '-G', 'Ninja', '-DCMAKE_MAKE_PROGRAM=' + common_dirs.get_ninja_binary(),
                   ]
            if common_dirs.use_clang_cl():
                res.extend(['-DCMAKE_CXX_COMPILER=clang-cl',
                            '-DCMAKE_C_COMPILER=clang-cl',
                            ])
            return res
        else:
            return [common_dirs.get_cmake_binary(),  # '-E', 'echo',
                    '-G', 'Visual Studio 17 2022', '-A', 'x64', '-T', 'host=x64'
                    ]
    elif common_dirs.is_linux():
        res = [common_dirs.get_cmake_binary(),  # '-E', 'echo',
               '-DCMAKE_BUILD_TYPE=Release',
               '-DCMAKE_EXPORT_COMPILE_COMMANDS:BOOL=ON',
               ]
        if common_dirs.use_ninja():
            res.extend(['-G', 'Ninja', '-DCMAKE_MAKE_PROGRAM=' + common_dirs.get_ninja_binary()
                        ])
        return res
    elif common_dirs.is_mac():
        res = [common_dirs.get_cmake_binary(),  # '-E', 'echo',
               '-DCMAKE_BUILD_TYPE=Release',
               '-DCMAKE_EXPORT_COMPILE_COMMANDS:BOOL=ON',
               '' if not arm64 else '-DCMAKE_SYSTEM_NAME=Darwin',
               '' if not arm64 else '-DCMAKE_SYSTEM_PROCESSOR=arm64',
               '' if not arm64 else '-DCMAKE_OSX_ARCHITECTURES=arm64',
               ]
        if common_dirs.use_ninja():
            res.extend(['-G', 'Ninja', '-DCMAKE_MAKE_PROGRAM=' + common_dirs.get_ninja_binary()
                        ])
        return res


def build_atlas(use_asan: bool = False, skip_test: bool = False, debug_version: bool = False, arm64: bool = False):
    logger.info(f'srcDIR: {common_dirs.atlas_repository_dir()}')
    logger.info(f'buildDIR: {common_dirs.atlas_build_dir(arm64=arm64)}')
    logger.info(f'useNinja: {common_dirs.use_ninja()}')

    skip_test = skip_test or use_asan or debug_version

    cmakecmd = get_cmake_cmd_common_part(arm64=arm64)
    cmakecmd[:] = [x for x in cmakecmd if x]

    cmakecmd.extend(['-DATLAS_SANITIZE_ADDRESS:BOOL=' + 'ON' if use_asan else 'OFF',
                     '-DATLAS_DEBUG_VERSION:BOOL=' + 'ON' if debug_version else 'OFF',
                     ])
    cmakecmd.extend([common_dirs.atlas_repository_dir()])

    if common_dirs.is_windows():
        env = build_ext_libs.get_vcvars_environment()
    env['caexcludepath'] = ';'.join([os.path.join(common_dirs.atlas_repository_dir(), 'src', '3rdparty'),
                                     common_dirs.intel_sw_dir(),
                                     r'C:\Program Files (x86)\Windows Kits',
                                     os.path.join(common_dirs.atlas_repository_dir(), 'test'),
                                     r'C:\Strawberry\perl\bin',
                                     ])
    env['PATH'] = (f'{env["PATH"]};{common_dirs.tbb_redist_dir()};{common_dirs.qt_bin_dir()};'
                   f'{common_dirs.freeimage_redist_dir()}')
    logger.info(env['PATH'])
    subprocess.run(cmakecmd,
                   cwd=common_dirs.atlas_build_dir(arm64=arm64), shell=False, check=True, env=env)
    if common_dirs.use_ninja():
        subprocess.run([build_ext_libs.get_ninja_binary()],
                       cwd=common_dirs.atlas_build_dir(arm64=arm64), shell=False, check=True, env=env)
    else:
        subprocess.run(['MSBuild', 'ALL_BUILD.vcxproj', '/property:Configuration=Release', '/maxcpucount'],
                       cwd=common_dirs.atlas_build_dir(arm64=arm64), shell=True, check=True, env=env)

    if not skip_test:
        download_atlas_test_data()
    env['CTEST_PARALLEL_LEVEL'] = str(os.cpu_count())
    subprocess.run([common_dirs.get_ctest_binary(), '--extra-verbose'],
                   cwd=common_dirs.atlas_build_dir(arm64=arm64), shell=False, check=True, env=env)
    else:
    env = os.environ.copy()
    if common_dirs.is_linux() and build_ext_libs.use_clang_in_linux():
        env['CC'] = build_ext_libs.get_clang_in_linux()
    env['CXX'] = build_ext_libs.get_clangplus_in_linux()
    subprocess.run(cmakecmd, cwd=common_dirs.atlas_build_dir(arm64=arm64), shell=False, check=True, env=env)
    if common_dirs.use_ninja():
        subprocess.run([common_dirs.get_ninja_binary()],
                       cwd=common_dirs.atlas_build_dir(arm64=arm64), shell=False, check=True, env=env)
    else:
        subprocess.run(['make', '-j' + str(os.cpu_count())],
                       cwd=common_dirs.atlas_build_dir(arm64=arm64), shell=False, check=True, env=env)

    if not skip_test:
        download_atlas_test_data()
    env['CTEST_PARALLEL_LEVEL'] = str(os.cpu_count())
    subprocess.run([common_dirs.get_ctest_binary(), '--extra-verbose'],
                   cwd=common_dirs.atlas_build_dir(arm64=arm64), shell=False, check=True, env=env)

    if __name__ == "__main__":
        logger = setup_logger()

    parser = argparse.ArgumentParser(
        epilog=f"""
Examples:

python build_atlas.py [--use-asan] [--skip-test] [--debug-version] [--arm64]
""",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--use-asan", action='store_true', help="use sanitizers")
    parser.add_argument("--skip-test", action='store_true', help="skip test")
    parser.add_argument("--debug-version", action='store_true', help="debug version")
    parser.add_argument("--arm64", action='store_true', help="build arm64 version")
    args = parser.parse_args()

    build_atlas(use_asan=args.use_asan, skip_test=args.skip_test, debug_version=args.debug_version, arm64=args.arm64)
