import os
from typing import Final, Iterable

import atlas_env

_PRIMARY_ENV_VAR: Final[str] = "ATLAS_FILE_HOST_PRIMARY"
_SECONDARY_ENV_VAR: Final[str] = "ATLAS_FILE_HOST_SECONDARY"
_BACKUP_ENV_VAR: Final[str] = "ATLAS_FILE_HOST_BACKUP"

_DOTENV_KEYS: Final[frozenset[str]] = frozenset(
    {_PRIMARY_ENV_VAR, _SECONDARY_ENV_VAR, _BACKUP_ENV_VAR}
)


def dotenv_keys() -> frozenset[str]:
    return _DOTENV_KEYS


def get_primary_and_backup_hosts() -> tuple[str, str]:
    """Return (primary_host, backup_host) for static asset downloads."""
    atlas_env.load_repo_dotenv(allowed_keys=_DOTENV_KEYS)

    primary_raw = os.environ.get(_PRIMARY_ENV_VAR)
    backup_raw = os.environ.get(_BACKUP_ENV_VAR)
    if not primary_raw:
        raise RuntimeError(
            f"{_PRIMARY_ENV_VAR} is not set. Define it in the environment or in the repo-root `.env.local`."
        )
    if not backup_raw:
        raise RuntimeError(
            f"{_BACKUP_ENV_VAR} is not set. Define it in the environment or in the repo-root `.env.local`."
        )

    primary = _normalize_origin(primary_raw, env_var=_PRIMARY_ENV_VAR)
    backup = _normalize_origin(backup_raw, env_var=_BACKUP_ENV_VAR)
    return primary, backup


def get_primary_and_secondary_hosts() -> tuple[str, str]:
    """Return (primary_host, secondary_host) for installer-related downloads."""
    atlas_env.load_repo_dotenv(allowed_keys=_DOTENV_KEYS)

    primary_raw = os.environ.get(_PRIMARY_ENV_VAR)
    secondary_raw = os.environ.get(_SECONDARY_ENV_VAR)
    if not primary_raw:
        raise RuntimeError(
            f"{_PRIMARY_ENV_VAR} is not set. Define it in the environment or in the repo-root `.env.local`."
        )
    if not secondary_raw:
        raise RuntimeError(
            f"{_SECONDARY_ENV_VAR} is not set. Define it in the environment or in the repo-root `.env.local`."
        )

    primary = _normalize_origin(primary_raw, env_var=_PRIMARY_ENV_VAR)
    secondary = _normalize_origin(secondary_raw, env_var=_SECONDARY_ENV_VAR)
    return primary, secondary


def static_file_url(*, host: str, static_subdir: str, relative_path: str) -> str:
    """Build a URL like: {host}/static/{static_subdir}/{relative_path}."""
    host_norm = _normalize_origin(host, env_var="<internal>")
    static_subdir_norm = static_subdir.strip().strip("/")
    if not static_subdir_norm:
        raise ValueError("static_subdir must be non-empty")

    rel = _normalize_relpath_for_url(relative_path)
    return f"{host_norm}/static/{static_subdir_norm}/{rel}"


def with_static_urls(
    raw_files: Iterable[dict],
    *,
    static_subdir: str,
) -> list[dict]:
    """Return `raw_files` with `url` and `fallback_url` filled in.

    Each input item must contain:
      - filename
      - expected_size
      - expected_sha256
    """
    primary, backup = get_primary_and_backup_hosts()

    primary_base = f"{primary}/static/{static_subdir.strip().strip('/')}"
    backup_base = f"{backup}/static/{static_subdir.strip().strip('/')}"

    out: list[dict] = []
    for item in raw_files:
        filename = item["filename"]
        rel = _normalize_relpath_for_url(filename)
        out.append(
            {
                **item,
                "url": f"{primary_base}/{rel}",
                "fallback_url": f"{backup_base}/{rel}",
            }
        )
    return out


def _normalize_origin(value: str, *, env_var: str) -> str:
    raw = str(value).strip()
    if not raw:
        raise RuntimeError(
            f"{env_var} is set but empty; expected an origin like https://example.com"
        )
    norm = raw.rstrip("/")
    if not (norm.startswith("https://") or norm.startswith("http://")):
        raise RuntimeError(
            f"{env_var} must start with http:// or https:// (got {value!r})"
        )
    return norm


def _normalize_relpath_for_url(relpath: str) -> str:
    # Convert Windows separators to URL separators; prevent accidental absolute paths.
    rel = str(relpath).replace("\\", "/").lstrip("/")
    if not rel:
        raise ValueError("relative_path must be non-empty")
    return rel
