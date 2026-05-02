import argparse
import importlib.util
import logging
import os
import shlex
import shutil
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

import atlas_env
import atlas_pypi
import atlas_version
import build_ext_libs
import common_dirs
from logger import setup_logger

logger = logging.getLogger(__name__)


def _run_checked(
    cmd: list[str],
    *,
    cwd: Path | None = None,
    env: dict[str, str] | None = None,
    display_cmd: list[str] | None = None,
) -> None:
    try:
        subprocess.run(
            cmd, cwd=str(cwd) if cwd is not None else None, env=env, check=True
        )
    except subprocess.CalledProcessError as e:
        display = display_cmd if display_cmd is not None else cmd
        raise RuntimeError(
            f"Command failed (exit={e.returncode}): {' '.join(display)}"
        ) from None


def _prepend_search_path(env: dict[str, str], *, key: str, dir_path: Path) -> None:
    existing = env.get(key, "")
    prefix = str(dir_path)
    if not existing:
        env[key] = prefix
        return
    if existing.split(os.pathsep)[0] == prefix:
        return
    env[key] = prefix + os.pathsep + existing


def _append_cmake_args(env: dict[str, str], extra_args: list[str]) -> None:
    if not extra_args:
        return
    existing = shlex.split(env.get("CMAKE_ARGS", ""))
    env["CMAKE_ARGS"] = shlex.join([*existing, *extra_args])


