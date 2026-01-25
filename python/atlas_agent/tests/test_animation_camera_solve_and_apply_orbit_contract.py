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


class _StubClient:
    def __init__(self) -> None:
        self._solve_keys: list[dict] = []

    def set_camera_interpolation_method(
        self, *, animation_id: int, method: str
    ) -> None:
        # Tool enforces this best-effort; no-op for tests.
        return None

    def camera_solve(self, **kwargs):  # type: ignore[no-untyped-def]
        return list(self._solve_keys)


def _ctx_for(client: _StubClient, dispatch_fn) -> ToolDispatchContext:  # type: ignore[no-untyped-def]
    return ToolDispatchContext(
        client=client,  # type: ignore[arg-type]
        atlas_dir=None,
        codegen_enabled=False,
        dispatch=dispatch_fn,
        param_to_dict=lambda p: {},
        resolve_json_key=lambda _id, candidate, name: candidate or name,
        json_key_exists=lambda _id, _jk: True,
        schema_validator_cache={},
        session_store=None,
        runtime_state={},
    )


def test_orbit_rejects_params_degrees_and_max_step_degrees() -> None:
    client = _StubClient()
    ctx = _ctx_for(client, lambda _name, _payload: json.dumps({"ok": True}))

    res = json.loads(
        handle(
            "animation_camera_solve_and_apply",
            {
                "animation_id": 1,
                "mode": "ORBIT",
                "ids": [123],
                "t0": 0.0,
                "t1": 1.0,
                "params": {"axis": "y", "degrees": 50.0, "max_step_degrees": 20.0},
                "degrees": 0.0,
                "max_step_degrees": 20.0,
                "clear_range": False,
            },
            ctx,
        )
        or "{}"
    )

    assert res.get("ok") is False
    assert "params.degrees" in str(res.get("error") or "")


def test_orbit_ensures_t0_and_t1_keys_even_if_solver_returns_only_t0() -> None:
    client = _StubClient()
    client._solve_keys = [{"time": 0.0, "value": {"Eye Position Vec3": [0, 0, 1]}}]

    applied_times: list[float] = []

    def _dispatch(name: str, payload_json: str) -> str:
        assert name == "animation_replace_key_camera"
        payload = json.loads(payload_json)
        applied_times.append(float(payload["time"]))
        return json.dumps({"ok": True})

    ctx = _ctx_for(client, _dispatch)

    res = json.loads(
        handle(
            "animation_camera_solve_and_apply",
            {
                "animation_id": 1,
                "mode": "ORBIT",
                "ids": [123],
                "t0": 0.0,
                "t1": 1.0,
                "degrees": 0.0,
                "max_step_degrees": 20.0,
                "clear_range": False,
                "tolerance": 1e-3,
            },
            ctx,
        )
        or "{}"
    )

    # Solver must return endpoint keys; the tool should fail fast rather than
    # silently patching the timeline.
    assert res.get("ok") is False
    assert "t1" in str(res.get("error") or "") or "endpoint" in str(
        res.get("error") or ""
    )
    assert applied_times == []


def test_orbit_applies_all_solver_keys() -> None:
    client = _StubClient()
    client._solve_keys = [
        {"time": 0.0, "value": {"Eye Position Vec3": [0, 0, 1]}},
        {"time": 1.0, "value": {"Eye Position Vec3": [0, 0, 1]}},
    ]

    applied_times: list[float] = []

    def _dispatch(name: str, payload_json: str) -> str:
        assert name == "animation_replace_key_camera"
        payload = json.loads(payload_json)
        applied_times.append(float(payload["time"]))
        return json.dumps({"ok": True})

    ctx = _ctx_for(client, _dispatch)

    res = json.loads(
        handle(
            "animation_camera_solve_and_apply",
            {
                "animation_id": 1,
                "mode": "ORBIT",
                "ids": [123],
                "t0": 0.0,
                "t1": 1.0,
                "degrees": 0.0,
                "max_step_degrees": 20.0,
                "clear_range": False,
                "tolerance": 1e-3,
            },
            ctx,
        )
        or "{}"
    )

    assert res.get("ok") is True
    assert sorted(applied_times) == [0.0, 1.0]
