import argparse
import difflib
import glob
import json
import logging
import os
import shutil
import stat
import subprocess
import sys
from collections import OrderedDict, deque
from pathlib import Path

from common_dirs import (
    atlas_repository_dir,
    clang_cl_binary,
    curl_ca_bundle_path,
    curl_def_path,
    curl_dll_path,
    curl_import_lib_path,
    curl_root_dir,
    ext_build_dir,
    ext_dir,
    find_latest_windows_curl_package,
    find_src_package_with_glob,
    get_cmake_binary,
    get_ffmpeg_binary,
    get_gperf_dir,
    get_ninja_binary,
    get_package_top_level_folder,
    handleRemoveReadonly,
    install_cmake,
    install_ffmpeg,
    install_gperf,
    install_ninja,
    intel_sw_dir,
    is_internal_dev_environment,
    is_linux,
    is_mac,
    is_windows,
    lld_link_binary,
    llvm_bin_dir,
    llvm_install_dir,
    llvm_tools_version,
    mkl_dir,
    remove_old_src_folder_with_glob,
    remove_old_src_folders_with_glob,
    rm_tree,
    src_package_dir,
    tbb_dir,
    tbb_redist_dir,
    tbb_root_dir,
    unpack_file_to_folder,
    unpack_tool_to_target_dir,
    use_clang_cl,
    use_ninja,
    vs_install_dir,
    windows_msbuild_platform_toolset,
    windows_native_platform_toolset,
    windows_visual_studio_generator,
    windows_visual_studio_major_version,
)
from download_atlas_deps import download_atlas_deps
from install_oneapi_pip import install_oneapi_pip
from logger import setup_logger

logger = logging.getLogger(__name__)


def macos_min_version():
    return "12.0"


def cpp_standard() -> int:
    return 20


def use_clang_in_linux() -> bool:
    return is_linux()


def use_windows_clang_cl() -> bool:
    return is_windows() and use_clang_cl()


def parallel_jobs_count() -> int:
    return os.cpu_count() or 1


def make_parallel_jobs_arg() -> str:
    return "-j" + str(parallel_jobs_count())


def _clang_major_env() -> str:
    # Allow CI to control clang version
    return os.environ.get("ATLAS_CLANG_MAJOR") or os.environ.get("LLVM_VERSION") or "22"


def get_clang_in_linux() -> str:
    return f"clang-{_clang_major_env()}"


def get_clangplus_in_linux() -> str:
    return f"clang++-{_clang_major_env()}"


def update_or_clone_git_repository(repository_folder: str, repository_url: str):
    if os.path.exists(repository_folder):
        logger.info(f"git pull {Path(repository_folder).name}")
        subprocess.run(["git", "pull"], cwd=repository_folder, shell=False, check=False)
    else:
        subprocess.run(
            ["git", "clone", repository_url, repository_folder], shell=False, check=True
        )


def update_or_clone_git_repository_with_submodules(
    repository_folder: str, repository_url: str
):
    if os.path.exists(repository_folder):
        logger.info(f"git pull {Path(repository_folder).name}")
        subprocess.run(["git", "pull"], cwd=repository_folder, shell=False, check=False)
        subprocess.run(
            ["git", "submodule", "update", "--init", "--recursive"],
            cwd=repository_folder,
            shell=False,
            check=False,
        )
    else:
        subprocess.run(
            ["git", "clone", "--recursive", repository_url, repository_folder],
            shell=False,
            check=True,
        )


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
    logger.info(f"update submodule {Path(target_folder).name}")
    subprocess.run(
        [
            "git",
            "submodule",
            "update",
            "--init",
            "--remote",
            "--",
            f"src/3rdparty/{Path(target_folder).name}",
        ],
        cwd=atlas_repository_dir(),
        shell=False,
        check=True,
    )
    if tag is not None:
        subprocess.run(
            ["git", "checkout", tag], cwd=target_folder, shell=False, check=True
        )


def cleanup_git_submodule(target_folder: str):
    assert os.path.isfile(os.path.join(target_folder, ".git")), (
        f"{target_folder} is not git submodule"
    )
    subprocess.run(
        ["git", "reset", "--hard"], cwd=target_folder, shell=False, check=True
    )
    subprocess.run(["git", "clean", "-dff"], cwd=target_folder, shell=False, check=True)


def create_build_dir(src_dir: str):
    build_dir = os.path.normpath(
        os.path.join(ext_build_dir(), "__" + Path(src_dir).name)
    )
    if src_dir.endswith("ITK"):
        build_dir = os.path.normpath(
            os.path.join(ext_build_dir(), "_I")
        )  # ITK windows build dir length limit
    if os.path.exists(build_dir):
        shutil.rmtree(build_dir, ignore_errors=False, onexc=handleRemoveReadonly)
    os.mkdir(build_dir)
    logger.info(f"created build dir: {build_dir}")
    return build_dir


def create_arm64_install_dir(src_dir: str):
    install_dir = os.path.normpath(
        os.path.join(ext_build_dir(), "__arm64_" + Path(src_dir).name)
    )
    if os.path.exists(install_dir):
        shutil.rmtree(install_dir, ignore_errors=False, onexc=handleRemoveReadonly)
    os.mkdir(install_dir)
    return install_dir


_UNIVERSAL_HEADER_MARKER = "Generated by Atlas' macOS universal dependency merge"


def _install_path_variants(path: str) -> list[bytes]:
    return list(
        dict.fromkeys(
            [
                os.fsencode(os.path.normpath(path)),
                os.fsencode(os.path.abspath(path)),
                os.fsencode(os.path.realpath(path)),
            ]
        )
    )


def _normalize_arm64_install_paths(
    data: bytes, arm64_install_dir: str, final_install_dir: str
) -> bytes:
    final_install_path = _install_path_variants(final_install_dir)[0]
    for old_path in _install_path_variants(arm64_install_dir):
        data = data.replace(old_path, final_install_path)
    return data


def _arch_header_name(filename: str, arch: str) -> str:
    stem, suffix = os.path.splitext(filename)
    if suffix:
        return f"{stem}.atlas-{arch}{suffix}"
    return f"{filename}.atlas-{arch}"


def _universal_header_guard(relative_path: str) -> str:
    sanitized = "".join(ch if ch.isalnum() else "_" for ch in relative_path.upper())
    return f"ATLAS_UNIVERSAL_HEADER_{sanitized}"


def _is_generated_universal_header(data: bytes) -> bool:
    return _UNIVERSAL_HEADER_MARKER.encode("utf-8") in data


def _write_universal_header_dispatcher(
    target_header: str,
    relative_path: str,
    x86_64_data: bytes,
    arm64_data: bytes,
):
    target_dir = os.path.dirname(target_header)
    basename = os.path.basename(target_header)
    x86_64_header = _arch_header_name(basename, "x86_64")
    arm64_header = _arch_header_name(basename, "arm64")

    Path(os.path.join(target_dir, x86_64_header)).write_bytes(x86_64_data)
    Path(os.path.join(target_dir, arm64_header)).write_bytes(arm64_data)

    guard = _universal_header_guard(relative_path)
    wrapper = f"""/* {_UNIVERSAL_HEADER_MARKER}; do not edit. */
#ifndef {guard}
#define {guard}

#if defined(__arm64__) || defined(__aarch64__)
#include "{arm64_header}"
#elif defined(__x86_64__)
#include "{x86_64_header}"
#else
#error "Unsupported architecture for Atlas universal dependency header: {relative_path}"
#endif

#endif  /* {guard} */
"""
    Path(target_header).write_text(wrapper, encoding="utf-8")


def _remove_stale_universal_header_sidecars(target_header: str):
    target_dir = os.path.dirname(target_header)
    basename = os.path.basename(target_header)
    for arch in ("x86_64", "arm64"):
        sidecar = os.path.join(target_dir, _arch_header_name(basename, arch))
        if os.path.exists(sidecar):
            logger.info(f"remove stale universal header sidecar {sidecar}")
            os.remove(sidecar)


def _read_x86_64_header_data(target_header: str) -> bytes:
    target_data = Path(target_header).read_bytes()
    if not _is_generated_universal_header(target_data):
        return target_data

    target_dir = os.path.dirname(target_header)
    basename = os.path.basename(target_header)
    sidecar = os.path.join(target_dir, _arch_header_name(basename, "x86_64"))
    if not os.path.exists(sidecar):
        raise RuntimeError(
            f"Universal header wrapper exists without x86_64 sidecar: {target_header}"
        )
    return Path(sidecar).read_bytes()


def _merge_universal_headers(arm64_install_dir: str, final_install_dir: str):
    arm64_include_dir = os.path.join(arm64_install_dir, "include")
    final_include_dir = os.path.join(final_install_dir, "include")
    if not os.path.isdir(arm64_include_dir):
        return
    if not os.path.isdir(final_include_dir):
        raise RuntimeError(
            f"arm64 install has headers but final install does not: {arm64_include_dir}"
        )

    generated_wrappers = []
    identical_headers = 0
    for root, _, files in os.walk(arm64_include_dir):
        for name in files:
            arm64_header = os.path.join(root, name)
            relative_path = os.path.relpath(arm64_header, arm64_include_dir)
            target_header = os.path.join(final_include_dir, relative_path)

            if Path(arm64_header).is_symlink() or Path(target_header).is_symlink():
                if not os.path.lexists(target_header):
                    raise RuntimeError(
                        f"arm64 header symlink has no x86_64 counterpart: {relative_path}"
                    )
                if (
                    not Path(arm64_header).is_symlink()
                    or not Path(target_header).is_symlink()
                    or os.readlink(arm64_header) != os.readlink(target_header)
                ):
                    raise RuntimeError(
                        "Cannot safely merge universal header symlink mismatch: "
                        f"{relative_path}"
                    )
                identical_headers += 1
                continue

            if not os.path.isfile(arm64_header):
                continue
            if not os.path.exists(target_header):
                raise RuntimeError(
                    f"arm64 header has no x86_64 counterpart: {relative_path}"
                )
            if not os.path.isfile(target_header):
                raise RuntimeError(
                    f"x86_64 counterpart is not a regular header: {target_header}"
                )

            x86_64_data = _read_x86_64_header_data(target_header)
            arm64_data = _normalize_arm64_install_paths(
                Path(arm64_header).read_bytes(), arm64_install_dir, final_install_dir
            )

            if x86_64_data == arm64_data:
                if _is_generated_universal_header(Path(target_header).read_bytes()):
                    Path(target_header).write_bytes(x86_64_data)
                _remove_stale_universal_header_sidecars(target_header)
                identical_headers += 1
                continue

            _write_universal_header_dispatcher(
                target_header, relative_path, x86_64_data, arm64_data
            )
            generated_wrappers.append(relative_path)

    if generated_wrappers:
        logger.info(
            "created %d architecture-dispatched universal headers: %s",
            len(generated_wrappers),
            ", ".join(generated_wrappers),
        )
    else:
        logger.info(f"verified {identical_headers} universal headers are identical")


def create_universal_binaries(
    arm64_install_dir, final_install_dir, remove_dylib: bool = False
):
    assert is_mac()
    logger.info(f"{arm64_install_dir} to {final_install_dir}")
    for root, dirs, files in os.walk(arm64_install_dir):
        for name in files:
            filename = os.path.join(root, name)
            if Path(filename).is_symlink():
                if remove_dylib and filename.endswith(".dylib"):
                    target_filename = filename.replace(
                        arm64_install_dir, final_install_dir
                    )
                    logger.info(f"deleting {target_filename}")
                    os.remove(target_filename)
                continue
            if (
                filename.endswith(".a")
                or filename.endswith(".dylib")
                or root.endswith("bin")
            ):
                if name.endswith(".sh"):  # text file
                    continue
                if name == "c_rehash":  # text file
                    continue
                if name == "libpng16-config":
                    continue
                if (
                    name == "xzdiff"
                    or name == "xzgrep"
                    or name == "xzless"
                    or name == "xzmore"
                ):  # text file
                    continue
                target_filename = filename.replace(arm64_install_dir, final_install_dir)
                if name.startswith("libtegra_hal.a"):
                    logger.info(f"copy {filename} to {target_filename}")
                    shutil.copyfile(filename, target_filename)
                    continue
                if remove_dylib and filename.endswith(".dylib"):
                    logger.info(f"deleting {target_filename}")
                    os.remove(target_filename)
                elif not os.path.exists(target_filename):
                    logger.info(f"copy {filename} to {target_filename}")
                    shutil.copyfile(filename, target_filename)
                else:
                    logger.info(f"merge {filename} to {target_filename}")
                    subprocess.run(
                        [
                            "lipo",
                            "-create",
                            filename,
                            target_filename,
                            "-output",
                            target_filename,
                        ],
                        shell=False,
                        check=True,
                    )
    _merge_universal_headers(arm64_install_dir, final_install_dir)


def build_macos_split_or_single(src_dir: str, install_dir: str, build_arch):
    build_arch(install_dir, arm64_only=False)
    if is_mac():
        arm64_install_dir = create_arm64_install_dir(src_dir)
        try:
            build_arch(arm64_install_dir, arm64_only=True)
            create_universal_binaries(arm64_install_dir, install_dir)
        finally:
            rm_tree(arm64_install_dir)


def cmake_prefix_path_for_arch(active_install_dir: str, install_dir: str) -> str:
    paths = [active_install_dir, install_dir]
    normalized_paths = []
    for path in paths:
        normalized_path = os.path.normpath(path)
        if normalized_path not in normalized_paths:
            normalized_paths.append(normalized_path)
    return ";".join(normalized_paths)


def cmake_path(path: str) -> str:
    return os.path.normpath(path).replace("\\", "/")


def get_bak_file_name(orig_file: str):
    return orig_file + ".bak"


def remove_path_contains(path: str, env=os.environ):
    env["PATH"] = os.pathsep.join(
        [x for x in env["PATH"].split(os.pathsep) if path.lower() not in x.lower()]
    )


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
        logger.info(f"{file} removed")


def _curl_root_dir_from_package(package_name: str) -> str:
    package_folder = get_package_top_level_folder(package_name)
    if package_folder:
        return os.path.join(ext_build_dir(), package_folder)
    return os.path.join(
        ext_build_dir(), os.path.splitext(os.path.basename(package_name))[0]
    )


def ensure_windows_curl_sdk() -> str:
    assert is_windows()

    package_name = find_latest_windows_curl_package()
    package_unpack_folder = _curl_root_dir_from_package(package_name)
    logger.info(f"curl package unpack folder: {package_unpack_folder}")
    if not os.path.exists(package_unpack_folder):
        remove_old_src_folders_with_glob(os.path.join(ext_build_dir(), "curl-*win*"))
        unpack_file_to_folder(package_name, ext_build_dir())
        assert os.path.exists(package_unpack_folder)

    dll_path = curl_dll_path()
    def_path = curl_def_path()
    import_lib_path = curl_import_lib_path()
    os.makedirs(os.path.dirname(import_lib_path), exist_ok=True)
    if (not os.path.exists(import_lib_path)) or (
        os.path.getmtime(import_lib_path) < os.path.getmtime(def_path)
    ):
        env = get_vcvars_environment()
        lib_exe = shutil.which("lib.exe", path=env.get("PATH")) or shutil.which(
            "lib", path=env.get("PATH")
        )
        if lib_exe is None:
            raise RuntimeError(
                "MSVC environment setup failed: `lib.exe` not found on PATH after vcvarsall.\n"
                "The Windows curl SDK import library must be generated with MSVC tools. "
                "Ensure Visual Studio is installed and `util/common_dirs.py:vs_install_dir()` "
                "points to it."
            )
        logger.info(f"Generating libcurl import library with {lib_exe}")
        subprocess.run(
            [
                lib_exe,
                f"/def:{def_path}",
                "/machine:x64",
                f"/name:{os.path.basename(dll_path)}",
                f"/out:{import_lib_path}",
            ],
            shell=False,
            check=True,
            env=env,
        )
    assert os.path.exists(import_lib_path)
    assert os.path.exists(curl_ca_bundle_path())
    return curl_root_dir()


def remove_installed_dynamic_library(install_dir: str, libnames: list):
    for libname in libnames:
        if is_linux():
            glob_remove(os.path.join(install_dir, "lib", f"lib{libname}.so*"))
        elif is_windows():
            filename = os.path.join(install_dir, "lib", f"{libname}.lib")
            if os.path.exists(filename):
                logger.info(f"deleting {filename}")
                os.remove(filename)
            filename = os.path.join(install_dir, "bin", f"{libname}.dll")
            if os.path.exists(filename):
                logger.info(f"deleting {filename}")
                os.remove(filename)
        else:
            glob_remove(os.path.join(install_dir, "lib", f"lib{libname}.*dylib"))


def get_vcvars_environment(
    remove_conda_from_path: bool = True, remove_scoop_from_path: bool = True
):
    """
    Returns a dictionary containing the environment variables set up by vcvarsall.bat amd64
    """

    vcvars = os.path.normpath(
        os.path.join(vs_install_dir(), "VC", "Auxiliary", "Build", "vcvarsall.bat")
    )
    return get_enviroment_from_shell_script(
        vcvars,
        "x64",
        remove_conda_from_path=remove_conda_from_path,
        remove_scoop_from_path=remove_scoop_from_path,
    )


def get_enviroment_from_shell_script(
    script: str,
    para: str = "",
    start_env=os.environ,
    remove_conda_from_path: bool = True,
    remove_scoop_from_path: bool = True,
):
    python = sys.executable
    if is_windows():
        process = subprocess.Popen(
            '"{}" {} >nul && "{}" -c "import os, json; print(json.dumps(dict(os.environ)))"'.format(
                script, para, python
            ),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            shell=False,
            universal_newlines=True,
            env=start_env,
        )
    else:
        source = '. "{}" {}'.format(script, para)
        dump = '"{}" -c "import os, json; print(json.dumps(dict(os.environ)))"'.format(
            python
        )
        process = subprocess.Popen(
            ["/bin/bash", "-c", "{} && {}".format(source, dump)],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            shell=False,
            universal_newlines=True,
            env=start_env,
        )
    stdout, stderr = process.communicate()
    exitcode = process.wait()
    if exitcode != 0:
        logger.info(stdout)
        logger.info(stderr)
        raise Exception("Got error code {} from subprocess.".format(exitcode))
    env = json.loads(stdout.strip())
    if remove_conda_from_path:
        remove_path_contains("miniconda", env)
        remove_path_contains("anaconda", env)
    if remove_scoop_from_path:
        remove_path_contains("scoop", env)
    remove_path_contains("mingw", env)
    if is_windows() and use_clang_cl():
        env["PATH"] = llvm_bin_dir() + os.pathsep + env["PATH"]
        env["LLVMInstallDir"] = llvm_install_dir()
        env["LLVMToolsVersion"] = llvm_tools_version()
    return env


def get_tbb_env():
    env = {}
    env["TBBROOT"] = tbb_root_dir()
    env["TBB_ROOT"] = env["TBBROOT"]
    return env


def get_common_build_flags(
    cpp_standard: int = cpp_standard(),
    with_optimization: bool = False,
    universal: bool = False,
    arm64_only: bool = False,
    no_hidden_visibility: bool = False,
):
    res = {}
    optimization = " -O3" if with_optimization else ""
    if is_mac():
        osx_sysroot = r"/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk"
        assert os.path.exists(osx_sysroot)

        arch_flag = " -mavx "
        if universal:
            arch_flag = " -mavx -mcpu=apple-m1 "
        elif arm64_only:
            arch_flag = " -mcpu=apple-m1 "

        res["CC"] = "clang"
        res["CFLAGS"] = (
            f"-isysroot {osx_sysroot} -mmacosx-version-min={macos_min_version()} "
            f"-fPIC {'' if no_hidden_visibility else '-fvisibility=hidden'} {arch_flag}"
            + optimization
        )
        res["LDFLAGS"] = "-stdlib=libc++"
        res["CXX"] = "clang++"
        res["CXXFLAGS"] = (
            f"-stdlib=libc++ -std=c++{cpp_standard} "
            f"-isysroot {osx_sysroot} -mmacosx-version-min={macos_min_version()} "
            f"{'' if no_hidden_visibility else '-fvisibility=hidden -fvisibility-inlines-hidden'} "
            f"-fPIC {arch_flag}" + optimization
        )
        res["ASMFLAGS"] = (
            f"-isysroot {osx_sysroot} -mmacosx-version-min={macos_min_version()}"
        )
    elif is_linux():
        if use_clang_in_linux():
            res["CC"] = get_clang_in_linux()
            res["CFLAGS"] = (
                f"-fPIC {'' if no_hidden_visibility else '-fvisibility=hidden'} -mavx"
                + optimization
            )
            res["CXX"] = get_clangplus_in_linux()
            res["CXXFLAGS"] = (
                f"-std=c++{cpp_standard} -fPIC "
                f"{'' if no_hidden_visibility else '-fvisibility=hidden -fvisibility-inlines-hidden'} "
                f"-mavx" + optimization
            )
        else:
            res["CFLAGS"] = (
                f"-fPIC {'' if no_hidden_visibility else '-fvisibility=hidden'} -mavx"
                + optimization
            )
            res["CXXFLAGS"] = (
                f"-std=c++{cpp_standard} -fPIC "
                f"{'' if no_hidden_visibility else '-fvisibility=hidden -fvisibility-inlines-hidden'} "
                f"-mavx" + optimization
            )
    elif is_windows():
        optimization = " /O2" if with_optimization else ""
        clang_cl_warning_suppressions = ""
        clang_cl_cxx_compat_suppressions = ""
        if use_windows_clang_cl():
            # Vendored projects built under clang-cl frequently enable very broad
            # warning groups intended for cl.exe. Keep the actionable diagnostics
            # while suppressing low-signal compatibility/style noise.
            clang_cl_warning_suppressions = (
                " /clang:-Wno-unused-command-line-argument"
                " /clang:-Wno-shadow-header"
                " /clang:-Wno-reserved-identifier"
                " /clang:-Wno-reserved-macro-identifier"
                " /clang:-Wno-unsafe-buffer-usage"
            )
            clang_cl_cxx_compat_suppressions = (
                " /clang:-Wno-c++98-compat"
                " /clang:-Wno-c++98-compat-pedantic"
                " /clang:-Wno-pre-c++14-compat"
                " /clang:-Wno-pre-c++17-compat"
                " /clang:-Wno-pre-c++20-compat"
            )
        res["CFLAGS"] = (
            "/utf-8 /arch:AVX" + clang_cl_warning_suppressions + optimization
        )
        res["CXXFLAGS"] = (
            f"/utf-8 /std:c++{cpp_standard} /EHsc /D_SILENCE_ALL_CXX17_DEPRECATION_WARNINGS "
            f"/DNOMINMAX /arch:AVX"
            + clang_cl_warning_suppressions
            + clang_cl_cxx_compat_suppressions
            + optimization
        )
    return res


def get_env_for_config_make(
    cpp_standard: int = cpp_standard(),
    remove_conda_from_path: bool = True,
    remove_scoop_from_path: bool = True,
    with_optimization: bool = True,
    universal: bool = False,
    arm64_only: bool = False,
    no_hidden_visibility: bool = False,
):
    env = (
        get_vcvars_environment(
            remove_conda_from_path=remove_conda_from_path,
            remove_scoop_from_path=remove_scoop_from_path,
        )
        if is_windows()
        else os.environ.copy()
    )
    cbf = get_common_build_flags(
        cpp_standard=cpp_standard,
        with_optimization=with_optimization,
        universal=universal,
        arm64_only=arm64_only,
        no_hidden_visibility=no_hidden_visibility,
    )
    if is_mac():
        env["CC"] = cbf["CC"]
        env["CFLAGS"] = cbf["CFLAGS"]
        env["LDFLAGS"] = cbf["LDFLAGS"]
        env["CXX"] = cbf["CXX"]
        env["CXXFLAGS"] = cbf["CXXFLAGS"]
    elif is_linux():
        if use_clang_in_linux():
            env["CC"] = cbf["CC"]
            env["CFLAGS"] = cbf["CFLAGS"]
            env["CXX"] = cbf["CXX"]
            env["CXXFLAGS"] = cbf["CXXFLAGS"]
        else:
            env["CFLAGS"] = cbf["CFLAGS"]
            env["CXXFLAGS"] = cbf["CXXFLAGS"]
        try:
            oneapi_lib_dir = tbb_redist_dir()
        except AssertionError:
            oneapi_lib_dir = ""
        if oneapi_lib_dir:
            env["LD_LIBRARY_PATH"] = (
                oneapi_lib_dir + os.pathsep + env.get("LD_LIBRARY_PATH", "")
            ).rstrip(os.pathsep)
    elif is_windows():
        env["CFLAGS"] = cbf["CFLAGS"]
        env["CXXFLAGS"] = cbf["CXXFLAGS"]
    return env


def get_cmake_cmd_common_part(
    install_dir: str,
    *,
    use_ninja: bool = use_ninja(),
    cpp_standard: int = cpp_standard(),
    cpp_extention: bool = False,
    universal: bool = False,
    arm64_only: bool = False,
    no_hidden_visibility: bool = False,
    enable_cxx: bool = True,
):
    cbf = get_common_build_flags(
        cpp_standard=cpp_standard,
        with_optimization=False,
        universal=universal,
        arm64_only=arm64_only,
        no_hidden_visibility=no_hidden_visibility,
    )
    if is_windows():
        res = [
            get_cmake_binary(),  # '-E', 'echo',
            "-DCMAKE_BUILD_TYPE=Release",
            "-DCMAKE_PREFIX_PATH=" + ext_build_dir(),
            # '-DCMAKE_MODULE_PATH=' + ext_build_dir(),
            # zlib 1.3.2's CMake build installs the Windows static archive as
            # zs.lib. CMake's FindZLIB only searches that name when static
            # lookup is requested.
            "-DZLIB_USE_STATIC_LIBS:BOOL=ON",
            "-DCMAKE_INSTALL_PREFIX=" + install_dir,
            "" if no_hidden_visibility else "-DCMAKE_VISIBILITY_INLINES_HIDDEN=ON",
            "" if no_hidden_visibility else "-DCMAKE_C_VISIBILITY_PRESET=hidden",
            f"-DCMAKE_C_FLAGS:STRING={cbf['CFLAGS']}",
            "-DCMAKE_LIBRARY_ARCHITECTURE=x86_64",
        ]
        if enable_cxx:
            res.extend(
                [
                    ""
                    if no_hidden_visibility
                    else "-DCMAKE_CXX_VISIBILITY_PRESET=hidden",
                    f"-DCMAKE_CXX_STANDARD={cpp_standard}",
                    "-DCMAKE_CXX_STANDARD_REQUIRED=ON",
                    "-DCMAKE_CXX_EXTENSIONS=OFF",
                    f"-DCMAKE_CXX_FLAGS:STRING={cbf['CXXFLAGS']}",
                ]
            )
        if use_clang_cl():
            res.append("-DCMAKE_C_COMPILER:FILEPATH=" + clang_cl_binary())
            if enable_cxx:
                res.append("-DCMAKE_CXX_COMPILER:FILEPATH=" + clang_cl_binary())
            res.append("-DCMAKE_LINKER:FILEPATH=" + lld_link_binary())
        if use_ninja:
            res.extend(["-G", "Ninja", "-DCMAKE_MAKE_PROGRAM=" + get_ninja_binary()])
        else:
            toolset = "host=x64"
            if use_clang_cl():
                toolset = "ClangCL,host=x64"
            res.extend(
                ["-G", windows_visual_studio_generator(), "-A", "x64", "-T", toolset]
            )
            if use_clang_cl():
                res.append(
                    "-DCMAKE_VS_GLOBALS:STRING="
                    + ";".join(
                        [
                            "LLVMInstallDir=" + llvm_install_dir(),
                            "LLVMToolsVersion=" + llvm_tools_version(),
                        ]
                    )
                )
        return res
    elif is_linux():
        res = [
            get_cmake_binary(),  # '-E', 'echo',
            "-DCMAKE_BUILD_TYPE=Release",
            "-DCMAKE_PREFIX_PATH=" + ext_build_dir(),
            "-DCMAKE_IGNORE_PREFIX_PATH=/usr/local",
            "-DCMAKE_IGNORE_PATH=/usr/local/bin",
            "-DCMAKE_MODULE_PATH=" + ext_build_dir(),
            "-DCMAKE_INSTALL_PREFIX=" + install_dir,
            "" if no_hidden_visibility else "-DCMAKE_VISIBILITY_INLINES_HIDDEN=ON",
            "" if no_hidden_visibility else "-DCMAKE_C_VISIBILITY_PRESET=hidden",
            f"-DCMAKE_C_FLAGS:STRING={cbf['CFLAGS']}",
            "-DCMAKE_LIBRARY_ARCHITECTURE=x86_64",
        ]
        if enable_cxx:
            res.extend(
                [
                    ""
                    if no_hidden_visibility
                    else "-DCMAKE_CXX_VISIBILITY_PRESET=hidden",
                    f"-DCMAKE_CXX_STANDARD={cpp_standard}",
                    "-DCMAKE_CXX_STANDARD_REQUIRED=ON",
                    "-DCMAKE_CXX_EXTENSIONS=" + ("ON" if cpp_extention else "OFF"),
                    f"-DCMAKE_CXX_FLAGS:STRING={cbf['CXXFLAGS']}",
                ]
            )
        if use_ninja:
            res.extend(["-G", "Ninja", "-DCMAKE_MAKE_PROGRAM=" + get_ninja_binary()])
        return res
    else:
        osx_sysroot = r"/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk"
        assert os.path.exists(osx_sysroot)

        arch = "x86_64"
        if universal:
            arch = "x86_64;arm64"
        elif arm64_only:
            arch = "arm64"

        res = [
            get_cmake_binary(),  # '-E', 'echo',
            "-DCMAKE_BUILD_TYPE=Release",
            # "-DCMAKE_SYSTEM_NAME=Darwin",
            # On macOS, downstream CMake projects must key target-arch
            # decisions off CMAKE_OSX_ARCHITECTURES, not
            # CMAKE_SYSTEM_PROCESSOR. In Rosetta/x86_64 shells, CMake can
            # still report CMAKE_SYSTEM_PROCESSOR=x86_64 for an arm64 target.
            "-DCMAKE_PREFIX_PATH=" + ext_build_dir(),
            "-DCMAKE_IGNORE_PREFIX_PATH=/usr/local",
            "-DCMAKE_IGNORE_PATH=/usr/local/bin",
            "-DCMAKE_FIND_FRAMEWORK=LAST",
            "-DCMAKE_MODULE_PATH=" + ext_build_dir(),
            "-DCMAKE_INSTALL_PREFIX=" + install_dir,
            "" if no_hidden_visibility else "-DCMAKE_VISIBILITY_INLINES_HIDDEN=ON",
            "" if no_hidden_visibility else "-DCMAKE_C_VISIBILITY_PRESET=hidden",
            "-DCMAKE_OSX_DEPLOYMENT_TARGET=" + macos_min_version(),
            "-DCMAKE_OSX_SYSROOT=" + osx_sysroot,
            f"-DCMAKE_OSX_ARCHITECTURES={arch}",
            f"-DCMAKE_C_FLAGS:STRING={cbf['CFLAGS']}",
        ]
        if enable_cxx:
            res.extend(
                [
                    ""
                    if no_hidden_visibility
                    else "-DCMAKE_CXX_VISIBILITY_PRESET=hidden",
                    f"-DCMAKE_CXX_STANDARD={cpp_standard}",
                    "-DCMAKE_CXX_STANDARD_REQUIRED=ON",
                    "-DCMAKE_CXX_EXTENSIONS=OFF",
                    f"-DCMAKE_CXX_FLAGS:STRING={cbf['CXXFLAGS']}",
                ]
            )
        if use_ninja:
            res.extend(["-G", "Ninja", "-DCMAKE_MAKE_PROGRAM=" + get_ninja_binary()])
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


def build_and_install_cmakecmd(
    cmakecmd,
    build_dir: str,
    *,
    additional_env=None,
    use_ninja=use_ninja(),
    use_cmake=False,
    ninja_para: str = "install",
):
    cmakecmd[:] = [x for x in cmakecmd if x]

    def apply_additional_env(env: dict[str, str]):
        if additional_env is None:
            return
        for key, value in additional_env.items():
            if key == "PATH" and value:
                env[key] = value + os.pathsep + env.get(key, "")
            else:
                env[key] = value

    if is_windows():
        env = get_vcvars_environment()
        apply_additional_env(env)
        subprocess.run(cmakecmd, cwd=build_dir, shell=False, check=True, env=env)
        if use_cmake:
            subprocess.run(
                [get_cmake_binary(), "--build", ".", "--target", "install"],
                cwd=build_dir,
                shell=False,
                check=True,
                env=env,
            )
        elif use_ninja:
            subprocess.run(
                [get_ninja_binary(), ninja_para],
                cwd=build_dir,
                shell=False,
                check=True,
                env=env,
            )
        else:
            msbuild_cmd = [
                "MSBuild",
                "INSTALL.vcxproj",
                "/property:Configuration=Release",
                "/maxcpucount",
            ]
            if use_clang_cl():
                msbuild_cmd.extend(
                    [
                        "/property:LLVMInstallDir=" + llvm_install_dir(),
                        "/property:LLVMToolsVersion=" + llvm_tools_version(),
                    ]
                )
            subprocess.run(
                msbuild_cmd,
                cwd=build_dir,
                shell=True,
                check=True,
                env=env,
            )
    else:
        env = get_env_for_config_make(with_optimization=False)
        apply_additional_env(env)
        if use_cmake:
            subprocess.run(cmakecmd, cwd=build_dir, shell=False, check=True, env=env)
            subprocess.run(
                [get_cmake_binary(), "--build", ".", "--target", "install"],
                cwd=build_dir,
                shell=False,
                check=True,
                env=env,
            )
        elif use_ninja:
            subprocess.run(cmakecmd, cwd=build_dir, shell=False, check=True, env=env)
            subprocess.run(
                [get_ninja_binary(), ninja_para],
                cwd=build_dir,
                shell=False,
                check=True,
                env=env,
            )
        else:
            subprocess.run(cmakecmd, cwd=build_dir, shell=False, check=True, env=env)
            subprocess.run(
                ["make", "-j" + str(os.cpu_count()), "install"],
                cwd=build_dir,
                shell=False,
                check=True,
                env=env,
            )


