from typing import Any, Dict, List

from ...tool_registry import Tool
from . import (
    animation_tools,
    camera_tools,
    docs_tools,
    fs_tools,
    general_tools,
    plan_tools,
    scene_tools,
    session_tools,
    verification_tools,
)

ALL_MODULES = [
    general_tools,
    plan_tools,
    verification_tools,
    session_tools,
    scene_tools,
    camera_tools,
    animation_tools,
    docs_tools,
    fs_tools,
]

def build_tools() -> List[Tool]:
    tools: List[Tool] = []
    seen: set[str] = set()
    for module in ALL_MODULES:
        for t in getattr(module, "TOOLS", []) or []:
            if not isinstance(t, Tool):
                continue
            name = str(t.name or "").strip()
            if not name or name in seen:
                continue
            tools.append(t)
            seen.add(name)
    return tools


def build_tool_list() -> List[Dict[str, Any]]:
    # Chat Completions tool shape: {"type":"function","function":{...}}
    return [t.to_chat_tool_spec() for t in build_tools()]
