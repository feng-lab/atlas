from __future__ import annotations

import json
import sys
import textwrap
from pathlib import Path

import pytest

# Add src layout to sys.path for local runs
ROOT_DIR = Path(__file__).resolve().parents[2]
SRC_DIR = ROOT_DIR / 'src'
if str(SRC_DIR) not in sys.path:
    sys.path.insert(0, str(SRC_DIR))

from atlas_agent.agent_team.tools_agent import scene_tools_and_dispatcher  # type: ignore  # noqa: E402
from atlas_agent.session_store import SessionStore  # type: ignore  # noqa: E402


TESTABLE_TOOL_NAMES = [
    "report_blocked",
    "system_info",
    "update_plan",
    "session_info",
    "session_get_plan",
    "session_get_memory",
    "session_search_transcript",
    "session_search_events",
    "python_write_and_run",
    "scene_animation_concepts",
    "docs_list",
    "docs_search",
    "docs_read",
]


class DummySceneClient:
    """Minimal stub for dispatcher creation; raises if unexpected RPC is invoked."""

    def __getattr__(self, name: str):
        raise AssertionError(f"Unexpected SceneClient method call: {name}")


@pytest.fixture()
def tool_runtime():
    """Provide tool list and dispatcher with codegen pathway enabled."""

    tools, dispatch = scene_tools_and_dispatcher(
        DummySceneClient(),
        atlas_dir=None,
        codegen_enabled=True,
    )
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


def test_docs_list(tool_runtime):
    dispatch = tool_runtime["dispatch"]
    payload = _call(dispatch, "docs_list")
    assert payload["ok"] is True
    assert any(d.get("name") == "SCENE_SERVER.md" for d in payload.get("docs", []))


def test_docs_search_and_read(tool_runtime):
    dispatch = tool_runtime["dispatch"]
    found = _call(
        dispatch,
        "docs_search",
        query="gRPC",
        include_paths=["SCENE_SERVER.md"],
        max_results=5,
    )
    assert found["ok"] is True
    assert found["total_matches"] > 0
    excerpt = _call(
        dispatch,
        "docs_read",
        doc_name="SCENE_SERVER.md",
        start_line=0,
        line_count=8,
    )
    assert excerpt["ok"] is True
    assert "Atlas Scene RPC" in excerpt["text"]


def test_update_plan(tool_runtime):
    dispatch = tool_runtime["dispatch"]
    payload = _call(
        dispatch,
        "update_plan",
        explanation="initial plan",
        plan=[
            {"step": "Load files", "status": "completed"},
            {"step": "Create animation", "status": "in_progress"},
            {"step": "Export video", "status": "pending"},
        ],
    )
    assert payload["ok"] is True


def test_session_tools_with_persistent_store(tmp_path):
    store = SessionStore.open(session="test", session_dir=str(tmp_path / "sessions"))
    store.append_transcript(role="user", content="hello world")
    store.append_transcript(role="assistant", content="ack hello")
    store.set_memory_summary("- user goal: test session tools")
    store.update_plan(
        [
            {"step": "Step A", "status": "completed"},
            {"step": "Step B", "status": "in_progress"},
        ],
        explanation="test",
        source="llm",
    )
    store.save()

    tools, dispatch = scene_tools_and_dispatcher(
        DummySceneClient(), atlas_dir=None, session_store=store
    )
    available = {
        spec.get("function", {}).get("name")
        for spec in tools
        if isinstance(spec, dict)
    }
    assert "session_info" in available

    info = _call(dispatch, "session_info")
    assert info["ok"] is True
    assert info["session_id"]
    assert info["log_path"].endswith("session.jsonl")

    plan = _call(dispatch, "session_get_plan")
    assert plan["ok"] is True
    assert plan["plan_source"] == "llm"
    assert plan["plan_explanation"] == "test"
    assert len(plan["plan"]) == 2

    mem = _call(dispatch, "session_get_memory")
    assert mem["ok"] is True
    assert "user goal" in mem["memory_summary"]

    hits = _call(dispatch, "session_search_transcript", query="hello", max_results=0)
    assert hits["ok"] is True
    assert hits["total_matches"] == 2

    ev = _call(dispatch, "session_search_events", query="tool_call", max_results=0)
    assert ev["ok"] is True
