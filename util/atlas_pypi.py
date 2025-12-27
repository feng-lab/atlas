import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

_GIT_DESCRIBE_RE = re.compile(
    r"^v?(\d+\.\d+\.\d+(?:\.\d+)?)(?:-(\d+)-g([0-9a-f]+))?(?:-(dirty))?$"
)


def pep440_version_from_git_describe(git_describe: str) -> str:
    match = _GIT_DESCRIBE_RE.match(git_describe.strip())
    if not match:
        raise RuntimeError(
            "Unsupported git-describe format for PyPI.\n"
            f"got: {git_describe!r}\n"
            "expected one of:\n"
            "  - vX.Y.Z\n"
            "  - vX.Y.Z-N-g<sha>\n"
        )

    base = match.group(1)
    commits_since_tag = match.group(2)
    # hash = match.group(3)  # intentionally unused: PyPI does not accept local-version (+) uploads.
    # dirty = match.group(4)  # intentionally unused: we do not gate uploads on repo dirtiness.

    if commits_since_tag:
        return f"{base}.dev{commits_since_tag}"
    return base


def is_clean_release_tag(git_describe: str) -> bool:
    match = _GIT_DESCRIBE_RE.match(git_describe.strip())
    return bool(match and match.group(2) is None)


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


def maybe_upload_to_pypi(
    dist_dir: Path,
    *,
    project: str,
    version: str,
    git_describe: str,
    dry_run: bool,
    allow_dev_upload: bool = False,
    skip_existing: bool = False,
    raw_git_version: str | None = None,
) -> None:
    if not is_clean_release_tag(git_describe) and not allow_dev_upload:
        print("Upload: skipped (not a clean release tag)")
        return

    token = os.environ.get("PYPI_API_TOKEN", "").strip()
    if dry_run:
        print(f"Project: {project}")
        if token:
            print("PYPI_API_TOKEN: set")
        else:
            print(
                "PYPI_API_TOKEN: not set (local runs can define it in `.env.local` at the repo root)"
            )
        skip = " --skip-existing" if skip_existing else ""
        print(f"Upload command: {sys.executable} -m twine upload{skip} {dist_dir}/*")
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
        print("PYPI_API_TOKEN is not set; skipping PyPI upload")
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
    print(f"Uploaded {project} {version} to PyPI")
