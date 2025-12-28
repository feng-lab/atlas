import argparse
import logging
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

import atlas_env
import atlas_pypi
import atlas_version
import common_dirs
from logger import setup_logger

logger = logging.getLogger(__name__)


def main() -> int:
    setup_logger()
    parser = argparse.ArgumentParser(
        description=("Build and publish the atlas-agent PyPI wheel."),
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

    args = parser.parse_args()

    atlas_env.load_repo_dotenv(allowed_keys={"PYPI_API_TOKEN"})

    repo_root = Path(common_dirs.atlas_repository_dir())
    py_project_src = repo_root / "python" / "atlas_agent"

    raw_git_version = atlas_version.read_git_version_from_header()
    git_describe = atlas_version.git_describe_from_git_version(raw_git_version)
    version = atlas_pypi.pep440_version_from_git_describe(git_describe)
    logger.info("Detected tag: %s -> atlas-agent version: %s", git_describe, version)

    out_dir = py_project_src / "dist"
    cmd = [sys.executable, "-m", "build", "--wheel", "--outdir", str(out_dir)]

    if args.dry_run:
        logger.info("Build command: %s", " ".join(cmd))
        atlas_pypi.maybe_upload_to_pypi(
            out_dir,
            project="atlas-agent",
            version=version,
            git_describe=git_describe,
            raw_git_version=raw_git_version,
            dry_run=True,
            allow_non_tag_upload=args.allow_non_tag_upload,
            skip_existing=args.allow_non_tag_upload,
        )
        return 0

    atlas_pypi.ensure_empty_dir(out_dir)

    with tempfile.TemporaryDirectory(prefix="atlas_agent_pypi_build_") as tmp:
        tmp_root = Path(tmp)
        tmp_project = tmp_root / "atlas_agent"
        shutil.copytree(
            py_project_src,
            tmp_project,
            ignore=shutil.ignore_patterns("dist", "__pycache__", "*.pyc", "*.pyo"),
        )
        atlas_pypi.update_pyproject_version(tmp_project / "pyproject.toml", version)
        subprocess.run(cmd, cwd=str(tmp_project), check=True)

    wheels = sorted(out_dir.glob("*.whl"))
    if wheels:
        for wheel in wheels:
            logger.info("Built: %s", wheel)
    else:
        logger.warning("No wheels found in: %s", out_dir)

    atlas_pypi.maybe_upload_to_pypi(
        out_dir,
        project="atlas-agent",
        version=version,
        git_describe=git_describe,
        raw_git_version=raw_git_version,
        dry_run=False,
        allow_non_tag_upload=args.allow_non_tag_upload,
        skip_existing=args.allow_non_tag_upload,
    )

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