def patch_file(
    orig_file: str, from_texts: list, to_texts: list, keep_bak_file: bool = True
) -> str:
    assert len(from_texts) == len(to_texts), (
        f"{orig_file}: from_texts/to_texts length mismatch"
    )

    txt = Path(orig_file).read_text(encoding="utf-8", errors="ignore")
    original_txt = txt

    for idx, (from_text, to_text) in enumerate(zip(from_texts, to_texts), start=1):
        if not from_text:
            continue

        assert from_text in txt, f"{orig_file}: {from_text}"
        txt = txt.replace(from_text, str(to_text))

    bak_file = get_bak_file_name(orig_file)
    tmp_file = orig_file + ".tmp"

    created_backup = False
    if not os.path.exists(bak_file):
        try:
            shutil.copy2(orig_file, bak_file)
            created_backup = True
        except Exception as e:
            raise RuntimeError(
                f"patch_file(): failed to create backup {bak_file} for {orig_file}: {e}"
            ) from e

    with open(tmp_file, mode="w", encoding="utf-8") as f:
        f.write(txt)
    os.replace(tmp_file, orig_file)

    # Log a unified diff (same behavior as the historical patcher).
    logger.info(
        "".join(
            difflib.unified_diff(
                original_txt.splitlines(keepends=True),
                txt.splitlines(keepends=True),
                fromfile=orig_file,
                tofile="<new>",
            )
        )
    )

    if not keep_bak_file and created_backup:
        try:
            os.remove(bak_file)
        except FileNotFoundError:
            pass

    return bak_file


class FilePatcher:
    def __init__(self, orig_file, from_texts, to_texts, patch_condition=lambda: True):
        self.orig_file = orig_file
        self.from_texts = from_texts
        self.to_texts = to_texts
        self.patch_condition = patch_condition
        self.bak_file = None

    def patch_file(self):
        if not self.patch_condition():
            # logger.debug("Patch condition not met, skipping patch.")
            return

        self.bak_file = patch_file(
            orig_file=self.orig_file,
            from_texts=self.from_texts,
            to_texts=self.to_texts,
            keep_bak_file=True,
        )
        logger.info(f"Patched {self.orig_file}")

    def restore_file(self):
        if not self.patch_condition():
            # logger.debug("Patch condition not met, skipping restore.")
            return

        if self.bak_file and os.path.exists(self.bak_file):
            os.replace(self.bak_file, self.orig_file)
            logger.info(f"Restored {self.orig_file} from backup.")


class PatchManager:
    def __init__(self, patches: list[FilePatcher]):
        self.patches = patches

    def apply_patches(self):
        applied = []
        try:
            for patcher in self.patches:
                patcher.patch_file()
                applied.append(patcher)
        except Exception:
            # If patch application fails mid-way, try to restore what we already
            # changed so callers don't have to remember to do it correctly.
            for patcher in reversed(applied):
                try:
                    patcher.restore_file()
                except Exception:
                    pass
            raise

    def restore_files(self):
        for patcher in reversed(self.patches):
            patcher.restore_file()


def build_fast_float(src_dir: str, install_dir: str):
    shutil.copytree(
        os.path.join(src_dir, "include", "fast_float"),
        os.path.join(install_dir, "include", "fast_float"),
        dirs_exist_ok=True,
    )


def build_zlib(src_dir: str, install_dir: str):
    build_dir = create_build_dir(src_dir)

    try:
        cmakecmd = get_cmake_cmd_common_part(install_dir, universal=True)

        cmakecmd.extend(
            [
                "-DZLIB_BUILD_TESTING:BOOL=OFF",
                "-DZLIB_BUILD_SHARED:BOOL=OFF",
                "-DZLIB_BUILD_STATIC:BOOL=ON",
            ]
        )

        cmakecmd.extend([src_dir])
        build_and_install_cmakecmd(cmakecmd, build_dir)
    finally:
        shutil.rmtree(build_dir, ignore_errors=False)


def write_boost_clang_win_user_config(config_path: str):
    assert is_windows()

    clang_cl = os.path.normpath(clang_cl_binary()).replace("\\", "/")
    config_text = "\n".join(
        [
            "using clang-win",
            f"    : {llvm_tools_version()}",
            f'    : "{clang_cl}"',
            "    : <assembler>ml.exe",
            "      <assembler-64>ml64.exe",
            "      <archiver>lib.exe",
            "      <manifest-tool>mt.exe",
            "      <resource-compiler>rc.exe",
            "      <mc-compiler>mc.exe",
            "      <idl-compiler>midl.exe",
            "    ;",
            "",
        ]
    )
    with open(config_path, "w", encoding="ascii", newline="\n") as f:
        f.write(config_text)


def build_boost(src_dir: str, install_dir: str):
    try:
        shutil.rmtree(os.path.join(src_dir, "bin.v2"), ignore_errors=True)
        if is_windows():
            cbf = get_common_build_flags(with_optimization=True)
            env = get_vcvars_environment()
            subprocess.run(["bootstrap"], cwd=src_dir, shell=True, check=True, env=env)
            boost_b2_cmd = [
                ".\\b2",
                "--prefix=" + install_dir,
                f"cxxflags={cbf['CXXFLAGS']}",
                "--with-headers",
                "--with-context",
                "--with-filesystem",
                "--with-program_options",
                "--with-thread",
                "--with-charconv",
            ]
            if use_clang_cl():
                clang_win_user_config = os.path.join(
                    install_dir, "boost-clang-win-user-config.jam"
                )
                os.makedirs(os.path.dirname(clang_win_user_config), exist_ok=True)
                write_boost_clang_win_user_config(clang_win_user_config)
                boost_b2_cmd.append("--user-config=" + clang_win_user_config)
                boost_b2_cmd.append("toolset=clang-win")
            boost_b2_cmd.extend(
                [
                    "address-model=64",
                    "variant=release",
                    "link=static",
                    "threading=multi",
                    "runtime-link=shared",
                    "install",
                ]
            )
            subprocess.run(
                boost_b2_cmd,
                cwd=src_dir,
                shell=True,
                check=True,
                env=env,
            )
        elif is_mac():
            arm64_install_dir = create_arm64_install_dir(src_dir)
            try:
                cbf = get_common_build_flags(with_optimization=True)
                env = get_env_for_config_make()
                subprocess.run(
                    [
                        "./bootstrap.sh",
                        "--with-libraries=headers,context,filesystem,program_options,thread,charconv",
                        "--without-icu",
                        "--prefix=" + install_dir,
                    ],
                    cwd=src_dir,
                    shell=False,
                    check=True,
                    env=env,
                )
                subprocess.run(
                    [
                        "./b2",
                        "--disable-icu",
                        "variant=release",
                        "link=static",
                        "threading=multi",
                        "runtime-link=shared",
                        f"cxxflags={cbf['CXXFLAGS']}",
                        f"linkflags={cbf['LDFLAGS']}",
                        f"cflags={cbf['CFLAGS']}",
                        f"asmflags={cbf['ASMFLAGS']}",
                        "target-os=darwin",
                        "architecture=x86",
                        "abi=sysv",
                        f"cxxflags={cbf['CXXFLAGS']} -arch x86_64",
                        f"linkflags={cbf['LDFLAGS']} -arch x86_64",
                        f"cflags={cbf['CFLAGS']} -arch x86_64",
                        f"asmflags={cbf['ASMFLAGS']} -arch x86_64",
                        "install",
                    ],
                    cwd=src_dir,
                    shell=False,
                    check=True,
                    env=env,
                )

                cbf = get_common_build_flags(with_optimization=True, arm64_only=True)
                env = get_env_for_config_make(arm64_only=True)
                subprocess.run(
                    [
                        "./bootstrap.sh",
                        "--with-libraries=headers,context,filesystem,program_options,thread,charconv",
                        "--without-icu",
                        "--prefix=" + arm64_install_dir,
                    ],
                    cwd=src_dir,
                    shell=False,
                    check=True,
                    env=env,
                )
                subprocess.run(
                    [
                        "./b2",
                        "--disable-icu",
                        "variant=release",
                        "link=static",
                        "threading=multi",
                        "runtime-link=shared",
                        "target-os=darwin",
                        "architecture=arm",
                        "abi=aapcs",
                        f"cxxflags={cbf['CXXFLAGS']} -arch arm64",
                        f"linkflags={cbf['LDFLAGS']} -arch arm64",
                        f"cflags={cbf['CFLAGS']} -arch arm64",
                        f"asmflags={cbf['ASMFLAGS']} -arch arm64",
                        "install",
                    ],
                    cwd=src_dir,
                    shell=False,
                    check=True,
                    env=env,
                )
                create_universal_binaries(arm64_install_dir, install_dir)
            finally:
                rm_tree(arm64_install_dir)
        else:
            cbf = get_common_build_flags(with_optimization=True)
            env = get_env_for_config_make()
            subprocess.run(
                [
                    "./bootstrap.sh",
                    "--with-toolset=clang" if use_clang_in_linux() else "",
                    "--with-libraries=headers,context,filesystem,program_options,thread,charconv",
                    "--without-icu",
                    "--prefix=" + install_dir,
                ],
                cwd=src_dir,
                shell=False,
                check=True,
                env=env,
            )

            subprocess.run(
                [
                    "./b2",
                    "--disable-icu",
                    "toolset=clang" if use_clang_in_linux() else "",
                    "address-model=64",
                    "variant=release",
                    "link=static",
                    "threading=multi",
                    "runtime-link=shared",
                    f"cxxflags={cbf['CXXFLAGS']}",
                    "install",
                ],
                cwd=src_dir,
                shell=False,
                check=True,
                env=env,
            )
    finally:
        logger.info("done")


def clean_boost(install_dir: str):
    shutil.rmtree(os.path.join(install_dir, "include", "boost"), ignore_errors=True)
    glob_remove(os.path.join(install_dir, "lib", "cmake", "boost*"))
    glob_remove(os.path.join(install_dir, "lib", "cmake", "Boost*"))
    glob_remove(os.path.join(install_dir, "lib", "libboost*"))


def build_tbb(src_dir: str, install_dir: str):
    build_dir = create_build_dir(src_dir)

    try:
        cmakecmd = get_cmake_cmd_common_part(install_dir, universal=True)
        cmakecmd.extend(["-DTBB_TEST:BOOL=OFF", "-DTBB_STRICT:BOOL=OFF"])

        cmakecmd.extend([src_dir])
        build_and_install_cmakecmd(cmakecmd, build_dir)
    finally:
        shutil.rmtree(build_dir, ignore_errors=False)


def build_eigen(src_dir: str, install_dir: str):
    build_dir = create_build_dir(src_dir)

    patches = [
        FilePatcher(
            orig_file=os.path.join(src_dir, "CMakeLists.txt"),
            from_texts=[
                r"add_subdirectory(blas",
                r"add_subdirectory(lapack",
            ],
            to_texts=[
                r"set(blas",
                r"set(lapack",
            ],
        ),
    ]
    patch_manager = PatchManager(patches)

    try:
        patch_manager.apply_patches()

        cmakecmd = get_cmake_cmd_common_part(install_dir, universal=True)

        cmakecmd.extend(
            [
                "-DBUILD_TESTING:BOOL=OFF",
                "-DEIGEN_BUILD_DOC:BOOL=OFF",
            ]
        )

        cmakecmd.extend([src_dir])
        build_and_install_cmakecmd(cmakecmd, build_dir)
    finally:
        shutil.rmtree(build_dir, ignore_errors=False)
        patch_manager.restore_files()


def build_pocketfft(src_dir: str, install_dir: str):
    shutil.copy2(
        os.path.join(src_dir, "pocketfft_hdronly.h"),
        os.path.join(install_dir, "include"),
    )


def build_reflect(src_dir: str, install_dir: str):
    shutil.copy2(os.path.join(src_dir, "reflect"), os.path.join(install_dir, "include"))


def build_simde(src_dir: str, install_dir: str):
    shutil.copytree(
        os.path.join(src_dir, "simde"),
        os.path.join(install_dir, "include", "simde"),
        dirs_exist_ok=True,
    )


def build_glm(src_dir: str, install_dir: str):
    build_dir = create_build_dir(src_dir)

    try:
        cmakecmd = get_cmake_cmd_common_part(install_dir, universal=True)

        cmakecmd.extend(
            [
                "-DGLM_BUILD_LIBRARY:BOOL=OFF",
                "-DGLM_BUILD_TESTS:BOOL=OFF",
            ]
        )

        cmakecmd.extend([src_dir])
        build_and_install_cmakecmd(cmakecmd, build_dir)

        orig_file = os.path.join(
            install_dir, "include", "glm", "detail", "type_vec_simd.inl"
        )
        patch_file(
            orig_file,
            from_texts=[
                r"""template<length_t L, qualifier Q, int E0, int E1, int E2, int E3>
	struct _swizzle_base1<L, float, Q, E0,E1,E2,E3, true> : public _swizzle_base0<float, L>
	{
		GLM_FUNC_QUALIFIER vec<L, float, Q> operator ()()  const
		{
			__m128 data = *reinterpret_cast<__m128 const*>(&this->_buffer);

			vec<L, float, Q> Result;
#			if GLM_ARCH & GLM_ARCH_AVX_BIT
				Result.data = _mm_permute_ps(data, _MM_SHUFFLE(E3, E2, E1, E0));
#			else
				Result.data = _mm_shuffle_ps(data, data, _MM_SHUFFLE(E3, E2, E1, E0));
#			endif
			return Result;
		}
	};""",
                r"""template<length_t L, qualifier Q, int E0, int E1, int E2, int E3>
	struct _swizzle_base1<L, int, Q, E0,E1,E2,E3, true> : public _swizzle_base0<int, L>
	{
		GLM_FUNC_QUALIFIER vec<L, int, Q> operator ()()  const
		{
			__m128i data = *reinterpret_cast<__m128i const*>(&this->_buffer);

			vec<L, int, Q> Result;
			Result.data = _mm_shuffle_epi32(data, _MM_SHUFFLE(E3, E2, E1, E0));
			return Result;
		}
	};""",
            ],
            to_texts=[
                r"""template<qualifier Q, int E0, int E1, int E2, int E3>
	struct _swizzle_base1<3, float, Q, E0,E1,E2,E3, true> : public _swizzle_base0<float, 3>
	{
		GLM_FUNC_QUALIFIER vec<3, float, Q> operator ()()  const
		{
			__m128 data = *reinterpret_cast<__m128 const*>(&this->_buffer);

			vec<3, float, Q> Result;
#			if GLM_ARCH & GLM_ARCH_AVX_BIT
				Result.data = _mm_permute_ps(data, _MM_SHUFFLE(E3, E2, E1, E0));
#			else
				Result.data = _mm_shuffle_ps(data, data, _MM_SHUFFLE(E3, E2, E1, E0));
#			endif
			return Result;
		}
	};
    template<qualifier Q, int E0, int E1, int E2, int E3>
	struct _swizzle_base1<4, float, Q, E0,E1,E2,E3, true> : public _swizzle_base0<float, 4>
	{
		GLM_FUNC_QUALIFIER vec<4, float, Q> operator ()()  const
		{
			__m128 data = *reinterpret_cast<__m128 const*>(&this->_buffer);

			vec<4, float, Q> Result;
#			if GLM_ARCH & GLM_ARCH_AVX_BIT
				Result.data = _mm_permute_ps(data, _MM_SHUFFLE(E3, E2, E1, E0));
#			else
				Result.data = _mm_shuffle_ps(data, data, _MM_SHUFFLE(E3, E2, E1, E0));
#			endif
			return Result;
		}
	};""",
                r"""template<qualifier Q, int E0, int E1, int E2, int E3>
	struct _swizzle_base1<3, int, Q, E0,E1,E2,E3, true> : public _swizzle_base0<int, 3>
	{
		GLM_FUNC_QUALIFIER vec<3, int, Q> operator ()()  const
		{
			__m128i data = *reinterpret_cast<__m128i const*>(&this->_buffer);

			vec<3, int, Q> Result;
			Result.data = _mm_shuffle_epi32(data, _MM_SHUFFLE(E3, E2, E1, E0));
			return Result;
		}
	};
    template<qualifier Q, int E0, int E1, int E2, int E3>
	struct _swizzle_base1<4, int, Q, E0,E1,E2,E3, true> : public _swizzle_base0<int, 4>
	{
		GLM_FUNC_QUALIFIER vec<4, int, Q> operator ()()  const
		{
			__m128i data = *reinterpret_cast<__m128i const*>(&this->_buffer);

			vec<4, int, Q> Result;
			Result.data = _mm_shuffle_epi32(data, _MM_SHUFFLE(E3, E2, E1, E0));
			return Result;
		}
	};""",
            ],
        )
    finally:
        shutil.rmtree(build_dir, ignore_errors=False)


def build_cpuinfo(src_dir: str, install_dir: str):
    build_dir = create_build_dir(src_dir)
    cmake_options = [
        "-DBUILD_GMOCK:BOOL=OFF",
        "-DCPUINFO_BUILD_MOCK_TESTS:BOOL=OFF",
        "-DCPUINFO_BUILD_BENCHMARKS:BOOL=OFF",
        "-DCPUINFO_BUILD_UNIT_TESTS:BOOL=OFF",
        "-DCPUINFO_BUILD_TOOLS:BOOL=ON",
    ]

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
            rm_tree(arm64_install_dir)


def build_gflags(src_dir: str, install_dir: str):
    build_dir = create_build_dir(src_dir)

    patches = [
        FilePatcher(
            orig_file=os.path.join(src_dir, "src", "gflags.cc"),
            from_texts=[r'ReportError(DIE, "ERROR: something wrong with'],
            to_texts=[r'ReportError(DO_NOT_DIE, "ERROR: something wrong with'],
            patch_condition=is_mac,
        ),
    ]
    patch_manager = PatchManager(patches)

    try:
        patch_manager.apply_patches()

        cmakecmd = get_cmake_cmd_common_part(install_dir, universal=True)
        cmakecmd.extend(
            [
                "-DGFLAGS_NAMESPACE=gflags",
            ]
        )

        cmakecmd.extend([src_dir])
        build_and_install_cmakecmd(cmakecmd, build_dir)
    finally:
        shutil.rmtree(build_dir, ignore_errors=False)
        patch_manager.restore_files()


def build_glog(src_dir: str, install_dir: str):
    build_dir = create_build_dir(src_dir)

    patches = [
        # Behavior policy: Atlas sets FLAGS_alsologtostderr while also writing log
        # files. Route mirrored console output through glog's stdout-aware helper
        # so normal mirrored log traffic does not all go to stderr; the helper
        # still honors stderrthreshold for severe messages.
        FilePatcher(
            orig_file=os.path.join(src_dir, "src", "logging.cc"),
            from_texts=[
                r"ColoredWriteToStderr(severity, message, message_len);",
            ],
            to_texts=[r"ColoredWriteToStdout(severity, message, message_len);"],
        ),
    ]
    patch_manager = PatchManager(patches)

    try:
        patch_manager.apply_patches()

        cmakecmd = get_cmake_cmd_common_part(install_dir, universal=True)
        cmakecmd.extend(
            [
                "-DBUILD_TESTING:BOOL=OFF",
                "-DBUILD_SHARED_LIBS:BOOL=OFF",
            ]
        )

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
        patch_manager.restore_files()


def build_benchmark(src_dir: str, install_dir: str):
    build_dir = create_build_dir(src_dir)

    try:
        cmakecmd = get_cmake_cmd_common_part(install_dir, universal=True)
        cmakecmd.extend(
            [
                "-DBENCHMARK_ENABLE_TESTING:BOOL=OFF",
                "-DBENCHMARK_ENABLE_GTEST_TESTS:BOOL=OFF",
            ]
        )

        if is_windows() and not use_clang_cl():
            cmakecmd.extend(["-DBENCHMARK_ENABLE_LTO:BOOL=ON"])
        elif is_mac():
            cmakecmd.extend(["-DBENCHMARK_USE_LIBCXX:BOOL=ON"])

        cmakecmd.extend([src_dir])
        build_and_install_cmakecmd(cmakecmd, build_dir)
    finally:
        shutil.rmtree(build_dir, ignore_errors=False)


def build_openssl(src_dir: str, install_dir: str, nasm_dir: str):
    def build_and_install_make(env: dict):
        subprocess.run(
            ["make", make_parallel_jobs_arg(), "build_sw"],
            cwd=src_dir,
            shell=False,
            check=True,
            env=env,
        )
        subprocess.run(
            ["make", "install_sw"], cwd=src_dir, shell=False, check=True, env=env
        )

    try:
        if is_linux():
            env = get_env_for_config_make()
            subprocess.run(
                [
                    "perl",
                    "./Configure",
                    "linux-x86_64",
                    "enable-ec_nistp_64_gcc_128",
                    "no-shared",
                    "no-tests",
                    "no-ui-console",
                    # 'no-legacy',
                    "--prefix=" + install_dir,
                    "--openssldir=" + os.path.join(install_dir, "ssl"),
                ],
                cwd=src_dir,
                shell=False,
                check=True,
                env=env,
            )
            build_and_install_make(env)
        elif is_mac():
            env = get_env_for_config_make()
            subprocess.run(
                [
                    "perl",
                    "./Configure",
                    "darwin64-x86_64",
                    "enable-ec_nistp_64_gcc_128",
                    "no-shared",
                    "no-tests",
                    "no-ui-console",
                    # 'no-legacy',
                    "--prefix=" + install_dir,
                    "--openssldir=" + os.path.join(install_dir, "ssl"),
                ],
                cwd=src_dir,
                shell=False,
                check=True,
                env=env,
            )
            build_and_install_make(env)
            subprocess.run(
                ["make", "clean", "distclean"],
                cwd=src_dir,
                shell=False,
                check=True,
                env=env,
            )

            env = get_env_for_config_make(arm64_only=True)
            arm64_install_dir = create_arm64_install_dir(src_dir)

            try:
                subprocess.run(
                    [
                        "perl",
                        "./Configure",
                        "darwin64-arm64",
                        "enable-ec_nistp_64_gcc_128",
                        "no-shared",
                        "no-tests",
                        "no-ui-console",
                        # 'no-legacy',
                        "--prefix=" + arm64_install_dir,
                        "--openssldir=" + os.path.join(arm64_install_dir, "ssl"),
                    ],
                    cwd=src_dir,
                    shell=False,
                    check=True,
                    env=env,
                )
                build_and_install_make(env)
                subprocess.run(
                    ["make", "clean", "distclean"],
                    cwd=src_dir,
                    shell=False,
                    check=True,
                    env=env,
                )

                create_universal_binaries(arm64_install_dir, install_dir)
            finally:
                rm_tree(arm64_install_dir)
        elif is_windows():
            env = get_env_for_config_make(remove_scoop_from_path=False)
            env["PATH"] = f"{env['PATH']};{nasm_dir}"
            # Keep OpenSSL on the upstream MSVC build path on Windows so we retain
            # the current assembly-enabled configuration even when Atlas adopts
            # clang-cl elsewhere.
            subprocess.run(
                [
                    "perl",
                    "./Configure",
                    "VC-WIN64A",
                    "no-shared",
                    "no-tests",
                    "no-ui-console",
                    # 'no-legacy',
                    "--prefix=" + install_dir,
                    "--openssldir=" + os.path.join(install_dir, "ssl"),
                ],
                cwd=src_dir,
                shell=True,
                check=True,
                env=env,
            )
            subprocess.run(
                ["nmake", "install_sw"], cwd=src_dir, shell=True, check=True, env=env
            )
    finally:
        logger.info("done")


def build_double_conversion(src_dir: str, install_dir: str):
    build_dir = create_build_dir(src_dir)

    try:
        cmakecmd = get_cmake_cmd_common_part(install_dir, universal=True)
        cmakecmd.extend(["-DBUILD_TESTING:BOOL=OFF", src_dir])
        build_and_install_cmakecmd(cmakecmd, build_dir)
    finally:
        shutil.rmtree(build_dir, ignore_errors=False)


def build_lz4(src_dir: str, install_dir: str):
    build_dir = create_build_dir(src_dir)

    try:
        cmakecmd = get_cmake_cmd_common_part(install_dir, universal=True)
        cmakecmd.extend(
            [
                "-DLZ4_BUNDLED_MODE:BOOL=OFF",
                "-DBUILD_SHARED_LIBS:BOOL=OFF",
                "-DBUILD_STATIC_LIBS:BOOL=ON",
                "-DLZ4_BUILD_LEGACY_LZ4C:BOOL=OFF",
                "-DLZ4_BUILD_CLI:BOOL=OFF",
                os.path.join(src_dir, "build", "cmake"),
            ]
        )
        build_and_install_cmakecmd(cmakecmd, build_dir)
    finally:
        shutil.rmtree(build_dir, ignore_errors=False)


def build_xz(src_dir: str, install_dir: str):
    build_dir = create_build_dir(src_dir)

    patches = [
        FilePatcher(
            orig_file=os.path.join(src_dir, "src", "liblzma", "api", "lzma.h"),
            from_texts=[
                r"#ifndef LZMA_API_IMPORT",
            ],
            to_texts=[
                "#ifndef LZMA_API_STATIC\n"
                "#define LZMA_API_STATIC\n"
                "#endif\n"
                "#ifndef LZMA_API_IMPORT\n",
            ],
        ),
    ]
    patch_manager = PatchManager(patches)

    try:
        patch_manager.apply_patches()

        cmakecmd = get_cmake_cmd_common_part(install_dir)
        cmakecmd.extend(
            [
                "-DBUILD_SHARED_LIBS:BOOL=OFF",
                "-DCREATE_XZ_SYMLINKS:BOOL=OFF",
                "-DCREATE_LZMA_SYMLINKS:BOOL=OFF",
                "-DENABLE_SMALL:BOOL=OFF",
                src_dir,
            ]
        )
        build_and_install_cmakecmd(cmakecmd, build_dir)

        if is_mac():
            build_dir = create_build_dir(src_dir)
            arm64_install_dir = create_arm64_install_dir(src_dir)

            try:
                cmakecmd = get_cmake_cmd_common_part(arm64_install_dir, arm64_only=True)
                cmakecmd.extend(
                    [
                        "-DBUILD_SHARED_LIBS:BOOL=OFF",
                        "-DCREATE_XZ_SYMLINKS:BOOL=OFF",
                        "-DCREATE_LZMA_SYMLINKS:BOOL=OFF",
                        "-DENABLE_SMALL:BOOL=OFF",
                        src_dir,
                    ]
                )
                build_and_install_cmakecmd(cmakecmd, build_dir)
                create_universal_binaries(arm64_install_dir, install_dir)
            finally:
                rm_tree(arm64_install_dir)
    finally:
        shutil.rmtree(build_dir, ignore_errors=False)
        patch_manager.restore_files()


def build_zstd(src_dir: str, install_dir: str):
    build_dir = create_build_dir(src_dir)

    patches = [
        FilePatcher(
            orig_file=os.path.join(src_dir, "lib", "zstd.h"),
            from_texts=[
                r"#ifdef ZSTD_DISABLE_DEPRECATE_WARNINGS",
            ],
            to_texts=[
                "#define ZSTD_DISABLE_DEPRECATE_WARNINGS\n"
                "#ifdef ZSTD_DISABLE_DEPRECATE_WARNINGS",
            ],
            patch_condition=is_mac,
        ),
    ]
    patch_manager = PatchManager(patches)

    try:
        patch_manager.apply_patches()

        cmakecmd = get_cmake_cmd_common_part(
            install_dir, universal=True, enable_cxx=False
        )
        cmakecmd.extend(
            [
                "-DBUILD_TESTING:BOOL=OFF",
                "-DZSTD_BUILD_TESTS:BOOL=OFF",
                "-DZSTD_USE_STATIC_RUNTIME:BOOL=OFF",
                "-DZSTD_BUILD_SHARED:BOOL=OFF",
                "-DZSTD_BUILD_STATIC:BOOL=ON",
                os.path.join(src_dir, "build", "cmake"),
            ]
        )
        build_and_install_cmakecmd(cmakecmd, build_dir)
    finally:
        shutil.rmtree(build_dir, ignore_errors=False)
        patch_manager.restore_files()


def build_brotli(src_dir: str, install_dir: str):
    build_dir = create_build_dir(src_dir)

    try:
        cmakecmd = get_cmake_cmd_common_part(install_dir, universal=True)
        cmakecmd.extend(
            [
                "-DBUILD_SHARED_LIBS:BOOL=OFF",
                "-DBROTLI_BUILD_TOOLS:BOOL=OFF",
                "-DBROTLI_DISABLE_TESTS:BOOL=ON",
                "-DBROTLI_BUILD_FOR_PACKAGE:BOOL=OFF",
                src_dir,
            ]
        )
        build_and_install_cmakecmd(cmakecmd, build_dir)
    finally:
        shutil.rmtree(build_dir, ignore_errors=False)


def build_giflib(src_dir: str, install_dir: str):
    version = Path(src_dir).name.removeprefix("giflib-")
    if version == Path(src_dir).name:
        version = "5.2.2"
    cmake_lists = os.path.join(src_dir, "CMakeLists.txt")
    cmake_text = """
cmake_minimum_required(VERSION 3.16)
project(GIF VERSION @ATLAS_GIF_VERSION@ LANGUAGES C)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(BUILD_SHARED_LIBS "Build shared libraries" OFF)

if(MSVC)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
endif()

set(GIF_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
if(WIN32)
    file(GLOB GIF_ORIGINALS
         "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
         "${CMAKE_CURRENT_SOURCE_DIR}/*.c")
    foreach(GIF_ORIGINAL ${GIF_ORIGINALS})
        file(RELATIVE_PATH GIF_RELATIVE_PATH
             "${CMAKE_CURRENT_SOURCE_DIR}" "${GIF_ORIGINAL}")
        set(GIF_PATCHED "${CMAKE_CURRENT_BINARY_DIR}/${GIF_RELATIVE_PATH}")
        get_filename_component(GIF_PATCHED_DIR "${GIF_PATCHED}" DIRECTORY)
        file(MAKE_DIRECTORY "${GIF_PATCHED_DIR}")
        file(READ "${GIF_ORIGINAL}" GIF_CONTENTS)
        string(REPLACE "#include <unistd.h>"
               "#ifdef _WIN32\\n#include <io.h>\\n#else\\n#include <unistd.h>\\n#endif"
               GIF_CONTENTS "${GIF_CONTENTS}")
        file(WRITE "${GIF_PATCHED}" "${GIF_CONTENTS}")
    endforeach()
    set(GIF_SOURCE_DIR "${CMAKE_CURRENT_BINARY_DIR}")
endif()

add_library(GIF
    "${GIF_SOURCE_DIR}/dgif_lib.c"
    "${GIF_SOURCE_DIR}/egif_lib.c"
    "${GIF_SOURCE_DIR}/gif_err.c"
    "${GIF_SOURCE_DIR}/gif_hash.c"
    "${GIF_SOURCE_DIR}/gifalloc.c"
    "${GIF_SOURCE_DIR}/openbsd-reallocarray.c"
    "${GIF_SOURCE_DIR}/quantize.c")
add_library(GIF::GIF ALIAS GIF)

set_target_properties(GIF PROPERTIES
    OUTPUT_NAME gif
    POSITION_INDEPENDENT_CODE ON)

target_include_directories(GIF PUBLIC
    $<BUILD_INTERFACE:${GIF_SOURCE_DIR}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

install(TARGETS GIF
    EXPORT GIFTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(FILES
    "${GIF_SOURCE_DIR}/gif_lib.h"
    "${GIF_SOURCE_DIR}/gif_hash.h"
    "${GIF_SOURCE_DIR}/gif_lib_private.h"
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT GIFTargets
    FILE GIFTargets.cmake
    NAMESPACE GIF::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/GIF)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/GIFConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)

set(GIF_CONFIG_IN "
@PACKAGE_INIT@
include(\\${CMAKE_CURRENT_LIST_DIR}/GIFTargets.cmake)
check_required_components(GIF)
")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/GIFConfig.cmake.in" "${GIF_CONFIG_IN}")
configure_package_config_file(
    "${CMAKE_CURRENT_BINARY_DIR}/GIFConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/GIFConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/GIF)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/GIFConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/GIFConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/GIF)
""".replace("@ATLAS_GIF_VERSION@", version)
    Path(cmake_lists).write_text(cmake_text, encoding="utf-8")

    def remove_giflib_outputs(active_install_dir: str):
        for output_path in [
            os.path.join(active_install_dir, "include", "gif_hash.h"),
            os.path.join(active_install_dir, "include", "gif_lib.h"),
            os.path.join(active_install_dir, "include", "gif_lib_private.h"),
            os.path.join(active_install_dir, "lib", "cmake", "GIF"),
        ]:
            if os.path.exists(output_path):
                if os.path.isdir(output_path):
                    rm_tree(output_path)
                else:
                    os.remove(output_path)
                    logger.info(f"{output_path} removed")
        for pattern in ["libGIF*", "libgif*", "GIF.lib", "gif.lib"]:
            glob_remove(os.path.join(active_install_dir, "lib", pattern))

    def build_arch(active_install_dir: str, *, arm64_only: bool):
        build_dir = create_build_dir(src_dir)
        try:
            remove_giflib_outputs(active_install_dir)
            cmakecmd = get_cmake_cmd_common_part(
                active_install_dir, arm64_only=arm64_only, enable_cxx=False
            )
            cmakecmd.extend(
                [
                    "-DBUILD_SHARED_LIBS:BOOL=OFF",
                    src_dir,
                ]
            )
            build_and_install_cmakecmd(cmakecmd, build_dir)
        finally:
            shutil.rmtree(build_dir, ignore_errors=False)

    build_macos_split_or_single(src_dir, install_dir, build_arch)


