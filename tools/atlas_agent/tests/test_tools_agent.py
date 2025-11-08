from __future__ import annotations

import json
import sys
import textwrap
from pathlib import Path

import pytest

ROOT_DIR = Path(__file__).resolve().parents[3]
if str(ROOT_DIR) not in sys.path:
    sys.path.insert(0, str(ROOT_DIR))

from tools.atlas_agent.agent_team.tools_agent import scene_tools_and_dispatcher  # noqa: E402


TESTABLE_TOOL_NAMES = [
    "report_blocked",
    "system_info",
    "python_write_and_run",
    "scene_animation_concepts",
]


class DummySceneClient:
    """Minimal stub for dispatcher creation; raises if unexpected RPC is invoked."""

    def __getattr__(self, name: str):
        raise AssertionError(f"Unexpected SceneClient method call: {name}")


@pytest.fixture()
def tool_runtime(monkeypatch):
    """Provide tool list and dispatcher with codegen pathway enabled."""

    monkeypatch.setenv("ATLAS_AGENT_ENABLE_CODEGEN", "1")
    tools, dispatch = scene_tools_and_dispatcher(DummySceneClient(), atlas_dir=None)
    return {"tools": tools, "dispatch": dispatch}


def _call(dispatch, name: str, **kwargs):
    return json.loads(dispatch(name, json.dumps(kwargs)))


def test_expected_tools_advertised(tool_runtime):
    tools = tool_runtime["tools"]
    available = {
        spec.get("function", {}).get("name")
        for spec in tools
        if isinstance(spec, dict)
    }
    assert set(TESTABLE_TOOL_NAMES).issubset(available)


def test_report_blocked(tool_runtime):
    dispatch = tool_runtime["dispatch"]
    payload = _call(
        dispatch,
        "report_blocked",
        reason="needs_input",
        details="waiting for user selection",
        suggestion="provide file path",
    )
    assert payload["ok"] is True
    assert payload["reason"] == "needs_input"
    assert payload["details"].startswith("waiting")
    assert payload["suggestion"] == "provide file path"


def test_system_info_returns_expected_fields(tool_runtime):
    dispatch = tool_runtime["dispatch"]
    info = _call(dispatch, "system_info")
    assert "system" in info
    assert "home" in info
    assert "cwd" in info
    assert "common_dirs" in info


def test_scene_animation_concepts(tool_runtime):
    dispatch = tool_runtime["dispatch"]
    summary = _call(dispatch, "scene_animation_concepts")
    assert summary["ok"] is True
    text = summary["text"]
    assert "Scene (.scene)" in text
    assert "Animation (.animation2d/.animation3d)" in text


def test_python_write_and_run_executes_script(tool_runtime):
    dispatch = tool_runtime["dispatch"]
    script = textwrap.dedent(
        """
        import sys
        print("hello from tool")
        sys.exit(0)
        """
    ).lstrip()
    result = _call(
        dispatch,
        "python_write_and_run",
        script=script,
        filename="test_script.py",
        echo_script=True,
    )
    assert result["ok"] is True
    assert result["exit_code"] == 0
    assert "hello from tool" in result["stdout"]
    assert "test_script.py" in result["path"]
    assert "hello from tool" in result["script"]
