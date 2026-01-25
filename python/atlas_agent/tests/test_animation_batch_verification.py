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
from atlas_agent.agent_team.tool_modules.context import ToolDispatchContext  # type: ignore  # noqa: E402


class _Key:
    def __init__(self, t: float) -> None:
        self.time = float(t)


class _ListKeysResp:
    def __init__(self, times: list[float]) -> None:
        self.keys = [_Key(t) for t in times]


class _TimeStatus:
    def __init__(self, duration: float) -> None:
        self.duration = float(duration)


class _StubClient:
    def __init__(self) -> None:
        self._batch_ok = True
        self._duration = 10.0
        # (target_id, json_key) -> times present
        self._keys_by_track: dict[tuple[int, str], list[float]] = {}

    def list_params(self, *, id: int):  # type: ignore[no-untyped-def]
        # Used only for schema validation in animation_batch. We advertise the exact
        # json_key in the test payload, so return it as valid.
        class _P:
            def __init__(self, json_key: str) -> None:
                self.json_key = json_key

        class _Resp:
            def __init__(self, params):  # type: ignore[no-untyped-def]
                self.params = params

        # Return a single param json_key that matches the test.
        return _Resp([_P("Global X Cut CutSpan")])

    def batch(self, **kwargs):  # type: ignore[no-untyped-def]
        return bool(self._batch_ok)

    def get_time(self, *, animation_id: int):  # type: ignore[no-untyped-def]
        return _TimeStatus(self._duration)

    def list_keys(  # type: ignore[no-untyped-def]
        self,
        *,
        animation_id: int,
        target_id: int,
        json_key: str = "",
        include_values: bool = False,
    ):
        return _ListKeysResp(
            self._keys_by_track.get((int(target_id), str(json_key)), [])
        )


def _ctx_for(client: _StubClient) -> ToolDispatchContext:
    return ToolDispatchContext(
        client=client,  # type: ignore[arg-type]
        atlas_dir=None,
        codegen_enabled=False,
        dispatch=lambda _name, _payload: json.dumps({"ok": False}),
        param_to_dict=lambda p: {},
        resolve_json_key=lambda _id, candidate, name: candidate or name,
        json_key_exists=lambda _id, _jk: True,
        schema_validator_cache={},
        session_store=None,
        runtime_state={},
    )


def test_animation_batch_fails_when_keys_missing_after_verify() -> None:
    client = _StubClient()
    # Only the first key exists.
    client._keys_by_track[(3, "Global X Cut CutSpan")] = [9.0]
    ctx = _ctx_for(client)

    res = json.loads(
        handle(
            "animation_batch",
            {
                "animation_id": 110,
                "commit": True,
                "remove_keys": [],
                "set_keys": [
                    {
                        "easing": "Linear",
                        "id": 3,
                        "json_key": "Global X Cut CutSpan",
                        "time": 9,
                        "value": {"Mode StringIntOption": "Absolute"},
                    },
                    {
                        "easing": "Linear",
                        "id": 3,
                        "json_key": "Global X Cut CutSpan",
                        "time": 10.5,
                        "value": {"Mode StringIntOption": "Absolute"},
                    },
                ],
            },
            ctx,
        )
        or "{}"
    )

    assert res.get("ok") is False
    assert "missing" in res
    missing = res.get("missing") or []
    assert any(abs(float(m.get("time", 0.0)) - 10.5) < 1e-9 for m in missing)


def test_animation_batch_warns_when_keys_beyond_duration() -> None:
    client = _StubClient()
    client._duration = 9.0
    client._keys_by_track[(3, "Global X Cut CutSpan")] = [9.0, 10.5]
    ctx = _ctx_for(client)

    res = json.loads(
        handle(
            "animation_batch",
            {
                "animation_id": 110,
                "commit": True,
                "remove_keys": [],
                "set_keys": [
                    {
                        "easing": "Linear",
                        "id": 3,
                        "json_key": "Global X Cut CutSpan",
                        "time": 9,
                        "value": {"Mode StringIntOption": "Absolute"},
                    },
                    {
                        "easing": "Linear",
                        "id": 3,
                        "json_key": "Global X Cut CutSpan",
                        "time": 10.5,
                        "value": {"Mode StringIntOption": "Absolute"},
                    },
                ],
            },
            ctx,
        )
        or "{}"
    )

    assert res.get("ok") is True
    assert "warning" in res
    assert res.get("suggested_duration") == 10.5
