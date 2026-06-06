import argparse
import glob
import logging
import os
from pathlib import Path
import shutil
import subprocess

import atlas_pypi
import build_ext_libs
import common_dirs
from download_atlas_test_data import download_atlas_test_data
from download_atlas_runtime_assets import download_atlas_runtime_assets
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


def _run_diagnostic_command(
    cmd: list[str],
    *,
    cwd: str,
    env: dict[str, str],
    title: str,
    timeout: int = 120,
) -> subprocess.CompletedProcess:
    logger.info("Diagnostic command (%s): %s", title, " ".join(cmd))
    try:
        result = subprocess.run(
            cmd,
            cwd=cwd,
            env=env,
            shell=False,
            check=False,
            text=True,
            capture_output=True,
            timeout=timeout,
        )
    except FileNotFoundError:
        logger.warning("Diagnostic command not found (%s): %s", title, cmd[0])
        return subprocess.CompletedProcess(cmd, 127)
    except subprocess.TimeoutExpired as e:
        logger.warning("Diagnostic command timed out (%s): %s", title, e)
        return subprocess.CompletedProcess(cmd, 124)

    if result.stdout:
        logger.info("%s stdout:\n%s", title, result.stdout.rstrip())
    if result.stderr:
        logger.info("%s stderr:\n%s", title, result.stderr.rstrip())
    if result.returncode != 0:
        logger.warning("%s exited with code %s", title, result.returncode)
    return result


def _find_windows_debugger_tool(tool_name: str, env: dict[str, str]) -> str | None:
    tool = shutil.which(tool_name, path=env.get("PATH"))
    if tool:
        return tool

    candidates = glob.glob(
        rf"C:\Program Files (x86)\Windows Kits\*\Debuggers\x64\{tool_name}"
    )
    candidates.extend(
        glob.glob(rf"C:\Program Files\Windows Kits\*\Debuggers\x64\{tool_name}")
    )
    if not candidates:
        return None
    return sorted(candidates)[-1]


def _find_windows_llvm_tool(tool_name: str, env: dict[str, str]) -> str | None:
    try:
        llvm_tool = os.path.join(common_dirs.llvm_bin_dir(), tool_name)
    except AssertionError:
        llvm_tool = None
    if llvm_tool and os.path.exists(llvm_tool):
        return llvm_tool
    return shutil.which(tool_name, path=env.get("PATH"))


def _find_windows_gflags(env: dict[str, str]) -> str | None:
    return _find_windows_debugger_tool("gflags.exe", env)


def _find_windows_cdb(env: dict[str, str]) -> str | None:
    return _find_windows_debugger_tool("cdb.exe", env)


def _find_windows_lldb(env: dict[str, str]) -> str | None:
    return _find_windows_llvm_tool("lldb.exe", env)


def _windows_focused_test_executable_names() -> list[str]:
    return [
        "zimglinksmoketest.exe",
        "zatlaslinksmoketest.exe",
        "zneutubeswcsignalfittertest.exe",
        "zimgometiffpacktest.exe",
        "zatlasheavytest.exe",
    ]


def _windows_discovered_test_executables() -> list[Path]:
    build_dir = Path(common_dirs.atlas_build_dir())
    executables = []
    seen_names = set()
    for executable in sorted(build_dir.rglob("*test.exe")):
        if not executable.is_file():
            continue
        normalized_name = executable.name.lower()
        if normalized_name in seen_names:
            logger.warning(
                "Skipping duplicate Windows test executable name for diagnostics: %s",
                executable,
            )
            continue
        seen_names.add(normalized_name)
        executables.append(executable)

    if not executables:
        logger.warning("No Windows *test.exe executables found under %s", build_dir)
    else:
        logger.info(
            "Discovered %d Windows test executables for diagnostics.",
            len(executables),
        )
    return executables


def _windows_diagnostic_test_executable_names() -> list[str]:
    discovered_names = [
        executable.name for executable in _windows_discovered_test_executables()
    ]
    if discovered_names:
        return discovered_names
    return _windows_focused_test_executable_names()