def build_highway(src_dir: str, install_dir: str):
    def build_arch(active_install_dir: str, *, arm64_only: bool):
        build_dir = create_build_dir(src_dir)
        try:
            cmakecmd = get_cmake_cmd_common_part(
                active_install_dir, arm64_only=arm64_only
            )
            cmakecmd.extend(
                [
                    "-DBUILD_SHARED_LIBS:BOOL=OFF",
                    "-DHWY_FORCE_STATIC_LIBS:BOOL=ON",
                    "-DHWY_ENABLE_CONTRIB:BOOL=OFF",
                    "-DHWY_ENABLE_EXAMPLES:BOOL=OFF",
                    "-DHWY_ENABLE_TESTS:BOOL=OFF",
                    "-DHWY_ENABLE_INSTALL:BOOL=ON",
                    src_dir,
                ]
            )
            build_and_install_cmakecmd(cmakecmd, build_dir)
        finally:
            shutil.rmtree(build_dir, ignore_errors=False)

    build_macos_split_or_single(src_dir, install_dir, build_arch)


def build_fmt(src_dir: str, install_dir: str):
    build_dir = create_build_dir(src_dir)

    try:
        cmakecmd = get_cmake_cmd_common_part(install_dir, universal=True)
        cmakecmd.extend(
            [
                "-DFMT_DOC:BOOL=OFF",
                "-DFMT_TEST:BOOL=OFF",
                "-DFMT_MODULE:BOOL=OFF",
                src_dir,
            ]
        )
        build_and_install_cmakecmd(cmakecmd, build_dir)
    finally:
        shutil.rmtree(build_dir, ignore_errors=False)


def build_libevent(src_dir: str, install_dir: str):
    build_dir = create_build_dir(src_dir)

    try:
        cmakecmd = get_cmake_cmd_common_part(install_dir, universal=True)
        cmakecmd.extend(
            [
                "-DEVENT__DISABLE_DEBUG_MODE:BOOL=ON",
                "-DEVENT__DISABLE_OPENSSL:BOOL=ON",
                "-DEVENT__DISABLE_BENCHMARK:BOOL=ON",
                "-DEVENT__DISABLE_TESTS:BOOL=ON",
                "-DEVENT__DISABLE_REGRESS:BOOL=ON",
                "-DEVENT__DISABLE_SAMPLES:BOOL=ON",
                "-DEVENT__DISABLE_MBEDTLS:BOOL=ON",
                "-DEVENT__MSVC_STATIC_RUNTIME:BOOL=OFF",
                "-DEVENT__DOXYGEN:BOOL=OFF",
                "-DEVENT__LIBRARY_TYPE=STATIC",
                src_dir,
            ]
        )
        build_and_install_cmakecmd(cmakecmd, build_dir)
    finally:
        shutil.rmtree(build_dir, ignore_errors=False)


def build_snappy(src_dir: str, install_dir: str):
    build_dir = create_build_dir(src_dir)

    patches = [
        FilePatcher(
            orig_file=os.path.join(src_dir, "CMakeLists.txt"),
            from_texts=[
                r'NOT CMAKE_CXX_FLAGS MATCHES "-Werror"',
                r'string(REGEX REPLACE "/GR" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")',
                r'set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /GR-")',
                r'string(REGEX REPLACE "-frtti" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")',
                r'set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-rtti")',
            ],
            to_texts=[
                r"OFF",
                r"",
                r"",
                r"",
                r"",
            ],
        ),
    ]
    patch_manager = PatchManager(patches)

    try:
        patch_manager.apply_patches()

        cmakecmd = get_cmake_cmd_common_part(install_dir)
        cmakecmd.extend(
            [
                "-DBUILD_SHARED_LIBS:BOOL=OFF",
                "-DSNAPPY_BUILD_TESTS:BOOL=OFF",
                "-DSNAPPY_BUILD_BENCHMARKS:BOOL=OFF",
                "-DSNAPPY_REQUIRE_AVX:BOOL=ON",
                "-DSNAPPY_REQUIRE_AVX2:BOOL=OFF",
                src_dir,
            ]
        )
        build_and_install_cmakecmd(cmakecmd, build_dir)

        if is_mac():
            build_dir = create_build_dir(src_dir)
            arm64_install_dir = create_arm64_install_dir(src_dir)

            try:
                cmakecmd = get_cmake_cmd_common_part(arm64_install_dir, arm64_only=True)
                cmakecmd.extend(
                    [
                        "-DBUILD_SHARED_LIBS:BOOL=OFF",
                        "-DSNAPPY_BUILD_TESTS:BOOL=OFF",
                        "-DSNAPPY_BUILD_BENCHMARKS:BOOL=OFF",
                        "-DSNAPPY_REQUIRE_AVX:BOOL=OFF",
                        "-DSNAPPY_REQUIRE_AVX2:BOOL=OFF",
                        src_dir,
                    ]
                )
                build_and_install_cmakecmd(cmakecmd, build_dir)
                create_universal_binaries(arm64_install_dir, install_dir)
            finally:
                rm_tree(arm64_install_dir)
    finally:
        shutil.rmtree(build_dir, ignore_errors=False)
        patch_manager.restore_files()


def build_bzip2(src_dir: str, install_dir: str):
    build_dir = create_build_dir(src_dir)

    try:
        cmakecmd = get_cmake_cmd_common_part(install_dir, universal=True)
        cmakecmd.extend(
            [
                "-DENABLE_DEBUG=OFF",
                "-DENABLE_APP=ON",
                "-DENABLE_DOCS=OFF",
                "-DENABLE_EXAMPLES=OFF",
                "-DENABLE_STATIC_LIB=ON",
                "-DENABLE_SHARED_LIB=OFF",
                src_dir,
            ]
        )
        build_and_install_cmakecmd(cmakecmd, build_dir)
        if not is_windows():
            shutil.copy2(
                os.path.join(install_dir, "lib", "libbz2_static.a"),
                os.path.join(install_dir, "lib", "libbz2.a"),
            )
        else:
            shutil.copy2(
                os.path.join(install_dir, "lib", "bz2_static.lib"),
                os.path.join(install_dir, "lib", "bz2.lib"),
            )
    finally:
        shutil.rmtree(build_dir, ignore_errors=False)


def build_libsodium(src_dir: str, install_dir: str):
    try:
        if is_windows():
            env = get_vcvars_environment()
            msvc_solution_dir = os.path.join(src_dir, "builds", "msvc", "vs2022")
            if not os.path.exists(os.path.join(msvc_solution_dir, "libsodium.sln")):
                raise FileNotFoundError(
                    f"Expected shipped libsodium VS2022 solution under {msvc_solution_dir}"
                )
            # Pre-generated VS solutions require a VS-installed platform toolset.
            msbuild_toolset = windows_native_platform_toolset()
            # Keep the MSBuild-based libsodium build on /MD to match Atlas'
            # CMake/Ninja Windows CRT policy.
            runtime_md_props = ext_dir() + "\\runtime_md.props"
            msbuild_cmd = [
                "MSBuild",
                "libsodium.sln",
                "/target:libsodium",
                "/property:Platform=x64",
                "/property:Configuration=StaticRelease",
                "/property:ForceImportBeforeCppTargets=" + runtime_md_props,
                "/property:PlatformToolset=" + msbuild_toolset,
                "/maxcpucount",
            ]
            if msbuild_toolset == windows_msbuild_platform_toolset():
                msbuild_cmd.extend(
                    [
                        "/property:LLVMInstallDir=" + llvm_install_dir(),
                        "/property:LLVMToolsVersion=" + llvm_tools_version(),
                    ]
                )
            subprocess.run(
                msbuild_cmd,
                cwd=msvc_solution_dir,
                shell=True,
                check=True,
                env=env,
            )
            glob_copy(
                os.path.join(src_dir, "src", "libsodium", "include", "*.h"),
                os.path.join(install_dir, "include"),
            )
            shutil.rmtree(
                os.path.join(install_dir, "include", "sodium"), ignore_errors=True
            )
            shutil.copytree(
                os.path.join(src_dir, "src", "libsodium", "include", "sodium"),
                os.path.join(install_dir, "include", "sodium"),
                dirs_exist_ok=True,
            )

            glob_copy(
                os.path.join(
                    src_dir,
                    "bin",
                    "x64",
                    "Release",
                    msbuild_toolset,
                    "static",
                    "*.lib",
                ),
                os.path.join(install_dir, "lib"),
            )

            orig_file = os.path.join(install_dir, "include", "sodium", "export.h")
            patch_file(
                orig_file,
                from_texts=[r"#ifdef SODIUM_STATIC"],
                to_texts=["#define SODIUM_STATIC\n#ifdef SODIUM_STATIC"],
            )
        else:
            env = get_env_for_config_make()
            if is_mac():
                env["CFLAGS"] += " -arch x86_64"
                env["CXXFLAGS"] += " -arch x86_64"
            subprocess.run(
                [
                    "./configure",
                    "--enable-shared=no",
                    "--enable-static=yes",
                    "--prefix=" + install_dir,
                ],
                cwd=src_dir,
                shell=False,
                check=True,
                env=env,
            )
            subprocess.run(
                ["make", "-j" + str(os.cpu_count()), "install"],
                cwd=src_dir,
                shell=False,
                check=True,
                env=env,
            )
            subprocess.run(
                ["make", "clean", "distclean"],
                cwd=src_dir,
                shell=False,
                check=True,
                env=env,
            )

            if is_mac():
                env = get_env_for_config_make(arm64_only=True)
                env["CFLAGS"] += " -arch arm64"
                env["CXXFLAGS"] += " -arch arm64"
                arm64_install_dir = create_arm64_install_dir(src_dir)

                try:
                    subprocess.run(
                        [
                            "./configure",
                            "--host=aarch64-apple-darwin20.6.0",
                            "--enable-shared=no",
                            "--enable-static=yes",
                            "--prefix=" + arm64_install_dir,
                        ],
                        cwd=src_dir,
                        shell=False,
                        check=True,
                        env=env,
                    )
                    subprocess.run(
                        ["make", "-j" + str(os.cpu_count()), "install"],
                        cwd=src_dir,
                        shell=False,
                        check=True,
                        env=env,
                    )
                    subprocess.run(
                        ["make", "clean", "distclean"],
                        cwd=src_dir,
                        shell=False,
                        check=True,
                        env=env,
                    )

                    create_universal_binaries(arm64_install_dir, install_dir)
                finally:
                    rm_tree(arm64_install_dir)
    finally:
        logger.info("done")


def build_folly(src_dir: str, install_dir: str, use_asan: bool = False):
    build_dir = create_build_dir(src_dir)
    fmt_format_include_files = [
        ["folly", "cli", "Args.cpp"],
        ["folly", "concurrency", "CacheLocality.cpp"],
        ["folly", "detail", "IPAddressSource.h"],
        ["folly", "fibers", "detail", "AtomicBatchDispatcher.cpp"],
        ["folly", "futures", "detail", "Core.cpp"],
        ["folly", "io", "async", "fdsock", "AsyncFdSocket.cpp"],
        ["folly", "IPAddress.cpp"],
        ["folly", "IPAddressV4.cpp"],
        ["folly", "IPAddressV6.cpp"],
        ["folly", "logging", "LogStreamProcessor.h"],
        ["folly", "observer", "WithJitter-inl.h"],
        ["folly", "python", "import.h"],
        ["folly", "result", "rich_error_base.h"],
        ["folly", "settings", "CommandLineParser.cpp"],
        ["folly", "SocketAddress.cpp"],
        ["folly", "system", "MemoryMapping.cpp"],
    ]

    patches = [
        FilePatcher(
            orig_file=os.path.join(src_dir, "CMake", "folly-config.cmake.in"),
            from_texts=[
                r"""# Find glog before loading targets, since targets reference the glog::glog target
if(NOT TARGET glog::glog)
  find_package(Glog QUIET)
endif()""",
                r"""# Set FOLLY_LIBRARIES from our Folly::folly target
set(FOLLY_LIBRARIES Folly::folly)

# Find folly's dependencies
find_dependency(fmt)""",
            ],
            to_texts=[
                r"""# Find dependencies before loading targets, since targets reference them.
find_dependency(fmt)
find_dependency(gflags CONFIG)

find_dependency(glog CONFIG)""",
                r"""# Set FOLLY_LIBRARIES from our Folly::folly target
set(FOLLY_LIBRARIES Folly::folly)""",
            ],
        ),
        FilePatcher(
            orig_file=os.path.join(src_dir, "CMake", "folly-config.cmake.in"),
            from_texts=[
                r"""find_package(Boost 1.69.0 REQUIRED
  COMPONENTS
    context
    filesystem
    program_options
    regex
    thread
)""",
            ],
            to_texts=[
                r"""find_package(Boost 1.69.0 REQUIRED
  COMPONENTS
    context
    filesystem
    program_options
    thread
)
if (NOT TARGET Boost::regex)
  if (TARGET Boost::headers)
    add_library(Boost::regex INTERFACE IMPORTED)
    target_link_libraries(Boost::regex INTERFACE Boost::headers)
  elseif (TARGET Boost::boost)
    add_library(Boost::regex INTERFACE IMPORTED)
    target_link_libraries(Boost::regex INTERFACE Boost::boost)
  else()
    message(FATAL_ERROR "Boost::regex target missing after Boost discovery")
  endif()
endif()""",
            ],
        ),
        FilePatcher(
            orig_file=os.path.join(src_dir, "CMake", "folly-deps.cmake"),
            from_texts=[
                r"${ZLIB_INCLUDE_DIRS}",
                r"${BZIP2_INCLUDE_DIRS}",
                r"find_package(OpenSSL 1.1.1 MODULE REQUIRED)",
                r"find_package(BZip2 MODULE)",
                r"find_package(LibLZMA MODULE)",
                r"find_package(LZ4 MODULE)",
                r"find_package(Zstd MODULE)",
                r"find_package(Snappy MODULE)",
                r"find_package(Libsodium)",
                r"find_package(Gflags MODULE)",
                r"find_package(Glog MODULE)",
                r"set(FOLLY_HAVE_LIBGLOG ${GLOG_FOUND})",
                r"find_package(LibDwarf)" if is_mac() else "",
                r"find_package(Libiberty)" if is_mac() else "",
                r"find_package(LibAIO)" if is_mac() else "",
                r"find_package(LibUring)" if is_mac() else "",
                r"find_package(LibUnwind)" if is_mac() else "",
                r"set(FOLLY_USE_SYMBOLIZER ON)",
            ],
            to_texts=[
                r"",
                r"",
                "find_package(OpenSSL 1.1.1 MODULE REQUIRED)\n"
                "if (WIN32)\n"
                "list(APPEND OPENSSL_LIBRARIES ${OPENSSL_LIBRARIES} Bcrypt.lib Crypt32.lib Ws2_32.lib)\n"
                "endif (WIN32)\n",
                r"find_package(BZip2 MODULE REQUIRED)",
                r"find_package(LibLZMA MODULE REQUIRED)",
                r"find_package(LZ4 MODULE REQUIRED)",
                f"find_package({'ZSTD' if is_mac() else 'Zstd'} MODULE REQUIRED)",
                f"find_package({'SNAPPY' if is_mac() else 'Snappy'} MODULE REQUIRED)",
                f"find_package({'LIBSODIUM' if is_mac() else 'Libsodium'} REQUIRED)",
                r"find_package(Gflags MODULE REQUIRED)",
                r"find_package(Glog CONFIG REQUIRED)",
                r"set(FOLLY_HAVE_LIBGLOG ON)",
                r"find_package(LIBDWARF)",
                r"find_package(LIBIBERTY)",
                r"find_package(LIBAIO)",
                r"find_package(LIBURING)",
                r"find_package(LIBUNWIND)",
                r"set(FOLLY_USE_SYMBOLIZER OFF)",
            ],
        ),
        FilePatcher(
            orig_file=os.path.join(src_dir, "CMake", "folly-deps.cmake"),
            from_texts=[
                r"""set(FOLLY_BOOST_COMPONENTS
    context
    filesystem
    program_options
    regex
)
if(WIN32)
  list(APPEND FOLLY_BOOST_COMPONENTS thread)
endif()

find_package(Boost 1.69.0 REQUIRED
  COMPONENTS
    ${FOLLY_BOOST_COMPONENTS}
)""",
            ],
            to_texts=[
                r"""set(FOLLY_BOOST_COMPONENTS
    context
    filesystem
    program_options
)
if(WIN32)
  list(APPEND FOLLY_BOOST_COMPONENTS thread)
endif()

find_package(Boost 1.69.0 REQUIRED
  COMPONENTS
    ${FOLLY_BOOST_COMPONENTS}
)
if (NOT TARGET Boost::regex)
  if (TARGET Boost::headers)
    add_library(Boost::regex INTERFACE IMPORTED)
    target_link_libraries(Boost::regex INTERFACE Boost::headers)
  elseif (TARGET Boost::boost)
    add_library(Boost::regex INTERFACE IMPORTED)
    target_link_libraries(Boost::regex INTERFACE Boost::boost)
  else()
    message(FATAL_ERROR "Boost::regex target missing after Boost discovery")
  endif()
endif()""",
            ],
        ),
        FilePatcher(
            orig_file=os.path.join(src_dir, "folly", "lang", "Exception.h"),
            from_texts=[
                r"""#include <exception>
#include <functional>""",
            ],
            to_texts=[
                r"""#include <exception>
#include <cstring>
#include <functional>""",
            ],
        ),
        FilePatcher(
            orig_file=os.path.join(
                src_dir, "folly", "crypto", "detail", "CMakeLists.txt"
            ),
            from_texts=[
                r"""if (IS_X86_64_ARCH)
  target_compile_options(folly_crypto_detail_math_operation_simple_obj
    PRIVATE -mno-avx -mno-avx2 -mno-sse2)
endif()""",
            ],
            to_texts=[
                r"""if (IS_X86_64_ARCH)
  if (APPLE)
    # Recent macOS SDK headers require SSE2 on x86_64. Keep the simple path
    # free of AVX/AVX2 without forcing an SDK-incompatible -mno-sse2 mode.
    target_compile_options(folly_crypto_detail_math_operation_simple_obj
      PRIVATE -mno-avx -mno-avx2)
  else()
    target_compile_options(folly_crypto_detail_math_operation_simple_obj
      PRIVATE -mno-avx -mno-avx2 -mno-sse2)
  endif()
endif()""",
            ],
            patch_condition=is_mac,
        ),
        FilePatcher(
            orig_file=os.path.join(src_dir, "CMakeLists.txt"),
            from_texts=[
                r"""if(NOT DEFINED IS_X86_64_ARCH AND ${CMAKE_SYSTEM_PROCESSOR} MATCHES "x86_64|AMD64")
  set(IS_X86_64_ARCH TRUE)
else()
  set(IS_X86_64_ARCH FALSE)
endif()

if(NOT DEFINED IS_AARCH64_ARCH AND ${CMAKE_SYSTEM_PROCESSOR} MATCHES "aarch64")
  set(IS_AARCH64_ARCH TRUE)
else()
  set(IS_AARCH64_ARCH FALSE)
endif()""",
            ],
            to_texts=[
                r"""# Atlas note:
# On macOS, CMAKE_SYSTEM_PROCESSOR follows the configure host in some setups
# (for example under Rosetta) and can report x86_64 even when the actual
# target arch list is arm64 or x86_64;arm64. Use CMAKE_OSX_ARCHITECTURES for
# Apple target decisions so universal and arm64 builds are deterministic across
# Intel, Apple Silicon, and Rosetta-hosted configures.
if(APPLE AND CMAKE_OSX_ARCHITECTURES MATCHES "(^|;)x86_64($|;)")
  set(IS_X86_64_ARCH TRUE)
elseif(APPLE)
  set(IS_X86_64_ARCH FALSE)
elseif(NOT DEFINED IS_X86_64_ARCH AND ${CMAKE_SYSTEM_PROCESSOR} MATCHES "x86_64|AMD64")
  set(IS_X86_64_ARCH TRUE)
else()
  set(IS_X86_64_ARCH FALSE)
endif()

# Keep Folly's AArch64 asm path disabled on Darwin even for arm64 targets.
# Those sources are Linux-only and fail on Apple's assembler/ABI.
if(APPLE)
  set(IS_AARCH64_ARCH FALSE)
elseif(NOT DEFINED IS_AARCH64_ARCH AND ${CMAKE_SYSTEM_PROCESSOR} MATCHES "aarch64")
  set(IS_AARCH64_ARCH TRUE)
else()
  set(IS_AARCH64_ARCH FALSE)
endif()""",
            ],
            patch_condition=is_mac,
        ),
        FilePatcher(
            orig_file=os.path.join(src_dir, "CMake", "FollyCompilerMSVC.cmake"),
            from_texts=[
                r"list(APPEND FOLLY_LINK_LIBRARIES Iphlpapi.lib Ws2_32.lib)",
                r"/favor:${MSVC_FAVORED_ARCHITECTURE} # Architecture to prefer when generating code.",
                r"$<$<BOOL:${MSVC_ENABLE_PARALLEL_BUILD}>:/MP> # Enable multi-processor compilation if requested.",
                r"/Qpar # Enable parallel code generation.",
                r"/Zc:referenceBinding # Disallow temporaries from binding to non-const lvalue references.",
                r"/Zc:implicitNoexcept # Enable implicit noexcept specifications where required, such as destructors.",
                r"/Zc:throwingNew # Assume operator new throws on failure.",
                r"/std:${MSVC_LANGUAGE_VERSION}",
                r"/EHs #",
            ],
            to_texts=[
                r"list(APPEND FOLLY_LINK_LIBRARIES Iphlpapi.lib Ws2_32.lib Bcrypt.lib Crypt32.lib)",
                r"$<$<NOT:$<CXX_COMPILER_ID:Clang>>:/favor:${MSVC_FAVORED_ARCHITECTURE}> # Architecture to prefer when generating code.",
                r"$<$<AND:$<BOOL:${MSVC_ENABLE_PARALLEL_BUILD}>,$<NOT:$<CXX_COMPILER_ID:Clang>>>:/MP> # Enable multi-processor compilation if requested.",
                r"$<$<NOT:$<CXX_COMPILER_ID:Clang>>:/Qpar> # Enable parallel code generation.",
                r"$<$<NOT:$<CXX_COMPILER_ID:Clang>>:/Zc:referenceBinding> # Disallow temporaries from binding to non-const lvalue references.",
                r"$<$<NOT:$<CXX_COMPILER_ID:Clang>>:/Zc:implicitNoexcept> # Enable implicit noexcept specifications where required, such as destructors.",
                r"$<$<NOT:$<CXX_COMPILER_ID:Clang>>:/Zc:throwingNew> # Assume operator new throws on failure.",
                r"/DGLOG_NO_ABBREVIATED_SEVERITIES #/std:${MSVC_LANGUAGE_VERSION}",
                r"/EHsc #",
            ],
        ),
        FilePatcher(
            orig_file=os.path.join(src_dir, "CMakeLists.txt"),
            from_texts=[
                r"""target_include_directories(folly_deps
  INTERFACE
    $<INSTALL_INTERFACE:include>
)""",
                r"""file(
  GENERATE
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/libfolly.pc
  INPUT ${CMAKE_CURRENT_BINARY_DIR}/libfolly.pc.gen
  ${target_arg}
)
install(
  FILES ${CMAKE_CURRENT_BINARY_DIR}/libfolly.pc
  DESTINATION ${LIB_INSTALL_DIR}/pkgconfig
  COMPONENT dev
)""",
            ],
            to_texts=[
                r"""target_include_directories(folly_deps
  INTERFACE
    $<INSTALL_INTERFACE:include>
)
if (WIN32 AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  target_compile_definitions(folly_deps INTERFACE LLVM_COROUTINES_CPP20)
endif()""",
                r"",
            ],
            patch_condition=use_windows_clang_cl,
        ),
        FilePatcher(
            orig_file=os.path.join(src_dir, "CMake", "FindLibsodium.cmake"),
            from_texts=[
                r"find_library(LIBSODIUM_LIBRARY NAMES sodium)",
            ],
            to_texts=[
                r"find_library(LIBSODIUM_LIBRARY NAMES sodium libsodium)",
            ],
            patch_condition=is_windows,
        ),
        FilePatcher(
            orig_file=os.path.join(
                src_dir, "folly", "logging", "BridgeFromGoogleLogging.h"
            ),
            from_texts=[
                """  void send(
      ::google::LogSeverity severity,
      const char* full_filename,
      const char* base_filename,
      int line,
      const struct ::tm* pTime,
      const char* message,
      size_t message_len,
      int32_t usecs);

  void send(
      ::google::LogSeverity severity,
      const char* full_filename,
      const char* base_filename,
      int line,
      const struct ::tm* pTime,
      const char* message,
      size_t message_len) override;""",
            ],
            to_texts=[
                """  void send(
      ::google::LogSeverity severity,
      const char* full_filename,
      const char* base_filename,
      int line,
      const ::google::LogMessageTime& time,
      const char* message,
      size_t message_len) override;""",
            ],
        ),
        FilePatcher(
            orig_file=os.path.join(
                src_dir, "folly", "logging", "BridgeFromGoogleLogging.cpp"
            ),
            from_texts=[
                """void BridgeFromGoogleLogging::send(
    ::google::LogSeverity severity,
    const char* full_filename,
    const char* base_filename,
    int line,
    const struct ::tm* pTime,
    const char* message,
    size_t message_len,
    int32_t usecs) {
  struct ::tm time = *pTime;
  folly::Logger const logger{full_filename};
  auto follyLevel = asFollyLogLevel(severity);
  if (logger.getCategory()->logCheck(follyLevel)) {
    folly::LogMessage logMessage{
        logger.getCategory(),
        follyLevel,
        std::chrono::system_clock::from_time_t(mktime(&time)) +
            std::chrono::microseconds(usecs),
        base_filename,
        static_cast<unsigned>(line),
        {},
        std::string{message, message_len}};
    // Make sure we don't abort on fatal messages and let glog library to
    // handle it. As this call is done under lock, this could lead to a deadlock
    logger.getCategory()->admitMessage(logMessage, /* skipAbortOnFatal */ true);
  }
}

void BridgeFromGoogleLogging::send(
    ::google::LogSeverity severity,
    const char* full_filename,
    const char* base_filename,
    int line,
    const struct ::tm* pTime,
    const char* message,
    size_t message_len) {
  struct ::tm time = *pTime;
  auto usecs = std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::system_clock::now() -
      std::chrono::system_clock::from_time_t(mktime(&time)));
  send(
      severity,
      full_filename,
      base_filename,
      line,
      pTime,
      message,
      message_len,
      folly::to_narrow(usecs.count()));
}""",
            ],
            to_texts=[
                """void BridgeFromGoogleLogging::send(
    ::google::LogSeverity severity,
    const char* full_filename,
    const char* base_filename,
    int line,
    const ::google::LogMessageTime& time,
    const char* message,
    size_t message_len) {
  folly::Logger const logger{full_filename};
  auto follyLevel = asFollyLogLevel(severity);
  if (logger.getCategory()->logCheck(follyLevel)) {
    folly::LogMessage logMessage{
        logger.getCategory(),
        follyLevel,
        time.when(),
        base_filename,
        static_cast<unsigned>(line),
        {},
        std::string{message, message_len}};
    // Make sure we don't abort on fatal messages and let glog library to
    // handle it. As this call is done under lock, this could lead to a deadlock
    logger.getCategory()->admitMessage(logMessage, /* skipAbortOnFatal */ true);
  }
}""",
            ],
        ),
        FilePatcher(
            orig_file=os.path.join(src_dir, "folly", "json", "DynamicParser.h"),
            from_texts=[
                """struct FOLLY_EXPORT DynamicParserLogicError : public std::logic_error {
  template <typename... Args>
  explicit DynamicParserLogicError(Args&&... args)
      : std::logic_error(folly::to<std::string>(std::forward<Args>(args)...)) {}
};""",
            ],
            to_texts=[
                """struct FOLLY_EXPORT DynamicParserLogicError : public std::logic_error {
  DynamicParserLogicError(const DynamicParserLogicError&) = default;
  DynamicParserLogicError(DynamicParserLogicError&&) = default;

  template <
      typename... Args,
      std::enable_if_t<
          !(sizeof...(Args) == 1 &&
            (... || std::is_same_v<
                        DynamicParserLogicError,
                        std::remove_cv_t<std::remove_reference_t<Args>>>)),
          int> = 0>
  explicit DynamicParserLogicError(Args&&... args)
      : std::logic_error(folly::to<std::string>(std::forward<Args>(args)...)) {}
};""",
            ],
            patch_condition=use_windows_clang_cl,
        ),
    ]
    patches.extend(
        [
            FilePatcher(
                orig_file=os.path.join(src_dir, *relative_path_parts),
                from_texts=[r"#include <fmt/core.h>"],
                to_texts=[r"#include <fmt/format.h>"],
            )
            for relative_path_parts in fmt_format_include_files
        ]
    )
    patch_manager = PatchManager(patches)

    try:
        patch_manager.apply_patches()

        cmakecmd_options = [
            "-DBUILD_SHARED_LIBS:BOOL=OFF",
            "-DPYTHON_EXTENSIONS:BOOL=OFF",
            "-DBUILD_TESTS:BOOL=OFF",
            "-DBOOST_LINK_STATIC=ON",
            "-DFOLLY_HAVE_WEAK_SYMBOLS:BOOL=OFF" if use_windows_clang_cl() else "",
            "-DFOLLY_LIBRARY_SANITIZE_ADDRESS:BOOL=" + ("ON" if use_asan else "OFF"),
            src_dir,
        ]

        cmakecmd = get_cmake_cmd_common_part(
            install_dir, cpp_extention=True, universal=True
        )
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
        #         rm_tree(arm64_install_dir)
    finally:
        shutil.rmtree(build_dir, ignore_errors=False)
        patch_manager.restore_files()
        cleanup_git_submodule(src_dir)


def build_suitesparse(src_dir: str, install_dir: str):
    build_dir = create_build_dir(src_dir)

    patches = [
        FilePatcher(
            orig_file=os.path.join(
                src_dir, "SuiteSparse_config", "cmake_modules", "SuiteSparseBLAS.cmake"
            ),
            from_texts=[
                """set ( BLA_VENDOR Intel10_64lp )
set ( BLA_SIZEOF_INTEGER 4 )
find_package ( BLAS )""",
            ],
            to_texts=[
                """set ( BLA_VENDOR Intel10_64lp )
set ( BLA_SIZEOF_INTEGER 4 )
# find_package ( BLAS )
include(libs)""",
            ],
        ),
        FilePatcher(
            orig_file=os.path.join(
                src_dir,
                "SuiteSparse_config",
                "cmake_modules",
                "SuiteSparse__blas_threading.cmake",
            ),
            from_texts=[
                """    get_filename_component ( ABS_SOURCE_PATH
        ${PROJECT_SOURCE_DIR}/../SuiteSparse_config/cmake_modules/check_mkl.c
        ABSOLUTE )
    try_run ( MKL_RUNS MKL_COMPILES
        ${CMAKE_CURRENT_BINARY_DIR}
        ${ABS_SOURCE_PATH}
        LINK_OPTIONS    ${BLAS_LINKER_FLAGS}
        LINK_LIBRARIES  ${BLAS_LIBRARIES}
        RUN_OUTPUT_VARIABLE MKL_OUTPUT )

    if ( ${MKL_COMPILES} )
        if ( ${MKL_RUNS} STREQUAL "FAILED_TO_RUN" )
            # MKL compiled but failed to run ... why?
            message ( FATAL_ERROR "Intel MKL failed to run" )
        endif ( )
        if ( ${MKL_OUTPUT} EQUAL 1 )
            message ( STATUS "BLAS: Intel MKL: single-threaded" )
        else ( )
            message ( STATUS "BLAS: Intel MKL: multi-threaded (threads: ${MKL_OUTPUT})" )
        endif ( )
    else ( )
        message ( FATAL_ERROR "BLAS: Intel MKL failed to compile" )
    endif ( )""",
            ],
            to_texts=[
                """    message ( STATUS "BLAS: Intel MKL threading probe skipped by Atlas" )""",
            ],
        ),
    ]
    patch_manager = PatchManager(patches)

    try:
        patch_manager.apply_patches()

        shutil.copy2(
            os.path.join(ext_dir(), "suitesparse-cmake", "libs.cmake"),
            os.path.join(src_dir, "SuiteSparse_config", "cmake_modules"),
        )

        # Resolve through Python so the Windows pip layout is prepared before
        # SuiteSparse tries to find MKL/TBB.
        intel_path = intel_sw_dir()

        cmakecmd = get_cmake_cmd_common_part(install_dir, no_hidden_visibility=True)
        cmakecmd_options = [
            "-DINTEL_PATH:PATH=" + intel_path,
            "-DSUITESPARSE_ENABLE_PROJECTS=suitesparse_config;amd;camd;ccolamd;colamd;cholmod;spqr",
            "-DGRAPHBLAS_BUILD_STATIC_LIBS:BOOL=ON",
            "-DSUITESPARSE_USE_OPENMP:BOOL=OFF",
            "-DSUITESPARSE_USE_CUDA:BOOL=OFF",
            "-DSUITESPARSE_USE_STRICT:BOOL=ON",
            "-DBUILD_SHARED_LIBS:BOOL=OFF",
            "-DBUILD_STATIC_LIBS:BOOL=ON",
            "-DBLA_STATIC:BOOL=ON",
            "-DSUITESPARSE_USE_64BIT_BLAS:BOOL=OFF",
            "-DSUITESPARSE_USE_FORTRAN:BOOL=OFF",
            "-DBUILD_TESTING:BOOL=OFF",
        ]

        cmakecmd.extend(cmakecmd_options)
        cmakecmd.extend([src_dir])

        build_and_install_cmakecmd(cmakecmd, build_dir)

        if is_mac():
            build_dir = create_build_dir(src_dir)
            arm64_install_dir = create_arm64_install_dir(src_dir)

            try:
                cmakecmd = get_cmake_cmd_common_part(
                    arm64_install_dir, arm64_only=True, no_hidden_visibility=True
                )

                cmakecmd.extend(cmakecmd_options)
                cmakecmd.extend([src_dir])

                build_and_install_cmakecmd(cmakecmd, build_dir)
                create_universal_binaries(arm64_install_dir, install_dir)
            finally:
                rm_tree(arm64_install_dir)
    finally:
        shutil.rmtree(build_dir, ignore_errors=False)
        patch_manager.restore_files()
        os.remove(
            os.path.join(src_dir, "SuiteSparse_config", "cmake_modules", "libs.cmake")
        )
        cleanup_git_submodule(src_dir)


