import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

import atlas_env
import atlas_pypi
import atlas_version
import common_dirs


def main() -> int:
    parser = argparse.ArgumentParser(
        description=("Build and publish the zimg PyPI wheel."),
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Print resolved version and commands without building/uploading.",
    )

    args = parser.parse_args()

    allowed_env_keys = {"PYPI_API_TOKEN"}
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

    if args.dry_run:
        print("Build command:", " ".join(cmd))
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

        subprocess.run(cmd, cwd=str(tmp_project), env=env, check=True)

    wheels = sorted(out_dir.glob("*.whl"))
    if wheels:
        for wheel in wheels:
            print(f"Built: {wheel}")
    else:
        print(f"No wheels found in: {out_dir}")

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
