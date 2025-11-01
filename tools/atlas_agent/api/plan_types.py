from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any, Literal, Optional


Scope = dict[str, Any]  # {"object": int} | {"group": str} | {"camera": True}


@dataclass
class SetParam:
    """Stateless scene parameter assignment (no time/easing)."""
    scope: Scope
    json_key: str
    value: Any


@dataclass
class SetKey:
    """Timeline key for object/group or camera."""
    scope: Scope  # {"camera": True} or {"object": int} or {"group": str}
    time: float
    value: Any
    easing: str = "Linear"
    json_key: Optional[str] = None  # required for non-camera


@dataclass
class RemoveKey:
    scope: Scope
    time: float
    json_key: Optional[str] = None  # required for non-camera


@dataclass
class Plan:
    """Unified plan for scene and animation edits.

    - set_params: stateless assignments (scene lane)
    - set_keys/remove_keys: timeline edits (animation lane)
    - commit: when True and any t=0 keys are present, server evaluates t=0 immediately
    """
    set_params: list[SetParam] = field(default_factory=list)
    set_keys: list[SetKey] = field(default_factory=list)
    remove_keys: list[RemoveKey] = field(default_factory=list)
    commit: bool = True