def build_ceres_solver(src_dir: str, install_dir: str):
    build_dir = create_build_dir(src_dir)

    patches = [
        FilePatcher(
            orig_file=os.path.join(src_dir, "cmake", "FindSuiteSparse.cmake"),
            from_texts=[
                r"${LAPACK_LIBRARIES}",
                r"${BLAS_LIBRARIES}",
                r"find_package(BLAS QUIET)",
                r"find_package(LAPACK QUIET)",
                r"find_package (METIS)",
                r"check_symbol_exists (cholmod_metis cholmod.h SuiteSparse_CHOLMOD_USES_METIS)",
                r'set(CMAKE_FIND_LIBRARY_PREFIXES "lib" "" "${CMAKE_FIND_LIBRARY_PREFIXES}")',
            ],
            to_texts=[
                r" ",
                r" ",
                r'set(BLAS_FOUND ON CACHE BOOL "")',
                r'set(LAPACK_FOUND ON CACHE BOOL "")',
                r"add_library (METIS::METIS IMPORTED INTERFACE)",
                r"set(SuiteSparse_CHOLMOD_USES_METIS 1)",
                'set(CMAKE_FIND_LIBRARY_PREFIXES "lib" "" "${CMAKE_FIND_LIBRARY_PREFIXES}")\n'
                r'set(CMAKE_FIND_LIBRARY_SUFFIXES "_static.lib" "${CMAKE_FIND_LIBRARY_SUFFIXES}")',
            ],
        ),
        FilePatcher(
            # we build ceres as static lib, so no point to hard link lapack now as we might link to mkl later
            orig_file=os.path.join(src_dir, "internal", "ceres", "CMakeLists.txt"),
            from_texts=[
                r" ${LAPACK_LIBRARIES}",
                r'add_definitions(-DCERES_SUITESPARSE_VERSION="${SuiteSparse_VERSION}")',
            ],
            to_texts=[
                r" ",
                'add_definitions(-DCERES_SUITESPARSE_VERSION="${SuiteSparse_VERSION}")\n'
                'add_definitions(-DCERES_METIS_VERSION="${METIS_VERSION}")',
            ],
        ),
    ]
    patch_manager = PatchManager(patches)

    try:
        patch_manager.apply_patches()

        os.rename(
            os.path.join(src_dir, "third_party", "abseil-cpp", "CMakeLists.txt"),
            os.path.join(src_dir, "third_party", "abseil-cpp", "__CMakeLists.txt"),
        )

        cmakecmd = get_cmake_cmd_common_part(install_dir)
        cmakecmd_options = [
            "-DBUILD_TESTING:BOOL=OFF",
            "-DSUITESPARSE:BOOL=ON",
            "-DACCELERATESPARSE:BOOL=" + ("ON" if is_mac() else "OFF"),
            "-DUSE_CUDA:BOOL=OFF",
            "-DEIGENMETIS:BOOL=OFF",
            "-DBUILD_EXAMPLES:BOOL=OFF",
            "-DBUILD_BENCHMARKS:BOOL=OFF",
            "-DBUILD_SHARED_LIBS:BOOL=OFF",
        ]

        cmakecmd.extend(cmakecmd_options)
        cmakecmd.extend([src_dir])

        env = {}
        env["MKLROOT"] = mkl_dir()
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
                rm_tree(arm64_install_dir)

        # on linux, cmake complains about could not find the fake target SuiteSparse::Partition
        orig_file3 = os.path.join(
            install_dir, "lib", "cmake", "Ceres", "CeresTargets.cmake"
        )
        patch_file(
            orig_file3,
            from_texts=[
                r";\$<LINK_ONLY:SuiteSparse::Partition>;\$<LINK_ONLY:METIS::METIS>",
            ],
            to_texts=[
                r"",
            ],
        )
    finally:
        shutil.rmtree(build_dir, ignore_errors=False)
        os.rename(
            os.path.join(src_dir, "third_party", "abseil-cpp", "__CMakeLists.txt"),
            os.path.join(src_dir, "third_party", "abseil-cpp", "CMakeLists.txt"),
        )
        patch_manager.restore_files()
        cleanup_git_submodule(src_dir)


def build_grpc(src_dir: str, install_dir: str, nasm_dir: str):
    logger.info(f"nasm_dir: {nasm_dir}")
    build_dir = create_build_dir(src_dir)

    patches = [
        FilePatcher(
            orig_file=os.path.join(src_dir, "cmake", "gRPCConfig.cmake.in"),
            from_texts=[
                r"if(NOT CMAKE_CROSSCOMPILING)",
            ],
            to_texts=[
                r"if(1)",
            ],
            patch_condition=is_mac,
        ),
    ]
    patch_manager = PatchManager(patches)

    try:
        patch_manager.apply_patches()

        cmakecmd = get_cmake_cmd_common_part(install_dir, universal=True)
        cmakecmd.extend(
            [
                "-DgRPC_INSTALL:BOOL=ON",
                "-DgRPC_BUILD_TESTS:BOOL=OFF",
                "-DgRPC_MSVC_STATIC_RUNTIME:BOOL=OFF" if is_windows() else "",
                "-DgRPC_ZLIB_PROVIDER:STRING=package",
                "-DgRPC_PROTOBUF_PROVIDER=module",
                "-DgRPC_CARES_PROVIDER=module",
                "-DgRPC_SSL_PROVIDER=package",
                f"-DOPENSSL_ROOT_DIR:PATH={install_dir}",
                "-DgRPC_BENCHMARK_PROVIDER:STRING=package",
                "-DgRPC_ABSL_PROVIDER:STRING=module",
                "-DgRPC_RE2_PROVIDER:STRING=module",
            ]
        )

        cmakecmd.extend([src_dir])
        build_and_install_cmakecmd(cmakecmd, build_dir)
    finally:
        shutil.rmtree(build_dir, ignore_errors=False)
        patch_manager.restore_files()


def build_glbinding(src_dir: str, install_dir: str):
    build_dir = create_build_dir(src_dir)

    try:
        cmakecmd = get_cmake_cmd_common_part(install_dir, universal=True)
        cmakecmd.extend(
            [
                "-DOPTION_BUILD_TOOLS:BOOL=OFF",
                "-DBUILD_SHARED_LIBS:BOOL=OFF",
                "-DOPTION_BUILD_TESTS:BOOL=OFF",
                "-DOPTION_BUILD_DOCS:BOOL=OFF",
                "-DOPTION_BUILD_EXAMPLES:BOOL=OFF",
                "-DOPTION_BUILD_OWN_KHR_HEADERS:BOOL=ON",
                src_dir,
            ]
        )
        build_and_install_cmakecmd(cmakecmd, build_dir)
    finally:
        shutil.rmtree(build_dir, ignore_errors=False)


def build_libjpeg(src_dir: str, install_dir: str, nasm_dir: str):
    build_dir = create_build_dir(src_dir)

    try:
        cmakecmd = get_cmake_cmd_common_part(install_dir)
        if is_windows():
            cmakecmd.extend(
                [
                    "-DENABLE_SHARED:BOOL=OFF",
                    "-DCMAKE_ASM_NASM_COMPILER:FILEPATH=" + nasm_dir + "\\nasm.exe",
                    "-DWITH_CRT_DLL:BOOL=ON",
                    src_dir,
                ]
            )
        elif is_linux():
            cmakecmd.extend(
                [
                    "-DENABLE_SHARED:BOOL=OFF",
                    "-DCMAKE_ASM_NASM_COMPILER:FILEPATH=nasm",
                    src_dir,
                ]
            )
        else:
            cmakecmd.extend(
                [
                    "-DENABLE_SHARED:BOOL=OFF",
                    "-DCMAKE_ASM_NASM_COMPILER:FILEPATH=" + nasm_dir + "/nasm",
                    src_dir,
                ]
            )
        build_and_install_cmakecmd(cmakecmd, build_dir)
    finally:
        shutil.rmtree(build_dir, ignore_errors=False)

    if is_mac():
        build_dir = create_build_dir(src_dir)
        arm64_install_dir = create_arm64_install_dir(src_dir)

        try:
            cmakecmd = get_cmake_cmd_common_part(arm64_install_dir, arm64_only=True)
            cmakecmd.extend(
                [
                    "-DENABLE_SHARED:BOOL=OFF",
                    "-DCMAKE_ASM_NASM_COMPILER:FILEPATH=" + nasm_dir + "/nasm",
                    src_dir,
                ]
            )
            build_and_install_cmakecmd(cmakecmd, build_dir)
            create_universal_binaries(
                arm64_install_dir=arm64_install_dir, final_install_dir=install_dir
            )
        finally:
            shutil.rmtree(build_dir, ignore_errors=False)
            rm_tree(arm64_install_dir)

        build_macos_libturbojpeg_dylib(src_dir, install_dir, nasm_dir)


def build_macos_libturbojpeg_dylib(src_dir: str, install_dir: str, nasm_dir: str):
    assert is_mac()

    def create_libturbojpeg_install_dir(arch: str) -> str:
        path = os.path.normpath(
            os.path.join(
                ext_build_dir(), f"__{arch}_libturbojpeg_" + Path(src_dir).name
            )
        )
        if os.path.exists(path):
            shutil.rmtree(path, ignore_errors=False, onexc=handleRemoveReadonly)
        os.mkdir(path)
        return path

    x86_install_dir = create_libturbojpeg_install_dir("x86_64")
    arm64_install_dir = create_libturbojpeg_install_dir("arm64")

    def build_shared(target_install_dir: str, *, arm64_only: bool):
        build_dir = create_build_dir(src_dir)
        try:
            cmakecmd = get_cmake_cmd_common_part(
                target_install_dir, arm64_only=arm64_only
            )
            cmakecmd.extend(
                [
                    "-DENABLE_SHARED:BOOL=ON",
                    "-DENABLE_STATIC:BOOL=OFF",
                    "-DWITH_TESTS:BOOL=OFF",
                    "-DWITH_TOOLS:BOOL=OFF",
                ]
            )
            if not arm64_only:
                cmakecmd.append(
                    "-DCMAKE_ASM_NASM_COMPILER:FILEPATH=" + nasm_dir + "/nasm"
                )
            cmakecmd.append(src_dir)
            build_and_install_cmakecmd(cmakecmd, build_dir)
        finally:
            shutil.rmtree(build_dir, ignore_errors=False)

    try:
        build_shared(x86_install_dir, arm64_only=False)
        build_shared(arm64_install_dir, arm64_only=True)

        x86_dylib = os.path.join(x86_install_dir, "lib", "libturbojpeg.dylib")
        arm64_dylib = os.path.join(arm64_install_dir, "lib", "libturbojpeg.dylib")
        output_dir = os.path.join(install_dir, "jars")
        os.makedirs(output_dir, exist_ok=True)
        output_dylib = os.path.join(output_dir, "libturbojpeg.dylib")
        subprocess.run(
            ["lipo", "-create", x86_dylib, arm64_dylib, "-output", output_dylib],
            shell=False,
            check=True,
        )
    finally:
        rm_tree(x86_install_dir)
        rm_tree(arm64_install_dir)


def build_libpng(src_dir: str, install_dir: str):
    build_dir = create_build_dir(src_dir)

    patches = [
        # Behavior policy: Atlas has historically accepted malformed PNGs that
        # contain a non-IDAT chunk before a later IDAT. libpng intentionally
        # treats that ordering as invalid; keep this targeted check disabled so
        # the updated libpng does not silently narrow Atlas' accepted inputs.
        FilePatcher(
            orig_file=os.path.join(src_dir, "pngread.c"),
            from_texts=[
                r"                || (png_ptr->mode & PNG_HAVE_CHUNK_AFTER_IDAT) != 0)"
            ],
            to_texts=[r"                && 0)"],
        ),
        FilePatcher(
            orig_file=os.path.join(src_dir, "pngpriv.h"),
            from_texts=[r"#if PNG_ZLIB_VERNUM != 0 && PNG_ZLIB_VERNUM != ZLIB_VERNUM"],
            to_texts=[r"#if 0"],
            patch_condition=lambda: is_mac() and os.path.exists("/usr/include"),
        ),
    ]
    patch_manager = PatchManager(patches)

    try:
        patch_manager.apply_patches()

        cmakecmd = get_cmake_cmd_common_part(install_dir)
        cmakecmd.extend(
            [
                "-DPNG_TESTS:BOOL=OFF",
                "-DPNG_SHARED:BOOL=OFF",
                "-DPNG_FRAMEWORK:BOOL=OFF",
            ]
        )
        cmakecmd.extend([src_dir])
        build_and_install_cmakecmd(cmakecmd, build_dir)

        if is_mac():
            build_dir = create_build_dir(src_dir)
            arm64_install_dir = create_arm64_install_dir(src_dir)

            try:
                cmakecmd = get_cmake_cmd_common_part(arm64_install_dir, arm64_only=True)

                cmakecmd.extend(
                    [
                        "-DPNG_TESTS:BOOL=OFF",
                        "-DPNG_SHARED:BOOL=OFF",
                        "-DPNG_FRAMEWORK:BOOL=OFF",
                        "-DCMAKE_OSX_INTERNAL_ARCHITECTURES=arm64",
                    ]
                )
                cmakecmd.extend([src_dir])

                build_and_install_cmakecmd(cmakecmd, build_dir)
                create_universal_binaries(arm64_install_dir, install_dir)
            finally:
                rm_tree(arm64_install_dir)
    finally:
        shutil.rmtree(build_dir, ignore_errors=False)
        patch_manager.restore_files()


def build_openjpeg(src_dir: str, install_dir: str):
    build_dir = create_build_dir(src_dir)

    patches = [
        FilePatcher(
            orig_file=os.path.join(src_dir, "src", "lib", "openjp2", "openjpeg.h"),
            from_texts=[r"#define OPENJPEG_H"],
            to_texts=[
                "#define OPENJPEG_H\n#ifndef OPJ_STATIC\n#define OPJ_STATIC\n#endif\n"
            ],
        ),
    ]
    patch_manager = PatchManager(patches)

    try:
        patch_manager.apply_patches()

        cmakecmd = get_cmake_cmd_common_part(install_dir, universal=True)

        cmakecmd.extend(
            [
                "-DBUILD_STATIC_LIBS:BOOL=ON",
                "-DBUILD_DOC:BOOL=OFF",
                "-DBUILD_SHARED_LIBS:BOOL=OFF",
                "-DBUILD_CODEC:BOOL=OFF",
            ]
        )

        cmakecmd.extend([src_dir])
        build_and_install_cmakecmd(cmakecmd, build_dir)
    finally:
        shutil.rmtree(build_dir, ignore_errors=False)
        patch_manager.restore_files()


def build_libwebp(src_dir: str, install_dir: str):
    cmakecmd_options = [
        "-DWEBP_BUILD_ANIM_UTILS:BOOL=OFF",
        "-DWEBP_BUILD_CWEBP:BOOL=OFF",
        "-DWEBP_BUILD_DWEBP:BOOL=OFF",
        "-DWEBP_BUILD_GIF2WEBP:BOOL=OFF",
        "-DWEBP_BUILD_IMG2WEBP:BOOL=OFF",
        "-DWEBP_BUILD_VWEBP:BOOL=OFF",
        "-DWEBP_BUILD_WEBPINFO:BOOL=OFF",
        "-DWEBP_BUILD_WEBPMUX:BOOL=OFF",
        "-DWEBP_BUILD_EXTRAS:BOOL=OFF",
        "-DWEBP_BUILD_WEBP_JS:BOOL=OFF",
    ]

    build_dir = create_build_dir(src_dir)
    try:
        cmakecmd = get_cmake_cmd_common_part(install_dir)
        cmakecmd.extend(cmakecmd_options)
        cmakecmd.extend([src_dir])
        build_and_install_cmakecmd(cmakecmd, build_dir)
    finally:
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
            shutil.rmtree(build_dir, ignore_errors=False)
            rm_tree(arm64_install_dir)

    orig_file = os.path.join(install_dir, "share", "WebP", "cmake", "WebPConfig.cmake")
    patch_file(
        orig_file,
        from_texts=[
            r"check_required_components(WebP)",
        ],
        to_texts=[
            r"#check_required_components(WebP)",
        ],
    )


def populate_libjxl_skcms(src_dir: str, skcms_package: str):
    skcms_dir = os.path.join(src_dir, "third_party", "skcms")
    rm_tree(skcms_dir)
    os.makedirs(skcms_dir)
    unpack_file_to_folder(skcms_package, skcms_dir)


def build_libjxl(src_dir: str, install_dir: str, skcms_package: str):
    populate_libjxl_skcms(src_dir, skcms_package)

    def build_arch(active_install_dir: str, *, arm64_only: bool):
        build_dir = create_build_dir(src_dir)
        try:
            cmakecmd = get_cmake_cmd_common_part(
                active_install_dir, arm64_only=arm64_only
            )
            cmakecmd.append(
                "-DCMAKE_PREFIX_PATH="
                + cmake_prefix_path_for_arch(active_install_dir, install_dir)
            )
            cmakecmd.extend(
                [
                    "-DBUILD_SHARED_LIBS:BOOL=OFF",
                    "-DBUILD_TESTING:BOOL=OFF",
                    "-DHWY_DIR:PATH="
                    + os.path.join(install_dir, "lib", "cmake", "hwy"),
                    "-DJPEGXL_ENABLE_FUZZERS:BOOL=OFF",
                    "-DJPEGXL_ENABLE_DEVTOOLS:BOOL=OFF",
                    "-DJPEGXL_ENABLE_TOOLS:BOOL=OFF",
                    "-DJPEGXL_ENABLE_JPEGLI:BOOL=OFF",
                    "-DJPEGXL_ENABLE_JPEGLI_LIBJPEG:BOOL=OFF",
                    "-DJPEGXL_INSTALL_JPEGLI_LIBJPEG:BOOL=OFF",
                    "-DJPEGXL_ENABLE_DOXYGEN:BOOL=OFF",
                    "-DJPEGXL_ENABLE_MANPAGES:BOOL=OFF",
                    "-DJPEGXL_ENABLE_BENCHMARK:BOOL=OFF",
                    "-DJPEGXL_ENABLE_EXAMPLES:BOOL=OFF",
                    "-DJPEGXL_BUNDLE_LIBPNG:BOOL=OFF",
                    "-DJPEGXL_ENABLE_JNI:BOOL=OFF",
                    "-DJPEGXL_ENABLE_SJPEG:BOOL=OFF",
                    "-DJPEGXL_ENABLE_OPENEXR:BOOL=OFF",
                    "-DJPEGXL_ENABLE_SKCMS:BOOL=ON",
                    "-DJPEGXL_ENABLE_VIEWERS:BOOL=OFF",
                    "-DJPEGXL_ENABLE_PLUGINS:BOOL=OFF",
                    "-DJPEGXL_ENABLE_TCMALLOC:BOOL=OFF",
                    "-DJPEGXL_ENABLE_TRANSCODE_JPEG:BOOL=OFF",
                    "-DJPEGXL_ENABLE_BOXES:BOOL=ON",
                    "-DJPEGXL_FORCE_SYSTEM_BROTLI:BOOL=ON",
                    "-DJPEGXL_FORCE_SYSTEM_GTEST:BOOL=ON",
                    "-DJPEGXL_FORCE_SYSTEM_HWY:BOOL=ON",
                    src_dir,
                ]
            )
            build_and_install_cmakecmd(cmakecmd, build_dir)
        finally:
            shutil.rmtree(build_dir, ignore_errors=False)

    build_macos_split_or_single(src_dir, install_dir, build_arch)


def build_libraw(libraw_src_dir: str, libraw_cmake_src_dir: str, install_dir: str):
    def build_arch(active_install_dir: str, *, arm64_only: bool):
        build_dir = create_build_dir(libraw_cmake_src_dir)
        try:
            cmakecmd = get_cmake_cmd_common_part(
                active_install_dir, arm64_only=arm64_only
            )
            cmakecmd.append(
                "-DCMAKE_PREFIX_PATH="
                + cmake_prefix_path_for_arch(active_install_dir, install_dir)
            )
            cmakecmd.extend(
                [
                    "-DBUILD_SHARED_LIBS:BOOL=OFF",
                    "-DLIBRAW_PATH:PATH=" + libraw_src_dir,
                    "-DLIBRAW_INSTALL:BOOL=ON",
                    "-DLIBRAW_UNINSTALL_TARGET:BOOL=OFF",
                    "-DENABLE_OPENMP:BOOL=OFF",
                    "-DENABLE_LCMS:BOOL=OFF",
                    "-DENABLE_JASPER:BOOL=OFF",
                    "-DENABLE_EXAMPLES:BOOL=OFF",
                    "-DENABLE_RAWSPEED:BOOL=OFF",
                    "-DENABLE_DCRAW_DEBUG:BOOL=OFF",
                    "-DENABLE_X3FTOOLS:BOOL=OFF",
                    "-DENABLE_6BY9RPI:BOOL=OFF",
                    libraw_cmake_src_dir,
                ]
            )
            build_and_install_cmakecmd(cmakecmd, build_dir)
        finally:
            shutil.rmtree(build_dir, ignore_errors=False)

    build_macos_split_or_single(libraw_cmake_src_dir, install_dir, build_arch)


def build_libtiff(src_dir: str, install_dir: str):
    build_dir = create_build_dir(src_dir)

    try:
        cmakecmd = get_cmake_cmd_common_part(install_dir, universal=True)
        cmakecmd.extend(
            [
                "-DBUILD_SHARED_LIBS:BOOL=OFF",
                "-Dtiff-static:BOOL=ON",
                "-Dtiff-tools:BOOL=OFF",
                "-Dtiff-tests:BOOL=OFF",
                "-Dtiff-contrib:BOOL=OFF",
                "-Dtiff-docs:BOOL=OFF",
                "-Dtiff-deprecated:BOOL=OFF",
                "-Dtiff-install:BOOL=ON",
                "-Dtiff-opengl:BOOL=OFF",
                "-Dstrip-chopping:BOOL=OFF",
                # Atlas does not currently build these optional codec
                # dependencies. Keep the exported TIFF target constrained to
                # dependencies already managed by util/build_ext_libs.py.
                "-Dlibdeflate:BOOL=OFF",
                "-Djbig:BOOL=OFF",
                "-Dlerc:BOOL=OFF",
                src_dir,
            ]
        )
        build_and_install_cmakecmd(cmakecmd, build_dir)
    finally:
        shutil.rmtree(build_dir, ignore_errors=False)

    # Upstream installs TiffConfig.cmake. Some consumers, including OIIO's
    # checked_find_package(TIFF), look for the all-caps package spelling first.
    cmake_dir = os.path.join(install_dir, "lib", "cmake", "tiff")
    patch_file(
        os.path.join(cmake_dir, "TiffConfig.cmake"),
        from_texts=[
            """if(NOT "OFF")
    # TODO: import dependencies
endif()
"""
        ],
        to_texts=[
            """include(CMakeFindDependencyMacro)

if(NOT TARGET ZLIB::ZLIB)
    find_dependency(ZLIB)
    if(TARGET ZLIB::ZLIBSTATIC AND NOT TARGET ZLIB::ZLIB)
        add_library(ZLIB::ZLIB INTERFACE IMPORTED)
        set_target_properties(ZLIB::ZLIB PROPERTIES
                              INTERFACE_LINK_LIBRARIES ZLIB::ZLIBSTATIC)
    endif()
endif()

if(NOT TARGET JPEG::JPEG)
    find_dependency(libjpeg-turbo CONFIG)
    if(TARGET libjpeg-turbo::jpeg)
        add_library(JPEG::JPEG INTERFACE IMPORTED)
        set_target_properties(JPEG::JPEG PROPERTIES
                              INTERFACE_LINK_LIBRARIES libjpeg-turbo::jpeg)
    elseif(TARGET libjpeg-turbo::jpeg-static)
        add_library(JPEG::JPEG INTERFACE IMPORTED)
        set_target_properties(JPEG::JPEG PROPERTIES
                              INTERFACE_LINK_LIBRARIES libjpeg-turbo::jpeg-static)
    else()
        find_dependency(JPEG)
    endif()
endif()

if(NOT TARGET liblzma::liblzma)
    find_dependency(liblzma CONFIG)
endif()

if(NOT TARGET ZSTD::ZSTD)
    find_dependency(zstd CONFIG)
    if(TARGET zstd::libzstd_static)
        add_library(ZSTD::ZSTD INTERFACE IMPORTED)
        set_target_properties(ZSTD::ZSTD PROPERTIES
                              INTERFACE_LINK_LIBRARIES zstd::libzstd_static)
    elseif(TARGET zstd::libzstd)
        add_library(ZSTD::ZSTD INTERFACE IMPORTED)
        set_target_properties(ZSTD::ZSTD PROPERTIES
                              INTERFACE_LINK_LIBRARIES zstd::libzstd)
    endif()
endif()

if(NOT TARGET WebP::webp)
    find_dependency(WebP CONFIG)
endif()

if(NOT TARGET CMath::CMath)
    add_library(CMath::CMath INTERFACE IMPORTED)
    find_library(CMATH_LIBRARY m)
    if(CMATH_LIBRARY)
        set_property(TARGET CMath::CMath PROPERTY
                     INTERFACE_LINK_LIBRARIES "${CMATH_LIBRARY}")
    endif()
endif()
"""
        ],
        keep_bak_file=False,
    )
    for stem in ("Config", "ConfigVersion"):
        mixed_case = os.path.join(cmake_dir, f"Tiff{stem}.cmake")
        upper_case = os.path.join(cmake_dir, f"TIFF{stem}.cmake")
        if os.path.exists(mixed_case):
            if os.path.exists(upper_case) and os.path.samefile(mixed_case, upper_case):
                continue
            shutil.copy2(mixed_case, upper_case)


