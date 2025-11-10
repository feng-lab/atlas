from dataclasses import dataclass
from typing import Any, Callable, Dict

from ...scene_rpc import SceneClient


@dataclass(slots=True)
class ToolDispatchContext:
    client: SceneClient
    atlas_dir: str | None
    dispatch: Callable[[str, str], str]
    param_to_dict: Callable[[Any], Dict[str, Any]]
    resolve_json_key: Callable[[int, str | None, str | None], str | None]
    json_key_exists: Callable[[int, str], bool]
    schema_validator_cache: Dict[str, object]
