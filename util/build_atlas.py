import os
import sys
import shutil
import subprocess

import common_dirs
import build_ext_libs


def get_cmake_cmd_common_part():
    if sys.platform.startswith('win'):
        return ['cmake',  # '-E', 'echo',
                '-G', 'Visual Studio 15 2017 Win64'
                ]
    elif sys.platform.startswith('linux'):
        cmake_folder = build_ext_libs.find_src_package_with_glob(os.path.join(os.path.expanduser('~'), 'software', 'cmake-*_64'))
        return [os.path.join(cmake_folder, 'bin', 'cmake'),  # '-E', 'echo',
                '-DCMAKE_BUILD_TYPE=Release',
                '-DCMAKE_EXPORT_COMPILE_COMMANDS:BOOL=ON'
                ]
    else:
        return ['cmake',  # '-E', 'echo',
                '-DCMAKE_BUILD_TYPE=Release',
                '-DCMAKE_EXPORT_COMPILE_COMMANDS:BOOL=ON'
                ]


def build_atlas():
    build_dir = common_dirs.build_dir()
    src_dir = common_dirs.repository_dir()
    print('srcDIR:', src_dir)
    print('buildDIR:', build_dir)

    cmakecmd = get_cmake_cmd_common_part()
    cmakecmd.extend([src_dir])

    if sys.platform.startswith('win'):
        env = build_ext_libs.get_vcvars_environment()
        subprocess.run(cmakecmd,
                       cwd=build_dir, shell=False, check=True, env=env)
        subprocess.run(['MSBuild', 'ALL_BUILD.vcxproj', '/property:Configuration=Release', '/maxcpucount'],
                       cwd=build_dir, shell=True, check=True, env=env)
    else:
        subprocess.run(cmakecmd, cwd=build_dir, shell=False, check=True)
        subprocess.run(['make', '-j' + str(os.cpu_count())],
                       cwd=build_dir, shell=False, check=True)


if __name__ == "__main__":
    build_atlas()