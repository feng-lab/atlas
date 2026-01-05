import json
import subprocess
import tempfile
from pathlib import Path

import atlas_file_hosts
import common_dirs
import download_utils

_REQUIRED_SCHEMA_FILES: tuple[str, ...] = (
    "animation3d.schema.json",
    "capabilities.json",
    "supported_file_formats.json",
)


def repo_schema_dir(repo_root: Path) -> Path:
    return (repo_root / "src" / "atlas" / "Resources" / "json" / "atlas").resolve()


def atlas_binary_from_atlas_dir(atlas_dir: Path) -> Path:
    if common_dirs.is_mac():
        return atlas_dir / "Contents" / "MacOS" / "Atlas"
    if common_dirs.is_windows():
        return atlas_dir / "Atlas.exe"
    return atlas_dir / "Atlas"


def missing_schema_files(out_dir: Path) -> list[str]:
    return [name for name in _REQUIRED_SCHEMA_FILES if not (out_dir / name).exists()]


def dump_schema_with_atlas(*, atlas_dir: Path, out_dir: Path) -> subprocess.CompletedProcess[bytes]:
    atlas_bin = atlas_binary_from_atlas_dir(atlas_dir)
    args: list[str] = [
        str(atlas_bin),
        "--run_dump_animation3d_schema",
        "--dump_output_dir",
        str(out_dir),
    ]
    if common_dirs.is_linux():
        args += ["-platform", "offscreen"]
    return subprocess.run(args, check=False)


def _download_json_file(*, url: str, backup_url: str, out_path: Path) -> None:
    out_path.parent.mkdir(parents=True, exist_ok=True)
    tmp_path: Path | None = None
    try:
        with tempfile.NamedTemporaryFile(
            "wb", delete=False, dir=out_path.parent
        ) as tmp:
            tmp_path = Path(tmp.name)

        ok = download_utils.download_file_with_resume(
            url,
            backup_url,
            str(tmp_path),
            expected_size=None,
            expected_sha256=None,
        )
        if not ok:
            raise RuntimeError(f"Failed to download {url}")

        try:
            json.loads(tmp_path.read_text(encoding="utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError) as e:
            raise RuntimeError(f"Downloaded file is not valid JSON: {url}") from e

        tmp_path.replace(out_path)
    finally:
        if tmp_path and tmp_path.exists():
            tmp_path.unlink()


def _download_missing_schema_files(*, out_dir: Path, missing: list[str]) -> None:
    primary_host, backup_host = atlas_file_hosts.get_file_hosts()
    for name in missing:
        _download_json_file(
            url=atlas_file_hosts.static_file_url(
                host=primary_host,
                static_subdir="installers",
                relative_path=name,
            ),
            backup_url=atlas_file_hosts.static_file_url(
                host=backup_host,
                static_subdir="installers",
                relative_path=name,
            ),
            out_path=(out_dir / name),
        )


def ensure_llm_schema_docs(
    *,
    atlas_dir: Path,
    out_dir: Path,
    force_schema_dump: bool = False,
) -> None:
    out_dir.mkdir(parents=True, exist_ok=True)

    if force_schema_dump or missing_schema_files(out_dir):
        proc = dump_schema_with_atlas(atlas_dir=atlas_dir, out_dir=out_dir)
        missing = missing_schema_files(out_dir)
        if missing:
            atlas_bin = atlas_binary_from_atlas_dir(atlas_dir)
            if common_dirs.is_windows():
                try:
                    _download_missing_schema_files(
                        out_dir=out_dir,
                        missing=missing,
                    )
                except Exception as e:
                    raise RuntimeError(
                        "Atlas schema dump did not produce required files: "
                        + ", ".join(missing)
                        + f" (atlas_bin={atlas_bin}, exit={proc.returncode}, out_dir={out_dir}; "
                        + f"fallback_download failed: {e})"
                    ) from e

                missing_after_download = missing_schema_files(out_dir)
                if not missing_after_download:
                    return
                missing = missing_after_download

            raise RuntimeError(
                "Atlas schema dump did not produce required files: "
                + ", ".join(missing)
                + f" (atlas_bin={atlas_bin}, exit={proc.returncode}, out_dir={out_dir})"
            )