def _configure_windows_local_dumps(env: dict[str, str], dump_dir: str):
    Path(dump_dir).mkdir(parents=True, exist_ok=True)
    logger.info("Windows test crash dump directory: %s", dump_dir)

    for exe_name in _windows_diagnostic_test_executable_names():
        key = (
            r"HKCU\Software\Microsoft\Windows\Windows Error Reporting\LocalDumps"
            + "\\"
            + exe_name
        )
        for value_name, value_type, value in [
            ("DumpFolder", "REG_EXPAND_SZ", dump_dir),
            ("DumpType", "REG_DWORD", "2"),
            ("DumpCount", "REG_DWORD", "10"),
        ]:
            _run_diagnostic_command(
                [
                    "reg",
                    "add",
                    key,
                    "/v",
                    value_name,
                    "/t",
                    value_type,
                    "/d",
                    value,
                    "/f",
                ],
                cwd=common_dirs.atlas_build_dir(),
                env=env,
                title=f"WER LocalDumps {exe_name} {value_name}",
            )


def _cleanup_windows_local_dumps(env: dict[str, str]):
    for exe_name in _windows_diagnostic_test_executable_names():
        key = (
            r"HKCU\Software\Microsoft\Windows\Windows Error Reporting\LocalDumps"
            + "\\"
            + exe_name
        )
        _run_diagnostic_command(
            ["reg", "delete", key, "/f"],
            cwd=common_dirs.atlas_build_dir(),
            env=env,
            title=f"cleanup WER LocalDumps {exe_name}",
        )


def _summarize_windows_crash_dumps(dump_dir: str):
    dump_path = Path(dump_dir)
    if not dump_path.exists():
        logger.info("Windows test crash dump directory does not exist: %s", dump_dir)
        return

    dumps = sorted(dump_path.glob("*.dmp"))
    if not dumps:
        logger.info("No Windows test crash dumps were written to %s", dump_dir)
        return

    logger.warning("Windows test crash dumps written to %s:", dump_dir)
    for dump in dumps:
        logger.warning("  %s (%d bytes)", dump, dump.stat().st_size)


def _analyze_windows_crash_dumps(env: dict[str, str], dump_dir: str):
    dump_path = Path(dump_dir)
    if not dump_path.exists():
        return

    dumps = sorted(dump_path.glob("*.dmp"))
    if not dumps:
        return

    cdb = _find_windows_cdb(env)
    if cdb:
        for dump in dumps:
            _run_diagnostic_command(
                [cdb, "-z", str(dump), "-c", "!analyze -v;kv;q"],
                cwd=common_dirs.atlas_build_dir(),
                env=env,
                title=f"cdb analyze {dump.name}",
                timeout=600,
            )
        return

    lldb = _find_windows_lldb(env)
    if lldb:
        for dump in dumps:
            _run_diagnostic_command(
                [
                    lldb,
                    "--batch",
                    "-c",
                    str(dump),
                    "-o",
                    "thread backtrace all",
                    "-o",
                    "image list",
                ],
                cwd=common_dirs.atlas_build_dir(),
                env=env,
                title=f"lldb analyze {dump.name}",
                timeout=600,
            )
        return

    logger.warning(
        "Neither cdb.exe nor lldb.exe was found; crash dumps cannot be analyzed in log."
    )


def _configure_windows_page_heap(env: dict[str, str]) -> str | None:
    gflags = _find_windows_gflags(env)
    if not gflags:
        logger.warning("gflags.exe not found; skipping page heap diagnostics.")
        return None

    logger.info("Using gflags for page heap diagnostics: %s", gflags)
    for exe_name in _windows_diagnostic_test_executable_names():
        _run_diagnostic_command(
            [gflags, "/p", "/enable", exe_name, "/full"],
            cwd=common_dirs.atlas_build_dir(),
            env=env,
            title=f"enable page heap {exe_name}",
        )

    _run_diagnostic_command(
        [gflags, "/p"],
        cwd=common_dirs.atlas_build_dir(),
        env=env,
        title="list page heap settings",
    )
    return gflags


def _disable_windows_page_heap(gflags: str | None, env: dict[str, str]):
    if not gflags:
        return

    for exe_name in _windows_diagnostic_test_executable_names():
        _run_diagnostic_command(
            [gflags, "/p", "/disable", exe_name],
            cwd=common_dirs.atlas_build_dir(),
            env=env,
            title=f"disable page heap {exe_name}",
        )