def build_openimageio(src_dir: str, install_dir: str):
    fmt_config_dir = os.path.join(install_dir, "lib", "cmake", "fmt")

    def remove_openimageio_outputs(active_install_dir: str):
        for output_path in [
            os.path.join(active_install_dir, "openimageio-local"),
            os.path.join(active_install_dir, "include", "OpenImageIO"),
            os.path.join(active_install_dir, "include", "Imath"),
            os.path.join(active_install_dir, "include", "OpenEXR"),
            os.path.join(active_install_dir, "include", "OpenColorIO"),
            os.path.join(active_install_dir, "include", "minizip-ng"),
            os.path.join(active_install_dir, "include", "pystring"),
            os.path.join(active_install_dir, "include", "tsl"),
            os.path.join(active_install_dir, "include", "yaml-cpp"),
            os.path.join(active_install_dir, "lib", "cmake", "Imath"),
            os.path.join(active_install_dir, "lib", "cmake", "minizip-ng"),
            os.path.join(active_install_dir, "lib", "cmake", "OpenColorIO"),
            os.path.join(active_install_dir, "lib", "cmake", "OpenEXR"),
            os.path.join(active_install_dir, "lib", "cmake", "OpenImageIO"),
            os.path.join(active_install_dir, "lib", "cmake", "yaml-cpp"),
            os.path.join(active_install_dir, "lib", "pkgconfig", "Imath.pc"),
            os.path.join(active_install_dir, "lib", "pkgconfig", "minizip-ng.pc"),
            os.path.join(active_install_dir, "lib", "pkgconfig", "OpenColorIO.pc"),
            os.path.join(active_install_dir, "lib", "pkgconfig", "OpenImageIO.pc"),
            os.path.join(active_install_dir, "lib", "pkgconfig", "OpenEXR.pc"),
            os.path.join(active_install_dir, "lib", "pkgconfig", "OpenEXRCore.pc"),
            os.path.join(active_install_dir, "lib", "pkgconfig", "yaml-cpp.pc"),
            os.path.join(active_install_dir, "share", "cmake", "tsl-robin-map"),
            os.path.join(active_install_dir, "share", "ocio"),
            os.path.join(active_install_dir, "share", "OpenColorIO"),
        ]:
            if os.path.exists(output_path):
                if os.path.isdir(output_path):
                    rm_tree(output_path)
                else:
                    os.remove(output_path)
                    logger.info(f"{output_path} removed")
        for library_pattern in [
            "Iex*OpenImageIO*.lib",
            "IlmThread*OpenImageIO*.lib",
            "Imath*OpenImageIO*.lib",
            "libIex*OpenImageIO*.a",
            "libIlmThread*OpenImageIO*.a",
            "libImath*OpenImageIO*.a",
            "libminizip-ng.a",
            "libOpenColorIO*.a",
            "libOpenEXR*OpenImageIO*.a",
            "libpystring.a",
            "libyaml-cpp.a",
            "OpenEXR*OpenImageIO*.lib",
        ]:
            glob_remove(os.path.join(active_install_dir, "lib", library_pattern))
        for library_pattern in [
            "Iex*OpenImageIO*.dll",
            "IlmThread*OpenImageIO*.dll",
            "Imath*OpenImageIO*.dll",
            "OpenEXR*OpenImageIO*.dll",
        ]:
            glob_remove(os.path.join(active_install_dir, "bin", library_pattern))
        glob_remove(os.path.join(active_install_dir, "lib", "libOpenImageIO*"))
        glob_remove(os.path.join(active_install_dir, "lib", "OpenImageIO*"))

    def patch_opencolorio_minizip_link(local_deps_install_dir: str):
        if not is_mac():
            return

        opencolorio_targets = os.path.join(
            local_deps_install_dir,
            "lib",
            "cmake",
            "OpenColorIO",
            "OpenColorIOTargets.cmake",
        )
        minizip_target_link = r"\$<LINK_ONLY:MINIZIP::minizip-ng>"
        hidden_minizip_link = (
            r"-Wl,-L${_IMPORT_PREFIX}/lib;"
            r"-Wl,-hidden-lminizip-ng"
        )
        legacy_hidden_minizip_link = (
            r"\$<LINK_ONLY:-Wl,-L${_IMPORT_PREFIX}/lib>;"
            r"$<LINK_ONLY:-Wl,-hidden-lminizip-ng>"
        )
        escaped_hidden_minizip_link = (
            r"\$<LINK_ONLY:-Wl,-L${_IMPORT_PREFIX}/lib>;"
            r"\$<LINK_ONLY:-Wl,-hidden-lminizip-ng>"
        )
        if os.path.exists(opencolorio_targets):
            opencolorio_targets_text = Path(opencolorio_targets).read_text(
                encoding="utf-8", errors="ignore"
            )
            if minizip_target_link in opencolorio_targets_text:
                patch_file(
                    opencolorio_targets,
                    from_texts=[minizip_target_link],
                    to_texts=[hidden_minizip_link],
                    keep_bak_file=False,
                )
            elif legacy_hidden_minizip_link in opencolorio_targets_text:
                patch_file(
                    opencolorio_targets,
                    from_texts=[legacy_hidden_minizip_link],
                    to_texts=[hidden_minizip_link],
                    keep_bak_file=False,
                )
            elif escaped_hidden_minizip_link in opencolorio_targets_text:
                patch_file(
                    opencolorio_targets,
                    from_texts=[escaped_hidden_minizip_link],
                    to_texts=[hidden_minizip_link],
                    keep_bak_file=False,
                )
            else:
                assert hidden_minizip_link in opencolorio_targets_text, (
                    f"{opencolorio_targets}: missing minizip-ng link entry"
                )

    def patch_openimageio_config(active_install_dir: str):
        patch_file(
            os.path.join(
                active_install_dir,
                "lib",
                "cmake",
                "OpenImageIO",
                "OpenImageIOConfig.cmake",
            ),
            from_texts=["find_dependency(fmt)", "find_dependency(TIFF)"],
            to_texts=[
                "find_dependency(fmt CONFIG)",
                "find_dependency(TIFF CONFIG)",
            ],
            keep_bak_file=False,
        )

    def openimageio_build_env(*, arm64_only: bool) -> dict[str, str]:
        env = {"CMAKE_BUILD_PARALLEL_LEVEL": str(parallel_jobs_count())}
        if use_ninja():
            path_entries = [os.path.dirname(get_ninja_binary())]
            if is_windows() and use_clang_cl():
                path_entries.append(llvm_bin_dir())
            env["PATH"] = os.pathsep.join(path_entries)
        if is_windows() and use_clang_cl():
            env["CC"] = clang_cl_binary()
            env["CXX"] = clang_cl_binary()
        if is_mac():
            cbf = get_common_build_flags(with_optimization=False, arm64_only=arm64_only)
            env.update(
                {
                    "CC": cbf["CC"],
                    "CFLAGS": cbf["CFLAGS"],
                    "LDFLAGS": cbf["LDFLAGS"],
                    "CXX": cbf["CXX"],
                    "CXXFLAGS": cbf["CXXFLAGS"],
                }
            )
        return env

    def build_openimageio_arch(active_install_dir: str, *, arm64_only: bool):
        active_build_dir = create_build_dir(src_dir)
        active_local_deps_root = os.path.join(active_install_dir, "openimageio-local")
        try:
            remove_openimageio_outputs(active_install_dir)
            cmakecmd = get_cmake_cmd_common_part(
                active_install_dir, arm64_only=arm64_only
            )
            cmakecmd.append(
                "-DCMAKE_PREFIX_PATH="
                + cmake_prefix_path_for_arch(active_install_dir, install_dir)
            )
            local_deps = [
                "pystring",
                # Keep Expat private to Atlas/OIIO instead of accepting
                # platform SDK or framework headers paired with system stubs.
                "expat",
            ]
            if arm64_only:
                local_deps.extend(
                    [
                        "Imath",
                        "OpenEXR",
                        "OpenColorIO",
                        "yaml-cpp",
                        "minizip-ng",
                        "Robinmap",
                    ]
                )
            cmakecmd.append("-DOpenImageIO_BUILD_LOCAL_DEPS=" + ";".join(local_deps))
            cmakecmd.extend(
                [
                    "-DBUILD_SHARED_LIBS:BOOL=OFF",
                    "-DLOCAL_BUILD_SHARED_LIBS_DEFAULT:BOOL=OFF",
                    "-DImath_BUILD_SHARED_LIBS:BOOL=OFF",
                    "-DOpenEXR_BUILD_SHARED_LIBS:BOOL=OFF",
                    "-DOpenColorIO_BUILD_SHARED_LIBS:BOOL=OFF",
                    "-DLINKSTATIC:BOOL=ON",
                    "-DEMBEDPLUGINS:BOOL=ON",
                    "-DOIIO_INTERNALIZE_FMT:BOOL=OFF",
                    "-Dfmt_DIR=" + fmt_config_dir,
                    "-DTBB_DIR:PATH=" + tbb_dir(),
                    "-DOIIO_BUILD_TOOLS:BOOL=OFF",
                    "-DOIIO_BUILD_TESTS:BOOL=OFF",
                    "-DBUILD_DOCS:BOOL=OFF",
                    "-DINSTALL_DOCS:BOOL=OFF",
                    "-DINSTALL_FONTS:BOOL=OFF",
                    "-DUSE_PYTHON:BOOL=OFF",
                    "-DUSE_QT:BOOL=OFF",
                    # OIIO 3.1 still compiles color_ocio.cpp into libOpenImageIO,
                    # so disabling OCIO leaves unresolved OpenColorIO symbols at
                    # downstream static-link time.
                    "-DUSE_OpenColorIO:BOOL=ON",
                    "-DUSE_OpenCV:BOOL=OFF",
                    "-DUSE_TBB:BOOL=ON",
                    "-DUSE_FFmpeg:BOOL=OFF",
                    "-DUSE_Freetype:BOOL=OFF",
                    "-DUSE_DCMTK:BOOL=OFF",
                    "-DUSE_GIF:BOOL=ON",
                    "-DUSE_JXL:BOOL=ON",
                    "-DUSE_libuhdr:BOOL=OFF",
                    "-DUSE_Libheif:BOOL=OFF",
                    "-DUSE_LibRaw:BOOL=ON",
                    "-DUSE_OpenVDB:BOOL=OFF",
                    "-DUSE_Ptex:BOOL=OFF",
                    "-DUSE_R3DSDK:BOOL=OFF",
                    "-DOIIO_USE_CUDA:BOOL=OFF",
                    "-DSTOP_ON_WARNING:BOOL=OFF",
                    "-DOpenImageIO_BUILD_MISSING_DEPS=required",
                    "-DOpenImageIO_LOCAL_DEPS_ROOT="
                    + cmake_path(active_local_deps_root),
                    "-DOpenImageIO_LOCAL_DEPS_INSTALL_DIR="
                    + cmake_path(active_install_dir),
                    # Make CMake 4.x tolerate older minimum-version declarations
                    # in OIIO's local dependency bootstrap projects.
                    "-DCMAKE_POLICY_VERSION_MINIMUM=3.5",
                    src_dir,
                ]
            )
            build_and_install_cmakecmd(
                cmakecmd,
                active_build_dir,
                additional_env=openimageio_build_env(arm64_only=arm64_only),
            )
            patch_opencolorio_minizip_link(active_install_dir)
            patch_openimageio_config(active_install_dir)
            rm_tree(active_local_deps_root)
        finally:
            shutil.rmtree(active_build_dir, ignore_errors=False)

    patches = [
        FilePatcher(
            orig_file=os.path.join(src_dir, "src", "cmake", "externalpackages.cmake"),
            from_texts=[
                """checked_find_package (TIFF REQUIRED
                      VERSION_MIN 4.0
""",
                """checked_find_package (OpenColorIO REQUIRED
                      VERSION_MIN 2.3
                      VERSION_MAX 2.9
                     )
if (NOT OPENCOLORIO_INCLUDES)
    get_target_property(OPENCOLORIO_INCLUDES OpenColorIO::OpenColorIO INTERFACE_INCLUDE_DIRECTORIES)
endif ()
include_directories(BEFORE ${OPENCOLORIO_INCLUDES})
""",
            ],
            to_texts=[
                """checked_find_package (TIFF REQUIRED CONFIG
                      VERSION_MIN 4.0
""",
                """checked_find_package (OpenColorIO REQUIRED
                      VERSION_MIN 2.3
                      VERSION_MAX 2.9
                     )
if (OpenColorIO_FOUND)
    if (NOT OPENCOLORIO_INCLUDES AND TARGET OpenColorIO::OpenColorIO)
        get_target_property(OPENCOLORIO_INCLUDES OpenColorIO::OpenColorIO INTERFACE_INCLUDE_DIRECTORIES)
    endif ()
    include_directories(BEFORE ${OPENCOLORIO_INCLUDES})
endif ()
""",
            ],
        ),
        FilePatcher(
            orig_file=os.path.join(src_dir, "src", "cmake", "externalpackages.cmake"),
            from_texts=[
                """set (OPENIMAGEIO_CONFIG_DO_NOT_FIND_IMATH OFF CACHE BOOL
     "Exclude find_dependency(Imath) from the exported OpenImageIOConfig.cmake")
"""
            ],
            to_texts=[
                """set (OPENIMAGEIO_CONFIG_DO_NOT_FIND_IMATH OFF CACHE BOOL
     "Exclude find_dependency(Imath) from the exported OpenImageIOConfig.cmake")
set (OPENIMAGEIO_CONFIG_DO_NOT_FIND_OPENEXR OFF CACHE BOOL
     "Exclude find_dependency(OpenEXR) from the exported OpenImageIOConfig.cmake")
"""
            ],
        ),
        FilePatcher(
            orig_file=os.path.join(src_dir, "src", "cmake", "modules", "FindJXL.cmake"),
            from_texts=[
                """find_library(JXL_THREADS_LIBRARY
  NAMES jxl_threads)
mark_as_advanced(JXL_THREADS_LIBRARY)

find_package_handle_standard_args(JXL
  REQUIRED_VARS JXL_LIBRARY JXL_THREADS_LIBRARY JXL_INCLUDE_DIR)

if(JXL_FOUND)
  set(JXL_LIBRARIES ${JXL_LIBRARY} ${JXL_THREADS_LIBRARY})
  set(JXL_INCLUDES ${JXL_INCLUDE_DIR})
endif(JXL_FOUND)
"""
            ],
            to_texts=[
                """find_library(JXL_THREADS_LIBRARY
  NAMES jxl_threads)
mark_as_advanced(JXL_THREADS_LIBRARY)

find_library(JXL_CMS_LIBRARY
  NAMES jxl_cms)
mark_as_advanced(JXL_CMS_LIBRARY)

find_library(HWY_LIBRARY
  NAMES hwy)
mark_as_advanced(HWY_LIBRARY)

find_library(BROTLIENC_LIBRARY
  NAMES brotlienc)
find_library(BROTLIDEC_LIBRARY
  NAMES brotlidec)
find_library(BROTLICOMMON_LIBRARY
  NAMES brotlicommon)
mark_as_advanced(BROTLIENC_LIBRARY BROTLIDEC_LIBRARY BROTLICOMMON_LIBRARY)

find_library(CMATH_LIBRARY m)
mark_as_advanced(CMATH_LIBRARY)

find_package_handle_standard_args(JXL
  REQUIRED_VARS JXL_LIBRARY JXL_THREADS_LIBRARY JXL_CMS_LIBRARY HWY_LIBRARY
                BROTLIENC_LIBRARY BROTLIDEC_LIBRARY BROTLICOMMON_LIBRARY
                JXL_INCLUDE_DIR)

if(JXL_FOUND)
  set(JXL_LIBRARIES
      ${JXL_LIBRARY}
      ${JXL_THREADS_LIBRARY}
      ${JXL_CMS_LIBRARY}
      ${HWY_LIBRARY}
      ${BROTLIENC_LIBRARY}
      ${BROTLIDEC_LIBRARY}
      ${BROTLICOMMON_LIBRARY})
  if(CMATH_LIBRARY)
    list(APPEND JXL_LIBRARIES ${CMATH_LIBRARY})
  endif()
  set(JXL_INCLUDES ${JXL_INCLUDE_DIR})
endif(JXL_FOUND)
"""
            ],
        ),
        FilePatcher(
            orig_file=os.path.join(
                src_dir, "src", "cmake", "modules", "FindLibRaw.cmake"
            ),
            from_texts=[
                """if (LINKSTATIC)
    # Necessary?
    find_package (Jasper)
    if (JASPER_FOUND)
        set (LibRaw_r_LIBRARIES ${LibRaw_r_LIBRARIES} ${JASPER_LIBRARIES})
    endif()
    find_library (LCMS2_LIBRARIES NAMES lcms2)
    if (LCMS2_LIBRARIES)
        set (LibRaw_r_LIBRARIES ${LibRaw_r_LIBRARIES} ${LCMS2_LIBRARIES})
    endif()
    if (MSVC)
        set (LibRaw_r_DEFINITIONS ${LibRaw_r_DEFINITIONS} -D LIBRAW_NODLL)
        set (LibRaw_DEFINITIONS ${LibRaw_DEFINITIONS} -D LIBRAW_NODLL)
    endif()
endif ()
"""
            ],
            to_texts=[
                """if (LINKSTATIC)
    find_package (Jasper)
    if (JASPER_FOUND)
        list (APPEND LibRaw_r_LIBRARIES ${JASPER_LIBRARIES})
        list (APPEND LibRaw_LIBRARIES ${JASPER_LIBRARIES})
    endif()

    find_package (ZLIB)
    if (TARGET ZLIB::ZLIB)
        list (APPEND LibRaw_r_LIBRARIES ZLIB::ZLIB)
        list (APPEND LibRaw_LIBRARIES ZLIB::ZLIB)
    elseif (ZLIB_LIBRARIES)
        list (APPEND LibRaw_r_LIBRARIES ${ZLIB_LIBRARIES})
        list (APPEND LibRaw_LIBRARIES ${ZLIB_LIBRARIES})
    endif()

    find_package (JPEG)
    if (TARGET JPEG::JPEG)
        list (APPEND LibRaw_r_LIBRARIES JPEG::JPEG)
        list (APPEND LibRaw_LIBRARIES JPEG::JPEG)
    elseif (JPEG_LIBRARIES)
        list (APPEND LibRaw_r_LIBRARIES ${JPEG_LIBRARIES})
        list (APPEND LibRaw_LIBRARIES ${JPEG_LIBRARIES})
    endif()

    find_package (Threads)
    if (CMAKE_THREAD_LIBS_INIT)
        list (APPEND LibRaw_r_LIBRARIES ${CMAKE_THREAD_LIBS_INIT})
        list (APPEND LibRaw_LIBRARIES ${CMAKE_THREAD_LIBS_INIT})
    endif()

    find_library (CMATH_LIBRARY m)
    if (CMATH_LIBRARY)
        list (APPEND LibRaw_r_LIBRARIES ${CMATH_LIBRARY})
        list (APPEND LibRaw_LIBRARIES ${CMATH_LIBRARY})
    endif()

    if (MSVC)
        list (APPEND LibRaw_r_DEFINITIONS -D LIBRAW_NODLL)
        list (APPEND LibRaw_DEFINITIONS -D LIBRAW_NODLL)
    endif()
endif ()
"""
            ],
        ),
        FilePatcher(
            orig_file=os.path.join(src_dir, "src", "cmake", "dependency_utils.cmake"),
            from_texts=[
                """    if (CMAKE_IGNORE_PATH)
        string(REPLACE ";" "\\\\;" CMAKE_IGNORE_PATH_ESCAPED "${CMAKE_IGNORE_PATH}")
        list(APPEND _pkg_CMAKE_ARGS "-DCMAKE_IGNORE_PATH=${CMAKE_IGNORE_PATH_ESCAPED}")
    endif()

    # Pass along any CMAKE_MSVC_RUNTIME_LIBRARY
""",
                """set_cache (${PROJECT_NAME}_LOCAL_DEPS_ROOT "${PROJECT_BINARY_DIR}/deps"
           "Directory were we do local builds of dependencies")
list (APPEND CMAKE_PREFIX_PATH ${${PROJECT_NAME}_LOCAL_DEPS_ROOT}/dist)
include_directories(BEFORE ${${PROJECT_NAME}_LOCAL_DEPS_ROOT}/include)
""",
                """    set (${pkgname}_LOCAL_INSTALL_DIR "${${PROJECT_NAME}_LOCAL_DEPS_ROOT}/dist")
""",
            ],
            to_texts=[
                """    set(_pkg_INHERITED_CMAKE_ARGS)
    if (CMAKE_GENERATOR)
        list(APPEND _pkg_INHERITED_CMAKE_ARGS "-G" "${CMAKE_GENERATOR}")
    endif()
    if (CMAKE_GENERATOR_PLATFORM)
        list(APPEND _pkg_INHERITED_CMAKE_ARGS "-A" "${CMAKE_GENERATOR_PLATFORM}")
    endif()
    if (CMAKE_GENERATOR_TOOLSET)
        list(APPEND _pkg_INHERITED_CMAKE_ARGS "-T" "${CMAKE_GENERATOR_TOOLSET}")
    endif()

    foreach (_pkg_INHERITED_CMAKE_VAR
             CMAKE_MAKE_PROGRAM
             CMAKE_C_COMPILER
             CMAKE_CXX_COMPILER
             CMAKE_LINKER
             CMAKE_AR
             CMAKE_RANLIB
             CMAKE_RC_COMPILER
             CMAKE_MT
             CMAKE_PREFIX_PATH
             CMAKE_IGNORE_PREFIX_PATH
             CMAKE_C_FLAGS
             CMAKE_CXX_FLAGS
             CMAKE_EXE_LINKER_FLAGS
             CMAKE_SHARED_LINKER_FLAGS
             CMAKE_MODULE_LINKER_FLAGS
             CMAKE_STATIC_LINKER_FLAGS
             CMAKE_FIND_FRAMEWORK
             CMAKE_FIND_ROOT_PATH
             CMAKE_FIND_ROOT_PATH_MODE_PROGRAM
             CMAKE_FIND_ROOT_PATH_MODE_LIBRARY
             CMAKE_FIND_ROOT_PATH_MODE_INCLUDE
             CMAKE_FIND_ROOT_PATH_MODE_PACKAGE
             CMAKE_POLICY_VERSION_MINIMUM)
        if (DEFINED ${_pkg_INHERITED_CMAKE_VAR} AND NOT "${${_pkg_INHERITED_CMAKE_VAR}}" STREQUAL "")
            string(REPLACE ";" "\\\\;" _pkg_INHERITED_CMAKE_VALUE "${${_pkg_INHERITED_CMAKE_VAR}}")
            list(APPEND _pkg_INHERITED_CMAKE_ARGS "-D${_pkg_INHERITED_CMAKE_VAR}=${_pkg_INHERITED_CMAKE_VALUE}")
        endif()
    endforeach()
    set(_pkg_CMAKE_ARGS ${_pkg_INHERITED_CMAKE_ARGS} ${_pkg_CMAKE_ARGS})

    if (CMAKE_IGNORE_PATH)
        string(REPLACE ";" "\\\\;" CMAKE_IGNORE_PATH_ESCAPED "${CMAKE_IGNORE_PATH}")
        list(APPEND _pkg_CMAKE_ARGS "-DCMAKE_IGNORE_PATH=${CMAKE_IGNORE_PATH_ESCAPED}")
    endif()

    if (APPLE AND CMAKE_OSX_ARCHITECTURES)
        string(REPLACE ";" "\\\\;" CMAKE_OSX_ARCHITECTURES_ESCAPED "${CMAKE_OSX_ARCHITECTURES}")
        list(APPEND _pkg_CMAKE_ARGS "-DCMAKE_OSX_ARCHITECTURES=${CMAKE_OSX_ARCHITECTURES_ESCAPED}")
        if (CMAKE_OSX_DEPLOYMENT_TARGET)
            list(APPEND _pkg_CMAKE_ARGS "-DCMAKE_OSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}")
        endif()
        if (CMAKE_OSX_SYSROOT)
            list(APPEND _pkg_CMAKE_ARGS "-DCMAKE_OSX_SYSROOT=${CMAKE_OSX_SYSROOT}")
        endif()
    endif()

    # Pass along any CMAKE_MSVC_RUNTIME_LIBRARY
""",
                """set_cache (${PROJECT_NAME}_LOCAL_DEPS_ROOT "${PROJECT_BINARY_DIR}/deps"
           "Directory were we do local builds of dependencies")
set_cache (${PROJECT_NAME}_LOCAL_DEPS_INSTALL_DIR "${${PROJECT_NAME}_LOCAL_DEPS_ROOT}/dist"
           "Directory where locally built dependencies are installed")
list (PREPEND CMAKE_PREFIX_PATH ${${PROJECT_NAME}_LOCAL_DEPS_INSTALL_DIR})
include_directories(BEFORE ${${PROJECT_NAME}_LOCAL_DEPS_INSTALL_DIR}/include)
""",
                """    set (${pkgname}_LOCAL_INSTALL_DIR "${${PROJECT_NAME}_LOCAL_DEPS_INSTALL_DIR}")
""",
            ],
        ),
        FilePatcher(
            orig_file=os.path.join(src_dir, "src", "cmake", "dependency_utils.cmake"),
            from_texts=[
                """                ${_pkg_CMAKE_ARGS}
        ${_pkg_exec_quiet}
        )
""",
                """    execute_process (COMMAND ${CMAKE_COMMAND}
                        --build ${${pkgname}_LOCAL_BUILD_DIR}
                        --config ${${PROJECT_NAME}_DEPENDENCY_BUILD_TYPE}
                     ${_pkg_exec_quiet}
                    )
""",
                """        execute_process (COMMAND ${CMAKE_COMMAND}
                            --build ${${pkgname}_LOCAL_BUILD_DIR}
                            --config ${${PROJECT_NAME}_DEPENDENCY_BUILD_TYPE}
                            --target install
                         ${_pkg_exec_quiet}
                        )
""",
            ],
            to_texts=[
                """                ${_pkg_CMAKE_ARGS}
        ${_pkg_exec_quiet}
        RESULT_VARIABLE _pkg_configure_result
        )
    if (_pkg_configure_result)
        message(FATAL_ERROR "Configuring local ${pkgname} failed with exit code ${_pkg_configure_result}")
    endif()
""",
                """    execute_process (COMMAND ${CMAKE_COMMAND}
                        --build ${${pkgname}_LOCAL_BUILD_DIR}
                        --config ${${PROJECT_NAME}_DEPENDENCY_BUILD_TYPE}
                     ${_pkg_exec_quiet}
                     RESULT_VARIABLE _pkg_build_result
                    )
    if (_pkg_build_result)
        message(FATAL_ERROR "Building local ${pkgname} failed with exit code ${_pkg_build_result}")
    endif()
""",
                """        execute_process (COMMAND ${CMAKE_COMMAND}
                            --build ${${pkgname}_LOCAL_BUILD_DIR}
                            --config ${${PROJECT_NAME}_DEPENDENCY_BUILD_TYPE}
                            --target install
                         ${_pkg_exec_quiet}
                         RESULT_VARIABLE _pkg_install_result
                        )
        if (_pkg_install_result)
            message(FATAL_ERROR "Installing local ${pkgname} failed with exit code ${_pkg_install_result}")
        endif()
""",
            ],
        ),
        FilePatcher(
            orig_file=os.path.join(src_dir, "src", "cmake", "build_minizip-ng.cmake"),
            from_texts=[
                """        -D ZLIB_LIBRARY=${ZLIB_LIBRARIES}
        -D ZLIB_INCLUDE_DIR=${ZLIB_INCLUDE_DIRS}
"""
            ],
            to_texts=[
                """        -D ZLIB_LIBRARY=${ZLIB_LIBRARIES}
        -D ZLIB_INCLUDE_DIR=${ZLIB_INCLUDE_DIRS}
        -D "CMAKE_C_FLAGS=${CMAKE_C_FLAGS} -Dmz_zip_writer_add_file=oiio_mz_zip_writer_add_file"
"""
            ],
        ),
        FilePatcher(
            orig_file=os.path.join(src_dir, "src", "cmake", "build_libdeflate.cmake"),
            from_texts=[
                """        -D CMAKE_INSTALL_LIBDIR=lib
        -D LIBDEFLATE_BUILD_GZIP=OFF
"""
            ],
            to_texts=[
                """        -D CMAKE_INSTALL_LIBDIR=lib
        -D LIBDEFLATE_BUILD_GZIP=OFF
        -D "CMAKE_C_FLAGS=${CMAKE_C_FLAGS} -D__EVEX512__"
"""
            ],
            patch_condition=use_windows_clang_cl,
        ),
        FilePatcher(
            orig_file=os.path.join(src_dir, "src", "cmake", "build_OpenColorIO.cmake"),
            from_texts=[
                """        -D CMAKE_INSTALL_LIBDIR=lib
        # Don't built unnecessary parts of OCIO
"""
            ],
            to_texts=[
                """        -D CMAKE_INSTALL_LIBDIR=lib
        -D ZLIB_LIBRARY=${ZLIB_LIBRARIES}
        -D ZLIB_INCLUDE_DIR=${ZLIB_INCLUDE_DIRS}
        -D ZLIB_CMAKE_ARGS=-DZLIB_BUILD_EXAMPLES=OFF
        # Don't built unnecessary parts of OCIO
"""
            ],
        ),
        FilePatcher(
            orig_file=os.path.join(src_dir, "src", "cmake", "build_OpenColorIO.cmake"),
            from_texts=[
                """        -D ZLIB_CMAKE_ARGS=-DZLIB_BUILD_EXAMPLES=OFF
        # Don't built unnecessary parts of OCIO
"""
            ],
            to_texts=[
                """        -D ZLIB_CMAKE_ARGS=-DZLIB_BUILD_EXAMPLES=OFF
        -D OCIO_USE_AVX=OFF
        -D OCIO_USE_AVX2=OFF
        -D OCIO_USE_AVX512=OFF
        -D OCIO_USE_F16C=OFF
        # Don't built unnecessary parts of OCIO
"""
            ],
            patch_condition=use_windows_clang_cl,
        ),
        FilePatcher(
            orig_file=os.path.join(src_dir, "src", "tiff.imageio", "CMakeLists.txt"),
            from_texts=[
                """add_oiio_plugin (tiffinput.cpp tiffoutput.cpp
                 LINK_LIBRARIES TIFF::TIFF
"""
            ],
            to_texts=[
                """add_oiio_plugin (tiffinput.cpp tiffoutput.cpp
                 INCLUDE_DIRS ${TIFF_INCLUDE_DIRS}
                 LINK_LIBRARIES TIFF::TIFF
"""
            ],
        ),
        FilePatcher(
            orig_file=os.path.join(src_dir, "src", "jpegxl.imageio", "CMakeLists.txt"),
            from_texts=[
                """                     LINK_LIBRARIES ${JXL_LIBRARIES}
                     DEFINITIONS "USE_JXL")
"""
            ],
            to_texts=[
                """                     LINK_LIBRARIES ${JXL_LIBRARIES}
                     DEFINITIONS "USE_JXL;JXL_STATIC_DEFINE;JXL_THREADS_STATIC_DEFINE;JXL_CMS_STATIC_DEFINE")
"""
            ],
            patch_condition=is_windows,
        ),
        FilePatcher(
            orig_file=os.path.join(
                src_dir, "src", "include", "OpenImageIO", "detail", "fmt.h"
            ),
            from_texts=[
                """// We want the header-only implementation of fmt
#ifndef FMT_HEADER_ONLY
#    define FMT_HEADER_ONLY
#endif
"""
            ],
            to_texts=[
                """// Atlas links OpenImageIO against the same compiled fmt target as the
// rest of the dependency graph. Do not force header-only fmt here: doing so
// emits fmt definitions into OpenImageIO.lib and conflicts with fmt.lib.
"""
            ],
        ),
        FilePatcher(
            orig_file=os.path.join(src_dir, "src", "libOpenImageIO", "CMakeLists.txt"),
            from_texts=[
                """            OpenColorIO::OpenColorIO
"""
            ],
            to_texts=[
                """            $<TARGET_NAME_IF_EXISTS:OpenColorIO::OpenColorIO>
"""
            ],
        ),
        FilePatcher(
            orig_file=os.path.join(src_dir, "src", "libOpenImageIO", "CMakeLists.txt"),
            from_texts=[
                """if (WIN32)
    configure_file(../build-scripts/version_win32.rc.in "${CMAKE_CURRENT_BINARY_DIR}/version_win32.rc" @ONLY)
    add_library (OpenImageIO ${libOpenImageIO_srcs} ${CMAKE_CURRENT_BINARY_DIR}/version_win32.rc)
else ()
    add_library (OpenImageIO ${libOpenImageIO_srcs})
endif ()
"""
            ],
            to_texts=[
                """if (WIN32 AND BUILD_SHARED_LIBS)
    configure_file(../build-scripts/version_win32.rc.in "${CMAKE_CURRENT_BINARY_DIR}/version_win32.rc" @ONLY)
    add_library (OpenImageIO ${libOpenImageIO_srcs} ${CMAKE_CURRENT_BINARY_DIR}/version_win32.rc)
else ()
    add_library (OpenImageIO ${libOpenImageIO_srcs})
endif ()
"""
            ],
            patch_condition=use_windows_clang_cl,
        ),
        FilePatcher(
            orig_file=os.path.join(src_dir, "src", "libutil", "CMakeLists.txt"),
            from_texts=[
                """        target_link_libraries (${targetname}
                               PUBLIC fmt::fmt-header-only)
"""
            ],
            to_texts=[
                """        target_link_libraries (${targetname}
                               PUBLIC fmt::fmt)
"""
            ],
        ),
        FilePatcher(
            orig_file=os.path.join(src_dir, "src", "libutil", "CMakeLists.txt"),
            from_texts=[
                """    if (WIN32)
        configure_file(../build-scripts/version_win32.rc.in "${CMAKE_CURRENT_BINARY_DIR}/version_win32.rc" @ONLY)
        add_library (${targetname} ${libtype} ${libOpenImageIO_Util_srcs} ${CMAKE_CURRENT_BINARY_DIR}/version_win32.rc)
    else ()
        add_library (${targetname} ${libtype} ${libOpenImageIO_Util_srcs})
    endif ()
"""
            ],
            to_texts=[
                """    if (WIN32 AND BUILD_SHARED_LIBS)
        configure_file(../build-scripts/version_win32.rc.in "${CMAKE_CURRENT_BINARY_DIR}/version_win32.rc" @ONLY)
        add_library (${targetname} ${libtype} ${libOpenImageIO_Util_srcs} ${CMAKE_CURRENT_BINARY_DIR}/version_win32.rc)
    else ()
        add_library (${targetname} ${libtype} ${libOpenImageIO_Util_srcs})
    endif ()
"""
            ],
            patch_condition=use_windows_clang_cl,
        ),
        FilePatcher(
            orig_file=os.path.join(src_dir, "src", "cmake", "Config.cmake.in"),
            from_texts=[
                """if (NOT @OPENIMAGEIO_CONFIG_DO_NOT_FIND_IMATH@ AND NOT OPENIMAGEIO_CONFIG_DO_NOT_FIND_IMATH)
    find_dependency(Imath @Imath_VERSION@
                    HINTS @Imath_DIR@)
endif ()
""",
                """    find_dependency(TIFF)
    find_dependency(OpenColorIO)
""",
                """        find_dependency(TBB)
""",
            ],
            to_texts=[
                """if (NOT @OPENIMAGEIO_CONFIG_DO_NOT_FIND_IMATH@ AND NOT OPENIMAGEIO_CONFIG_DO_NOT_FIND_IMATH)
    find_dependency(Imath @Imath_VERSION@
                    HINTS @Imath_DIR@)
endif ()
if (NOT @OPENIMAGEIO_CONFIG_DO_NOT_FIND_OPENEXR@ AND NOT OPENIMAGEIO_CONFIG_DO_NOT_FIND_OPENEXR)
    find_dependency(OpenEXR CONFIG
                    HINTS @OpenEXR_DIR@)
endif ()
""",
                """    find_dependency(TIFF)
    if (@GIF_FOUND@)
        find_dependency(GIF CONFIG)
    endif()
    if (@OpenColorIO_FOUND@)
        find_dependency(OpenColorIO)
    endif()
""",
                """        find_dependency(TBB CONFIG)
""",
            ],
        ),
    ]
    patch_manager = PatchManager(patches)

    try:
        assert os.path.isdir(fmt_config_dir), (
            f"Atlas fmt package is required before OpenImageIO: {fmt_config_dir}"
        )
        patch_manager.apply_patches()

        build_openimageio_arch(install_dir, arm64_only=False)
        if is_mac():
            arm64_install_dir = create_arm64_install_dir(src_dir)
            try:
                build_openimageio_arch(arm64_install_dir, arm64_only=True)
                create_universal_binaries(arm64_install_dir, install_dir)
            finally:
                rm_tree(arm64_install_dir)
    finally:
        patch_manager.restore_files()


