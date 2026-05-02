import argparse
import logging
import os
import subprocess

import atlas_pypi
import build_ext_libs
import common_dirs
from download_atlas_test_data import download_atlas_test_data
from logger import setup_logger

logger = logging.getLogger(__name__)


def _git_describe(repo_dir: str) -> str | None:
    try:
        res = subprocess.run(
            ["git", "describe", "--tags", "--dirty", "--always"],
            cwd=repo_dir,
            shell=False,
            check=False,
            text=True,
            capture_output=True,
        )
    except FileNotFoundError:
        return None

    if res.returncode != 0:
        return None
    return res.stdout.strip() or None


def _is_clean_release_tag_build(repo_dir: str) -> tuple[bool, str | None]:
    git_describe = _git_describe(repo_dir)
    if not git_describe:
        return (False, None)
    if git_describe.endswith("-dirty"):
        return (False, git_describe)
    return (atlas_pypi.is_clean_release_tag(git_describe), git_describe)


def _resolve_skip_test(
    skip_test: bool | None, *, use_asan: bool, debug_version: bool
) -> bool:
    if skip_test is not None:
        if use_asan and not skip_test:
            # Tests are currently excluded from ASAN builds in CMake (see CMakeLists.txt).
            logger.warning("Tests are disabled for ASAN builds; forcing --skip-test.")
            return True
        return skip_test

    # Preserve historical behavior: ASAN and debug builds skip tests by default.
    if use_asan or debug_version:
        return True

    is_release_tag, git_describe = _is_clean_release_tag_build(
        common_dirs.atlas_repository_dir()
    )
    if is_release_tag:
        logger.info(
            "Detected clean release tag build (%s); skipping tests by default.",
            git_describe,
        )
        return True
    if git_describe:
        logger.info("git describe: %s", git_describe)
    return False


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
                        "-DCMAKE_C_COMPILER:FILEPATH=" + common_dirs.clang_cl_binary(),
                        "-DCMAKE_CXX_COMPILER:FILEPATH="
                        + common_dirs.clang_cl_binary(),
                        "-DCMAKE_LINKER:FILEPATH=" + common_dirs.lld_link_binary(),
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
                common_dirs.windows_visual_studio_generator(),
                "-A",
                "x64",
                "-T",
                toolset,
            ]
            if common_dirs.use_clang_cl():
                res.extend(
                    [
                        "-DCMAKE_C_COMPILER:FILEPATH=" + common_dirs.clang_cl_binary(),
                        "-DCMAKE_CXX_COMPILER:FILEPATH="
                        + common_dirs.clang_cl_binary(),
                        "-DCMAKE_LINKER:FILEPATH=" + common_dirs.lld_link_binary(),
                    ]
                )
                res.append(
                    "-DCMAKE_VS_GLOBALS:STRING="
                    + ";".join(
                        [
                            "LLVMInstallDir=" + common_dirs.llvm_install_dir(),
                            "LLVMToolsVersion=" + common_dirs.llvm_tools_version(),
                        ]
                    )
                )
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
    skip_test: bool | None = None,
    debug_version: bool = False,
    release_pdb: bool = False,
    enable_network_tests: bool = False,
):
    logger.info(f"srcDIR: {common_dirs.atlas_repository_dir()}")
    logger.info(f"buildDIR: {common_dirs.atlas_build_dir()}")
    logger.info(f"useNinja: {common_dirs.use_ninja()}")

    skip_test = _resolve_skip_test(
        skip_test, use_asan=use_asan, debug_version=debug_version
    )

    cmakecmd = get_cmake_cmd_common_part()
    cmakecmd[:] = [x for x in cmakecmd if x]

    cmakecmd.extend(
        [
            "-DBUILD_TESTING:BOOL=" + ("OFF" if skip_test else "ON"),
            "-DATLAS_SANITIZE_ADDRESS:BOOL=" + ("ON" if use_asan else "OFF"),
            "-DATLAS_DEBUG_VERSION:BOOL=" + ("ON" if debug_version else "OFF"),
            "-DATLAS_ENABLE_RELEASE_PDB:BOOL=" + ("ON" if release_pdb else "OFF"),
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
            f"{common_dirs.freeimage_redist_dir()};{common_dirs.curl_bin_dir()}"
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
            msbuild_cmd = [
                "MSBuild",
                "ALL_BUILD.vcxproj",
                "/property:Configuration=Release",
                "/maxcpucount",
            ]
            if common_dirs.use_clang_cl():
                msbuild_cmd.extend(
                    [
                        "/property:LLVMInstallDir=" + common_dirs.llvm_install_dir(),
                        "/property:LLVMToolsVersion="
                        + common_dirs.llvm_tools_version(),
                    ]
                )
            subprocess.run(
                msbuild_cmd,
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
        if common_dirs.is_linux():
            oneapi_lib_dir = common_dirs.tbb_redist_dir()
            env["LD_LIBRARY_PATH"] = (
                oneapi_lib_dir + os.pathsep + env.get("LD_LIBRARY_PATH", "")
            ).rstrip(os.pathsep)
            if build_ext_libs.use_clang_in_linux():
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

python build_atlas.py [--use-asan] [--skip-test|--run-test] [--debug-version] [--release-pdb]
""",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--use-asan", action="store_true", help="use sanitizers")
    tests = parser.add_mutually_exclusive_group()
    tests.add_argument(
        "--skip-test",
        dest="skip_test",
        action="store_true",
        help="skip building and running tests",
    )
    tests.add_argument(
        "--run-test",
        dest="skip_test",
        action="store_false",
        help="force building and running tests (overrides auto-skip on release tags)",
    )
    parser.set_defaults(skip_test=None)
    parser.add_argument("--debug-version", action="store_true", help="debug version")
    parser.add_argument(
        "--release-pdb",
        action="store_true",
        help="emit PDBs for optimized Release builds on Windows without disabling Release IPO/LTO",
    )
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
        release_pdb=args.release_pdb,
        enable_network_tests=args.enable_network_tests,
    )
