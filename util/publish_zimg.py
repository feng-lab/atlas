import argparse
import base64
import csv
import hashlib
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
from linuxdeployqt import linux_deploy_deps_to_lib_dir

_WHEEL_RECORD_HASH_CHUNK_SIZE = 1024 * 1024


def _run_checked(
    cmd: list[str],
    *,
    cwd: Path | None = None,
    env: dict[str, str] | None = None,
    display_cmd: list[str] | None = None,
) -> None:
    try:
        subprocess.run(cmd, cwd=str(cwd) if cwd is not None else None, env=env, check=True)
    except subprocess.CalledProcessError as e:
        display = display_cmd if display_cmd is not None else cmd
        raise RuntimeError(
            f"Command failed (exit={e.returncode}): {' '.join(display)}"
        ) from None


def _prepend_path(env: dict[str, str], dir_path: Path) -> None:
    existing = env.get("PATH", "")
    prefix = str(dir_path)
    if not existing:
        env["PATH"] = prefix
        return
    if existing.split(os.pathsep)[0] == prefix:
        return
    env["PATH"] = prefix + os.pathsep + existing


def _append_cmake_args(env: dict[str, str], extra_args: list[str]) -> None:
    if not extra_args:
        return
    existing = shlex.split(env.get("CMAKE_ARGS", ""))
    env["CMAKE_ARGS"] = shlex.join([*existing, *extra_args])


def _apply_scikit_build_core_toolchain_env(env: dict[str, str]) -> None:
    """
    Match the native toolchain selection used by `util/build_atlas.py` /
    `util/build_ext_libs.py` so zimg wheels build consistently across platforms.
    """

    if common_dirs.use_ninja():
        ninja_path = Path(common_dirs.get_ninja_binary())
        if not ninja_path.exists():
            raise RuntimeError(
                "Ninja was not found at the expected location. "
                f"Expected: {ninja_path}. "
                "Did you run `python3 util/build_ext_libs.py all` (or otherwise stage tools into src/3rdparty/build/)?"
            )

        _prepend_path(env, ninja_path.parent)
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

    if common_dirs.is_windows() and common_dirs.use_clang_cl():
        _append_cmake_args(env, ["-DCMAKE_C_COMPILER=clang-cl", "-DCMAKE_CXX_COMPILER=clang-cl"])


def _sha256_digest_for_wheel_record(path: Path) -> tuple[str, int]:
    digest = hashlib.sha256()
    size = 0
    with path.open("rb") as f:
        while True:
            chunk = f.read(_WHEEL_RECORD_HASH_CHUNK_SIZE)
            if not chunk:
                break
            digest.update(chunk)
            size += len(chunk)

    b64 = base64.urlsafe_b64encode(digest.digest()).rstrip(b"=").decode("ascii")
    return (b64, size)


def _rewrite_wheel_record(*, wheel_root: Path, dist_info_dir: Path) -> None:
    record_path = dist_info_dir / "RECORD"
    record_rel = record_path.relative_to(wheel_root).as_posix()

    rows: list[tuple[str, str, str]] = []
    for path in sorted(
        (p for p in wheel_root.rglob("*") if p.is_file()),
        key=lambda p: p.relative_to(wheel_root).as_posix(),
    ):
        rel = path.relative_to(wheel_root).as_posix()
        if rel == record_rel:
            continue
        digest_b64, size = _sha256_digest_for_wheel_record(path)
        rows.append((rel, f"sha256={digest_b64}", str(size)))

    rows.append((record_rel, "", ""))

    record_path.parent.mkdir(parents=True, exist_ok=True)
    with record_path.open("w", encoding="utf-8", newline="") as f:
        writer = csv.writer(f, lineterminator="\n")
        writer.writerows(rows)


def _repair_linux_wheel_in_place(wheel_path: Path) -> None:
    with tempfile.TemporaryDirectory(prefix="zimg_wheel_repair_") as tmp:
        wheel_root = Path(tmp)
        with zipfile.ZipFile(wheel_path, "r") as zf:
            zf.extractall(wheel_root)

        dist_info_dirs = [
            p for p in wheel_root.iterdir() if p.is_dir() and p.name.endswith(".dist-info")
        ]
        if len(dist_info_dirs) != 1:
            raise RuntimeError(
                f"Wheel repair expected 1 dist-info directory, got {len(dist_info_dirs)}: {wheel_path}"
            )
        dist_info_dir = dist_info_dirs[0]

        modules = sorted(wheel_root.glob("zimg/*imgpy*.so"))
        if not modules:
            raise RuntimeError(
                f"Wheel repair could not find zimg/*imgpy*.so inside: {wheel_path}"
            )

        lib_dir = wheel_root / "zimg" / "lib"
        for module_path in modules:
            linux_deploy_deps_to_lib_dir(str(module_path), lib_dir=str(lib_dir))

        _rewrite_wheel_record(wheel_root=wheel_root, dist_info_dir=dist_info_dir)

        tmp_wheel = wheel_path.with_name(f"{wheel_path.name}.tmp")
        if tmp_wheel.exists():
            tmp_wheel.unlink()

        with zipfile.ZipFile(tmp_wheel, "w", compression=zipfile.ZIP_DEFLATED) as zf:
            for path in sorted(
                (p for p in wheel_root.rglob("*") if p.is_file()),
                key=lambda p: p.relative_to(wheel_root).as_posix(),
            ):
                rel = path.relative_to(wheel_root).as_posix()
                zf.write(path, rel)

        os.replace(tmp_wheel, wheel_path)


