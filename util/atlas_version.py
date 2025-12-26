import os

import common_dirs


def read_git_version_from_header() -> str:
    """Read `GIT_VERSION` from generated `src/version/version.h`.

    Returns the raw C string content without surrounding quotes.
    Raises RuntimeError if the header is missing or malformed.
    """
    repo_root = common_dirs.atlas_repository_dir()
    version_header_path = os.path.join(repo_root, "src", "version", "version.h")
    if not os.path.exists(version_header_path):
        raise RuntimeError(f"version.h not found: {version_header_path}")

    try:
        with open(version_header_path, "r", encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if line.startswith("#define GIT_VERSION"):
                    start = line.find('"')
                    end = line.rfind('"')
                    if start != -1 and end != -1 and end > start:
                        return line[start + 1 : end]
        raise RuntimeError("GIT_VERSION define not found in version.h")
    except Exception as e:
        raise RuntimeError(f"Failed reading version.h: {e}")


def git_describe_from_git_version(git_version: str) -> str:
    """Extract the `git describe` portion from a `GIT_VERSION` string."""
    parts = git_version.split(" build ", 1)
    token = parts[0].strip()
    if not token:
        raise RuntimeError("Empty git-describe token derived from GIT_VERSION")
    return token


def git_describe_from_header() -> str:
    return git_describe_from_git_version(read_git_version_from_header())


def version_token_for_filename() -> str:
    """Return a filesystem-friendly version token for filenames."""
    token = git_describe_from_header()
    token = token.replace(" ", "_").replace("/", "-")
    if not token:
        raise RuntimeError("Empty version token derived from GIT_VERSION")
    return token
