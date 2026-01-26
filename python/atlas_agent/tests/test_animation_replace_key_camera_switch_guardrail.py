from __future__ import annotations

import json
import sys
from pathlib import Path

# Add src layout to sys.path for local runs
ROOT_DIR = Path(__file__).resolve().parents[2]
SRC_DIR = ROOT_DIR / "src"
if str(SRC_DIR) not in sys.path:
    sys.path.insert(0, str(SRC_DIR))

from atlas_agent.agent_team.tool_modules.animation_tools import handle  # type: ignore  # noqa: E402
from atlas_agent.agent_team.tool_modules.context import (  # type: ignore  # noqa: E402
    ToolDispatchContext,
)


class _Key:
    def __init__(self, time: float) -> None:
        self.time = float(time)


class _ListKeysResponse:
    def __init__(self, times: list[float]) -> None:
        self.keys = [_Key(t) for t in times]


class _StubClient:
    def __init__(self) -> None:
        self.remove_calls: list[dict] = []
        self.set_camera_calls: list[dict] = []
        self.validate_calls: list[dict] = []
        self.sample_calls: list[dict] = []
        self._existing_key_times: list[float] = []
        self._sample_value: dict = {}

    def set_camera_interpolation_method(
        self, *, animation_id: int, method: str
    ) -> None:
        return None

    def list_keys(self, **kwargs):  # type: ignore[no-untyped-def]
        return _ListKeysResponse(self._existing_key_times)

    def remove_key(self, **kwargs):  # type: ignore[no-untyped-def]
        self.remove_calls.append(dict(kwargs))
        return True

    def camera_sample(self, *, animation_id: int, times: list[float]) -> list[dict]:
        self.sample_calls.append({"animation_id": animation_id, "times": list(times)})
        return [{"time": float(times[0]), "value": dict(self._sample_value)}]

    def camera_validate(self, **kwargs):  # type: ignore[no-untyped-def]
        self.validate_calls.append(dict(kwargs))
        return {"ok": True, "results": [{"adjusted": False, "reason": ""}]}

    def set_key_camera(self, **kwargs):  # type: ignore[no-untyped-def]
        self.set_camera_calls.append(dict(kwargs))
        return True


def _ctx_for(client: _StubClient) -> ToolDispatchContext:
    return ToolDispatchContext(
        client=client,  # type: ignore[arg-type]
        atlas_dir=None,
        codegen_enabled=False,
        dispatch=lambda _name, _payload: json.dumps({"ok": True}),
        param_to_dict=lambda p: {},
        resolve_json_key=lambda _id, candidate, name: candidate or name,
        json_key_exists=lambda _id, _jk: True,
        schema_validator_cache={},
        session_store=None,
        runtime_state={},
    )


def test_switch_key_rejects_large_jump_without_opt_in() -> None:
    client = _StubClient()
    client._existing_key_times = [1.0]
    client._sample_value = {
        "Eye Position Vec3": [0.0, 0.0, 10.0],
        "Center Position Vec3": [0.0, 0.0, 0.0],
        "Up Vector Vec3": [0.0, 1.0, 0.0],
        "Field of View Float": 45.0,
        "Projection Type StringIntOption": "Perspective",
    }
    ctx = _ctx_for(client)

    res = json.loads(
        handle(
            "animation_replace_key_camera",
            {
                "animation_id": 1,
                "time": 1.0,
                "easing": "Switch",
                "ids": [123],
                "value": {
                    "Eye Position Vec3": [100.0, 0.0, 10.0],  # big jump vs sampled
                    "Center Position Vec3": [0.0, 0.0, 0.0],
                    "Up Vector Vec3": [0.0, 1.0, 0.0],
                    "Field of View Float": 45.0,
                    "Projection Type StringIntOption": "Perspective",
                },
            },
            ctx,
        )
        or "{}"
    )

    assert res.get("ok") is False
    assert "allow_jump_cut" in str(res.get("error") or "")
    # Fail-fast: no timeline mutation should occur.
    assert client.remove_calls == []
    assert client.set_camera_calls == []
    # Dry-run validation is allowed; the guardrail must prevent writes/removals.
    assert len(client.validate_calls) == 1


def test_switch_key_allows_jump_with_allow_jump_cut_true() -> None:
    client = _StubClient()
    client._existing_key_times = [1.0]
    client._sample_value = {
        "Eye Position Vec3": [0.0, 0.0, 10.0],
        "Center Position Vec3": [0.0, 0.0, 0.0],
        "Up Vector Vec3": [0.0, 1.0, 0.0],
        "Field of View Float": 45.0,
        "Projection Type StringIntOption": "Perspective",
    }
    ctx = _ctx_for(client)

    res = json.loads(
        handle(
            "animation_replace_key_camera",
            {
                "animation_id": 1,
                "time": 1.0,
                "easing": "Switch",
                "allow_jump_cut": True,
                "ids": [123],
                "value": {
                    "Eye Position Vec3": [100.0, 0.0, 10.0],
                    "Center Position Vec3": [0.0, 0.0, 0.0],
                    "Up Vector Vec3": [0.0, 1.0, 0.0],
                    "Field of View Float": 45.0,
                    "Projection Type StringIntOption": "Perspective",
                },
            },
            ctx,
        )
        or "{}"
    )

    assert res.get("ok") is True
    assert len(client.remove_calls) == 1
    assert len(client.set_camera_calls) == 1
