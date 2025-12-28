import argparse
import base64
import csv
import hashlib
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


def _macos_is_macho_binary(path: Path) -> bool:
    return _macos_lipo_archs(path) is not None


def _macos_update_wheel_tag(*, dist_info_dir: Path, macos_target: str) -> None:
    wheel_path = dist_info_dir / "WHEEL"
    text = wheel_path.read_text(encoding="utf-8")
    lines = text.splitlines()

    tags = [ln for ln in lines if ln.startswith("Tag:")]
    if not tags:
        raise RuntimeError(f"Missing Tag line in {wheel_path}")
    if len(tags) != 1:
        raise RuntimeError(f"Expected 1 Tag line in {wheel_path}, got {len(tags)}")

    tag = tags[0].split(":", 1)[1].strip()
    parts = tag.split("-", 2)
    if len(parts) != 3:
        raise RuntimeError(f"Unexpected Tag format in {wheel_path}: {tag!r}")
    pyver, abi, _platform = parts

    token = macos_target.strip()
    if not token:
        raise RuntimeError(f"Empty macOS target for wheel tag (from {macos_target!r})")
    token = token.replace(".", "_")
    new_tag = f"{pyver}-{abi}-macosx_{token}_universal2"

    new_lines = [ln for ln in lines if not ln.startswith("Tag:")]
    new_lines.append(f"Tag: {new_tag}")
    wheel_path.write_text("\n".join(new_lines) + "\n", encoding="utf-8")


def _build_single_wheel(
    *,
    project_dir: Path,
    dist_dir: Path,
    env: dict[str, str],
) -> Path:
    dist_dir.mkdir(parents=True, exist_ok=True)
    for entry in dist_dir.iterdir():
        if entry.is_dir():
            shutil.rmtree(entry)
        else:
            entry.unlink()

    cmd = [sys.executable, "-m", "build", "--wheel", "--outdir", str(dist_dir)]
    _run_checked(cmd, cwd=project_dir, env=env)

    wheels = sorted(dist_dir.glob("*.whl"))
    if len(wheels) != 1:
        raise RuntimeError(f"Expected exactly 1 wheel in {dist_dir}, got {len(wheels)}: {wheels}")
    return wheels[0]