def _log_windows_runtime_resolution(env: dict[str, str]):
    logger.info("Windows CTEST_PARALLEL_LEVEL: %s", env.get("CTEST_PARALLEL_LEVEL"))
    logger.info(
        "Windows ATLAS_TEST_STARTUP_DIAGNOSTICS: %s",
        env.get("ATLAS_TEST_STARTUP_DIAGNOSTICS"),
    )

    for tool_name in [
        "cdb.exe",
        "gflags.exe",
        "lldb.exe",
        "llvm-symbolizer.exe",
        "llvm-pdbutil.exe",
        "dumpbin.exe",
    ]:
        if tool_name.startswith("llvm") or tool_name == "lldb.exe":
            tool_path = _find_windows_llvm_tool(tool_name, env)
        elif tool_name in {"cdb.exe", "gflags.exe"}:
            tool_path = _find_windows_debugger_tool(tool_name, env)
        else:
            tool_path = shutil.which(tool_name, path=env.get("PATH"))
        if tool_path:
            logger.info("Windows diagnostic tool %s: %s", tool_name, tool_path)
        else:
            logger.warning("Windows diagnostic tool %s was not found.", tool_name)

    for dll_name in [
        "ucrtbase.dll",
        "vcruntime140.dll",
        "vcruntime140_1.dll",
        "msvcp140.dll",
        "concrt140.dll",
        "vcomp140.dll",
        "tbb12.dll",
        "tbbmalloc.dll",
        "tbbmalloc_proxy.dll",
        "Qt6Core.dll",
        "libcurl.dll",
    ]:
        _run_diagnostic_command(
            ["where", dll_name],
            cwd=common_dirs.atlas_build_dir(),
            env=env,
            title=f"where {dll_name}",
        )

    for exe_path in _windows_discovered_test_executables():
        _run_diagnostic_command(
            ["dumpbin", "/dependents", str(exe_path)],
            cwd=common_dirs.atlas_build_dir(),
            env=env,
            title=f"dumpbin dependents {exe_path.name}",
        )


def _focused_windows_test_commands() -> list[tuple[str, list[str]]]:
    return [
        (
            "zimglink smoke",
            [
                "zimglinksmoketest.exe",
                "--gtest_filter=ZImgLinkSmoke.StartsAndStops",
                "--gtest_also_run_disabled_tests",
                "--gtest_catch_exceptions=0",
            ],
        ),
        (
            "zatlaslink smoke",
            [
                "zatlaslinksmoketest.exe",
                "--gtest_filter=ZAtlasLinkSmoke.StartsAndStops",
                "--gtest_also_run_disabled_tests",
                "--gtest_catch_exceptions=0",
            ],
        ),
        (
            "zneutubeswcsignalfitter focused",
            [
                "zneutubeswcsignalfittertest.exe",
                "--gtest_filter=ZNeutubeSwcSignalFitter.FitsRadiusOnSimpleDisk",
                "--gtest_also_run_disabled_tests",
                "--gtest_catch_exceptions=0",
            ],
        ),
        (
            "zimgometiffpack focused",
            [
                "zimgometiffpacktest.exe",
                "--gtest_filter=ZImgOmeTiffPack.DetailedInfoLoadsMetadataOnDemand",
                "--gtest_also_run_disabled_tests",
                "--gtest_catch_exceptions=0",
            ],
        ),
        (
            "zatlasheavy focused",
            [
                "zatlasheavytest.exe",
                "--gtest_filter=Z3DBlockIdCollectorTest.IgnoresSentinelsAndSortsBothDirections",
                "--gtest_also_run_disabled_tests",
                "--gtest_catch_exceptions=0",
            ],
        ),
    ]


def _windows_gtest_list_commands() -> list[tuple[str, list[str]]]:
    return [
        (
            f"{executable.name} gtest list",
            [
                str(executable),
                "--gtest_list_tests",
                "--gtest_also_run_disabled_tests",
                "--gtest_catch_exceptions=0",
            ],
        )
        for executable in _windows_discovered_test_executables()
    ]


def _run_windows_gtest_list_preflight(env: dict[str, str]):
    logger.info("Running Windows GoogleTest listing preflight diagnostics.")
    for title, cmd in _windows_gtest_list_commands():
        _run_diagnostic_command(
            cmd,
            cwd=common_dirs.atlas_build_dir(),
            env=env,
            title=title,
            timeout=300,
        )


def _run_windows_focused_test_preflight(env: dict[str, str]):
    logger.info("Running focused Windows test preflight diagnostics.")
    for title, cmd in _focused_windows_test_commands():
        _run_diagnostic_command(
            cmd,
            cwd=common_dirs.atlas_build_dir(),
            env=env,
            title=title,
            timeout=300,
        )


