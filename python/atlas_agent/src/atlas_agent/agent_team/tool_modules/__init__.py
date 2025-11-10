import copy
from typing import Any, Dict, List

from . import animation_tools, camera_tools, fs_tools, general_tools, scene_tools

ALL_MODULES = [
    general_tools,
    scene_tools,
    camera_tools,
    animation_tools,
    fs_tools,
]

TOOL_TO_MODULE = {
    name: module for module in ALL_MODULES for name in module.HANDLED_TOOLS
}


def build_tool_list() -> List[Dict[str, Any]]:
    tools: List[Dict[str, Any]] = []
    seen: set[str] = set()
    for module in ALL_MODULES:
        for spec in module.TOOL_SPECS:
            name = spec.get("function", {}).get("name")  # type: ignore[union-attr]
            if not isinstance(name, str) or not name or name in seen:
                continue
            tools.append(copy.deepcopy(spec))
            seen.add(name)
    return tools
