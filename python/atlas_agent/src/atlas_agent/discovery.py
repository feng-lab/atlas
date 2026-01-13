import platform
from pathlib import Path
from typing import List, Optional, Tuple


def discover_schema_dir(user_schema_dir: Optional[str], atlas_dir: Optional[str]) -> Tuple[Optional[Path], List[str]]:
    searched: List[str] = []
    # Priority 1: explicit schema dir
    if user_schema_dir:
        p = Path(user_schema_dir)
        if _contains_schema(p):
            return p, searched
        searched.append(str(p))
    # Priority 2: from atlas_dir
    if atlas_dir:
        atlas_bin, schema_dir = compute_paths_from_atlas_dir(Path(atlas_dir))
        searched.append(str(schema_dir))
        if _contains_schema(schema_dir):
            return schema_dir, searched
    # Priority 3: default install locations
    for default_dir in default_install_dirs():
        atlas_bin, schema_dir = compute_paths_from_atlas_dir(default_dir)
        searched.append(str(schema_dir))
        if _contains_schema(schema_dir):
            return schema_dir, searched
    return None, searched


def _contains_schema(p: Path) -> bool:
    return (p / "animation3d.schema.json").exists() and (p / "capabilities.json").exists()


def compute_paths_from_atlas_dir(atlas_dir: Path) -> Tuple[Path, Path]:
    """Return (atlas_bin, schema_dir) derived from an install root.

    Accepted atlas_dir forms:
    - macOS: /Applications/fenglab/Atlas.app
    - Windows: C:\\Program Files\\fenglab\\Atlas
    - Linux: /opt/fenglab/Atlas
    """
    system = platform.system()
    if system == "Darwin":
        # atlas_dir expected to be the .app bundle root
        atlas_bin = atlas_dir / "Contents/MacOS/Atlas"
        schema_dir = atlas_dir / "Contents/Resources/json/atlas"
    elif system == "Windows":
        atlas_bin = atlas_dir / "Atlas.exe"
        schema_dir = atlas_dir / "Resources/json/atlas"
    else:
        # Linux
        atlas_bin = atlas_dir / "Atlas"
        schema_dir = atlas_dir / "Resources/json/atlas"
    return atlas_bin, schema_dir


def compute_docs_dir_from_atlas_dir(atlas_dir: Path) -> Path:
    """Return docs_dir derived from an install root."""
    system = platform.system()
    if system == "Darwin":
        return atlas_dir / "Contents/Resources/docs"
    if system == "Windows":
        return atlas_dir / "Resources/docs"
    # Linux
    return atlas_dir / "Resources/docs"


def compute_protos_dir_from_atlas_dir(atlas_dir: Path) -> Path:
    """Return protos_dir derived from an install root.

    This directory contains RPC `.proto` files shipped with the running Atlas app.
    External agents can compile client stubs against these protos to avoid drift
    between the app and the agent package.
    """
    system = platform.system()
    if system == "Darwin":
        return atlas_dir / "Contents/Resources/protos"
    if system == "Windows":
        return atlas_dir / "Resources/protos"
    # Linux
    return atlas_dir / "Resources/protos"


def default_install_dirs() -> list[Path]:
    system = platform.system()
    candidates: list[Path] = []
    if system == "Darwin":
        candidates.append(Path("/Applications/fenglab/Atlas.app"))
        candidates.append(Path("/Applications/Atlas.app"))
    elif system == "Windows":
        candidates.append(Path(r"C:\\Program Files\\fenglab\\Atlas"))
        candidates.append(Path(r"C:\\Program Files (x86)\\fenglab\\Atlas"))
    else:
        # Linux
        candidates.append(Path("/opt/fenglab/Atlas"))
        candidates.append(Path("/opt/fenglab/atlas"))
        candidates.append(Path("/usr/local/fenglab/Atlas"))
    return candidates