def _stage_conda_zimg_from_wheel(*, wheel_path: Path, conda_source_dir: Path) -> Path:
    with tempfile.TemporaryDirectory(prefix="zimg_wheel_extract_") as tmp:
        wheel_root = Path(tmp)
        with zipfile.ZipFile(wheel_path, "r") as zf:
            zf.extractall(wheel_root)

        src_pkg_dir = wheel_root / "zimg"
        if not src_pkg_dir.is_dir():
            raise RuntimeError(f"Wheel is missing top-level zimg/ package dir: {wheel_path}")

        dst_pkg_dir = conda_source_dir / "zimg"
        if dst_pkg_dir.exists():
            shutil.rmtree(dst_pkg_dir)

        conda_source_dir.mkdir(parents=True, exist_ok=True)
        shutil.copytree(src_pkg_dir, dst_pkg_dir, symlinks=False)
        return dst_pkg_dir


def main() -> int:
    parser = argparse.ArgumentParser(
        description=("Build and publish the zimg wheel (PyPI) and conda package."),
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Print resolved version and commands without building/uploading.",
    )

    args = parser.parse_args()

    allowed_env_keys = {"PYPI_API_TOKEN", "ANACONDA_API_TOKEN"}
    if common_dirs.is_mac():
        allowed_env_keys.add("MACOS_CODESIGN_IDENTITY")
    atlas_env.load_repo_dotenv(allowed_keys=allowed_env_keys)

    repo_root = Path(common_dirs.atlas_repository_dir())
    py_project_src = repo_root / "python" / "zimg"

    zimg_src_dir = repo_root / "src" / "python"
    if not (zimg_src_dir / "CMakeLists.txt").exists():
        raise RuntimeError(f"Expected CMakeLists.txt missing: {zimg_src_dir}")

    raw_git_version = atlas_version.read_git_version_from_header()
    git_describe = atlas_version.git_describe_from_git_version(raw_git_version)
    version = atlas_pypi.pep440_version_from_git_describe(git_describe)
    print(f"Detected tag: {git_describe} -> zimg version: {version}")

    out_dir = repo_root / "python" / "zimg" / "dist"
    cmd = [sys.executable, "-m", "build", "--wheel", "--outdir", str(out_dir)]

    conda_token = os.environ.get("ANACONDA_API_TOKEN", "").strip()
    conda_source_dir: Path | None = None
    conda_cmd: list[str] | None = None
    conda_cmd_display: list[str] | None = None
    if conda_token:
        conda_source_dir = Path(common_dirs.ext_conda_build_dir())
        conda_cmd = [
            "conda-build",
            "--token",
            conda_token,
            "--user",
            "fenglab",
            "zimg-recipe",
        ]
        conda_cmd_display = ["conda-build", "--token", "$ANACONDA_API_TOKEN", "zimg-recipe"]

    if args.dry_run:
        print("Build command:", " ".join(cmd))
        if common_dirs.is_linux():
            print("Linux wheel repair: enabled (linuxdeployqt + RECORD rewrite)")
        if conda_token:
            assert conda_source_dir is not None
            assert conda_cmd_display is not None
            print(f"Conda stage dir: {conda_source_dir}/zimg")
            print("Conda build command:", " ".join(conda_cmd_display))
            print("Conda upload: enabled via conda-build")
        else:
            print("Conda: skipped (ANACONDA_API_TOKEN not set)")
        atlas_pypi.maybe_upload_to_pypi(
            out_dir,
            project="zimg",
            version=version,
            git_describe=git_describe,
            raw_git_version=raw_git_version,
            dry_run=True,
        )
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

        env = os.environ.copy()
        env["ZIMG_SRC_DIR"] = str(zimg_src_dir)
        if common_dirs.is_linux() or common_dirs.is_windows():
            _apply_scikit_build_core_toolchain_env(env)

        _run_checked(cmd, cwd=tmp_project, env=env)

    wheels = sorted(out_dir.glob("*.whl"))
    if wheels:
        if common_dirs.is_linux():
            for wheel_path in wheels:
                print(f"Repairing Linux wheel: {wheel_path.name}")
                _repair_linux_wheel_in_place(wheel_path)
        for wheel in wheels:
            print(f"Built: {wheel}")
    else:
        print(f"No wheels found in: {out_dir}")

    if not wheels:
        raise RuntimeError("Wheel build did not produce any artifacts.")

    if conda_token:
        assert conda_cmd is not None
        assert conda_cmd_display is not None
        assert conda_source_dir is not None

        wheel_path = wheels[0]
        print(f"Staging conda zimg package from wheel: {wheel_path.name}")
        staged_dir = _stage_conda_zimg_from_wheel(
            wheel_path=wheel_path, conda_source_dir=conda_source_dir
        )
        print(f"Staged conda package dir: {staged_dir}")
        _run_checked(conda_cmd, cwd=repo_root, display_cmd=conda_cmd_display)
    else:
        print("Skipping conda build/upload (ANACONDA_API_TOKEN not set)")

    atlas_pypi.maybe_upload_to_pypi(
        out_dir,
        project="zimg",
        version=version,
        git_describe=git_describe,
        raw_git_version=raw_git_version,
        dry_run=False,
    )

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
