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
                    '-G', 'Visual Studio 16 2019', '-A', 'x64', '-T', 'host=x64'
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


def build_atlas():
    print('srcDIR:', atlas_repository_dir())
    print('buildDIR:', atlas_build_dir())
    print('useNinja:', use_ninja())

    cmakecmd = get_cmake_cmd_common_part()
    cmakecmd.extend([atlas_repository_dir()])

    if is_windows():
        env = build_ext_libs.get_vcvars_environment()
        print(env['PATH'])
        env['caexcludepath'] = ';'.join([os.path.join(atlas_repository_dir(), 'src', '3rdparty'),
                                         intel_sw_dir(),
                                         ])
        subprocess.run(cmakecmd,
                       cwd=atlas_build_dir(), shell=False, check=True, env=env)
        if use_ninja():
            subprocess.run([build_ext_libs.get_ninja_binary()],
                           cwd=atlas_build_dir(), shell=False, check=True, env=env)
        else:
            subprocess.run(['MSBuild', 'ALL_BUILD.vcxproj', '/property:Configuration=Release', '/maxcpucount'],
                           cwd=atlas_build_dir(), shell=True, check=True, env=env)
    else:
        subprocess.run(cmakecmd, cwd=atlas_build_dir(), shell=False, check=True)
        if use_ninja():
            subprocess.run([build_ext_libs.get_ninja_binary()],
                           cwd=atlas_build_dir(), shell=False, check=True)
        else:
            subprocess.run(['make', '-j' + str(os.cpu_count())],
                           cwd=atlas_build_dir(), shell=False, check=True)


if __name__ == "__main__":
    build_atlas()