def build_jxrlib(src_dir: str, install_dir: str):
    try:
        orig_file = os.path.join(src_dir, "Makefile")
        from_texts = [
            r"CFLAGS=-I. -Icommon/include -I$(DIR_SYS) "
            r"$(ENDIANFLAG) -D__ANSI__ -DDISABLE_PERF_MEASUREMENT -w $(PICFLAG) -O",
            r"@python -c ",
        ]
        if is_linux():
            to_texts = [
                r"CFLAGS=-I. -Icommon/include -I$(DIR_SYS) "
                r"$(ENDIANFLAG) -D__ANSI__ -DDISABLE_PERF_MEASUREMENT -w $(PICFLAG) -O3 -fPIC -mavx "
                r"-Wno-error=implicit-function-declaration ",
                r"cp $< $@ # @python -c ",
            ]
        else:
            to_texts = [
                r"CFLAGS=-arch x86_64 -arch arm64 -I. -Icommon/include -I$(DIR_SYS) "
                r"$(ENDIANFLAG) -D__ANSI__ -DDISABLE_PERF_MEASUREMENT -w $(PICFLAG) -O3 -mavx -mcpu=apple-m1 "
                r"-Wno-error=implicit-function-declaration "
                r"-mmacosx-version-min={0}".format(macos_min_version()),
                r"cp $< $@ # @python -c ",
            ]
        patch_file(orig_file, from_texts=from_texts, to_texts=to_texts)

        # Security policy: upstream JXR's ANSI temp-file path uses tmpnam()
        # followed by fopen(), which is race-prone. Add a mkstemp-backed stream
        # helper and route non-Windows encoder temp files through it below.
        orig_file = os.path.join(src_dir, "image", "sys", "strcodec.c")
        patch_file(
            orig_file,
            from_texts=[r"ERR CloseWS_File(struct WMPStream** ppWS)"],
            to_texts=[
                r"""ERR CreateWS_FileTemp(struct WMPStream** ppWS, char* szFilename, const char* szMode)
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

ERR CloseWS_File(struct WMPStream** ppWS)"""
            ],
        )

        orig_file = os.path.join(src_dir, "image", "encode", "strenc.c")
        patch_file(
            orig_file,
            from_texts=[
                r"""#else //DPK needs to support ANSI 
                pSC->ppTempFile[i] = (char *)malloc(FILENAME_MAX * sizeof(char));
                if(pSC->ppTempFile[i] == NULL) return ICERR_ERROR;

                if ((pFilename = tmpnam(NULL)) == NULL)
                    return ICERR_ERROR;                
                strcpy(pSC->ppTempFile[i], pFilename);
#endif
                if(CreateWS_File(pSC->ppWStream + i, pFilename, "w+b") != ICERR_OK) return ICERR_ERROR;"""
            ],
            to_texts=[
                r"""                if(CreateWS_File(pSC->ppWStream + i, pFilename, "w+b") != ICERR_OK) return ICERR_ERROR;

#else //DPK needs to support ANSI 
                pSC->ppTempFile[i] = (char *)malloc(FILENAME_MAX * sizeof(char));
                if(pSC->ppTempFile[i] == NULL) return ICERR_ERROR;
                pFilename = NULL;
                snprintf(pSC->ppTempFile[i], L_tmpnam, "%s/tmp.XXXXXXXXXX", P_tmpdir);
                if(CreateWS_FileTemp(pSC->ppWStream + i, pSC->ppTempFile[i], "w+b") != ICERR_OK) return ICERR_ERROR;
#endif"""
            ],
        )

        if is_windows():
            env = get_vcvars_environment()
            msbuild_toolset = windows_native_platform_toolset()
            subprocess.run(
                ["devenv", "JXR_vc14.sln", "/Upgrade"],
                cwd=os.path.join(src_dir, "jxrencoderdecoder"),
                shell=True,
                check=True,
                env=env,
            )
            vcxproj_patch_manager = PatchManager([])
            if use_windows_clang_cl():
                vcxproj_patchers = []
                for vcxproj in Path(src_dir).rglob("*_vc14.vcxproj"):
                    txt = vcxproj.read_text(encoding="utf-8", errors="ignore")
                    from_texts = []
                    to_texts = []
                    if (
                        "<WholeProgramOptimization>true</WholeProgramOptimization>"
                        in txt
                    ):
                        from_texts.append(
                            "<WholeProgramOptimization>true</WholeProgramOptimization>"
                        )
                        to_texts.append(
                            "<WholeProgramOptimization>false</WholeProgramOptimization>"
                        )
                    if "<LinkTimeCodeGeneration>true</LinkTimeCodeGeneration>" in txt:
                        from_texts.append(
                            "<LinkTimeCodeGeneration>true</LinkTimeCodeGeneration>"
                        )
                        to_texts.append(
                            "<LinkTimeCodeGeneration>false</LinkTimeCodeGeneration>"
                        )
                    if (
                        "<LinkTimeCodeGeneration>UseLinkTimeCodeGeneration</LinkTimeCodeGeneration>"
                        in txt
                    ):
                        from_texts.append(
                            "<LinkTimeCodeGeneration>UseLinkTimeCodeGeneration</LinkTimeCodeGeneration>"
                        )
                        to_texts.append(
                            "<LinkTimeCodeGeneration>Default</LinkTimeCodeGeneration>"
                        )
                    if (
                        "<AdditionalOptions>/LTCG %(AdditionalOptions)</AdditionalOptions>"
                        in txt
                    ):
                        from_texts.append(
                            "<AdditionalOptions>/LTCG %(AdditionalOptions)</AdditionalOptions>"
                        )
                        to_texts.append(
                            "<AdditionalOptions>%(AdditionalOptions)</AdditionalOptions>"
                        )
                    if from_texts:
                        vcxproj_patchers.append(
                            FilePatcher(
                                orig_file=str(vcxproj),
                                from_texts=from_texts,
                                to_texts=to_texts,
                            )
                        )

                vcxproj_patch_manager = PatchManager(vcxproj_patchers)
                vcxproj_patch_manager.apply_patches()
            msbuild_cmd = [
                "MSBuild",
                "JXR_vc14.sln",
                "/target:JXRDecApp",
                "/property:Platform=x64",
                "/property:WindowsTargetPlatformVersion="
                + env["UCRTVERSION"],  # like 10.0.16299.0
                "/property:ForceImportBeforeCppTargets="
                + ext_dir()
                + "\\runtime_md.props",
                "/property:PlatformToolset=" + msbuild_toolset,
                "/property:Configuration=Release",
                "/maxcpucount",
            ]
            if msbuild_toolset == windows_msbuild_platform_toolset():
                msbuild_cmd.extend(
                    [
                        "/property:LLVMInstallDir=" + llvm_install_dir(),
                        "/property:LLVMToolsVersion=" + llvm_tools_version(),
                    ]
                )
            try:
                subprocess.run(
                    msbuild_cmd,
                    cwd=os.path.join(src_dir, "jxrencoderdecoder"),
                    shell=True,
                    check=True,
                    env=env,
                )
            finally:
                vcxproj_patch_manager.restore_files()
            glob_copy(
                os.path.join(src_dir, "common", "include", "*.h"),
                os.path.join(install_dir, "include", "libjxr", "common"),
            )
            glob_copy(
                os.path.join(src_dir, "image", "x86", "*.h"),
                os.path.join(install_dir, "include", "libjxr", "image", "x86"),
            )
            glob_copy(
                os.path.join(src_dir, "image", "sys", "*.h"),
                os.path.join(install_dir, "include", "libjxr", "image"),
            )
            glob_copy(
                os.path.join(src_dir, "image", "encode", "*.h"),
                os.path.join(install_dir, "include", "libjxr", "image"),
            )
            glob_copy(
                os.path.join(src_dir, "image", "decode", "*.h"),
                os.path.join(install_dir, "include", "libjxr", "image"),
            )
            glob_copy(
                os.path.join(src_dir, "jxrgluelib", "*.h"),
                os.path.join(install_dir, "include", "libjxr", "glue"),
            )

            glob_copy(
                os.path.join(
                    src_dir, "jxrgluelib", "Release", "JXRGlueLib", "x64", "*.lib"
                ),
                os.path.join(install_dir, "lib"),
            )
            glob_copy(
                os.path.join(
                    src_dir,
                    "image",
                    "vc14projects",
                    "Release",
                    "JXRCommonLib",
                    "x64",
                    "*.lib",
                ),
                os.path.join(install_dir, "lib"),
            )
            glob_copy(
                os.path.join(
                    src_dir,
                    "image",
                    "vc14projects",
                    "Release",
                    "JXRDecodeLib",
                    "x64",
                    "*.lib",
                ),
                os.path.join(install_dir, "lib"),
            )
            glob_copy(
                os.path.join(
                    src_dir,
                    "image",
                    "vc14projects",
                    "Release",
                    "JXREncodeLib",
                    "x64",
                    "*.lib",
                ),
                os.path.join(install_dir, "lib"),
            )
        else:
            subprocess.run(
                [
                    "make",
                    "-j" + str(os.cpu_count()),
                    "install",
                    "DIR_INSTALL=" + install_dir,
                ],
                cwd=src_dir,
                shell=False,
                check=True,
            )
    finally:
        # if not is_windows():
        #     shutil.rmtree(os.path.join(src_dir, 'build'), ignore_errors=True)
        #     os.replace(bak_file, orig_file)
        cleanup_git_submodule(src_dir)


def build_assimp(src_dir: str, install_dir: str):
    build_dir = create_build_dir(src_dir)

    patches = [
        # Behavior policy: Atlas can load very large meshes. Raise Assimp's
        # per-allocation guard rather than rejecting valid inputs at 256 MB.
        FilePatcher(
            orig_file=os.path.join(src_dir, "include", "assimp", "defs.h"),
            from_texts=[
                r"#define AI_MAX_ALLOC(type) ((256U * 1024 * 1024) / sizeof(type))"
            ],
            to_texts=[
                r"#define AI_MAX_ALLOC(type) ((size_t(256) * 1024 * 1024 * 1024) / sizeof(type))"
            ],
        ),
        FilePatcher(
            orig_file=os.path.join(src_dir, "code", "CMakeLists.txt"),
            from_texts=[r"-Werror", r"/WX"],
            to_texts=[r" ", r" "],
        ),
        # Draco policy: Assimp forces the glTF-only Draco bitstream profile for
        # its importer, but Atlas links Draco directly for Neuroglancer
        # precomputed meshes. Keep full Draco features enabled.
        FilePatcher(
            orig_file=os.path.join(src_dir, "CMakeLists.txt"),
            from_texts=[
                r" /WX",
                r"ADD_COMPILE_OPTIONS(/source-charset:utf-8)",
                r'SET(DRACO_GLTF_BITSTREAM ON CACHE BOOL "" FORCE)',
            ],
            to_texts=[
                r" ",
                r"",
                r'SET(DRACO_GLTF_BITSTREAM OFF CACHE BOOL "" FORCE)',
            ],
        ),
    ]
    patch_manager = PatchManager(patches)

    try:
        patch_manager.apply_patches()

        os.remove(os.path.join(src_dir, "cmake-modules", "FindZLIB.cmake"))

        cmakecmd = get_cmake_cmd_common_part(install_dir, universal=True)
        cmakecmd.extend(
            [
                "-DASSIMP_BUILD_ASSIMP_TOOLS:BOOL=OFF",
                "-DBUILD_SHARED_LIBS:BOOL=OFF",
                "-DASSIMP_BUILD_FRAMEWORK:BOOL=OFF",
                "-DASSIMP_BUILD_TESTS:BOOL=OFF",
                "-DASSIMP_BUILD_ZLIB:BOOL=OFF",
                "-DASSIMP_HUNTER_ENABLED:BOOL=OFF",
                "-DASSIMP_BUILD_DRACO_STATIC:BOOL=ON",
            ]
        )

        cmakecmd.extend([src_dir])
        build_and_install_cmakecmd(cmakecmd, build_dir)

        # Assimp's exported `assimp::draco` target points to `${_IMPORT_PREFIX}/include`.
        # Install Draco headers (and the generated draco_features.h) there so Atlas can
        # include <draco/...> directly when decoding Neuroglancer precomputed meshes.
        draco_src_include_dir = os.path.join(
            src_dir, "contrib", "draco", "src", "draco"
        )
        draco_dst_include_dir = os.path.join(install_dir, "include", "draco")
        if not os.path.isdir(draco_src_include_dir):
            raise RuntimeError(
                f"Draco headers not found in Assimp source tree: {draco_src_include_dir}"
            )
        if os.path.exists(draco_dst_include_dir):
            shutil.rmtree(
                draco_dst_include_dir, ignore_errors=False, onexc=handleRemoveReadonly
            )
        os.makedirs(draco_dst_include_dir, exist_ok=True)
        for root, dirs, files in os.walk(draco_src_include_dir):
            rel = os.path.relpath(root, draco_src_include_dir)
            dst_root = (
                draco_dst_include_dir
                if rel == "."
                else os.path.join(draco_dst_include_dir, rel)
            )
            os.makedirs(dst_root, exist_ok=True)
            for name in files:
                if not name.lower().endswith(".h"):
                    continue
                shutil.copy2(os.path.join(root, name), os.path.join(dst_root, name))

        draco_features_src = os.path.join(build_dir, "draco", "draco_features.h")
        if not os.path.isfile(draco_features_src):
            raise RuntimeError(
                f"draco_features.h was not generated during the Assimp build (expected at {draco_features_src}). "
                "Ensure Draco is enabled for Assimp (ASSIMP_BUILD_DRACO_STATIC=ON)."
            )
        shutil.copy2(
            draco_features_src, os.path.join(draco_dst_include_dir, "draco_features.h")
        )

        # if is_mac():
        #     subprocess.run(['install_name_tool', '-id', '@rpath/libIrrXML.dylib', 'lib/libIrrXML.dylib'],
        #                    cwd=install_dir, shell=False, check=True)
        #     subprocess.run(['install_name_tool', '-change', 'libIrrXML.dylib', '@loader_path/libIrrXML.dylib',
        #                     'lib/libassimp.dylib'],
        #                    cwd=install_dir, shell=False, check=True)

    finally:
        shutil.rmtree(build_dir, ignore_errors=False)
        patch_manager.restore_files()
        cleanup_git_submodule(src_dir)


def build_hdf5(src_dir: str, install_dir: str):
    build_dir = create_build_dir(src_dir)

    try:
        cmakecmd = get_cmake_cmd_common_part(install_dir, universal=True)
        cmakecmd.extend(
            [
                "-DBUILD_TESTING:BOOL=OFF",
                "-DBUILD_SHARED_LIBS:BOOL=OFF",
                "-DHDF5_ENABLE_DEPRECATED_SYMBOLS:BOOL=ON",
                "-DHDF5_ENABLE_ZLIB_SUPPORT:BOOL=ON",
                "-DHDF5_USE_ZLIB_STATIC:BOOL=ON",
                # HDF5 installs h5cc/h5c++ wrappers only when pkg-config is
                # found. Those wrappers can make CMake FindHDF5 probe shared
                # libraries even though our dependency build is static-only.
                "-DCMAKE_DISABLE_FIND_PACKAGE_PkgConfig:BOOL=ON",
                # '-DHDF5_ENABLE_SZIP_SUPPORT:BOOL=ON',
                "-DHDF5_ENABLE_THREADSAFE:BOOL=OFF",
                "-DHDF5_BUILD_EXAMPLES:BOOL=OFF",
                "-DHDF5_BUILD_CPP_LIB:BOOL=ON",
            ]
        )

        cmakecmd.extend([src_dir])
        build_and_install_cmakecmd(cmakecmd, build_dir)

        patch_file(
            os.path.join(install_dir, "cmake", "hdf5-targets.cmake"),
            from_texts=[r";\$<LINK_ONLY:ZLIB::ZLIB>"],
            to_texts=[r""],
        )
    finally:
        print()
        shutil.rmtree(build_dir, ignore_errors=False)


def build_itk(src_dir: str, install_dir: str):
    build_dir = create_build_dir(src_dir)

    patches = [
        FilePatcher(
            orig_file=os.path.join(
                src_dir,
                "Modules",
                "ThirdParty",
                "NIFTI",
                "src",
                "nifti",
                "niftilib",
                "nifti1_io.c",
            ),
            from_texts=[r"#include <limits.h>"],
            to_texts=["#include <limits.h>\n#include <stdint.h>"],
        ),
        FilePatcher(
            orig_file=os.path.join(
                src_dir, "Modules", "ThirdParty", "Eigen3", "CMakeLists.txt"
            ),
            from_texts=[r"set(_Eigen3_min_version 3.3)"],
            to_texts=[r"set(_Eigen3_min_version 5)"],
        ),
        FilePatcher(
            orig_file=os.path.join(
                src_dir,
                "Modules",
                "ThirdParty",
                "MINC",
                "src",
                "libminc",
                "config.h.cmake",
            ),
            from_texts=[
                """#cmakedefine HAVE_RINT 1
"""
            ],
            to_texts=[
                """#cmakedefine HAVE_RINT 1

#ifdef _WIN32
#include <io.h>
#include <process.h>

#ifndef tempnam
#define tempnam _tempnam
#endif
#ifndef getpid
#define getpid _getpid
#endif
#ifndef open
#define open _open
#endif
#ifndef close
#define close _close
#endif
#ifndef unlink
#define unlink _unlink
#endif
#ifndef lseek
#define lseek _lseek
#endif
#endif /* _WIN32 */
"""
            ],
            patch_condition=is_windows,
        ),
        FilePatcher(
            orig_file=os.path.join(
                src_dir, "Modules", "ThirdParty", "TIFF", "itk-module.cmake"
            ),
            from_texts=[
                """itk_module(ITKTIFF
  DEPENDS
"""
            ],
            to_texts=[
                """itk_module(ITKTIFF
  EXCLUDE_FROM_DEFAULT
  DEPENDS
"""
            ],
        ),
        FilePatcher(
            orig_file=os.path.join(
                src_dir, "Modules", "IO", "TIFF", "itk-module.cmake"
            ),
            from_texts=[
                """itk_module(
  ITKIOTIFF
  ENABLE_SHARED
"""
            ],
            to_texts=[
                """itk_module(
  ITKIOTIFF
  EXCLUDE_FROM_DEFAULT
  ENABLE_SHARED
"""
            ],
        ),
        FilePatcher(
            orig_file=os.path.join(src_dir, "Modules", "IO", "LSM", "itk-module.cmake"),
            from_texts=[
                """itk_module(
  ITKIOLSM
  DEPENDS
"""
            ],
            to_texts=[
                """itk_module(
  ITKIOLSM
  EXCLUDE_FROM_DEFAULT
  DEPENDS
"""
            ],
        ),
        FilePatcher(
            orig_file=os.path.join(
                src_dir, "Modules", "Core", "TestKernel", "itk-module.cmake"
            ),
            from_texts=[
                """itk_module(
  ITKTestKernel
  DEPENDS
"""
            ],
            to_texts=[
                """itk_module(
  ITKTestKernel
  EXCLUDE_FROM_DEFAULT
  DEPENDS
"""
            ],
        ),
        FilePatcher(
            orig_file=os.path.join(
                src_dir, "Modules", "Nonunit", "Review", "itk-module.cmake"
            ),
            from_texts=[
                """  ITKIOTIFF
  ITKIOVTK
""",
                """  ITKIOLSM
  DESCRIPTION
""",
            ],
            to_texts=[
                """  ITKIOVTK
""",
                """  DESCRIPTION
""",
            ],
        ),
        FilePatcher(
            orig_file=os.path.join(
                src_dir, "Modules", "Nonunit", "Review", "src", "CMakeLists.txt"
            ),
            from_texts=[
                """    ${ITKTestKernel_LIBRARIES}
    ${ITKIOLSM_LIBRARIES}
    itkopenjpeg
"""
            ],
            to_texts=[
                """    itkopenjpeg
"""
            ],
        ),
    ]
    patch_manager = PatchManager(patches)

    try:
        patch_manager.apply_patches()

        cmakecmd = get_cmake_cmd_common_part(install_dir, universal=True)
        cmakecmd.extend(
            [
                "-DBUILD_EXAMPLES:BOOL=OFF",
                "-DBUILD_TESTING:BOOL=OFF",
                "-DITK_USE_64BITS_IDS:BOOL=ON",
                "-DITK_FUTURE_LEGACY_REMOVE:BOOL=ON",
                "-DITK_LEGACY_REMOVE:BOOL=ON",
                "-DITK_USE_GPU:BOOL=OFF",
                "-DITK_DOXYGEN_HTML:BOOL=OFF",
                "-DModule_ITKReview:BOOL=ON",
                "-DModule_ITKTBB:BOOL=ON",
                "-DTBB_DIR:PATH=" + tbb_dir(),
                "-DITK_USE_SYSTEM_DOUBLECONVERSION:BOOL=ON",
                "-DITK_USE_SYSTEM_EIGEN:BOOL=ON",
                "-DITK_USE_SYSTEM_HDF5:BOOL=ON",
                "-DITK_USE_SYSTEM_JPEG:BOOL=ON",
                "-DITK_USE_SYSTEM_PNG:BOOL=ON",
                "-DITK_USE_SYSTEM_ZLIB:BOOL=ON",
                # '-DGDCM_USE_SYSTEM_OPENJPEG:BOOL=ON',
                "-DModule_MorphologicalContourInterpolation=OFF",  # example how to turn on a remote module
            ],
        )

        if is_windows():
            cmakecmd.extend(
                [
                    # '-DZLIB_INCLUDE_DIR:PATH=' + ext_dir() + '\\zlib\\include',
                    # '-DZLIB_LIBRARY_RELEASE:FILEPATH=' + ext_dir() + '\\zlib\\lib\\zlibstatic.lib',
                    "-DHDF5_DIR:PATH=" + ext_build_dir() + "/share/cmake",
                ]
            )
        # else:
        #     cmakecmd.extend(['-DHDF5_DIR:PATH=' + ext_dir() + '/hdf5/share/cmake/hdf5',
        #                      ])

        cmakecmd.extend([src_dir])
        build_and_install_cmakecmd(cmakecmd, build_dir)

        # duplicated call to find_package cause cmake error
        # remove tbb from itk interface to make it work with conda tbb
        patch_file(
            os.path.join(
                install_dir, "lib", "cmake", "ITK-6.0", "Modules", "ITKTBB.cmake"
            ),
            from_texts=[
                r"find_package(TBB REQUIRED CONFIG)",
                r"set(ITKTBB_INCLUDE_DIRS",
                r"set(ITKTBB_LIBRARIES",
                r"set(TBB_DIR",
            ],
            to_texts=[
                r"#find_package(TBB REQUIRED CONFIG)",
                r"#set(ITKTBB_INCLUDE_DIRS",
                r"#set(ITKTBB_LIBRARIES",
                r"#set(TBB_DIR",
            ],
        )
    finally:
        shutil.rmtree(build_dir, ignore_errors=False)
        patch_manager.restore_files()


def build_vtk(src_dir: str, install_dir: str):
    build_dir = create_build_dir(src_dir)

    patches = [
        FilePatcher(
            orig_file=os.path.join(
                src_dir, "ThirdParty", "eigen", "vtkeigen", "CMakeLists.txt"
            ),
            from_texts=[
                r"-std=c++11",
                r"set(CMAKE_CXX_STANDARD 11)",
            ],
            to_texts=[
                f"-std=c++{cpp_standard()}",
                f"set(CMAKE_CXX_STANDARD {cpp_standard()})",
            ],
        ),
        FilePatcher(
            orig_file=os.path.join(
                src_dir, "ThirdParty", "libproj", "vtklibproj", "CMakeLists.txt"
            ),
            from_texts=[
                r"set(CMAKE_CXX_STANDARD 11)",
                r"-Werror -Wall",
            ],
            to_texts=[
                f"set(CMAKE_CXX_STANDARD {cpp_standard()})",
                r"-Wall",
            ],
        ),
        FilePatcher(
            orig_file=os.path.join(
                src_dir,
                "ThirdParty",
                "netcdf",
                "vtknetcdf",
                "libdispatch",
                "dpathmgr.c",
            ),
            from_texts=[
                r"        if(_stat64(cvtpath,buf) < 0) {status = errno; goto done;}",
                r"        if(_wstat64(wpath,buf) < 0) {status = errno; goto done;}",
            ],
            to_texts=[
                r"        if(_stat64i32(cvtpath,(struct _stat64i32*)buf) < 0) {status = errno; goto done;}",
                r"        if(_wstat64i32(wpath,(struct _stat64i32*)buf) < 0) {status = errno; goto done;}",
            ],
            patch_condition=is_windows,
        ),
        FilePatcher(
            orig_file=os.path.join(
                src_dir,
                "ThirdParty",
                "netcdf",
                "vtknetcdf",
                "libdispatch",
                "dinfermodel.c",
            ),
            from_texts=[
                """#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif
"""
            ],
            to_texts=[
                """#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif
#ifdef _WIN32
#include <io.h>
#endif
"""
            ],
            patch_condition=is_windows,
        ),
        FilePatcher(
            orig_file=os.path.join(
                src_dir,
                "ThirdParty",
                "netcdf",
                "vtknetcdf",
                "libdispatch",
                "XGetopt.c",
            ),
            from_texts=[
                r"    char** p;",
            ],
            to_texts=[
                r"    const char* p;",
            ],
            patch_condition=is_windows,
        ),
        FilePatcher(
            orig_file=os.path.join(
                src_dir, "Common", "DataModel", "vtkBoundingBox.cxx"
            ),
            from_texts=[
                """template void vtkBoundingBox::ComputeBounds(
  vtkPoints* pts, int* ptIds, vtkIdType numberOfPointsIds, double bounds[6]);""",
                """template void vtkBoundingBox::ComputeBounds(
  vtkPoints* pts, long long* ptIds, vtkIdType numberOfPointsIds, double bounds[6]);""",
            ],
            to_texts=[
                """template <>
void vtkBoundingBox::ComputeBounds(
  vtkPoints* pts, int* ptIds, vtkIdType numberOfPointsIds, double bounds[6])
{
  vtkBoundingBox bbox;
  double point[3];
  for (vtkIdType i = 0; i < numberOfPointsIds; ++i)
  {
    pts->GetPoint(static_cast<vtkIdType>(ptIds[i]), point);
    bbox.AddPoint(point);
  }
  bbox.GetBounds(bounds);
}""",
                """template <>
void vtkBoundingBox::ComputeBounds(
  vtkPoints* pts, long long* ptIds, vtkIdType numberOfPointsIds, double bounds[6])
{
  vtkBoundingBox bbox;
  double point[3];
  for (vtkIdType i = 0; i < numberOfPointsIds; ++i)
  {
    pts->GetPoint(static_cast<vtkIdType>(ptIds[i]), point);
    bbox.AddPoint(point);
  }
  bbox.GetBounds(bounds);
}""",
            ],
            patch_condition=use_windows_clang_cl,
        ),
    ]
    patch_manager = PatchManager(patches)

    try:
        patch_manager.apply_patches()

        cmakecmd = get_cmake_cmd_common_part(install_dir, universal=True)

        vtk_smp_backend = "STDThread"
        vtk_use_external_hdf5 = not (is_windows() and not use_clang_cl())
        # OIIO's local static Expat on Windows uses the dynamic CRT archive name
        # libexpatMD.lib; VTK's static-name search only includes MT variants.
        vtk_expat_use_static_names = not is_windows()

        # Keep these cache args aligned with the current vendored VTK tree.
        # VTK_DATA_EXCLUDE_FROM_ALL only matters when VTK_BUILD_TESTING is ON,
        # VTK_LEGACY_REMOVE is now inert, and the old doubleconversion toggle
        # no longer exists in this VTK checkout.
        cmakecmd.extend(
            [
                "-DVTK_BUILD_EXAMPLES:BOOL=OFF",
                "-DBUILD_TESTING:BOOL=OFF",
                "-DBUILD_SHARED_LIBS:BOOL=OFF",
                "-DVTK_BUILD_TESTING:STRING=OFF",
                "-DBUILD_SHARED_LIBS:BOOL=OFF",
                # Keep VTK's broad default groups, but reject VTK::tiff. Modules
                # that merely WANT TIFF-backed image IO will be skipped by VTK's
                # dependency resolver instead of falling back to vendored libtiff.
                "-DVTK_MODULE_ENABLE_VTK_tiff:STRING=NO",
                "-DVTK_MODULE_USE_EXTERNAL_VTK_eigen:BOOL=ON",
                "-DVTK_MODULE_USE_EXTERNAL_VTK_expat:BOOL=ON",
                "-DEXPAT_USE_STATIC_LIBS:BOOL="
                + ("ON" if vtk_expat_use_static_names else "OFF"),
                "-DVTK_MODULE_USE_EXTERNAL_VTK_hdf5:BOOL="
                + ("ON" if vtk_use_external_hdf5 else "OFF"),
                "-DVTK_MODULE_USE_EXTERNAL_VTK_jpeg:BOOL=ON",
                "-DVTK_MODULE_USE_EXTERNAL_VTK_lz4:BOOL=ON",
                "-DVTK_MODULE_USE_EXTERNAL_VTK_lzma:BOOL=ON",
                "-DVTK_MODULE_USE_EXTERNAL_VTK_png:BOOL=ON",
                "-DVTK_MODULE_USE_EXTERNAL_VTK_zlib:BOOL=ON",
                "-DVTK_MODULE_ENABLE_VTK_IOADIOS2:STRING=NO",
                "-DVTK_MODULE_ENABLE_VTK_diy2:STRING=NO",
                "-DVTK_SMP_IMPLEMENTATION_TYPE:STRING=" + vtk_smp_backend,
                "-DVTK_WRAP_PYTHON:BOOL=OFF",
            ]
        )
        if vtk_use_external_hdf5:
            # Linux CI hit CMake FindHDF5's wrapper-probing fallback when
            # pkg-config made HDF5 install h5cc wrappers. Prefer static HDF5
            # when VTK uses our external static-only HDF5 build. VTK's vendored
            # FindHDF5 also still expects the pre-2.0 parallel-state variable.
            cmakecmd.extend(
                [
                    "-DHDF5_USE_STATIC_LIBRARIES:BOOL=ON",
                    "-DHDF5_ENABLE_PARALLEL:BOOL=OFF",
                ]
            )
        if vtk_smp_backend == "TBB":
            cmakecmd.append("-DTBB_DIR:PATH=" + tbb_dir())
        if is_mac():
            # VTK's PCH path is special on macOS universal builds: clang cannot
            # emit one fat precompiled header for both x86_64 and arm64, so CMake
            # splits the PCH build into per-arch compilations. That exposes Atlas'
            # universal tuning flags (-mavx and -mcpu=apple-m1) as invalid on the
            # individual x86_64/arm64 PCH invocations even though normal fat-object
            # compilation works for our other universal third-party libraries.
            cmakecmd.append("-DVTK_USE_PCH:BOOL=OFF")

        cmakecmd.extend([src_dir])
        build_and_install_cmakecmd(cmakecmd, build_dir, additional_env=get_tbb_env())
    finally:
        shutil.rmtree(build_dir, ignore_errors=False)
        patch_manager.restore_files()


def build_opencv(src_dir: str, src_contrib_dir: str, install_dir: str):
    build_dir = create_build_dir(src_dir)

    patches = [
        FilePatcher(
            orig_file=os.path.join(
                src_dir, "modules", "videoio", "src", "cap_msmf.cpp"
            ),
            from_texts=[
                r"#include <initguid.h>",
            ],
            to_texts=[
                "#include <initguid.h>\n#include <ks.h>\n",
            ],
        ),
        FilePatcher(
            orig_file=os.path.join(src_dir, "cmake", "OpenCVFindMKL.cmake"),
            from_texts=[
                r"macro(mkl_fail)",
                r"set(mkl_lib_find_paths ${MKL_LIB_FIND_PATHS} ${MKL_ROOT_DIR}/lib)",
            ],
            to_texts=[
                "set(CMAKE_FIND_LIBRARY_SUFFIXES .lib .a ${CMAKE_FIND_LIBRARY_SUFFIXES})\n"
                "macro(mkl_fail)\n",
                r"set(mkl_lib_find_paths ${MKL_LIB_FIND_PATHS} ${MKL_ROOT_DIR}/lib ${MKL_ROOT_DIR}/../tbb/lib ${MKL_ROOT_DIR}/../tbb/lib/intel64/gcc4.8 ${MKL_ROOT_DIR}/../tbb/lib/intel64/vc14)",
            ],
        ),
        FilePatcher(
            orig_file=os.path.join(src_dir, "modules", "calib3d", "CMakeLists.txt"),
            from_texts=[r"${LAPACK_LIBRARIES}"],
            to_texts=[r""],
        ),
        FilePatcher(
            orig_file=os.path.join(src_dir, "modules", "core", "CMakeLists.txt"),
            from_texts=[r"${LAPACK_LIBRARIES}"],
            to_texts=[r""],
        ),
    ]
    patch_manager = PatchManager(patches)

    try:
        patch_manager.apply_patches()

        def get_cmakecmd_options(arm64_build: bool = False):
            cmakecmd_options = [
                "-DOPENCV_SKIP_CMAKE_CXX_STANDARD:BOOL=ON",
                "-DHAVE_CXX11:BOOL=ON",
                "-DOPENCV_ENABLE_NONFREE:BOOL=OFF",
                "-DOPENCV_FORCE_3RDPARTY_BUILD:BOOL=OFF",
                "-DBUILD_ZLIB:BOOL=OFF",
                "-DBUILD_TIFF:BOOL=OFF",
                "-DBUILD_JASPER:BOOL=OFF",
                "-DBUILD_JPEG:BOOL=OFF",
                "-DBUILD_PNG:BOOL=OFF",
                "-DBUILD_OPENEXR:BOOL=ON",
                "-DBUILD_WEBP:BOOL=OFF",
                "-DBUILD_OPENJPEG:BOOL=OFF",
                "-DBUILD_PROTOBUF:BOOL=OFF",
                "-DWITH_1394:BOOL=OFF",
                "-DWITH_VTK:BOOL=OFF",
                "-DWITH_CUDA:BOOL=OFF",
                "-DWITH_EIGEN:BOOL=ON",
                "-DWITH_FFMPEG:BOOL=ON",
                "-DWITH_GSTREAMER:BOOL=OFF",
                "-DWITH_JASPER:BOOL=OFF",
                "-DWITH_OPENJPEG:BOOL=ON",
                "-DWITH_JPEG:BOOL=ON",
                "-DWITH_WEBP:BOOL=ON",
                "-DWITH_OPENEXR:BOOL=ON",
                "-DWITH_PNG:BOOL=ON",
                "-DWITH_TBB:BOOL=ON",
                "-DWITH_TIFF:BOOL=OFF",
                "-DWITH_OPENCL:BOOL=OFF",
                "-DWITH_OPENCL_SVM:BOOL=OFF",
                "-DWITH_OPENCLAMDFFT:BOOL=OFF",
                "-DWITH_OPENCLAMDBLAS:BOOL=OFF",
                "-DWITH_LAPACK:BOOL=ON",
                # '-DENABLE_NEON:BOOL=' + ('ON' if arm64_build else 'OFF'),
                "-DWITH_IPP:BOOL=" + ("OFF" if arm64_build else "ON"),
                "-DWITH_MKL:BOOL=" + ("OFF" if arm64_build else "ON"),
                "-DMKL_WITH_TBB:BOOL="
                + ("OFF" if is_windows() or arm64_build else "ON"),
                # mkl_tbb link with static run lib (/MT)
                "-DMKL_WITH_OPENMP:BOOL=OFF",
                "-DWITH_PROTOBUF:BOOL=ON",
                "-DWITH_QUIRC:BOOL=OFF",
                "-DBUILD_SHARED_LIBS:BOOL=OFF",
                "-DBUILD_opencv_apps:BOOL=OFF",
                "-DBUILD_opencv_js:BOOL=OFF",
                "-DBUILD_DOCS:BOOL=OFF",
                "-DBUILD_EXAMPLES:BOOL=OFF",
                "-DBUILD_PACKAGE:BOOL=OFF",
                "-DBUILD_PERF_TESTS:BOOL=OFF",
                "-DBUILD_TESTS:BOOL=OFF",
                "-DBUILD_WITH_DEBUG_INFO:BOOL=OFF",
                "-DBUILD_FAT_JAVA_LIB:BOOL=OFF",
                "-DBUILD_JAVA:BOOL=OFF",
                "-DENABLE_PRECOMPILED_HEADERS:BOOL=OFF",
                "-DBUILD_opencv_video:BOOL=ON",
                "-DBUILD_opencv_videoio:BOOL=ON",
                "-DBUILD_opencv_ts:BOOL=OFF",
                "-DBUILD_opencv_dnn:BOOL=OFF",
                "-DBUILD_opencv_world:BOOL=OFF",
                "-DBUILD_opencv_python2:BOOL=OFF",
                "-DBUILD_opencv_python3:BOOL=OFF",
                "-DPYTHON3_EXECUTABLE=" + sys.executable,
                "-DBUILD_opencv_java:BOOL=OFF",
                "-DBUILD_opencv_calib3d:BOOL=OFF",
                "-DBUILD_opencv_stereo:BOOL=OFF",
                "-DBUILD_opencv_dnn_objdetect:BOOL=OFF",
                "-DBUILD_opencv_hdf:BOOL=OFF",
                "-DBUILD_opencv_matlab:BOOL=OFF",
                "-DBUILD_opencv_sfm:BOOL=OFF",
                "-DBUILD_opencv_videostab:BOOL=ON",
                "-DBUILD_opencv_xfeatures2d:BOOL=ON",
                "-DBUILD_opencv_freetype:BOOL=OFF",
            ]

            cmakecmd_options.extend(
                [
                    "-DTBB_DIR:PATH=" + tbb_dir(),
                ]
            )
            if not arm64_build:
                cmakecmd_options.extend(
                    [
                        "-DMKL_ROOT_DIR=" + mkl_dir(),
                    ]
                )

            if is_windows():
                cmakecmd_options.extend(
                    [
                        "-DBUILD_WITH_STATIC_CRT:BOOL=OFF",
                        "-DWITH_WIN32UI:BOOL=OFF",
                        "-DOpenJPEG_DIR=" + ext_build_dir() + "\\lib\\openjpeg-2.4",
                        "-DOPENCV_EXTRA_MODULES_PATH:PATH="
                        + src_contrib_dir
                        + "\\modules",
                    ]
                )
            elif is_linux():
                cmakecmd_options.extend(
                    [
                        "-DWITH_V4L:BOOL=ON",
                        "-DWITH_PTHREADS_PF:BOOL=OFF",
                        "-DOPENCV_EXTRA_MODULES_PATH:PATH="
                        + src_contrib_dir
                        + "/modules",
                    ]
                )
            else:
                cmakecmd_options.extend(
                    [
                        "-DWITH_PTHREADS_PF:BOOL=OFF",
                        "-DOPENCV_EXTRA_MODULES_PATH:PATH="
                        + src_contrib_dir
                        + "/modules",
                    ]
                )

            return cmakecmd_options

        cmakecmd = get_cmake_cmd_common_part(install_dir)
        if is_windows():
            for idx, cmd in enumerate(cmakecmd):
                if cmd.startswith("-DCMAKE_CXX_FLAGS:"):
                    cmakecmd[idx] = cmd + " /DWIN32_LEAN_AND_MEAN"
        cmakecmd.extend(get_cmakecmd_options(arm64_build=False))
        # logger.debug(cmakecmd)
        cmakecmd.extend([src_dir])

        build_and_install_cmakecmd(cmakecmd, build_dir)

        if is_mac():
            build_dir = create_build_dir(src_dir)
            arm64_install_dir = create_arm64_install_dir(src_dir)
            try:
                cmakecmd = get_cmake_cmd_common_part(arm64_install_dir, arm64_only=True)
                cmakecmd.extend(get_cmakecmd_options(arm64_build=True))
                # logger.debug(cmakecmd)
                cmakecmd.extend([src_dir])
                build_and_install_cmakecmd(cmakecmd, build_dir)
                create_universal_binaries(arm64_install_dir, install_dir)
            finally:
                print()
                rm_tree(arm64_install_dir)

        if is_windows():
            orig_file_2 = os.path.join(
                install_dir,
                "x64",
                f"vc{windows_visual_studio_major_version()}",
                "staticlib",
                "OpenCVModules.cmake",
            )
        else:
            orig_file_2 = os.path.join(
                install_dir, "lib", "cmake", "opencv4", "OpenCVModules.cmake"
            )

        patch_file(
            orig_file_2,
            from_texts=[
                r";\$<LINK_ONLY:tbb>",
            ],
            to_texts=[
                r"",
            ],
        )
    finally:
        shutil.rmtree(build_dir, ignore_errors=False)
        patch_manager.restore_files()


