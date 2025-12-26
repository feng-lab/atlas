import os
from typing import AbstractSet, Optional

import common_dirs


def load_repo_dotenv(*, allowed_keys: Optional[AbstractSet[str]] = None) -> None:
    """Load repo-root `.env` and `.env.local` into `os.environ`.

    Rules:
    - `.env` only sets variables that are not already present in the process environment.
    - `.env.local` only overrides variables that were set by `.env` in this call.
      (It will not override values that were already present in the OS environment.)
    - If `allowed_keys` is provided, only those keys will be imported.
    """
    repo_root = common_dirs.atlas_repository_dir()
    env_path = os.path.join(repo_root, ".env")
    env_local_path = os.path.join(repo_root, ".env.local")

    keys_from_env: set[str] = set()
    _load_env_file(
        env_path,
        keys_from_env=keys_from_env,
        override_only_keys=None,
        allowed_keys=allowed_keys,
    )
    _load_env_file(
        env_local_path,
        keys_from_env=None,
        override_only_keys=keys_from_env,
        allowed_keys=allowed_keys,
    )


def _load_env_file(
    path: str,
    *,
    keys_from_env: Optional[set[str]],
    override_only_keys: Optional[set[str]],
    allowed_keys: Optional[AbstractSet[str]],
) -> None:
    if not os.path.exists(path):
        return

    try:
        with open(path, "r", encoding="utf-8") as f:
            for raw_line in f:
                line = raw_line.strip()
                if not line or line.startswith("#"):
                    continue
                if line.startswith("export "):
                    line = line[len("export ") :].lstrip()
                if "=" not in line:
                    continue

                key, value = line.split("=", 1)
                key = key.strip()
                value = value.strip()
                if not key:
                    continue

                if allowed_keys is not None and key not in allowed_keys:
                    continue

                if (
                    value.startswith(("'", '"'))
                    and len(value) >= 2
                    and value[-1] == value[0]
                ):
                    value = value[1:-1]

                if key in os.environ:
                    if override_only_keys is None or key not in override_only_keys:
                        continue

                os.environ[key] = value
                if keys_from_env is not None:
                    keys_from_env.add(key)
    except Exception as e:
        raise RuntimeError(f"Failed reading env file: {path}: {e}")
