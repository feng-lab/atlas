import json
from typing import Any, Dict, List

from ...tool_registry import Tool, tool_from_schema
from .context import ToolDispatchContext

UPDATE_PLAN_DESCRIPTION = (
    "Updates the task plan.\n"
    "Provide an optional explanation and a list of plan items, each with a step and status.\n"
    "At most one step can be in_progress at a time."
)

UPDATE_PLAN_PARAMETERS: Dict[str, Any] = {
    "type": "object",
    "properties": {
        "explanation": {
            "type": "string",
            "description": "Optional explanation of why the plan changed.",
        },
        "plan": {
            "type": "array",
            "description": "The list of steps.",
            "items": {
                "type": "object",
                "properties": {
                    "step_id": {
                        "type": "string",
                        "description": "Optional stable id for linking verification/evidence across turns.",
                    },
                    "step": {"type": "string"},
                    "status": {
                        "type": "string",
                        "description": "One of: pending, in_progress, completed",
                    },
                },
                "required": ["step", "status"],
                "additionalProperties": False,
            },
        },
    },
    "required": ["plan"],
    "additionalProperties": False,
}


def _tool_handler(tool_name: str):
    def _call(args: dict[str, Any], ctx: ToolDispatchContext):
        return handle(tool_name, args, ctx)

    return _call


TOOLS: List[Tool] = [
    tool_from_schema(
        name="update_plan",
        description=UPDATE_PLAN_DESCRIPTION,
        parameters_schema=UPDATE_PLAN_PARAMETERS,
        handler=_tool_handler("update_plan"),
    )
]


def handle(name: str, args: dict, ctx: ToolDispatchContext) -> str | None:
    if name != "update_plan":
        return None

    raw_plan = args.get("plan")
    explanation = args.get("explanation")
    if not isinstance(raw_plan, list):
        return json.dumps({"ok": False, "error": "plan must be an array"})

    normalized: list[dict[str, str]] = []
    in_progress = 0
    for it in raw_plan:
        if not isinstance(it, dict):
            continue
        step_id = str(it.get("step_id") or "").strip()
        step = str(it.get("step") or "").strip()
        status = str(it.get("status") or "").strip()
        if not step:
            continue
        if status not in ("pending", "in_progress", "completed"):
            return json.dumps(
                {
                    "ok": False,
                    "error": f"invalid status '{status}' (must be pending|in_progress|completed)",
                }
            )
        if status == "in_progress":
            in_progress += 1
        entry: dict[str, str] = {"step": step, "status": status}
        if step_id:
            entry["step_id"] = step_id
        normalized.append(entry)

    if in_progress > 1:
        return json.dumps(
            {"ok": False, "error": "at most one plan item may be in_progress"}
        )

    # Persist into session store when available; otherwise stash into runtime_state.
    try:
        if ctx.session_store is not None:
            ctx.session_store.update_plan(
                normalized,
                explanation=str(explanation) if isinstance(explanation, str) else None,
                source="llm",
            )
            ctx.session_store.save()
        else:
            ctx.runtime_state["plan"] = normalized
            if isinstance(explanation, str):
                ctx.runtime_state["plan_explanation"] = explanation
    except Exception as e:
        return json.dumps({"ok": False, "error": str(e)})

    # Tool output is intentionally minimal; UIs render the plan from the session store.
    return json.dumps({"ok": True, "message": "Plan updated"})