def build_rocksdb(src_dir: str, install_dir: str):
    build_dir = create_build_dir(src_dir)

    patches = [
        FilePatcher(
            orig_file=os.path.join(src_dir, "CMakeLists.txt"),
            from_texts=[
                r""
                if is_linux()
                else r'set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--copy-dt-needed-entries")',
                r"find_package(TBB REQUIRED)",
                r"find_package(zstd REQUIRED)",
            ],
            to_texts=[
                r'#set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--copy-dt-needed-entries")',
                "find_package(TBB REQUIRED)\n"
                r"add_library(TBB::TBB ALIAS TBB::tbb)",
                "find_package(zstd REQUIRED)\n"
                r"add_library(zstd::zstd ALIAS zstd::libzstd_static)",
            ],
        ),
    ]
    patch_manager = PatchManager(patches)

    try:
        patch_manager.apply_patches()

        os.rename(
            os.path.join(src_dir, "cmake", "modules", "FindTBB.cmake"),
            os.path.join(src_dir, "cmake", "modules", "__FindTBB.cmake"),
        )
        os.rename(
            os.path.join(src_dir, "cmake", "modules", "Findzstd.cmake"),
            os.path.join(src_dir, "cmake", "modules", "__Findzstd.cmake"),
        )

        cmakecmd_options = [
            "-DWITH_SNAPPY:BOOL=ON",
            "-DWITH_LZ4:BOOL=ON",
            "-DWITH_ZSTD:BOOL=ON",
            "-DROCKSDB_BUILD_SHARED:BOOL=OFF",
            "-DROCKSDB_SKIP_THIRDPARTY:BOOL=ON",
            "-DWITH_GFLAGS:BOOL=ON",
            "-DWITH_TBB:BOOL=ON",
            "-DUSE_COROUTINES:BOOL=OFF",
            "-DUSE_FOLLY:BOOL=ON",
            "-DROCKSDB_INSTALL_ON_WINDOWS:BOOL=ON",
            "-DFAIL_ON_WARNINGS:BOOL=OFF",
        ]

        cmakecmd = get_cmake_cmd_common_part(install_dir)
        cmakecmd.extend(cmakecmd_options)
        cmakecmd.extend(
            [
                "-DPORTABLE=haswell",
            ]
        )
        cmakecmd.extend([src_dir])
        build_and_install_cmakecmd(cmakecmd, build_dir)

        if is_mac():
            build_dir = create_build_dir(src_dir)
            arm64_install_dir = create_arm64_install_dir(src_dir)
            try:
                cmakecmd = get_cmake_cmd_common_part(arm64_install_dir, arm64_only=True)
                cmakecmd.extend(cmakecmd_options)
                cmakecmd.extend(
                    [
                        "-DPORTABLE=1",
                    ]
                )
                cmakecmd.extend([src_dir])
                build_and_install_cmakecmd(cmakecmd, build_dir)
                create_universal_binaries(arm64_install_dir, install_dir)
            finally:
                rm_tree(arm64_install_dir)
    finally:
        shutil.rmtree(build_dir, ignore_errors=False, onexc=handleRemoveReadonly)
        patch_manager.restore_files()
        os.rename(
            os.path.join(src_dir, "cmake", "modules", "__FindTBB.cmake"),
            os.path.join(src_dir, "cmake", "modules", "FindTBB.cmake"),
        )
        os.rename(
            os.path.join(src_dir, "cmake", "modules", "__Findzstd.cmake"),
            os.path.join(src_dir, "cmake", "modules", "Findzstd.cmake"),
        )


def build_llfio(src_dir: str, install_dir: str):
    build_dir = create_build_dir(src_dir)

    try:
        cmakecmd = get_cmake_cmd_common_part(install_dir, universal=True)
        cmakecmd.extend(
            [
                "-DLLFIO_FORCE_NETWORKING_OFF:BOOL=ON",
                # '-DLLFIO_FORCE_COROUTINES_OFF:BOOL=ON',
                # '-DLLFIO_FORCE_CONCEPTS_OFF:BOOL=ON',
                "-DLLFIO_FORCE_OPENSSL_OFF:BOOL=ON",
            ]
        )

        cmakecmd.extend([src_dir])
        build_and_install_cmakecmd(cmakecmd, build_dir, ninja_para="install.sl")
        if os.path.exists(os.path.join(build_dir, "install")):
            shutil.copytree(
                os.path.join(build_dir, "install"),
                os.path.join(ext_build_dir()),
                dirs_exist_ok=True,
            )
    finally:
        shutil.rmtree(build_dir, ignore_errors=False, onexc=handleRemoveReadonly)


def build_jansson(src_dir: str, install_dir: str):
    build_dir = create_build_dir(src_dir)

    try:
        cmakecmd = get_cmake_cmd_common_part(install_dir, universal=True)
        cmakecmd.extend(
            [
                "-DJANSSON_STATIC_CRT:BOOL=OFF",
                "-DJANSSON_EXAMPLES:BOOL=OFF",
                "-DCMAKE_POLICY_VERSION_MINIMUM=3.5",
            ]
        )

        cmakecmd.extend([src_dir])
        build_and_install_cmakecmd(cmakecmd, build_dir)
    finally:
        shutil.rmtree(build_dir, ignore_errors=False)


def build_pcre(src_dir: str, install_dir: str):
    build_dir = create_build_dir(src_dir)

    try:
        cmakecmd = get_cmake_cmd_common_part(install_dir, universal=True)
        cmakecmd.extend(
            [
                "-DBUILD_STATIC_LIBS:BOOL=ON",
            ]
        )

        cmakecmd.extend([src_dir])
        build_and_install_cmakecmd(cmakecmd, build_dir)
    finally:
        shutil.rmtree(build_dir, ignore_errors=False)


def build_fizz(src_dir: str, install_dir: str):
    build_dir = create_build_dir(src_dir)

    patches = [
        FilePatcher(
            orig_file=os.path.join(src_dir, "CMakeLists.txt"),
            from_texts=[
                r"""find_package(Sodium REQUIRED)
set(FIZZ_HAVE_SODIUM ${Sodium_FOUND})""",
                r"    ${sodium_INCLUDE_DIR}",
            ],
            to_texts=[
                r"""if (WIN32)
  set(_ATLAS_SODIUM_INCLUDE "${CMAKE_INSTALL_PREFIX}/include")
  set(_ATLAS_SODIUM_LIBRARY "${CMAKE_INSTALL_PREFIX}/lib/libsodium.lib")
  if (NOT EXISTS "${_ATLAS_SODIUM_INCLUDE}/sodium.h")
    message(FATAL_ERROR "Expected Atlas-installed libsodium headers at ${_ATLAS_SODIUM_INCLUDE}")
  endif()
  if (NOT EXISTS "${_ATLAS_SODIUM_LIBRARY}")
    message(FATAL_ERROR "Expected Atlas-installed libsodium library at ${_ATLAS_SODIUM_LIBRARY}")
  endif()
  if (NOT TARGET sodium)
    add_library(sodium STATIC IMPORTED)
  endif()
  set_target_properties(sodium PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${_ATLAS_SODIUM_INCLUDE}"
    INTERFACE_COMPILE_DEFINITIONS "SODIUM_STATIC"
    IMPORTED_LINK_INTERFACE_LANGUAGES "C"
    IMPORTED_LOCATION "${_ATLAS_SODIUM_LIBRARY}"
    IMPORTED_LOCATION_DEBUG "${_ATLAS_SODIUM_LIBRARY}"
    IMPORTED_LOCATION_RELEASE "${_ATLAS_SODIUM_LIBRARY}"
    IMPORTED_LOCATION_RELWITHDEBINFO "${_ATLAS_SODIUM_LIBRARY}"
    IMPORTED_LOCATION_MINSIZEREL "${_ATLAS_SODIUM_LIBRARY}"
  )
  set(Sodium_FOUND ON)
else()
  find_package(Sodium REQUIRED)
endif()
set(FIZZ_HAVE_SODIUM ${Sodium_FOUND})""",
                r"",
            ],
            patch_condition=use_windows_clang_cl,
        ),
        FilePatcher(
            orig_file=os.path.join(src_dir, "cmake", "fizz-config.cmake.in"),
            from_texts=[
                r"""set(FIZZ_HAVE_SODIUM "@FIZZ_HAVE_SODIUM@")

if (NOT TARGET fizz::fizz)""",
                r"find_dependency(Sodium)",
            ],
            to_texts=[
                r"""set(FIZZ_HAVE_SODIUM "@FIZZ_HAVE_SODIUM@")

if (FIZZ_HAVE_SODIUM AND NOT TARGET sodium)
  set(_ATLAS_SODIUM_INCLUDE "${PACKAGE_PREFIX_DIR}/include")
  if (WIN32)
    set(_ATLAS_SODIUM_LIBRARY "${PACKAGE_PREFIX_DIR}/lib/libsodium.lib")
  else()
    set(_ATLAS_SODIUM_LIBRARY "${PACKAGE_PREFIX_DIR}/lib/libsodium.a")
  endif()
  if (EXISTS "${_ATLAS_SODIUM_INCLUDE}/sodium.h" AND EXISTS "${_ATLAS_SODIUM_LIBRARY}")
    add_library(sodium STATIC IMPORTED)
    set_target_properties(sodium PROPERTIES
      INTERFACE_INCLUDE_DIRECTORIES "${_ATLAS_SODIUM_INCLUDE}"
      INTERFACE_COMPILE_DEFINITIONS "SODIUM_STATIC"
      IMPORTED_LINK_INTERFACE_LANGUAGES "C"
      IMPORTED_LOCATION "${_ATLAS_SODIUM_LIBRARY}"
      IMPORTED_LOCATION_DEBUG "${_ATLAS_SODIUM_LIBRARY}"
      IMPORTED_LOCATION_RELEASE "${_ATLAS_SODIUM_LIBRARY}"
      IMPORTED_LOCATION_RELWITHDEBINFO "${_ATLAS_SODIUM_LIBRARY}"
      IMPORTED_LOCATION_MINSIZEREL "${_ATLAS_SODIUM_LIBRARY}"
    )
  else()
    message(FATAL_ERROR "Expected Atlas-installed libsodium under ${PACKAGE_PREFIX_DIR}")
  endif()
endif()

if (NOT TARGET fizz::fizz)""",
                r"",
            ],
        ),
        # Lifetime safety:
        # If AsyncFizzClientT is destroyed while an underlying AsyncSocket connect
        # is still in-flight, Windows can still deliver the completion callback
        # and invoke `connectSuccess()` on a freed object (UAF).
        #
        # Fix this in two layers:
        # 1) Best-effort: cancel any in-flight connect from the destructor.
        # 2) Correctness: keep AsyncFizzClientT alive for the full duration of
        #    the underlying socket connect by holding a DelayedDestruction guard
        #    until connectSuccess/connectErr, and route every public close path
        #    through a shared helper that drops that keepalive before cancelling
        #    the socket connect.
        #
        # This is correctness-improving and should be safe on all platforms.
        FilePatcher(
            orig_file=os.path.join(src_dir, "client", "AsyncFizzClient.h"),
            from_texts=[
                r"""~AsyncFizzClientT() override = default;
  void writeAppData(""",
                r""" private:
  void deliverAllErrors(
      const folly::AsyncSocketException& ex,
      bool closeTransport = true);
  void deliverHandshakeError(folly::exception_wrapper ex);""",
                r"  folly::Optional<AsyncClientCallbackPtr> callback_;",
            ],
            to_texts=[
                r"""~AsyncFizzClientT() override {
    // Prevent the underlying AsyncSocket from invoking connect callbacks into a
    // dying object if the socket connect is still in-flight.
    if (auto* sock =
            transport_ ? transport_->getUnderlyingTransport<folly::AsyncSocket>()
                       : nullptr) {
      sock->cancelConnect();
    }
  }
  void writeAppData(""",
                r""" private:
  void cancelInFlightConnect();
  void deliverAllErrors(
      const folly::AsyncSocketException& ex,
      bool closeTransport = true);
  void deliverHandshakeError(folly::exception_wrapper ex);""",
                r"""  // Keep this alive while the underlying socket connect is in-flight.
  // Without this keepalive, the owning coroutine/task can drop its UniquePtr
  // while the OS still completes the connect, leading to connect callbacks
  // running on a freed object (observed on Windows). Public close paths clear
  // this via cancelInFlightConnect() so teardown does not depend on a later
  // connect callback to release it.
  DelayedDestruction::DestructorGuard connectGuard_{nullptr};

  folly::Optional<AsyncClientCallbackPtr> callback_;""",
            ],
        ),
        FilePatcher(
            orig_file=os.path.join(src_dir, "client", "AsyncFizzClient-inl.h"),
            from_texts=[
                r"""  if (underlyingSocket) {
    underlyingSocket->disableTransparentTls();
    underlyingSocket->connect(
        this,
        connectAddr,
        static_cast<int>(socketTimeout.count()),
        options,
        bindOptions);
  } else {""",
                r"""void AsyncFizzClientT<SM>::close() {
  if (transport_->good()) {
    fizzClient_.appCloseImmediate();
  } else {
    DelayedDestruction::DestructorGuard dg(this);
    folly::AsyncSocketException ase(
        folly::AsyncSocketException::END_OF_FILE, "socket closed locally");
    deliverAllErrors(ase, false);
    transport_->close();
  }
}""",
                r"""void AsyncFizzClientT<SM>::closeWithReset() {
  DelayedDestruction::DestructorGuard dg(this);
  if (transport_->good()) {""",
                r"""void AsyncFizzClientT<SM>::closeNow() {
  DelayedDestruction::DestructorGuard dg(this);
  if (transport_->good()) {""",
                r"""void AsyncFizzClientT<SM>::connectSuccess() noexcept {
  startTransportReads();

  folly::Optional<CachedPsk> cachedPsk = folly::none;
  if (pskIdentity_) {
    cachedPsk = fizzContext_->getPsk(*pskIdentity_);
  }
  fizzClient_.connect(
      fizzContext_,
      std::move(verifier_),
      sni_,
      std::move(cachedPsk),
      folly::Optional<std::vector<ech::ParsedECHConfig>>(folly::none),
      extensions_);
}""",
                r"""void AsyncFizzClientT<SM>::connectErr(
    const folly::AsyncSocketException& ex) noexcept {
  deliverAllErrors(ex, false);
}""",
            ],
            to_texts=[
                r"""  if (underlyingSocket) {
    underlyingSocket->disableTransparentTls();
    // Keep this alive until the socket connect completes (success/failure) or
    // we explicitly close/cancel.
    connectGuard_ = this;
    underlyingSocket->connect(
        this,
        connectAddr,
        static_cast<int>(socketTimeout.count()),
        options,
        bindOptions);
  } else {""",
                r"""void AsyncFizzClientT<SM>::cancelInFlightConnect() {
  DelayedDestruction::DestructorGuard dg(this);
  if (!connectGuard_) {
    return;
  }

  // Release the keepalive before cancelling the socket connect so callers can
  // continue teardown without waiting for a later connect callback.
  connectGuard_ = nullptr;

  if (auto* sock =
          transport_ ? transport_->getUnderlyingTransport<folly::AsyncSocket>()
                     : nullptr) {
    sock->cancelConnect();
  }
}

template <typename SM>
void AsyncFizzClientT<SM>::close() {
  DelayedDestruction::DestructorGuard dg(this);
  cancelInFlightConnect();
  if (transport_->good()) {
    fizzClient_.appCloseImmediate();
  } else {
    folly::AsyncSocketException ase(
        folly::AsyncSocketException::END_OF_FILE, "socket closed locally");
    deliverAllErrors(ase, false);
    transport_->close();
  }
}""",
                r"""void AsyncFizzClientT<SM>::closeWithReset() {
  DelayedDestruction::DestructorGuard dg(this);
  cancelInFlightConnect();
  if (transport_->good()) {""",
                r"""void AsyncFizzClientT<SM>::closeNow() {
  DelayedDestruction::DestructorGuard dg(this);
  cancelInFlightConnect();
  if (transport_->good()) {""",
                r"""void AsyncFizzClientT<SM>::connectSuccess() noexcept {
  DelayedDestruction::DestructorGuard dg(this);

  // Release the keepalive now that we're running inside the connect callback.
  connectGuard_ = nullptr;

  // If the owner requested destruction while connect was in-flight, don't
  // proceed with handshake or invoke callbacks that may no longer exist.
  if (getDestroyPending()) {
    cancelHandshakeTimeout();
    callback_ = folly::none;
    transport_->closeNow();
    return;
  }

  startTransportReads();

  folly::Optional<CachedPsk> cachedPsk = folly::none;
  if (pskIdentity_) {
    cachedPsk = fizzContext_->getPsk(*pskIdentity_);
  }
  fizzClient_.connect(
      fizzContext_,
      std::move(verifier_),
      sni_,
      std::move(cachedPsk),
      folly::Optional<std::vector<ech::ParsedECHConfig>>(folly::none),
      extensions_);
}""",
                r"""void AsyncFizzClientT<SM>::connectErr(
    const folly::AsyncSocketException& ex) noexcept {
  DelayedDestruction::DestructorGuard dg(this);

  connectGuard_ = nullptr;

  if (getDestroyPending()) {
    cancelHandshakeTimeout();
    callback_ = folly::none;
    return;
  }

  deliverAllErrors(ex, false);
}""",
            ],
        ),
    ]
    patch_manager = PatchManager(patches)

    try:
        patch_manager.apply_patches()

        cmakecmd = get_cmake_cmd_common_part(install_dir, universal=True)
        cmakecmd.extend(
            [
                "-DFIZZ_BUILD_AEGIS:BOOL=OFF",
                "-DBUILD_TESTS:BOOL=OFF",
                "-DBUILD_EXAMPLES:BOOL=OFF",
            ]
        )

        cmakecmd.extend([src_dir])
        build_and_install_cmakecmd(cmakecmd, build_dir)
    finally:
        shutil.rmtree(build_dir, ignore_errors=False)
        patch_manager.restore_files()


def build_mvfst(src_dir: str, install_dir: str):
    build_dir = create_build_dir(src_dir)

    try:
        cmakecmd = get_cmake_cmd_common_part(install_dir, universal=True)

        cmakecmd.extend([src_dir])
        build_and_install_cmakecmd(cmakecmd, build_dir)
    finally:
        shutil.rmtree(build_dir, ignore_errors=False)


def build_wangle(src_dir: str, install_dir: str):
    build_dir = create_build_dir(src_dir)

    try:
        cmakecmd = get_cmake_cmd_common_part(install_dir, universal=True)
        cmakecmd.extend(
            [
                "-DBUILD_TESTS:BOOL=OFF",
            ]
        )

        cmakecmd.extend([src_dir])
        build_and_install_cmakecmd(cmakecmd, build_dir)
    finally:
        shutil.rmtree(build_dir, ignore_errors=False)


def build_proxygen(src_dir: str, install_dir: str):
    build_dir = create_build_dir(src_dir)

    patches = [
        FilePatcher(
            orig_file=os.path.join(src_dir, "CMakeLists.txt"),
            from_texts=[
                r"find_program(PROXYGEN_PYTHON python3)",
                r"-Wextra",
                r"""find_package(Threads)
find_package(Cares REQUIRED)
find_package(Glog REQUIRED)""",
            ],
            to_texts=[
                r"find_program(PROXYGEN_PYTHON python)",
                r"",
                r"""find_package(Threads)
find_package(c-ares CONFIG REQUIRED)
if(NOT TARGET cares AND TARGET c-ares::cares)
  add_library(cares INTERFACE IMPORTED)
  set_target_properties(cares PROPERTIES INTERFACE_LINK_LIBRARIES "c-ares::cares")
endif()
find_package(Glog REQUIRED)""",
            ],
            patch_condition=is_windows,
        ),
        FilePatcher(
            orig_file=os.path.join(src_dir, "proxygen", "lib", "CMakeLists.txt"),
            from_texts=[
                r"""${PROXYGEN_FBCODE_ROOT}
        ${PROXYGEN_GENERATED_ROOT}/proxygen/lib/http""",
            ],
            to_texts=[
                "${PROXYGEN_FBCODE_ROOT}\n${PROXYGEN_GENERATED_ROOT}/proxygen/lib/http\n${PROXYGEN_GPERF}",
            ],
        ),
        FilePatcher(
            orig_file=os.path.join(src_dir, "cmake", "proxygen-config.cmake.in"),
            from_texts=[
                r"find_dependency(c-ares REQUIRED)",
            ],
            to_texts=[
                r"""find_dependency(c-ares REQUIRED)
if(NOT TARGET cares AND TARGET c-ares::cares)
  add_library(cares INTERFACE IMPORTED)
  set_target_properties(cares PROPERTIES INTERFACE_LINK_LIBRARIES "c-ares::cares")
endif()""",
            ],
        ),
        # Proxygen keeps reusable HQ server targets under samples/hq, but the
        # sample-only proxygen_hq_samples target should not build when Atlas
        # configures Proxygen with BUILD_SAMPLES=OFF.
        FilePatcher(
            orig_file=os.path.join(
                src_dir, "proxygen", "httpserver", "samples", "hq", "CMakeLists.txt"
            ),
            from_texts=[
                r"proxygen_add_library(proxygen_hq_samples",
                r"add_subdirectory(devious)",
            ],
            to_texts=[
                """if (BUILD_SAMPLES)
  proxygen_add_library(proxygen_hq_samples""",
                """  add_subdirectory(devious)
endif()""",
            ],
        ),
        # Cancellation / timeout safety:
        # `TimedBaton::wait()` can complete with `cancelled` / `timedout`. Without
        # explicitly cancelling the underlying connect/handshake, the platform
        # socket layer may still invoke completion callbacks into a destroyed
        # ConnectCB / coroutine frame (observed on Windows, but correctness is
        # cross-platform). Cancel the connect before propagating cancellation.
        FilePatcher(
            orig_file=os.path.join(
                src_dir,
                "proxygen",
                "lib",
                "http",
                "coro",
                "client",
                "HTTPCoroConnector.cpp",
            ),
            from_texts=[
                r"""  co_await cb.baton.wait();
  co_await folly::coro::co_safe_point;
  if (cb.exception) {
    co_yield co_error(*cb.exception);
  }
  if (connParams.congestionFlavor) {
    asyncSocket->setCongestionFlavor(*connParams.congestionFlavor);
  }""",
                r"""  co_await cb.baton.wait();
  co_await folly::coro::co_safe_point;
  if (cb.exception) {
    co_yield co_error(*cb.exception);
  }
  initTransportInfoFromFizz(tinfo, *fizzClient);""",
                r"""    co_await cb.baton.wait();
    co_await folly::coro::co_safe_point;
    if (cb.exception) {
      co_yield co_error(*cb.exception);
    }
    initTransportInfoFromSSLSocket(tinfo, *sslSock);""",
            ],
            to_texts=[
                r"""  auto batonStatus = co_await cb.baton.wait();
  if (batonStatus != TimedBaton::Status::signalled) {
    asyncSocket->cancelConnect();
    if (batonStatus == TimedBaton::Status::timedout) {
      co_yield co_error(folly::AsyncSocketException(
          folly::AsyncSocketException::TIMED_OUT, "Connect timed out"));
    }
    co_yield folly::coro::co_cancelled;
  }
  co_await folly::coro::co_safe_point;
  if (cb.exception) {
    co_yield co_error(*cb.exception);
  }
  if (connParams.congestionFlavor) {
    asyncSocket->setCongestionFlavor(*connParams.congestionFlavor);
  }""",
                r"""  auto batonStatus = co_await cb.baton.wait();
  if (batonStatus != TimedBaton::Status::signalled) {
    if (auto* sock = fizzClient->getUnderlyingTransport<folly::AsyncSocket>()) {
      sock->cancelConnect();
    }
    fizzClient->closeNow();
    if (batonStatus == TimedBaton::Status::timedout) {
      co_yield co_error(folly::AsyncSocketException(
          folly::AsyncSocketException::TIMED_OUT, "Connect timed out"));
    }
    co_yield folly::coro::co_cancelled;
  }
  co_await folly::coro::co_safe_point;
  if (cb.exception) {
    co_yield co_error(*cb.exception);
  }
  initTransportInfoFromFizz(tinfo, *fizzClient);""",
                r"""    auto batonStatus = co_await cb.baton.wait();
    if (batonStatus != TimedBaton::Status::signalled) {
      sslSock->cancelConnect();
      if (batonStatus == TimedBaton::Status::timedout) {
        co_yield co_error(folly::AsyncSocketException(
            folly::AsyncSocketException::TIMED_OUT, "Connect timed out"));
      }
      co_yield folly::coro::co_cancelled;
    }
    co_await folly::coro::co_safe_point;
    if (cb.exception) {
      co_yield co_error(*cb.exception);
    }
    initTransportInfoFromSSLSocket(tinfo, *sslSock);""",
            ],
        ),
        # CONNECT tunnels must retain a keepalive on the underlying proxy
        # session for the full tunnel lifetime, regardless of whether the
        # stream uniquely owns that session. Without this keepalive, pooled
        # proxied HTTPS can tear down the proxy session while the tunneled TLS
        # transport is still draining, which later crashes in
        # Proxygen/Folly coroutine teardown on Windows. Track teardown
        # ownership explicitly so destructor behavior is easy to reason about.
        FilePatcher(
            orig_file=os.path.join(
                src_dir,
                "proxygen",
                "lib",
                "http",
                "coro",
                "transport",
                "HTTPConnectStream.h",
            ),
            from_texts=[
                r"""  HTTPCoroSession* session_{nullptr};
  folly::EventBase* eventBase_;""",
            ],
            to_texts=[
                r"""  HTTPCoroSession* session_{nullptr};
  HTTPSessionContextPtr sessionCtx_;
  bool ownsSession_{false};
  folly::EventBase* eventBase_;""",
            ],
        ),
        FilePatcher(
            orig_file=os.path.join(
                src_dir,
                "proxygen",
                "lib",
                "http",
                "coro",
                "transport",
                "HTTPConnectStream.cpp",
            ),
            from_texts=[
                r"""    : session_(ownership == Ownership::Unique ? session : nullptr),
      eventBase_(session->getEventBase()),""",
                r"""  egressSource_->setHeapAllocated();
  if (session_) {
    session_->addLifecycleObserver(this);
  }
}""",
                r"""  if (session_) {
    session_->removeLifecycleObserver(this);
    session_->initiateDrain();
  }""",
            ],
            to_texts=[
                r"""    : session_(session),
      sessionCtx_(session->acquireKeepAlive()),
      ownsSession_(ownership == Ownership::Unique),
      eventBase_(session->getEventBase()),""",
                r"""  egressSource_->setHeapAllocated();
  session_->addLifecycleObserver(this);
}""",
                r"""  if (session_) {
    session_->removeLifecycleObserver(this);
    if (ownsSession_) {
      session_->initiateDrain();
    }
  }""",
            ],
        ),
        FilePatcher(
            orig_file=os.path.join(
                src_dir, "proxygen", "httpserver", "samples", "hq", "ConnIdLogger.h"
            ),
            from_texts=[
                r"const struct ::tm* tm_time,",
                r"tm_time);",
            ],
            to_texts=[
                r"const google::LogMessageTime& tm_time,",
                "&(tm_time.tm()));",
            ],
        ),
        FilePatcher(
            orig_file=os.path.join(src_dir, "cmake", "FindZstd.cmake"),
            from_texts=[
                r"""find_library(ZSTD_LIBRARIES
  NAMES zstd""",
                r'if("${ZSTD_LIBRARIES}" MATCHES ".*.a$")',
            ],
            to_texts=[
                r"""find_library(ZSTD_LIBRARIES
  NAMES zstd zstd_static""",
                r"if(TRUE)",
            ],
            patch_condition=is_windows,
        ),
        FilePatcher(
            orig_file=os.path.join(src_dir, "proxygen", "external", "CMakeLists.txt"),
            from_texts=[
                r'"-Wno-implicit-fallthrough"',
            ],
            to_texts=[
                r"",
            ],
            patch_condition=is_windows,
        ),
        FilePatcher(
            orig_file=os.path.join(src_dir, "proxygen", "lib", "CMakeLists.txt"),
            from_texts=[
                r"""    COMMAND
        ${CMAKE_CURRENT_SOURCE_DIR}/http/gen_HTTPCommonHeaders.sh""",
                r"""    COMMAND
        ${CMAKE_CURRENT_SOURCE_DIR}/stats/gen_StatsWrapper.sh""",
            ],
            to_texts=[
                r"""    COMMAND
        bash
        ${CMAKE_CURRENT_SOURCE_DIR}/http/gen_HTTPCommonHeaders.sh""",
                r"""    COMMAND
        bash
        ${CMAKE_CURRENT_SOURCE_DIR}/stats/gen_StatsWrapper.sh""",
            ],
            patch_condition=is_windows,
        ),
        FilePatcher(
            orig_file=os.path.join(
                src_dir, "proxygen", "lib", "services", "RequestWorkerThread.cpp"
            ),
            from_texts=[
                r"sigset_t ss;",
                r"PCHECK(pthread_sigmask(SIG_BLOCK, &ss, nullptr) == 0);",
            ],
            to_texts=[
                "#ifndef _MSC_VER\nsigset_t ss;",
                "PCHECK(pthread_sigmask(SIG_BLOCK, &ss, nullptr) == 0);\n#endif",
            ],
            patch_condition=is_windows,
        ),
    ]
    patch_manager = PatchManager(patches)

    try:
        patch_manager.apply_patches()

        cmakecmd = get_cmake_cmd_common_part(install_dir, universal=True)
        if is_windows():
            cmakecmd.extend(
                [
                    f"-DCMAKE_PROGRAM_PATH={get_gperf_dir()};{os.path.dirname(sys.executable)}",
                    "-DBUILD_SAMPLES:BOOL=OFF",
                ]
            )
        else:
            cmakecmd.extend(
                [
                    f"-DCMAKE_PROGRAM_PATH={os.path.dirname(sys.executable)}",
                ]
            )

        cmakecmd.extend([src_dir])

        build_and_install_cmakecmd(cmakecmd, build_dir)
    finally:
        shutil.rmtree(build_dir, ignore_errors=False)
        patch_manager.restore_files()


