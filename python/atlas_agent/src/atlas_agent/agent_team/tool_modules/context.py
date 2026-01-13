from dataclasses import dataclass
from typing import Any, Callable, Dict, Optional

from ...scene_rpc import SceneClient
from ...session_store import SessionStore


@dataclass(slots=True)
class ToolDispatchContext:
    client: SceneClient
    atlas_dir: str | None
    codegen_enabled: bool
    dispatch: Callable[[str, str], str]
    param_to_dict: Callable[[Any], Dict[str, Any]]
    resolve_json_key: Callable[[int, str | None, str | None], str | None]
    json_key_exists: Callable[[int, str], bool]
    schema_validator_cache: Dict[str, object]
    session_store: Optional[SessionStore]
    runtime_state: Dict[str, Any]
