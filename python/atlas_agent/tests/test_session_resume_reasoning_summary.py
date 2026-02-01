import json

from atlas_agent.session_resume import iter_resume_items


def _write_jsonl(path, events: list[dict]) -> None:
    with open(path, "w", encoding="utf-8") as f:
        for ev in events:
            f.write(json.dumps(ev, ensure_ascii=False) + "\n")


def test_iter_resume_items_reasoning_summary_gated_by_flag(tmp_path):
    log_path = tmp_path / "session.jsonl"
    _write_jsonl(
        log_path,
        [
            {"type": "transcript", "role": "user", "content": "hi", "ts": 1.0},
            {
                "type": "reasoning_summary",
                "phase": "planner",
                "summaries": ["Plan: do X then verify Y."],
                "ts": 2.0,
            },
            {
                "type": "tool_call",
                "tool": "scene_list_objects",
                "args": {},
                "result_policy": "summary",
                "result_summary": {"ok": True},
                "ts": 3.0,
            },
            {
                "type": "plan_updated",
                "event_id": "p1",
                "plan": [{"step": "Do X", "status": "pending"}],
                "ts": 4.0,
            },
        ],
    )

    items = list(iter_resume_items(log_path))
    kinds = [it.kind for it in items]
    assert "reasoning_summary" not in kinds
    assert kinds == ["transcript", "tool_call", "plan"]

    items_with_summary = list(
        iter_resume_items(log_path, include_reasoning_summary=True)
    )
    kinds_with_summary = [it.kind for it in items_with_summary]
    assert kinds_with_summary == [
        "transcript",
        "reasoning_summary",
        "tool_call",
        "plan",
    ]


def test_iter_resume_items_includes_only_latest_plan_updated(tmp_path):
    log_path = tmp_path / "session.jsonl"
    _write_jsonl(
        log_path,
        [
            {"type": "transcript", "role": "user", "content": "hi", "ts": 1.0},
            {
                "type": "plan_updated",
                "event_id": "p_old",
                "plan": [{"step": "Old plan", "status": "pending"}],
                "ts": 2.0,
            },
            {
                "type": "transcript",
                "role": "assistant",
                "content": "ok",
                "ts": 3.0,
            },
            {
                "type": "plan_updated",
                "event_id": "p_new",
                "plan": [{"step": "New plan", "status": "completed"}],
                "ts": 4.0,
            },
        ],
    )

    items = list(iter_resume_items(log_path))
    kinds = [it.kind for it in items]
    assert kinds == ["transcript", "transcript", "plan"]
    assert items[-1].event.get("event_id") == "p_new"
