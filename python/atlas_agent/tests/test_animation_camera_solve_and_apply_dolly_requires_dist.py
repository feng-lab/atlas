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
        self.solve_called = False

    def set_camera_interpolation_method(
        self, *, animation_id: int, method: str
    ) -> None:
        # Tool enforces this best-effort; no-op for tests.
        return None

    def camera_solve(self, **kwargs):  # type: ignore[no-untyped-def]
        self.solve_called = True
        # Provide endpoint keys so the handler would proceed if called.
        return [
            {"time": 0.0, "value": {"Eye Position Vec3": [0, 0, 1]}},
            {"time": 1.0, "value": {"Eye Position Vec3": [0, 0, 1]}},
        ]


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


def test_dolly_requires_start_or_end_distance() -> None:
    client = _StubClient()
    ctx = _ctx_for(client)

    res = json.loads(
        handle(
            "animation_camera_solve_and_apply",
            {
                "animation_id": 1,
                "mode": "DOLLY",
                "ids": [123],
                "t0": 0.0,
                "t1": 1.0,
                # Missing params.start_dist/end_dist should error (otherwise DOLLY is a no-op).
                "params": {},
                "clear_range": False,
            },
            ctx,
        )
        or "{}"
    )

    assert res.get("ok") is False
    assert "start_dist" in str(res.get("error") or "") or "end_dist" in str(
        res.get("error") or ""
    )
    assert client.solve_called is False