def _fuse_macos_universal2_wheels(
    *,
    wheel_x86: Path,
    wheel_arm: Path,
    out_dir: Path,
    macos_target: str,
) -> Path:
    desired_archs = {"x86_64", "arm64"}

    with tempfile.TemporaryDirectory(prefix="zimg_universal2_fuse_") as tmp:
        tmp_root = Path(tmp)
        x86_root = tmp_root / "x86"
        arm_root = tmp_root / "arm"
        x86_root.mkdir()
        arm_root.mkdir()

        with zipfile.ZipFile(wheel_x86, "r") as zf:
            zf.extractall(x86_root)
        with zipfile.ZipFile(wheel_arm, "r") as zf:
            zf.extractall(arm_root)

        dist_info_x86 = [p for p in x86_root.iterdir() if p.is_dir() and p.name.endswith(".dist-info")]
        dist_info_arm = [p for p in arm_root.iterdir() if p.is_dir() and p.name.endswith(".dist-info")]
        if len(dist_info_x86) != 1:
            raise RuntimeError(
                f"Universal2 fuse expected 1 dist-info directory in x86 wheel, got {len(dist_info_x86)}: {wheel_x86}"
            )
        if len(dist_info_arm) != 1:
            raise RuntimeError(
                f"Universal2 fuse expected 1 dist-info directory in arm wheel, got {len(dist_info_arm)}: {wheel_arm}"
            )
        if dist_info_x86[0].name != dist_info_arm[0].name:
            raise RuntimeError(
                "Universal2 fuse expected matching dist-info directory names, got: "
                f"{dist_info_x86[0].name} vs {dist_info_arm[0].name}"
            )
        dist_info_dir = dist_info_x86[0]

        files_x86 = sorted(
            p.relative_to(x86_root).as_posix() for p in x86_root.rglob("*") if p.is_file()
        )
        files_arm = sorted(
            p.relative_to(arm_root).as_posix() for p in arm_root.rglob("*") if p.is_file()
        )
        if files_x86 != files_arm:
            missing_from_arm = sorted(set(files_x86) - set(files_arm))
            missing_from_x86 = sorted(set(files_arm) - set(files_x86))
            raise RuntimeError(
                "Universal2 fuse requires identical wheel layouts.\n"
                f"Missing from arm: {missing_from_arm}\n"
                f"Missing from x86: {missing_from_x86}\n"
            )

        for rel in files_x86:
            x86_path = x86_root / rel
            arm_path = arm_root / rel

            x86_archs = _macos_lipo_archs(x86_path)
            arm_archs = _macos_lipo_archs(arm_path)
            if x86_archs is None and arm_archs is None:
                if rel.endswith(".dist-info/RECORD") or rel.endswith(".dist-info/WHEEL"):
                    continue
                x86_digest, x86_size = _sha256_digest_for_wheel_record(x86_path)
                arm_digest, arm_size = _sha256_digest_for_wheel_record(arm_path)
                if x86_size != arm_size or x86_digest != arm_digest:
                    raise RuntimeError(
                        "Universal2 fuse detected mismatched non-binary file contents: "
                        f"{rel} (x86={wheel_x86.name}, arm={wheel_arm.name})"
                    )
                continue

            if x86_archs is None or arm_archs is None:
                raise RuntimeError(
                    "Universal2 fuse detected file type mismatch between wheels: "
                    f"{rel} (x86_macho={x86_archs is not None}, arm_macho={arm_archs is not None})"
                )

            union_archs = x86_archs | arm_archs
            if not desired_archs.issubset(union_archs):
                raise RuntimeError(
                    "Universal2 fuse requires both x86_64 and arm64 slices; "
                    f"{rel} has archs x86={sorted(x86_archs)} arm={sorted(arm_archs)}"
                )

            if desired_archs.issubset(x86_archs):
                continue
            if desired_archs.issubset(arm_archs):
                shutil.copy2(arm_path, x86_path)
                continue

            if x86_archs & arm_archs:
                raise RuntimeError(
                    "Universal2 fuse cannot lipo binaries with overlapping arch sets; "
                    f"{rel} archs x86={sorted(x86_archs)} arm={sorted(arm_archs)}"
                )

            tmp_out = x86_path.with_name(x86_path.name + ".tmp")
            subprocess.run(
                ["lipo", "-create", str(x86_path), str(arm_path), "-output", str(tmp_out)],
                check=True,
            )
            os.replace(tmp_out, x86_path)

            fused_archs = _macos_lipo_archs(x86_path)
            if fused_archs is None or not desired_archs.issubset(fused_archs):
                raise RuntimeError(
                    "Universal2 fuse produced an unexpected binary: "
                    f"{rel} archs={sorted(fused_archs or set())}"
                )

        module_path = x86_root / "zimg" / "_imgpy.abi3.so"
        if module_path.exists():
            archs = _macos_lipo_archs(module_path)
            if archs is None or not desired_archs.issubset(archs):
                raise RuntimeError(
                    f"Universal2 wheel validation failed for {module_path}: archs={sorted(archs or set())}"
                )

        _macos_update_wheel_tag(dist_info_dir=dist_info_dir, macos_target=macos_target)
        _rewrite_wheel_record(wheel_root=x86_root, dist_info_dir=dist_info_dir)

        out_dir.mkdir(parents=True, exist_ok=True)
        out_name = wheel_x86.name
        if out_name.endswith("_x86_64.whl"):
            out_name = out_name[: -len("_x86_64.whl")] + "_universal2.whl"
        else:
            raise RuntimeError(f"Unexpected x86 wheel filename (expected *_x86_64.whl): {wheel_x86.name}")

        out_path = out_dir / out_name
        if out_path.exists():
            out_path.unlink()

        with zipfile.ZipFile(out_path, "w", compression=zipfile.ZIP_DEFLATED) as zf:
            for path in sorted(
                (p for p in x86_root.rglob("*") if p.is_file()),
                key=lambda p: p.relative_to(x86_root).as_posix(),
            ):
                rel = path.relative_to(x86_root).as_posix()
                zf.write(path, rel)

        return out_path


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