def _run_windows_parallel_startup_probe(env: dict[str, str]):
    probe_commands = [
        (
            "parallel zimglink smoke",
            [
                "zimglinksmoketest.exe",
                "--gtest_filter=ZImgLinkSmoke.StartsAndStops",
                "--gtest_also_run_disabled_tests",
                "--gtest_catch_exceptions=0",
            ],
        ),
        (
            "parallel zatlaslink smoke",
            [
                "zatlaslinksmoketest.exe",
                "--gtest_filter=ZAtlasLinkSmoke.StartsAndStops",
                "--gtest_also_run_disabled_tests",
                "--gtest_catch_exceptions=0",
            ],
        ),
        (
            "parallel zneutubeswcsignalfitter",
            [
                "zneutubeswcsignalfittertest.exe",
                "--gtest_filter=ZNeutubeSwcSignalFitter.FitsRadiusOnSimpleDisk",
                "--gtest_also_run_disabled_tests",
                "--gtest_catch_exceptions=0",
            ],
        ),
    ]
    process_count = min(os.cpu_count() or 1, 16)
    logger.info(
        "Running Windows parallel startup probe with %d processes.", process_count
    )

    for title, cmd in probe_commands:
        logger.info("Starting %s: %s", title, " ".join(cmd))
        processes = []
        for _ in range(process_count):
            processes.append(
                subprocess.Popen(
                    cmd,
                    cwd=common_dirs.atlas_build_dir(),
                    env=env,
                    shell=False,
                    text=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                )
            )

        failures = []
        for idx, proc in enumerate(processes):
            try:
                stdout, stderr = proc.communicate(timeout=300)
            except subprocess.TimeoutExpired:
                proc.kill()
                stdout, stderr = proc.communicate()
                failures.append((idx, 124, stdout, stderr))
                continue
            if proc.returncode != 0:
                failures.append((idx, proc.returncode, stdout, stderr))

        if not failures:
            logger.info("%s passed for all %d processes.", title, process_count)
            continue

        logger.warning("%s had %d failing processes.", title, len(failures))
        for idx, returncode, stdout, stderr in failures:
            logger.warning("%s process %d exited with %s", title, idx, returncode)
            if stdout:
                logger.warning("%s process %d stdout:\n%s", title, idx, stdout.rstrip())
            if stderr:
                logger.warning("%s process %d stderr:\n%s", title, idx, stderr.rstrip())


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
    download_atlas_runtime_assets()

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
            f"{env['PATH']};{common_dirs.vc_CRT_redist_dir()};"
            f"{common_dirs.tbb_redist_dir()};{common_dirs.qt_bin_dir()};"
            f"{common_dirs.curl_bin_dir()}"
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
            env["ATLAS_TEST_STARTUP_DIAGNOSTICS"] = "1"
            # Network tests are opt-in. Force them off by default so CI/self-hosted
            # runners don't fail when outbound access is restricted.
            env["ATLAS_ENABLE_NETWORK_TESTS"] = "1" if enable_network_tests else "0"
            dump_dir = os.path.join(common_dirs.atlas_build_dir(), "windows-test-dumps")
            _configure_windows_local_dumps(env, dump_dir)
            page_heap_gflags = _configure_windows_page_heap(env)
            _log_windows_runtime_resolution(env)
            _run_windows_gtest_list_preflight(env)
            _run_windows_focused_test_preflight(env)
            _run_windows_parallel_startup_probe(env)
            ctest_cmd = [
                common_dirs.get_ctest_binary(),
                "--extra-verbose",
                "--output-on-failure",
            ]
            ctest_result = subprocess.CompletedProcess(ctest_cmd, 127)
            try:
                ctest_result = subprocess.run(
                    ctest_cmd,
                    cwd=common_dirs.atlas_build_dir(),
                    shell=False,
                    check=False,
                    env=env,
                )
            finally:
                _summarize_windows_crash_dumps(dump_dir)
                _analyze_windows_crash_dumps(env, dump_dir)
                _disable_windows_page_heap(page_heap_gflags, env)
                _cleanup_windows_local_dumps(env)

            if ctest_result.returncode != 0:
                raise subprocess.CalledProcessError(
                    ctest_result.returncode,
                    ctest_cmd,
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
