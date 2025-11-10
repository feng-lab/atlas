import json
from functools import lru_cache
from pathlib import Path
from typing import Dict, Iterable

from ...discovery import discover_schema_dir

SCENE_LOAD_CATEGORIES = ("images", "meshes", "swc", "puncta", "roi", "svg", "animations")


def _normalize_extension(ext: str) -> str:
    trimmed = (ext or "").strip()
    if not trimmed:
        return ""
    if not trimmed.startswith("."):
        trimmed = "." + trimmed
    return trimmed.lower()


def _build_catalog(schema_dir_hint: str | None, atlas_dir: str | None) -> Dict[str, list[str]]:
    schema_dir, searched = discover_schema_dir(schema_dir_hint, atlas_dir)
    if not schema_dir:
        raise FileNotFoundError(
            "supported_file_formats.json not found: schema directory not located. "
            f"Searched: {searched}"
        )
    fmt_path = Path(schema_dir) / "supported_file_formats.json"
    if not fmt_path.exists():
        raise FileNotFoundError(
            f"supported_file_formats.json missing at {fmt_path}. "
            "Run Atlas with --run_dump_animation3d_schema to regenerate."
        )
    payload = json.loads(fmt_path.read_text(encoding="utf-8"))
    categories = payload.get("categories", {})
    catalog: Dict[str, list[str]] = {}
    for key, entry in categories.items():
        raw_exts = entry.get("extensions", [])
        collected = {_normalize_extension(ext) for ext in raw_exts if _normalize_extension(ext)}
        if collected:
            catalog[key] = sorted(collected)
    if not catalog:
        raise ValueError(
            f"supported_file_formats.json at {fmt_path} did not contain any category data."
        )
    return catalog


@lru_cache(maxsize=32)
def _catalog_cache(schema_dir_hint: str | None, atlas_dir: str | None):
    catalog = _build_catalog(schema_dir_hint, atlas_dir)
    return tuple(sorted((key, tuple(values)) for key, values in catalog.items()))


def load_supported_format_catalog(schema_dir: str | None, atlas_dir: str | None) -> Dict[str, list[str]]:
    cached = _catalog_cache(schema_dir, atlas_dir)
    return {key: list(values) for key, values in cached}


def get_supported_extensions(
    schema_dir: str | None,
    atlas_dir: str | None,
    *,
    categories: Iterable[str] | None = None,
) -> list[str]:
    catalog = load_supported_format_catalog(schema_dir, atlas_dir)
    wanted = list(categories) if categories else list(SCENE_LOAD_CATEGORIES)
    exts: set[str] = set()
    for cat in wanted:
        exts.update(catalog.get(cat, []))
    return sorted(exts)
