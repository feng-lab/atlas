import logging
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

logger = logging.getLogger(__name__)

_GIT_DESCRIBE_RE = re.compile(
    r"^v?(\d+\.\d+(?:\.\d+)*)(?:-(\d+)-g([0-9a-f]+))?(?:-(dirty))?$"
)


def _parse_git_describe_for_pypi(git_describe: str) -> tuple[str, int]:
    match = _GIT_DESCRIBE_RE.match(git_describe.strip())
    if not match:
        raise RuntimeError(
            "Unsupported git-describe format for PyPI.\n"
            f"got: {git_describe!r}\n"
            "expected one of:\n"
            "  - vX.Y (optionally with additional .N segments)\n"
            "  - vX.Y-N-g<sha> (optionally with additional .N segments)\n"
            "  - append -dirty to either form\n"
        )

    base = match.group(1)
    commits_since_tag = int(match.group(2) or "0")
    # hash = match.group(3)  # intentionally unused: PyPI does not accept local-version (+) uploads.
    # dirty = match.group(4)  # intentionally unused: we do not gate uploads on repo dirtiness.
    return (base, commits_since_tag)


def pep440_version_from_git_describe(git_describe: str) -> str:
    base, commits_since_tag = _parse_git_describe_for_pypi(git_describe)
    if commits_since_tag:
        return f"{base}.{commits_since_tag}"
    return base


def is_clean_release_tag(git_describe: str) -> bool:
    try:
        _base, commits_since_tag = _parse_git_describe_for_pypi(git_describe)
    except RuntimeError:
        return False
    return commits_since_tag == 0


def update_pyproject_version(pyproject_path: Path, version: str) -> None:
    text = pyproject_path.read_text(encoding="utf-8")
    new_text, count = re.subn(
        r'(?m)^\s*version\s*=\s*"[^"]*"',
        f'version = "{version}"',
        text,
    )
    if count != 1:
        raise RuntimeError(
            "Failed to update version in pyproject.toml "
            f"(expected 1 replacement, got {count})"
        )
    pyproject_path.write_text(new_text, encoding="utf-8")


def ensure_empty_dir(path: Path) -> None:
    path.mkdir(parents=True, exist_ok=True)
    for entry in path.iterdir():
        if entry.is_dir():
            shutil.rmtree(entry)
        else:
            entry.unlink()


def report_dist_artifact_sizes(dist_dir: Path) -> None:
    pypi_upload_file_size_limit_bytes = 100 * 1024 * 1024
    for artifact in sorted(p for p in dist_dir.iterdir() if p.is_file()):
        size_bytes = artifact.stat().st_size
        logger.info("Package size: %s is %s bytes", artifact.name, f"{size_bytes:,}")
        if size_bytes > pypi_upload_file_size_limit_bytes:
            logger.warning(
                "Package exceeds PyPI's default per-file upload limit of %s bytes: %s is %s bytes",
                f"{pypi_upload_file_size_limit_bytes:,}",
                artifact.name,
                f"{size_bytes:,}",
            )


def maybe_upload_to_pypi(
    dist_dir: Path,
    *,
    project: str,
    version: str,
    git_describe: str,
    dry_run: bool,
    allow_non_tag_upload: bool = False,
    skip_existing: bool = False,
    raw_git_version: str | None = None,
) -> None:
    if not is_clean_release_tag(git_describe) and not allow_non_tag_upload:
        logger.info("Upload: skipped (not a clean release tag)")
        return

    token = os.environ.get("PYPI_API_TOKEN", "").strip()
    if dry_run:
        logger.info("Project: %s", project)
        if token:
            logger.info("PYPI_API_TOKEN: set")
        else:
            logger.info(
                "PYPI_API_TOKEN: not set (local runs can define it in `.env.local` at the repo root)"
            )
        skip = " --skip-existing" if skip_existing else ""
        logger.info(
            "Upload command: %s -m twine upload%s %s/*",
            sys.executable,
            skip,
            dist_dir,
        )
        return

    if not token:
        if os.environ.get("GITHUB_ACTIONS", "").strip().lower() == "true":
            detail = ""
            if raw_git_version:
                detail = f"\nGIT_VERSION: {raw_git_version}\n"
            raise RuntimeError(
                "Refusing to publish to PyPI: PYPI_API_TOKEN is not set."
                f"{detail}git_describe: {git_describe}\n"
            )
        logger.info("PYPI_API_TOKEN is not set; skipping PyPI upload")
        return

    dist_files = sorted(p for p in dist_dir.iterdir() if p.is_file())
    if not dist_files:
        raise RuntimeError(f"No dist artifacts found to upload in: {dist_dir}")

    cmd = [sys.executable, "-m", "twine", "upload"]
    if skip_existing:
        cmd.append("--skip-existing")
    cmd.extend(str(p) for p in dist_files)
    env = os.environ.copy()
    env["TWINE_USERNAME"] = "__token__"
    env["TWINE_PASSWORD"] = token
    subprocess.run(cmd, env=env, check=True)
    logger.info("Uploaded %s %s to PyPI", project, version)
