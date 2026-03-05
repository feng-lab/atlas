import argparse
import logging
import os
import subprocess

import build_ext_libs
import common_dirs
from download_atlas_test_data import download_atlas_test_data
from logger import setup_logger

logger = logging.getLogger(__name__)


def get_cmake_cmd_common_part():
    if common_dirs.is_windows():
        if common_dirs.use_ninja():
            res = [
                common_dirs.get_cmake_binary(),  # '-E', 'echo',
                "-DCMAKE_BUILD_TYPE=Release",
                "-G",
                "Ninja",
                "-DCMAKE_MAKE_PROGRAM=" + common_dirs.get_ninja_binary(),
            ]
            if common_dirs.use_clang_cl():
                res.extend(
                    [
                        "-DCMAKE_CXX_COMPILER=clang-cl",
                        "-DCMAKE_C_COMPILER=clang-cl",
                        "-DCMAKE_LINKER=lld-link",
                        # "-DBoost_COMPILER=vc143",
                    ]
                )
            return res
        else:
            toolset = "host=x64"
            if common_dirs.use_clang_cl():
                toolset = "ClangCL,host=x64"
            res = [
                common_dirs.get_cmake_binary(),  # '-E', 'echo',
                "-G",
                "Visual Studio 17 2022",
                "-A",
                "x64",
                "-T",
                toolset,
            ]
            if common_dirs.use_clang_cl():
                res.append("-DCMAKE_LINKER=lld-link")
                # res.append("-DBoost_COMPILER=vc143")
            return res
    elif common_dirs.is_linux():
        res = [
            common_dirs.get_cmake_binary(),  # '-E', 'echo',
            "-DCMAKE_BUILD_TYPE=Release",
            "-DCMAKE_EXPORT_COMPILE_COMMANDS:BOOL=ON",
        ]
        if common_dirs.use_ninja():
            res.extend(
                [
                    "-G",
                    "Ninja",
                    "-DCMAKE_MAKE_PROGRAM=" + common_dirs.get_ninja_binary(),
                ]
            )
        return res
    elif common_dirs.is_mac():
        res = [
            common_dirs.get_cmake_binary(),  # '-E', 'echo',
            "-DCMAKE_BUILD_TYPE=Release",
            "-DCMAKE_EXPORT_COMPILE_COMMANDS:BOOL=ON",
        ]
        if common_dirs.use_ninja():
            res.extend(
                [
                    "-G",
                    "Ninja",
                    "-DCMAKE_MAKE_PROGRAM=" + common_dirs.get_ninja_binary(),
                ]
            )
        return res


