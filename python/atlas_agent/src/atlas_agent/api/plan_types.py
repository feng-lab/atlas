from dataclasses import dataclass, field
from typing import Any, Optional

Id = int  # Unified addressing: 0=camera, 1=background, 2=axis, 3=global, >=4 object ids


@dataclass
class SetParam:
    """Stateless scene parameter assignment (no time/easing)."""
    id: Id
    json_key: str
    value: Any


@dataclass
class SetKey:
    """Timeline key for id target (camera/object)."""
    id: Id  # 0=camera, >=4 objects; 1-3 reserved engine groups
    time: float
    value: Any
    easing: str = "Linear"
    json_key: Optional[str] = None  # required for non-camera


@dataclass
class RemoveKey:
    id: Id
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
