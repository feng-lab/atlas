import json

from atlas_agent.agent_team.tool_modules.context import ToolDispatchContext
from atlas_agent.agent_team.tool_modules import fs_tools


def _dummy_ctx() -> ToolDispatchContext:
    return ToolDispatchContext(
        client=None,  # fs tools do not use the SceneClient
        atlas_dir=None,
        codegen_enabled=False,
        dispatch=lambda _name, _args: "",
        param_to_dict=lambda _p: {},
        resolve_json_key=lambda _id, _json_key, _display_name: None,
        json_key_exists=lambda _id, _json_key: False,
        schema_validator_cache={},
        session_store=None,
        runtime_state={},
    )


def test_fs_hint_resolve_exact_match(tmp_path, monkeypatch):
    # Keep the search deterministic and fast by redirecting "~" expansion into tmp_path.
    monkeypatch.setenv("HOME", str(tmp_path))

    docs = tmp_path / "Documents"
    (docs / "atlas_test" / "slice15").mkdir(parents=True)
    (tmp_path / "Downloads").mkdir(parents=True)
    (tmp_path / "Desktop").mkdir(parents=True)

    actual = docs / "atlas_test" / "slice15" / "atlas_agent_unit_test__nim_stitching_log.txt"
    actual.write_text("hello", encoding="utf-8")

    res = fs_tools.handle(
        "fs_hint_resolve",
        {
            "expected_name": "atlas_agent_unit_test__nim_stitching_log.txt",
            "possible_dirs": ["~/Documents/atlas_test"],
            "kind": "file",
            "max_depth": 4,
            "max_results": 10,
        },
        _dummy_ctx(),
    )
    assert res is not None
    parsed = json.loads(res)
    assert parsed["ok"] is True
    assert parsed["match"] == "exact"
    assert parsed["path"] == str(actual)
    assert parsed["expected_name"] == "atlas_agent_unit_test__nim_stitching_log.txt"
    assert parsed["searched_dirs"] == [str(docs / "atlas_test")]
    assert parsed["missing_dirs"] == []


def test_fs_hint_resolve_best_candidate_when_name_is_close(tmp_path, monkeypatch):
    monkeypatch.setenv("HOME", str(tmp_path))

    docs = tmp_path / "Documents"
    (docs / "atlas_test" / "slice15").mkdir(parents=True)
    (tmp_path / "Downloads").mkdir(parents=True)
    (tmp_path / "Desktop").mkdir(parents=True)

    actual = docs / "atlas_test" / "slice15" / "atlas_agent_unit_test__nim_stitching_log.txt"
    actual.write_text("hello", encoding="utf-8")

    res = fs_tools.handle(
        "fs_hint_resolve",
        {
            "expected_name": "atlas_agent_unit_test__stitching_log.txt",
            "possible_dirs": ["~/Documents/atlas_test"],
            "kind": "file",
            "max_depth": 4,
            "max_results": 10,
        },
        _dummy_ctx(),
    )
    assert res is not None
    parsed = json.loads(res)
    assert parsed["ok"] is True
    assert parsed["match"] == "best_candidate"
    assert parsed["expected_name"] == "atlas_agent_unit_test__stitching_log.txt"
    assert parsed["path"] == str(actual)
    assert "hint" in parsed


def test_fs_hint_resolve_not_found_when_dirs_missing(tmp_path, monkeypatch):
    monkeypatch.setenv("HOME", str(tmp_path))
    (tmp_path / "Documents").mkdir(parents=True)

    res = fs_tools.handle(
        "fs_hint_resolve",
        {
            "expected_name": "does_not_exist.txt",
            "possible_dirs": ["~/Documents/atlas_test"],
            "kind": "file",
            "max_depth": 2,
            "max_results": 10,
        },
        _dummy_ctx(),
    )
    assert res is not None
    parsed = json.loads(res)
    assert parsed["ok"] is False
    assert parsed["error"] == "not_found"
    assert parsed["searched_dirs"] == []
    assert parsed["missing_dirs"]