def build_atlas(
    use_asan: bool = False,
    skip_test: bool = False,
    debug_version: bool = False,
    enable_network_tests: bool = False,
):
    logger.info(f"srcDIR: {common_dirs.atlas_repository_dir()}")
    logger.info(f"buildDIR: {common_dirs.atlas_build_dir()}")
    logger.info(f"useNinja: {common_dirs.use_ninja()}")

    skip_test = skip_test or use_asan or debug_version

    cmakecmd = get_cmake_cmd_common_part()
    cmakecmd[:] = [x for x in cmakecmd if x]

    cmakecmd.extend(
        [
            "-DATLAS_SANITIZE_ADDRESS:BOOL=" + ("ON" if use_asan else "OFF"),
            "-DATLAS_DEBUG_VERSION:BOOL=" + ("ON" if debug_version else "OFF"),
            # Developer-only tool; keep it out of deployed builds by default.
            "-DATLAS_ENABLE_CUSTOM_COMMAND:BOOL=" + ("ON" if debug_version else "OFF"),
        ]
    )
    cmakecmd.extend([common_dirs.atlas_repository_dir()])

    print(cmakecmd)

    if common_dirs.is_windows():
        env = build_ext_libs.get_vcvars_environment()
        env["caexcludepath"] = ";".join(
            [
                os.path.join(common_dirs.atlas_repository_dir(), "src", "3rdparty"),
                common_dirs.intel_sw_dir(),
                r"C:\Program Files (x86)\Windows Kits",
                os.path.join(common_dirs.atlas_repository_dir(), "test"),
                r"C:\Strawberry\perl\bin",
            ]
        )
        env["PATH"] = (
            f"{env['PATH']};{common_dirs.tbb_redist_dir()};{common_dirs.qt_bin_dir()};"
            f"{common_dirs.freeimage_redist_dir()}"
        )
        logger.info(env["PATH"])
        subprocess.run(
            cmakecmd,
            cwd=common_dirs.atlas_build_dir(),
            shell=False,
            check=True,
            env=env,
        )
        if common_dirs.use_ninja():
            subprocess.run(
                [build_ext_libs.get_ninja_binary()],
                cwd=common_dirs.atlas_build_dir(),
                shell=False,
                check=True,
                env=env,
            )
        else:
            subprocess.run(
                [
                    "MSBuild",
                    "ALL_BUILD.vcxproj",
                    "/property:Configuration=Release",
                    "/maxcpucount",
                ],
                cwd=common_dirs.atlas_build_dir(),
                shell=True,
                check=True,
                env=env,
            )

        if not skip_test:
            download_atlas_test_data()
            env["CTEST_PARALLEL_LEVEL"] = str(os.cpu_count())
            # Network tests are opt-in. Force them off by default so CI/self-hosted
            # runners don't fail when outbound access is restricted.
            env["ATLAS_ENABLE_NETWORK_TESTS"] = "1" if enable_network_tests else "0"
            subprocess.run(
                [common_dirs.get_ctest_binary(), "--extra-verbose"],
                cwd=common_dirs.atlas_build_dir(),
                shell=False,
                check=True,
                env=env,
            )
    else:
        env = os.environ.copy()
        if common_dirs.is_linux() and build_ext_libs.use_clang_in_linux():
            env["CC"] = build_ext_libs.get_clang_in_linux()
            env["CXX"] = build_ext_libs.get_clangplus_in_linux()
        subprocess.run(
            cmakecmd,
            cwd=common_dirs.atlas_build_dir(),
            shell=False,
            check=True,
            env=env,
        )
        if common_dirs.use_ninja():
            subprocess.run(
                [common_dirs.get_ninja_binary()],
                cwd=common_dirs.atlas_build_dir(),
                shell=False,
                check=True,
                env=env,
            )
        else:
            subprocess.run(
                ["make", "-j" + str(os.cpu_count())],
                cwd=common_dirs.atlas_build_dir(),
                shell=False,
                check=True,
                env=env,
            )

        if not skip_test:
            download_atlas_test_data()
            env["CTEST_PARALLEL_LEVEL"] = str(os.cpu_count())
            # Network tests are opt-in. Force them off by default so CI/self-hosted
            # runners don't fail when outbound access is restricted.
            env["ATLAS_ENABLE_NETWORK_TESTS"] = "1" if enable_network_tests else "0"
            subprocess.run(
                [common_dirs.get_ctest_binary(), "--extra-verbose"],
                cwd=common_dirs.atlas_build_dir(),
                shell=False,
                check=True,
                env=env,
            )


if __name__ == "__main__":
    logger = setup_logger()

    parser = argparse.ArgumentParser(
        epilog="""
Examples:

python build_atlas.py [--use-asan] [--skip-test] [--debug-version]
""",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--use-asan", action="store_true", help="use sanitizers")
    parser.add_argument("--skip-test", action="store_true", help="skip test")
    parser.add_argument("--debug-version", action="store_true", help="debug version")
    parser.add_argument(
        "--enable-network-tests",
        action="store_true",
        help="enable network-dependent tests (disabled by default for CI/firewalled environments)",
    )
    args = parser.parse_args()

    build_atlas(
        use_asan=args.use_asan,
        skip_test=args.skip_test,
        debug_version=args.debug_version,
        enable_network_tests=args.enable_network_tests,
    )
