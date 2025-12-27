import subprocess
from pathlib import Path

import common_dirs

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
            raise RuntimeError(
                "Atlas schema dump did not produce required files: "
                + ", ".join(missing)
                + f" (atlas_bin={atlas_bin}, exit={proc.returncode}, out_dir={out_dir})"
            )