def build_ospray(src_dir: str, install_dir: str):
    build_dir = create_build_dir(src_dir)

    try:
        cmakecmd = get_cmake_cmd_common_part(install_dir, use_ninja=False)
        cmakecmd.extend(
            [
                "-DDOWNLOAD_ISPC=ON",
                "-DDOWNLOAD_TBB=OFF",
                "-DBUILD_EMBREE_FROM_SOURCE=OFF",
                "-DBUILD_GLFW=ON",
                "-DBUILD_OIDN_FROM_SOURCE=OFF",
            ]
        )

        cmakecmd.extend([os.path.join(src_dir, "scripts", "superbuild")])
        build_and_install_cmakecmd(cmakecmd, build_dir, use_ninja=False, use_cmake=True)
    finally:
        shutil.rmtree(build_dir, ignore_errors=False)


def build_ants(src_dir: str, install_dir: str):
    build_dir = create_build_dir(src_dir)

    try:
        cmakecmd = get_cmake_cmd_common_part(install_dir, use_ninja=False)

        cmakecmd.extend([src_dir])
        # build_cmakecmd(cmakecmd, build_dir, use_ninja=False)
        subprocess.run(
            ["make", "install"],
            cwd=os.path.join(build_dir, "ANTS-build"),
            shell=False,
            check=True,
        )
    finally:
        shutil.rmtree(build_dir, ignore_errors=False)


def build_skia(src_dir: str, install_dir: str):
    try:
        subprocess.run(
            ["python", "tools/git-sync-deps"], cwd=src_dir, shell=False, check=True
        )
        subprocess.run(
            [
                "bin/gn",
                "gen",
                "out/Static",
                "--args=is_official_build=true skia_use_libjpeg_turbo_decode=false skia_use_libjpeg_turbo_encode=false skia_use_libpng_decode=false skia_use_libpng_encode=false skia_use_libwebp_decode=false skia_use_libwebp_encode=false skia_use_icu=false skia_use_harfbuzz=false skia_use_fontconfig=false skia_use_expat=false skia_use_freetype=false skia_use_gl=false skia_use_x11=false skia_enable_gpu=false",
            ],
            cwd=src_dir,
            shell=False,
            check=True,
        )
        subprocess.run(
            [get_ninja_binary(), "-C", "out/Static"],
            cwd=src_dir,
            shell=False,
            check=True,
        )
        skia_include_dir = os.path.join(install_dir, "include", "skia", "include")
        if os.path.exists(skia_include_dir):
            shutil.rmtree(skia_include_dir, ignore_errors=False)
        shutil.copytree(os.path.join(src_dir, "include"), skia_include_dir)
        skia_lib_dir = os.path.join(install_dir, "lib", "skia")
        if os.path.exists(skia_lib_dir):
            shutil.rmtree(skia_lib_dir, ignore_errors=False)
        glob_copy(os.path.join(src_dir, "out", "Static", "*.a"), skia_lib_dir)
    finally:
        logger.info("done")


def build_or_tools(src_dir: str, install_dir: str):
    build_dir = create_build_dir(src_dir)

    try:
        cmakecmd = get_cmake_cmd_common_part(install_dir, universal=True)
        cmakecmd.extend(
            [
                "-DBUILD_DEPS:BOOL=OFF",
                "-DBUILD_CoinUtils:BOOL=ON",
                "-DBUILD_Osi:BOOL=ON",
                "-DBUILD_Clp:BOOL=ON",
                "-DBUILD_Cgl:BOOL=ON",
                "-DBUILD_Cbc:BOOL=ON",
                "-DBUILD_HIGHS:BOOL=ON",
                "-DBUILD_SCIP:BOOL=ON",
                "-DBUILD_SHARED_LIBS:BOOL=OFF",
                "-DBUILD_TESTING:BOOL=OFF",
            ]
        )

        cmakecmd.extend([src_dir])
        build_and_install_cmakecmd(cmakecmd, build_dir)
    finally:
        shutil.rmtree(build_dir, ignore_errors=False)


def build_libs(libs: OrderedDict, use_asan: bool):
    logger.info(f"extDIR: {ext_dir()}")

    download_atlas_deps()
    if is_windows() or is_linux():
        install_oneapi_pip()
    logger.info(f"srcPackageDIR: {src_package_dir()}")

    remove_path_contains("miniconda")
    remove_path_contains("anaconda")
    logger.info(f"PATH: {os.environ['PATH']}")

    if is_windows():
        os.environ["HOME"] = os.path.expanduser("~")

    logger.info(f"HOME: {os.environ['HOME']}")

    for lib_name, build_lib in libs.items():
        if not build_lib:
            continue

        if lib_name == "cmake":
            install_cmake()

        if lib_name == "ninja":
            install_ninja()

        if lib_name == "curl":
            if is_windows():
                ensure_windows_curl_sdk()

        if lib_name == "gperf":
            if is_windows():
                install_gperf()

        if lib_name == "ffmpeg":
            install_ffmpeg()
            if is_windows() or is_linux():
                shutil.copy2(get_ffmpeg_binary(), ext_build_dir())

        if lib_name == "java":
            shutil.rmtree(os.path.join(ext_build_dir(), "jars"), ignore_errors=True)
            shutil.copytree(
                os.path.join(src_package_dir(), "jars"),
                os.path.join(ext_build_dir(), "jars"),
                dirs_exist_ok=True,
            )

            if is_mac():
                package_name = find_src_package_with_glob(
                    os.path.join(src_package_dir(), "*-jre_x64*mac*")
                )
            elif is_linux():
                package_name = find_src_package_with_glob(
                    os.path.join(src_package_dir(), "*-jre_x64*linux*")
                )
            else:
                package_name = find_src_package_with_glob(
                    os.path.join(src_package_dir(), "*-jre_x64*windows*")
                )
            jre_dir = os.path.join(
                ext_build_dir(), get_package_top_level_folder(package_name)
            )
            logger.info(jre_dir)
            if not os.path.exists(jre_dir):
                if os.path.exists(os.path.join(ext_build_dir(), "jre")):
                    os.remove(os.path.join(ext_build_dir(), "jre"))
                remove_old_src_folder_with_glob(os.path.join(ext_build_dir(), "jre*"))
                unpack_file_to_folder(package_name, ext_build_dir())
                assert os.path.exists(jre_dir)
                if os.path.lexists(os.path.join(ext_build_dir(), "jre")):
                    os.remove(os.path.join(ext_build_dir(), "jre"))
                    logger.info("link jre")
                    os.symlink(jre_dir, os.path.join(ext_build_dir(), "jre"))
            if not os.path.lexists(os.path.join(ext_build_dir(), "jre")):
                logger.info("link jre")
                os.symlink(jre_dir, os.path.join(ext_build_dir(), "jre"))

            if is_mac():
                package_name = find_src_package_with_glob(
                    os.path.join(src_package_dir(), "*-jre_aarch64*mac*")
                )
                if not os.path.lexists(os.path.join(ext_build_dir(), "jrearm")):
                    os.mkdir(os.path.join(ext_build_dir(), "jrearm"))
                jre_dir = os.path.join(
                    ext_build_dir(),
                    "jrearm",
                    get_package_top_level_folder(package_name),
                )
                logger.info(jre_dir)
                if not os.path.exists(jre_dir):
                    if os.path.exists(os.path.join(ext_build_dir(), "jre-arm")):
                        os.remove(os.path.join(ext_build_dir(), "jre-arm"))
                    unpack_file_to_folder(
                        package_name, os.path.join(ext_build_dir(), "jrearm")
                    )
                    assert os.path.exists(jre_dir)
                    if os.path.lexists(os.path.join(ext_build_dir(), "jre-arm")):
                        os.remove(os.path.join(ext_build_dir(), "jre-arm"))
                        logger.info("link jre-arm")
                        os.symlink(jre_dir, os.path.join(ext_build_dir(), "jre-arm"))
                if not os.path.lexists(os.path.join(ext_build_dir(), "jre-arm")):
                    logger.info("link jre-arm")
                    os.symlink(jre_dir, os.path.join(ext_build_dir(), "jre-arm"))

        if lib_name == "fast_float":
            src_dir = os.path.join(ext_dir(), "fast_float")
            build_fast_float(src_dir, ext_build_dir())

        if lib_name == "zlib":
            package_name = find_src_package_with_glob(
                os.path.join(src_package_dir(), "zlib*")
            )
            src_dir = os.path.join(
                ext_dir(), get_package_top_level_folder(package_name)
            )
            if not os.path.exists(src_dir):
                remove_old_src_folder_with_glob(os.path.join(ext_dir(), "zlib*"))
                unpack_file_to_folder(package_name, ext_dir())
                assert os.path.exists(src_dir)
            build_zlib(src_dir, ext_build_dir())

        if lib_name == "boost":
            package_name = find_src_package_with_glob(
                os.path.join(src_package_dir(), "boost*")
            )
            src_dir = os.path.join(
                ext_dir(), get_package_top_level_folder(package_name)
            )
            if not os.path.exists(src_dir):
                remove_old_src_folder_with_glob(os.path.join(ext_dir(), "boost*"))
                clean_boost(ext_build_dir())
                unpack_file_to_folder(package_name, ext_dir())
                assert os.path.exists(src_dir)
            build_boost(src_dir, ext_build_dir())

        if lib_name == "tbb":
            if is_mac():
                src_dir = os.path.join(ext_dir(), "oneTBB")
                build_tbb(src_dir, ext_build_dir())

        if lib_name == "eigen":
            src_dir = os.path.join(ext_dir(), "eigen")
            build_eigen(src_dir, ext_build_dir())

        if lib_name == "pocketfft":
            src_dir = os.path.join(ext_dir(), "pocketfft")
            build_pocketfft(src_dir, ext_build_dir())

        if lib_name == "reflect":
            src_dir = os.path.join(ext_dir(), "reflect")
            build_reflect(src_dir, ext_build_dir())

        if lib_name == "simde":
            src_dir = os.path.join(ext_dir(), "simde")
            build_simde(src_dir, ext_build_dir())

        if lib_name == "glm":
            src_dir = os.path.join(ext_dir(), "glm")
            build_glm(src_dir, ext_build_dir())

        if lib_name == "googletest":
            logger.info("googletest")

        if lib_name == "cpuinfo":
            src_dir = os.path.join(ext_dir(), "cpuinfo")
            build_cpuinfo(src_dir, ext_build_dir())

        if lib_name == "gflags":
            gflags_src_dir = os.path.join(ext_dir(), "gflags")
            build_gflags(gflags_src_dir, ext_build_dir())

        if lib_name == "glog":
            src_dir = os.path.join(ext_dir(), "glog")
            build_glog(src_dir, ext_build_dir())

        if lib_name == "benchmark":
            src_dir = os.path.join(ext_dir(), "benchmark")
            build_benchmark(src_dir, ext_build_dir())

        if lib_name == "openssl":
            package_name = find_src_package_with_glob(
                os.path.join(src_package_dir(), "openssl*")
            )
            src_dir = os.path.join(
                ext_dir(), get_package_top_level_folder(package_name)
            )
            if not os.path.exists(src_dir):
                remove_old_src_folder_with_glob(os.path.join(ext_dir(), "openssl*"))
                unpack_file_to_folder(package_name, ext_dir())
                assert os.path.exists(src_dir)
            if is_windows():
                nasm_dir = unpack_tool_to_target_dir(
                    src_package_dir(), "nasm*win64*", "nasm-*"
                )
            else:
                nasm_dir = ""  # does not need
            build_openssl(src_dir, ext_build_dir(), nasm_dir=nasm_dir)

        if lib_name == "double-conversion":
            dc_src_dir = os.path.join(ext_dir(), "double-conversion")
            build_double_conversion(dc_src_dir, ext_build_dir())

        if lib_name == "lz4":
            lz4_src_dir = os.path.join(ext_dir(), "lz4")
            build_lz4(lz4_src_dir, ext_build_dir())

        # lzma
        if lib_name == "xz":
            xz_src_dir = os.path.join(ext_dir(), "xz")
            build_xz(xz_src_dir, ext_build_dir())

        if lib_name == "zstd":
            zstd_src_dir = os.path.join(ext_dir(), "zstd")
            build_zstd(zstd_src_dir, ext_build_dir())

        if lib_name == "brotli":
            package_name = find_src_package_with_glob(
                os.path.join(src_package_dir(), "brotli*")
            )
            src_dir = os.path.join(
                ext_dir(), get_package_top_level_folder(package_name)
            )
            if not os.path.exists(src_dir):
                remove_old_src_folder_with_glob(os.path.join(ext_dir(), "brotli*"))
                unpack_file_to_folder(package_name, ext_dir())
                assert os.path.exists(src_dir)
            build_brotli(src_dir, ext_build_dir())

        if lib_name == "fmt":
            fmt_src_dir = os.path.join(ext_dir(), "fmt")
            build_fmt(fmt_src_dir, ext_build_dir())

        if lib_name == "libevent":
            le_src_dir = os.path.join(ext_dir(), "libevent")
            build_libevent(le_src_dir, ext_build_dir())

        if lib_name == "snappy":
            snappy_src_dir = os.path.join(ext_dir(), "snappy")
            build_snappy(snappy_src_dir, ext_build_dir())

        if lib_name == "bzip2":
            bz2_src_dir = os.path.join(ext_dir(), "bzip2")
            build_bzip2(bz2_src_dir, ext_build_dir())

        if lib_name == "libsodium":
            package_name = find_src_package_with_glob(
                os.path.join(src_package_dir(), "libsodium*")
            )
            src_dir = os.path.join(
                ext_dir(), get_package_top_level_folder(package_name)
            )
            if not os.path.exists(src_dir):
                remove_old_src_folder_with_glob(os.path.join(ext_dir(), "libsodium*"))
                unpack_file_to_folder(package_name, ext_dir())
                assert os.path.exists(src_dir)
            build_libsodium(src_dir, ext_build_dir())

        if lib_name == "folly":
            src_dir = os.path.join(ext_dir(), "folly")
            build_folly(src_dir, ext_build_dir(), use_asan=use_asan)

        if lib_name == "suitesparse":
            # package_name = find_src_package_with_glob(os.path.join(src_package_dir(), 'SuiteSparse*'))
            # src_dir = get_package_top_level_folder(package_name, ext_dir())
            # if not os.path.exists(src_dir):
            #     remove_old_src_folder_with_glob(os.path.join(ext_dir(), 'SuiteSparse*'))
            #     unpack_file_to_folder(package_name, ext_dir())
            #     assert os.path.exists(src_dir)
            src_dir = os.path.join(ext_dir(), "SuiteSparse")
            build_suitesparse(src_dir, ext_build_dir())

        if lib_name == "ceres-solver":
            src_dir = os.path.join(ext_dir(), "ceres-solver")
            build_ceres_solver(src_dir, ext_build_dir())

        if lib_name == "grpc":
            src_dir = os.path.join(ext_dir(), "grpc")
            if is_windows():
                nasm_dir = unpack_tool_to_target_dir(
                    src_package_dir(), "nasm*win64*", "nasm-*"
                )
            else:
                nasm_dir = ""  # does not need
            build_grpc(src_dir, ext_build_dir(), nasm_dir=nasm_dir)

        if lib_name == "glbinding":
            src_dir = os.path.join(ext_dir(), "glbinding")
            build_glbinding(src_dir, ext_build_dir())

        if lib_name == "libjpeg":
            if is_windows():
                nasm_dir = unpack_tool_to_target_dir(
                    src_package_dir(), "nasm*win64*", "nasm-*"
                )
            elif is_mac():
                nasm_dir = unpack_tool_to_target_dir(
                    src_package_dir(), "nasm*macosx*", "nasm-*"
                )
                os.chown(os.path.join(nasm_dir, "nasm"), os.getuid(), os.getgid())
                os.chmod(
                    os.path.join(nasm_dir, "nasm"),
                    os.stat(os.path.join(nasm_dir, "nasm")).st_mode | stat.S_IXUSR,
                )
            else:
                nasm_dir = ""
            package_name = find_src_package_with_glob(
                os.path.join(src_package_dir(), "libjpeg*")
            )
            src_dir = os.path.join(
                ext_dir(), get_package_top_level_folder(package_name)
            )
            if not os.path.exists(src_dir):
                remove_old_src_folder_with_glob(os.path.join(ext_dir(), "libjpeg*"))
                unpack_file_to_folder(package_name, ext_dir())
                assert os.path.exists(src_dir)
            build_libjpeg(src_dir, ext_build_dir(), nasm_dir=nasm_dir)

        if lib_name == "libpng":
            package_name = find_src_package_with_glob(
                os.path.join(src_package_dir(), "libpng*")
            )
            src_dir = os.path.join(
                ext_dir(), get_package_top_level_folder(package_name)
            )
            if not os.path.exists(src_dir):
                remove_old_src_folder_with_glob(os.path.join(ext_dir(), "libpng*"))
                unpack_file_to_folder(package_name, ext_dir())
                assert os.path.exists(src_dir)
            build_libpng(src_dir, ext_build_dir())

        if lib_name == "openjpeg":
            package_name = find_src_package_with_glob(
                os.path.join(src_package_dir(), "openjpeg*")
            )
            src_dir = os.path.join(
                ext_dir(), get_package_top_level_folder(package_name)
            )
            if not os.path.exists(src_dir):
                remove_old_src_folder_with_glob(os.path.join(ext_dir(), "openjpeg*"))
                unpack_file_to_folder(package_name, ext_dir())
                assert os.path.exists(src_dir)
            build_openjpeg(src_dir, ext_build_dir())

        if lib_name == "libwebp":
            package_name = find_src_package_with_glob(
                os.path.join(src_package_dir(), "libwebp*")
            )
            src_dir = os.path.join(
                ext_dir(), get_package_top_level_folder(package_name)
            )
            if not os.path.exists(src_dir):
                remove_old_src_folder_with_glob(os.path.join(ext_dir(), "libwebp*"))
                unpack_file_to_folder(package_name, ext_dir())
                assert os.path.exists(src_dir)
            build_libwebp(src_dir, ext_build_dir())

        if lib_name == "giflib":
            package_name = find_src_package_with_glob(
                os.path.join(src_package_dir(), "giflib*")
            )
            src_dir = os.path.join(
                ext_dir(), get_package_top_level_folder(package_name)
            )
            if not os.path.exists(src_dir):
                remove_old_src_folder_with_glob(os.path.join(ext_dir(), "giflib*"))
                unpack_file_to_folder(package_name, ext_dir())
                assert os.path.exists(src_dir)
            build_giflib(src_dir, ext_build_dir())

        if lib_name == "highway":
            package_name = find_src_package_with_glob(
                os.path.join(src_package_dir(), "highway*")
            )
            src_dir = os.path.join(
                ext_dir(), get_package_top_level_folder(package_name)
            )
            if not os.path.exists(src_dir):
                remove_old_src_folder_with_glob(os.path.join(ext_dir(), "highway*"))
                unpack_file_to_folder(package_name, ext_dir())
                assert os.path.exists(src_dir)
            build_highway(src_dir, ext_build_dir())

        if lib_name == "libjxl":
            package_name = find_src_package_with_glob(
                os.path.join(src_package_dir(), "libjxl*")
            )
            skcms_package_name = find_src_package_with_glob(
                os.path.join(src_package_dir(), "skcms*")
            )
            src_dir = os.path.join(
                ext_dir(), get_package_top_level_folder(package_name)
            )
            if not os.path.exists(src_dir):
                remove_old_src_folder_with_glob(os.path.join(ext_dir(), "libjxl*"))
                unpack_file_to_folder(package_name, ext_dir())
                assert os.path.exists(src_dir)
            build_libjxl(src_dir, ext_build_dir(), skcms_package_name)

        if lib_name == "libtiff":
            src_dir = os.path.join(ext_dir(), "libtiff")
            build_libtiff(src_dir, ext_build_dir())

        if lib_name == "libraw":
            libraw_package_name = find_src_package_with_glob(
                os.path.join(src_package_dir(), "LibRaw-[0-9]*")
            )
            libraw_cmake_package_name = find_src_package_with_glob(
                os.path.join(src_package_dir(), "LibRaw-cmake*")
            )
            libraw_src_dir = os.path.join(
                ext_dir(), get_package_top_level_folder(libraw_package_name)
            )
            libraw_cmake_src_dir = os.path.join(
                ext_dir(), get_package_top_level_folder(libraw_cmake_package_name)
            )
            if not os.path.exists(libraw_src_dir) or not os.path.exists(
                libraw_cmake_src_dir
            ):
                remove_old_src_folders_with_glob(os.path.join(ext_dir(), "LibRaw*"))
                unpack_file_to_folder(libraw_package_name, ext_dir())
                unpack_file_to_folder(libraw_cmake_package_name, ext_dir())
                assert os.path.exists(libraw_src_dir)
                assert os.path.exists(libraw_cmake_src_dir)
            build_libraw(libraw_src_dir, libraw_cmake_src_dir, ext_build_dir())

        if lib_name == "openimageio":
            package_name = find_src_package_with_glob(
                os.path.join(src_package_dir(), "OpenImageIO*")
            )
            src_dir = os.path.join(
                ext_dir(), get_package_top_level_folder(package_name)
            )
            remove_old_src_folder_with_glob(os.path.join(ext_dir(), "OpenImageIO*"))
            unpack_file_to_folder(package_name, ext_dir())
            assert os.path.exists(src_dir)
            build_openimageio(src_dir, ext_build_dir())

        if lib_name == "jxrlib":
            src_dir = os.path.join(ext_dir(), "jxrlib")
            build_jxrlib(src_dir, ext_build_dir())

        if lib_name == "geometrictools":
            logger.info("geometrictools")

        if lib_name == "assimp":
            src_dir = os.path.join(ext_dir(), "assimp")
            build_assimp(src_dir, ext_build_dir())

        if lib_name == "hdf5":
            package_name = find_src_package_with_glob(
                os.path.join(src_package_dir(), "hdf5*")
            )
            src_dir = os.path.join(
                ext_dir(), get_package_top_level_folder(package_name)
            )
            if not os.path.exists(src_dir):
                remove_old_src_folder_with_glob(os.path.join(ext_dir(), "hdf5*"))
                unpack_file_to_folder(package_name, ext_dir())
                assert os.path.exists(src_dir)
            build_hdf5(src_dir, ext_build_dir())

        if lib_name == "itk":
            src_dir = os.path.join(ext_dir(), "ITK")
            build_itk(src_dir, ext_build_dir())

        if lib_name == "vtk":
            src_dir = os.path.join(ext_dir(), "vtk")
            build_vtk(src_dir, ext_build_dir())

        if lib_name == "opencv":
            src_dir = os.path.join(ext_dir(), "opencv")
            src_contrib_dir = os.path.join(ext_dir(), "opencv_contrib")
            build_opencv(src_dir, src_contrib_dir, ext_build_dir())

        if lib_name == "neuTube":
            if is_windows():
                suffix = "Windows"
            elif is_mac():
                suffix = "macOS"
            else:
                suffix = "Linux"
            shutil.copytree(
                os.path.join(src_package_dir(), "packages-" + suffix),
                os.path.join(ext_build_dir(), "packages-" + suffix),
                dirs_exist_ok=True,
            )
            if is_internal_dev_environment():
                package_name = find_src_package_with_glob(
                    os.path.join(src_package_dir(), "neurolabi-internal-src*")
                )
                source_dir = os.path.join(ext_dir(), "neurolabi")
                if not os.path.exists(os.path.join(source_dir, "c", "CMakeLists.txt")):
                    remove_old_src_folder_with_glob(
                        os.path.join(ext_dir(), "neurolabi")
                    )
                    unpack_file_to_folder(package_name, ext_dir())
                    assert os.path.exists(
                        os.path.join(source_dir, "c", "CMakeLists.txt")
                    )

        if lib_name == "rocksdb":
            src_dir = os.path.join(ext_dir(), "rocksdb")
            build_rocksdb(src_dir, ext_build_dir())

        if lib_name == "llfio":
            src_dir = os.path.join(ext_dir(), "llfio")
            build_llfio(src_dir, ext_build_dir())

        if lib_name == "jansson" and is_internal_dev_environment():
            package_name = find_src_package_with_glob(
                os.path.join(src_package_dir(), "jansson*")
            )
            src_dir = os.path.join(
                ext_dir(), get_package_top_level_folder(package_name)
            )
            if not os.path.exists(src_dir):
                remove_old_src_folder_with_glob(os.path.join(ext_dir(), "jansson*"))
                unpack_file_to_folder(package_name, ext_dir())
                assert os.path.exists(src_dir)
            build_jansson(src_dir, ext_build_dir())

        if lib_name == "pcre" and is_internal_dev_environment():
            if is_windows():
                package_name = find_src_package_with_glob(
                    os.path.join(src_package_dir(), "pcre2*")
                )
                src_dir = os.path.join(
                    ext_dir(), get_package_top_level_folder(package_name)
                )
                if not os.path.exists(src_dir):
                    remove_old_src_folder_with_glob(os.path.join(ext_dir(), "pcre2*"))
                    unpack_file_to_folder(package_name, ext_dir())
                    assert os.path.exists(src_dir)
                build_pcre(src_dir, ext_build_dir())

        if lib_name == "fizz":
            src_dir = os.path.join(ext_dir(), "fizz", "fizz")
            build_fizz(src_dir, ext_build_dir())

        if lib_name == "mvfst":
            src_dir = os.path.join(ext_dir(), "mvfst")
            build_mvfst(src_dir, ext_build_dir())

        if lib_name == "wangle":
            src_dir = os.path.join(ext_dir(), "wangle", "wangle")
            build_wangle(src_dir, ext_build_dir())

        if lib_name == "proxygen":
            src_dir = os.path.join(ext_dir(), "proxygen")
            build_proxygen(src_dir, ext_build_dir())

        if lib_name == "ospray":
            package_name = find_src_package_with_glob(
                os.path.join(src_package_dir(), "ospray*")
            )
            src_dir = os.path.join(
                ext_dir(), get_package_top_level_folder(package_name)
            )
            if not os.path.exists(src_dir):
                remove_old_src_folder_with_glob(os.path.join(ext_dir(), "ospray*"))
                unpack_file_to_folder(package_name, ext_dir())
                assert os.path.exists(src_dir)
            build_ospray(src_dir, ext_build_dir())

        if lib_name == "ants":
            src_dir = os.path.join(atlas_repository_dir(), "..", "ANTs")
            if not os.path.exists(src_dir):
                logger.info("no ANTs")
            else:
                build_ants(src_dir, os.path.join(ext_build_dir(), "ANTs"))

        if lib_name == "skia":
            src_dir = os.path.join(atlas_repository_dir(), "..", "skia")
            if not os.path.exists(src_dir):
                logger.info("no skia")
            else:
                build_skia(src_dir, ext_build_dir())

        if lib_name == "or-tools":
            src_dir = os.path.join(atlas_repository_dir(), "..", "or-tools")
            if not os.path.exists(src_dir):
                logger.info("no or-tools")
            else:
                build_or_tools(src_dir, ext_build_dir())


def parse_inputs(argv: list):
    lib_list = [
        "cmake",
        "ninja",
        "curl",
        "gperf",
        "ffmpeg",
        "java",
        "fast_float",
        "zlib",
        "boost",
        "tbb",
        "eigen",
        "pocketfft",
        "reflect",
        "simde",
        "glm",
        "googletest",
        "cpuinfo",
        "gflags",
        "glog",
        "benchmark",
        "openssl",
        "double-conversion",
        "lz4",
        "xz",
        "zstd",
        "brotli",
        "giflib",
        "highway",
        "fmt",
        "libevent",
        "snappy",
        "bzip2",
        "libsodium",
        "folly",
        "suitesparse",
        "grpc",
        "ceres-solver",
        "glbinding",
        "libjpeg",
        "libpng",
        "openjpeg",
        "libwebp",
        "libjxl",
        "libtiff",
        "libraw",
        "openimageio",
        "jxrlib",
        "geometrictools",
        "assimp",
        "hdf5",
        "itk",
        "vtk",
        "opencv",
        "neuTube",
        "rocksdb",
        "llfio",
        "jansson",
        "pcre",
        "fizz",
        "mvfst",
        "wangle",
        "proxygen",
        "ospray",
        "ants",
        "skia",
        "or-tools",
    ]
    libs: OrderedDict[str, bool] = OrderedDict([(lib, False) for lib in lib_list])

    # Historical builders retained for possible future reactivation, but these
    # libraries are intentionally disabled for both `all` and explicit requests.
    lib_skip_list = [
        "ospray",
        "ants",
        "skia",
        "geometrictools",
        "rocksdb",
        "llfio",
        "or-tools",
    ]

    libs_reverse_depends = {
        "eigen": ["opencv", "ceres-solver", "itk", "vtk"],
        "libpng": ["openimageio", "opencv", "itk", "vtk"],
        "libjpeg": ["libtiff", "libraw", "openimageio", "opencv", "itk", "vtk"],
        "zlib": [
            "libpng",
            "libtiff",
            "libraw",
            "openimageio",
            "assimp",
            "hdf5",
            "itk",
            "vtk",
            "opencv",
            "grpc",
            "folly",
            "proxygen",
        ],
        # Atlas still builds glog/gflags for Folly, but current Ceres uses
        # Abseil logging and this OpenCV configuration does not consume glog.
        "gflags": ["glog", "folly"],
        "glog": ["folly"],
        "benchmark": ["grpc"],
        "openssl": ["grpc", "folly"],
        "hdf5": ["itk", "vtk"],
        "suitesparse": ["ceres-solver"],
        "ceres-solver": ["opencv"],  # only if we need opencv sfm
        "boost": ["folly"],
        "libevent": ["folly"],
        "double-conversion": ["folly", "itk", "vtk"],
        "lz4": ["vtk", "folly", "rocksdb"],
        "xz": ["libtiff", "vtk", "folly"],
        "zstd": ["libtiff", "folly", "rocksdb"],
        "brotli": ["libjxl", "folly"],
        "giflib": ["openimageio"],
        "highway": ["libjxl"],
        "fmt": ["openimageio", "folly"],
        "openjpeg": ["openimageio", "opencv"],
        "libjxl": ["openimageio"],
        "libwebp": ["libtiff", "openimageio", "opencv"],
        "libtiff": ["openimageio"],
        "libraw": ["openimageio"],
        "snappy": ["folly", "rocksdb"],
        "bzip2": ["folly"],
        "libsodium": ["folly"],
        "folly": ["rocksdb", "proxygen", "wangle", "fizz", "mvfst"],
        "wangle": ["proxygen"],
        "mvfst": ["proxygen"],
        "gperf": ["proxygen"],
        "fizz": ["mvfst"],
    }

    logger.info(f"current interpreter: {sys.executable}")

    parser = argparse.ArgumentParser(
        epilog="""
Examples:

python build_ext_libs.py [all or libs...] [--exclude-libs] [libs...] [--start-from] [lib] [--use-asan]
""",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        "libs",
        nargs="+",
        choices=list(libs.keys()) + ["all"],
        help="all or a list of libs",
    )
    parser.add_argument(
        "--exclude-libs",
        nargs="+",
        choices=list(libs.keys()),
        help="a list of libs to exclude from building",
    )
    parser.add_argument(
        "--start-from",
        choices=list(libs.keys()),
        help="skip libs before the specified lib",
    )
    parser.add_argument(
        "--stop-before",
        choices=list(libs.keys()),
        help="stop building before the specified lib (exclusive)",
    )
    parser.add_argument("--use-asan", action="store_true", help="use sanitizers")

    # parse arguments from the provided argv instead of implicitly reading
    # sys.argv. This keeps programmatic callers deterministic.
    cli_args = list(argv[1:]) if argv else []
    args = parser.parse_args(args=cli_args if cli_args else ["--help"])

    # Track if user explicitly requested "all"; used to decide cleanup
    requested_all = any(lib.lower() == "all" for lib in args.libs)

    for lib in args.libs:
        if lib.lower() == "all":
            for vlib in libs:
                if vlib not in lib_skip_list:
                    libs[vlib] = True
        elif lib in lib_skip_list:
            logger.info(f"skipping disabled external library: {lib}")
        else:
            libs[lib] = True

    excluded_libs = set(args.exclude_libs or [])

    # Expand to transitive reverse dependents. A single pass is order-dependent
    # and can miss chains like fizz -> mvfst -> proxygen.
    pending_libs = deque(
        lib for lib, enabled in libs.items() if enabled and lib not in excluded_libs
    )
    while pending_libs:
        lib = pending_libs.popleft()
        for dependent_lib in libs_reverse_depends.get(lib, []):
            if dependent_lib in lib_skip_list or dependent_lib in excluded_libs:
                continue
            if libs[dependent_lib]:
                continue
            libs[dependent_lib] = True
            pending_libs.append(dependent_lib)

    for lib in excluded_libs:
        libs[lib] = False

    if args.start_from is not None:
        started = False
        for lib in libs:
            if started:
                break
            started = args.start_from.lower() == lib.lower()
            if not started:
                libs[lib] = False

    if args.stop_before is not None:
        stopping = False
        for lib in libs:
            if not stopping and args.stop_before.lower() == lib.lower():
                stopping = True
            if stopping:
                libs[lib] = False

    for lib in lib_skip_list:
        libs[lib] = False

    build_all = True
    for lib in libs:
        if lib not in lib_skip_list:
            build_all = build_all and libs[lib]
    # Clean when building everything, or when the user explicitly asked for
    # "all" (even with exclusions). Avoid cleaning when using --start-from
    # because that flag implies continuing from an existing build.
    if build_all or (requested_all and args.start_from is None):
        shutil.rmtree(ext_build_dir(), ignore_errors=True)

    return libs, args.use_asan


if __name__ == "__main__":
    logger = setup_logger()

    build_libs(*parse_inputs(sys.argv))