def _assert_linux_wheel_contains_expected_libs(wheel_path: Path) -> None:
    expected_libs = {
        "libfreeimageplus.so.3",
        "libtbb.so.12",
        "libQt6Gui.so.6",
        "libQt6Core.so.6",
    }

    with zipfile.ZipFile(wheel_path, "r") as zf:
        entries = set(zf.namelist())

    missing = sorted(
        f"zimg/lib/{name}" for name in expected_libs if f"zimg/lib/{name}" not in entries
    )
    if missing:
        raise RuntimeError(
            "Linux wheel is missing expected shared libraries under zimg/lib. "
            "These are installed during the CMake install step so `RPATH=$ORIGIN/lib` keeps working "
            "after the wheel is relocated into site-packages.\n"
            f"wheel: {wheel_path}\n"
            f"missing: {missing}"
        )
    logger.info("Linux wheel contains expected shared libraries (%d)", len(expected_libs))


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
    setup_logger()
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
    logger.info("Detected tag: %s -> zimg version: %s", git_describe, version)

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
        logger.info("Build command: %s", " ".join(cmd))
        if common_dirs.is_mac():
            logger.info("macOS wheel: build x86_64 + arm64, then fuse to universal2")
        atlas_pypi.maybe_upload_to_pypi(
            out_dir,
            project="zimg",
            version=version,
            git_describe=git_describe,
            raw_git_version=raw_git_version,
            dry_run=True,
        )
        if conda_token:
            assert conda_source_dir is not None
            assert conda_cmd_display is not None
            logger.info("Conda stage dir: %s/zimg", conda_source_dir)
            logger.info("Conda build command: %s", " ".join(conda_cmd_display))
            logger.info("Conda upload: enabled via conda-build")
        else:
            logger.info("Conda: skipped (ANACONDA_API_TOKEN not set)")
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

        env_base = os.environ.copy()
        env_base["ZIMG_SRC_DIR"] = str(zimg_src_dir)

        if common_dirs.is_mac():
            env_base["MACOSX_DEPLOYMENT_TARGET"] = build_ext_libs.macos_min_version()

            dist_x86 = tmp_root / "dist_x86"
            dist_arm = tmp_root / "dist_arm"

            env_x86 = env_base.copy()
            env_x86["ARCHFLAGS"] = "-arch x86_64"
            wheel_x86 = _build_single_wheel(project_dir=tmp_project, dist_dir=dist_x86, env=env_x86)

            env_arm = env_base.copy()
            env_arm["ARCHFLAGS"] = "-arch arm64"
            _append_cmake_args(
                env_arm,
                [
                    "-DCMAKE_SYSTEM_NAME=Darwin",
                    "-DCMAKE_SYSTEM_PROCESSOR=arm64",
                    "-DCMAKE_OSX_ARCHITECTURES=arm64",
                ],
            )
            wheel_arm = _build_single_wheel(project_dir=tmp_project, dist_dir=dist_arm, env=env_arm)

            fused = _fuse_macos_universal2_wheels(
                wheel_x86=wheel_x86,
                wheel_arm=wheel_arm,
                out_dir=out_dir,
                macos_target=build_ext_libs.macos_min_version(),
            )
            logger.info("Fused universal2 wheel: %s", fused)
        else:
            env = env_base
            if common_dirs.is_linux() or common_dirs.is_windows():
                _apply_scikit_build_core_toolchain_env(env)

            _run_checked(cmd, cwd=tmp_project, env=env)

    wheels = sorted(out_dir.glob("*.whl"))
    if wheels:
        if common_dirs.is_linux():
            for wheel_path in wheels:
                _assert_linux_wheel_contains_expected_libs(wheel_path)
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
    )

    if conda_token:
        assert conda_cmd is not None
        assert conda_cmd_display is not None
        assert conda_source_dir is not None

        wheel_path = wheels[0]
        logger.info("Staging conda zimg package from wheel: %s", wheel_path.name)
        staged_dir = _stage_conda_zimg_from_wheel(
            wheel_path=wheel_path, conda_source_dir=conda_source_dir
        )
        logger.info("Staged conda package dir: %s", staged_dir)
        _run_checked(conda_cmd, cwd=repo_root, display_cmd=conda_cmd_display)
    else:
        logger.info("Skipping conda build/upload (ANACONDA_API_TOKEN not set)")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