def _macos_lipo_archs(path: Path) -> set[str] | None:
    proc = subprocess.run(
        ["lipo", "-archs", str(path)],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if proc.returncode != 0:
        return None
    archs = proc.stdout.strip().split()
    if not archs:
        return None
    return set(archs)


def _wheel_tags_from_wheel_metadata(wheel_path: Path) -> list[str]:
    with zipfile.ZipFile(wheel_path, "r") as zf:
        wheel_files = [n for n in zf.namelist() if n.endswith(".dist-info/WHEEL")]
        if len(wheel_files) != 1:
            raise RuntimeError(
                f"Expected exactly 1 *.dist-info/WHEEL file in {wheel_path}, got {len(wheel_files)}: {wheel_files}"
            )
        text = zf.read(wheel_files[0]).decode("utf-8", errors="replace")

    tags: list[str] = []
    for line in text.splitlines():
        if line.startswith("Tag:"):
            tag = line.split(":", 1)[1].strip()
            if tag:
                tags.append(tag)
    if not tags:
        raise RuntimeError(f"Wheel metadata contains no Tag entries: {wheel_path}")
    return tags


def _assert_macos_wheel_has_universal2_tag(
    *, wheel_path: Path, macos_target: str
) -> None:
    token = macos_target.strip()
    if not token:
        raise RuntimeError(
            f"Empty macOS target for wheel tag check (from {macos_target!r})"
        )
    token = token.replace(".", "_")
    expected_platform = f"macosx_{token}_universal2"

    tags = _wheel_tags_from_wheel_metadata(wheel_path)
    for tag in tags:
        parts = tag.split("-", 2)
        if len(parts) != 3:
            continue
        _pyver, _abi, platform = parts
        if platform == expected_platform:
            return

    raise RuntimeError(
        "macOS wheel does not have the expected universal2 platform tag.\n"
        f"wheel: {wheel_path.name}\n"
        f"expected: {expected_platform}\n"
        f"found tags: {tags}"
    )


_MACOS_UNIVERSAL2_ARCHS: frozenset[str] = frozenset({"arm64", "x86_64"})
_MACOS_DISALLOWED_MACHO_ARCHS: frozenset[str] = frozenset({"i386", "ppc", "ppc64"})
_MACOS_UNIVERSAL2_ARCHFLAGS = "-arch arm64 -arch x86_64"


def _assert_macos_wheel_is_universal2(*, wheel_root: Path, wheel_name: str) -> None:
    macho_files: list[Path] = []
    for path in wheel_root.rglob("*"):
        if not path.is_file():
            continue
        archs = _macos_lipo_archs(path)
        if archs is None:
            continue
        macho_files.append(path)

        disallowed = archs & _MACOS_DISALLOWED_MACHO_ARCHS
        if disallowed:
            raise RuntimeError(
                "macOS wheel contains unsupported architectures in a Mach-O binary.\n"
                f"wheel: {wheel_name}\n"
                f"path: {path.relative_to(wheel_root).as_posix()}\n"
                f"archs: {sorted(archs)}\n"
                f"disallowed: {sorted(disallowed)}"
            )
        if not _MACOS_UNIVERSAL2_ARCHS.issubset(archs):
            raise RuntimeError(
                "macOS wheel does not contain universal2 slices in a Mach-O binary.\n"
                f"wheel: {wheel_name}\n"
                f"path: {path.relative_to(wheel_root).as_posix()}\n"
                f"archs: {sorted(archs)}\n"
                f"required: {sorted(_MACOS_UNIVERSAL2_ARCHS)}"
            )

    module_path = wheel_root / "zimg" / "_imgpy.abi3.so"
    if not module_path.exists():
        raise RuntimeError(f"macOS wheel is missing zimg/_imgpy.abi3.so: {wheel_name}")

    if not macho_files:
        raise RuntimeError(
            f"macOS wheel contains no Mach-O binaries (unexpected): {wheel_name}"
        )


def _apply_scikit_build_core_toolchain_env(env: dict[str, str]) -> None:
    """
    Match the native toolchain selection used by `util/build_atlas.py` /
    `util/build_ext_libs.py` so zimg wheels build consistently across platforms.
    """

    cmake_path = Path(common_dirs.get_cmake_binary())
    env["PATH"] = str(cmake_path.parent) + os.pathsep + env["PATH"]

    if common_dirs.use_ninja():
        ninja_path = Path(common_dirs.get_ninja_binary())
        env["PATH"] = str(ninja_path.parent) + os.pathsep + env["PATH"]
        env["CMAKE_GENERATOR"] = "Ninja"
        _append_cmake_args(env, [f"-DCMAKE_MAKE_PROGRAM={ninja_path}"])

    if common_dirs.is_linux() and build_ext_libs.use_clang_in_linux():
        cc = build_ext_libs.get_clang_in_linux()
        cxx = build_ext_libs.get_clangplus_in_linux()
        env["CC"] = cc
        env["CXX"] = cxx

        if shutil.which(cc, path=env.get("PATH")) is None:
            raise RuntimeError(
                f"Expected clang compiler not found in PATH: {cc}. "
                "Install clang or set `ATLAS_CLANG_MAJOR` / `LLVM_VERSION` to a version available on PATH."
            )
        if shutil.which(cxx, path=env.get("PATH")) is None:
            raise RuntimeError(
                f"Expected clang compiler not found in PATH: {cxx}. "
                "Install clang++ or set `ATLAS_CLANG_MAJOR` / `LLVM_VERSION` to a version available on PATH."
            )

    if common_dirs.is_linux():
        oneapi_lib_dir = Path(common_dirs.tbb_redist_dir())
        _prepend_search_path(env, key="LD_LIBRARY_PATH", dir_path=oneapi_lib_dir)

    if common_dirs.is_windows() and common_dirs.use_clang_cl():
        env["PATH"] = common_dirs.llvm_bin_dir() + os.pathsep + env["PATH"]
        env["LLVMInstallDir"] = common_dirs.llvm_install_dir()
        env["LLVMToolsVersion"] = common_dirs.llvm_tools_version()
        _append_cmake_args(
            env,
            [
                "-DCMAKE_C_COMPILER:FILEPATH=" + common_dirs.clang_cl_binary(),
                "-DCMAKE_CXX_COMPILER:FILEPATH=" + common_dirs.clang_cl_binary(),
                "-DCMAKE_LINKER:FILEPATH=" + common_dirs.lld_link_binary(),
            ],
        )


def _assert_linux_wheel_contains_expected_libs(
    wheel_path: Path,
    *,
    expect_freeimage: bool,
) -> None:
    expected_exact_libs = {
        "libtbb.so.12",
        "libQt6Core.so.6",
    }

    with zipfile.ZipFile(wheel_path, "r") as zf:
        entries = set(zf.namelist())

    lib_dirs = ("zimg/lib/", "zimg.libs/")
    lib_names: set[str] = set()
    for entry in entries:
        if entry.endswith("/"):
            continue
        if not any(entry.startswith(prefix) for prefix in lib_dirs):
            continue
        lib_names.add(Path(entry).name)

    def _has_expected_lib(name: str) -> bool:
        if name in lib_names:
            return True
        # auditwheel may inject a hash into the SONAME to avoid collisions
        # (e.g. libQt6Core-<hash>.so.6).
        if ".so" not in name:
            return False
        prefix, suffix = name.split(".so", 1)
        if not prefix:
            return False
        needle = f"{prefix}-"
        return any(
            candidate.startswith(needle) and candidate.endswith(f".so{suffix}")
            for candidate in lib_names
        )

    missing = sorted(
        name for name in expected_exact_libs if not _has_expected_lib(name)
    )

    freeimage_present = any(
        name.startswith("libfreeimageplus.so") or name.startswith("libfreeimageplus-")
        for name in lib_names
    )
    if expect_freeimage:
        if not freeimage_present:
            missing.append("libfreeimageplus.so*")
    else:
        if freeimage_present:
            unexpected = sorted(
                name
                for name in lib_names
                if name.startswith("libfreeimageplus.so")
                or name.startswith("libfreeimageplus-")
            )
            raise RuntimeError(
                "Linux wheel contains FreeImage shared libraries, but FreeImage was disabled.\n"
                f"wheel: {wheel_path}\n"
                f"unexpected: {unexpected}"
            )

    if missing:
        raise RuntimeError(
            "Linux wheel is missing expected shared libraries. "
            "These are installed during the CMake install step so the extension can find them "
            "after the wheel is relocated into site-packages.\n"
            f"wheel: {wheel_path}\n"
            f"searched wheel dirs: {list(lib_dirs)}\n"
            f"missing: {missing}"
        )
    logger.info(
        "Linux wheel contains expected shared libraries (%d + FreeImage=%s)",
        len(expected_exact_libs),
        "on" if expect_freeimage else "off",
    )


def _assert_linux_wheel_has_pypi_compatible_platform_tag(*, wheel_path: Path) -> None:
    """
    PyPI rejects native Linux wheels tagged as `linux_x86_64` (and similar).
    We must upload `manylinux*` or `musllinux*` wheels.
    """

    tags = _wheel_tags_from_wheel_metadata(wheel_path)
    for tag in tags:
        parts = tag.split("-", 2)
        if len(parts) != 3:
            continue
        _pyver, _abi, platform = parts
        if platform.startswith("manylinux") or platform.startswith("musllinux"):
            return

    raise RuntimeError(
        "Linux wheel does not have a PyPI-compatible platform tag.\n"
        "PyPI rejects native Linux wheels tagged as `linux_x86_64`; upload a `manylinux*` or `musllinux*` wheel.\n"
        f"wheel: {wheel_path.name}\n"
        f"found tags: {tags}"
    )


def _repair_linux_wheel_with_auditwheel(*, wheel_path: Path, out_dir: Path) -> Path:
    auditwheel_spec = importlib.util.find_spec("auditwheel")
    if auditwheel_spec is None:
        raise RuntimeError(
            "auditwheel is required to publish Linux wheels to PyPI, but it is not installed.\n"
            "Install it in the active Python environment and retry.\n"
            "For example:\n"
            "  python -m pip install --upgrade auditwheel\n"
        )

    env = os.environ.copy()

    # auditwheel discovers and bundles non-manylinux shared-library dependencies.
    # When we vendor Qt into the wheel, its transitive deps (e.g. ICU) still live
    # in the Qt install prefix. Add Qt's lib dir to LD_LIBRARY_PATH so auditwheel
    # can locate and bundle those dependencies deterministically.
    qt_lib_dir = Path(common_dirs.qt_base_dir()) / "lib"
    if qt_lib_dir.is_dir():
        _prepend_search_path(env, key="LD_LIBRARY_PATH", dir_path=qt_lib_dir)
        logger.info(
            "auditwheel: prepending LD_LIBRARY_PATH with: %s", qt_lib_dir.as_posix()
        )
    else:
        logger.warning(
            "auditwheel: Qt lib dir not found; repair may fail (qt_lib_dir=%s)",
            qt_lib_dir.as_posix(),
        )

    try:
        oneapi_lib_dir = Path(common_dirs.tbb_redist_dir())
    except AssertionError:
        oneapi_lib_dir = None
    if oneapi_lib_dir is not None and oneapi_lib_dir.is_dir():
        _prepend_search_path(env, key="LD_LIBRARY_PATH", dir_path=oneapi_lib_dir)
        logger.info(
            "auditwheel: prepending LD_LIBRARY_PATH with: %s",
            oneapi_lib_dir.as_posix(),
        )

    with tempfile.TemporaryDirectory(prefix="zimg_auditwheel_") as tmp:
        wheelhouse = Path(tmp)
        cmd = [
            sys.executable,
            "-m",
            "auditwheel",
            "repair",
            "--strip",
            "--wheel-dir",
            str(wheelhouse),
            str(wheel_path),
        ]
        _run_checked(
            cmd,
            env=env,
            display_cmd=[
                "python",
                "-m",
                "auditwheel",
                "repair",
                "--strip",
                "--wheel-dir",
                "<tmp>",
                wheel_path.name,
            ],
        )

        repaired = sorted(wheelhouse.glob("*.whl"))
        if len(repaired) != 1:
            raise RuntimeError(
                "Expected auditwheel to produce exactly 1 repaired wheel.\n"
                f"input wheel: {wheel_path}\n"
                f"output dir: {wheelhouse}\n"
                f"found: {[p.name for p in repaired]}"
            )

        repaired_path = repaired[0]
        final_path = out_dir / repaired_path.name
        shutil.move(str(repaired_path), str(final_path))

    try:
        wheel_path.unlink()
    except FileNotFoundError:
        pass

    logger.info("Repaired Linux wheel with auditwheel: %s", final_path.name)
    return final_path


def _stage_conda_zimg_from_wheel(*, wheel_path: Path, conda_source_dir: Path) -> Path:
    with tempfile.TemporaryDirectory(prefix="zimg_wheel_extract_") as tmp:
        wheel_root = Path(tmp)
        with zipfile.ZipFile(wheel_path, "r") as zf:
            zf.extractall(wheel_root)

        src_pkg_dir = wheel_root / "zimg"
        if not src_pkg_dir.is_dir():
            raise RuntimeError(
                f"Wheel is missing top-level zimg/ package dir: {wheel_path}"
            )

        dst_pkg_dir = conda_source_dir / "zimg"
        if dst_pkg_dir.exists():
            shutil.rmtree(dst_pkg_dir)

        conda_source_dir.mkdir(parents=True, exist_ok=True)
        shutil.copytree(src_pkg_dir, dst_pkg_dir, symlinks=False)

        # auditwheel uses a sibling `<project>.libs/` directory for vendored
        # shared libraries; include it so the conda package matches the wheel.
        src_libs_dir = wheel_root / "zimg.libs"
        if src_libs_dir.is_dir():
            dst_libs_dir = conda_source_dir / "zimg.libs"
            if dst_libs_dir.exists():
                shutil.rmtree(dst_libs_dir)
            shutil.copytree(src_libs_dir, dst_libs_dir, symlinks=False)
        return dst_pkg_dir


def main() -> int:
    setup_logger()
    parser = argparse.ArgumentParser(
        description=("Build and publish the zimg wheel (PyPI) and conda package."),
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Print resolved version and commands without building/uploading.",
    )
    parser.add_argument(
        "--allow-non-tag-upload",
        dest="allow_non_tag_upload",
        action="store_true",
        help=(
            "Allow uploading non-tag versions (e.g. X.Y[.Z[...]].N where N is commits since the last tag) "
            "to PyPI (skips the clean-tag gate)."
        ),
    )
    parser.add_argument(
        "--disable-freeimage",
        dest="disable_freeimage",
        action="store_true",
        help="Build zimg without FreeImage support (passes -DZIMG_DISABLE_FREEIMAGE=ON).",
    )
    parser.add_argument(
        "--conda-upload",
        dest="conda_upload",
        choices=("auto", "always", "never"),
        default="auto",
        help=(
            "Control whether to build/upload the conda package. "
            "'auto' (default) uploads conda only for non-tag builds; "
            "'always' uploads conda whenever ANACONDA_API_TOKEN and conda-build are available; "
            "'never' skips conda entirely."
        ),
    )

    args = parser.parse_args()

    repo_root = Path(common_dirs.atlas_repository_dir())
    py_project_src = repo_root / "python" / "zimg"

    zimg_src_dir = repo_root / "src" / "python"
    if not (zimg_src_dir / "CMakeLists.txt").exists():
        raise RuntimeError(f"Expected CMakeLists.txt missing: {zimg_src_dir}")

    raw_git_version = atlas_version.read_git_version_from_header()
    git_describe = atlas_version.git_describe_from_git_version(raw_git_version)
    version = atlas_pypi.pep440_version_from_git_describe(git_describe)
    logger.info("Detected tag: %s -> zimg version: %s", git_describe, version)
    is_release_tag = atlas_pypi.is_clean_release_tag(git_describe)

    want_conda_upload = True
    conda_policy_skip_reason: str | None = None
    if args.conda_upload == "never":
        want_conda_upload = False
        conda_policy_skip_reason = "disabled via --conda-upload=never"
    elif args.conda_upload == "auto" and is_release_tag:
        want_conda_upload = False
        conda_policy_skip_reason = (
            "disabled for clean release tags (--conda-upload=auto)"
        )

    allowed_env_keys = {"PYPI_API_TOKEN"}
    if want_conda_upload:
        allowed_env_keys.add("ANACONDA_API_TOKEN")
    if common_dirs.is_mac():
        allowed_env_keys.add("MACOS_CODESIGN_IDENTITY")
    atlas_env.load_repo_dotenv(allowed_keys=allowed_env_keys)

    out_dir = repo_root / "python" / "zimg" / "dist"
    cmd = [sys.executable, "-m", "build", "--wheel", "--outdir", str(out_dir)]

    if args.dry_run:
        logger.info("Build command: %s", " ".join(cmd))
        logger.info(
            "FreeImage: %s", "disabled" if args.disable_freeimage else "enabled"
        )
        logger.info("Conda upload policy: %s", args.conda_upload)
        atlas_pypi.maybe_upload_to_pypi(
            out_dir,
            project="zimg",
            version=version,
            git_describe=git_describe,
            raw_git_version=raw_git_version,
            dry_run=True,
            allow_non_tag_upload=args.allow_non_tag_upload,
            skip_existing=args.allow_non_tag_upload,
        )
        if want_conda_upload:
            # Dry-run: print the command we'd run without checking tool/env availability.
            conda_cmd_display = [
                "conda-build",
                "--token",
                "$ANACONDA_API_TOKEN",
                "zimg-recipe",
            ]
            logger.info("Conda build command: %s", " ".join(conda_cmd_display))
            logger.info(
                "Conda upload: enabled by policy (requires ANACONDA_API_TOKEN and conda-build)"
            )
        else:
            assert conda_policy_skip_reason is not None
            logger.info("Conda: skipped (%s)", conda_policy_skip_reason)
        return 0

    atlas_pypi.ensure_empty_dir(out_dir)

    with tempfile.TemporaryDirectory(prefix="zimg_pypi_build_") as tmp:
        tmp_root = Path(tmp)
        tmp_project = tmp_root / "zimg"
        shutil.copytree(
            py_project_src,
            tmp_project,
            ignore=shutil.ignore_patterns("dist", "__pycache__", "*.pyc", "*.pyo"),
        )
        atlas_pypi.update_pyproject_version(tmp_project / "pyproject.toml", version)

        if common_dirs.is_windows():
            # `python -m build` (PEP 517) executes the backend in an isolated venv but
            # still inherits our process environment. When running under MSYS2 /
            # MinGW shells, `PATH` often contains GNU toolchains (e.g. `c++.exe`)
            # which causes CMake+Ninja to silently select the wrong compiler.
            #
            # Match `util/build_atlas.py`: enter an MSVC dev environment and scrub
            # MinGW from PATH so scikit-build-core configures with MSVC.
            env_base = build_ext_libs.get_vcvars_environment()
            # Build deterministically with MSVC. Some shells (MSYS2) predefine CC/CXX
            # to GNU toolchains; leaving them set can override CMake's compiler
            # selection even when the MSVC environment is active.
            env_base.pop("CC", None)
            env_base.pop("CXX", None)
            env_base.pop("CFLAGS", None)
            env_base.pop("CXXFLAGS", None)
            cl_path = shutil.which("cl", path=env_base.get("PATH"))
            if cl_path is None:
                raise RuntimeError(
                    "MSVC environment setup failed: `cl.exe` not found on PATH after vcvarsall.\n"
                    "This wheel must be built with MSVC (not MinGW). Ensure Visual Studio "
                    "is installed and `util/common_dirs.py:vs_install_dir()` points to it."
                )
            logger.info("Using MSVC toolchain for wheel build (cl=%s)", cl_path)
        else:
            env_base = os.environ.copy()
        env_base["ZIMG_SRC_DIR"] = str(zimg_src_dir)
        if args.disable_freeimage:
            _append_cmake_args(env_base, ["-DZIMG_DISABLE_FREEIMAGE=ON"])

        env = env_base.copy()
        if common_dirs.is_mac():
            env["MACOSX_DEPLOYMENT_TARGET"] = build_ext_libs.macos_min_version()
            env["ARCHFLAGS"] = _MACOS_UNIVERSAL2_ARCHFLAGS
        _apply_scikit_build_core_toolchain_env(env)
        _run_checked(cmd, cwd=tmp_project, env=env)

    wheels = sorted(out_dir.glob("*.whl"))
    if wheels:
        if common_dirs.is_linux():
            repaired: list[Path] = []
            for wheel_path in wheels:
                repaired.append(
                    _repair_linux_wheel_with_auditwheel(
                        wheel_path=wheel_path, out_dir=out_dir
                    )
                )
            wheels = repaired
            for wheel_path in wheels:
                _assert_linux_wheel_has_pypi_compatible_platform_tag(
                    wheel_path=wheel_path
                )

        if common_dirs.is_mac():
            for wheel_path in wheels:
                _assert_macos_wheel_has_universal2_tag(
                    wheel_path=wheel_path,
                    macos_target=build_ext_libs.macos_min_version(),
                )
                with tempfile.TemporaryDirectory(
                    prefix="zimg_macos_wheel_validate_"
                ) as validate_dir:
                    validate_root = Path(validate_dir)
                    with zipfile.ZipFile(wheel_path, "r") as zf:
                        zf.extractall(validate_root)
                    _assert_macos_wheel_is_universal2(
                        wheel_root=validate_root,
                        wheel_name=wheel_path.name,
                    )
        if common_dirs.is_linux():
            for wheel_path in wheels:
                _assert_linux_wheel_contains_expected_libs(
                    wheel_path, expect_freeimage=not args.disable_freeimage
                )
        for wheel in wheels:
            logger.info("Built: %s", wheel)
    else:
        logger.warning("No wheels found in: %s", out_dir)

    if not wheels:
        raise RuntimeError("Wheel build did not produce any artifacts.")

    atlas_pypi.maybe_upload_to_pypi(
        out_dir,
        project="zimg",
        version=version,
        git_describe=git_describe,
        raw_git_version=raw_git_version,
        dry_run=False,
        allow_non_tag_upload=args.allow_non_tag_upload,
        skip_existing=args.allow_non_tag_upload,
    )

    if want_conda_upload:
        conda_token = os.environ.get("ANACONDA_API_TOKEN", "").strip()
        conda_build_exe = shutil.which("conda-build")

        conda_source_dir: Path | None = None
        conda_cmd: list[str] | None = None
        conda_cmd_display: list[str] | None = None
        conda_skip_reason: str | None = None
        if not conda_token:
            conda_skip_reason = "ANACONDA_API_TOKEN not set"
        elif conda_build_exe is None:
            conda_skip_reason = "conda-build not found on PATH"
        else:
            conda_source_dir = Path(common_dirs.ext_conda_build_dir())
            conda_cmd = [
                "conda-build",
                "--token",
                conda_token,
                "--user",
                "fenglab",
                "zimg-recipe",
            ]
            conda_cmd_display = [
                "conda-build",
                "--token",
                "$ANACONDA_API_TOKEN",
                "zimg-recipe",
            ]

        if conda_skip_reason is None:
            assert conda_cmd is not None
            assert conda_source_dir is not None
            wheel_path = wheels[0]
            logger.info("Staging conda zimg package from wheel: %s", wheel_path.name)
            staged_dir = _stage_conda_zimg_from_wheel(
                wheel_path=wheel_path, conda_source_dir=conda_source_dir
            )
            logger.info("Staged conda package dir: %s", staged_dir)
            _run_checked(conda_cmd, cwd=repo_root, display_cmd=conda_cmd_display)
        else:
            logger.warning("Skipping conda build/upload (%s)", conda_skip_reason)
            if conda_skip_reason == "conda-build not found on PATH":
                logger.warning(
                    "To enable conda uploads, install `conda-build` and `anaconda-client` "
                    "(e.g. in a conda env) and ensure `conda-build` is on PATH."
                )
    else:
        assert conda_policy_skip_reason is not None
        logger.info("Skipping conda build/upload (%s)", conda_policy_skip_reason)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
