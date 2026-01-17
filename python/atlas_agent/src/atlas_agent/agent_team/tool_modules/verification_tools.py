import json
from typing import Any, Dict, List

from ...tool_registry import Tool, tool_from_schema
from .context import ToolDispatchContext
from .preconditions import require_session_store

VERIFICATION_GET_DESCRIPTION = "Return verification requirements + current verification status for the current plan steps."
VERIFICATION_GET_PARAMETERS: Dict[str, Any] = {
    "type": "object",
    "properties": {
        "include_plan": {
            "type": "boolean",
            "default": True,
            "description": "When true, include the current plan (step_id/step/status).",
        }
    },
    "additionalProperties": False,
}

VERIFICATION_SET_REQUIREMENTS_DESCRIPTION = (
    "Attach verification requirements to plan steps.\n"
    "Use this to express that a single plan step may require multiple verification modes.\n\n"
    "Policy format:\n"
    "  policy: {\n"
    "    all_of: [\n"
    "      { any_of: [\"tool\"], description: \"...\" },\n"
    "      { any_of: [\"visual\", \"human\"], description: \"...\" }\n"
    "    ]\n"
    "  }\n"
    "Meaning: all groups must be satisfied; each group is satisfied if any of its modes is PASS.\n"
)
VERIFICATION_SET_REQUIREMENTS_PARAMETERS: Dict[str, Any] = {
    "type": "object",
    "properties": {
        "steps": {
            "type": "array",
            "description": "Steps to update (by step_id preferred).",
            "items": {
                "type": "object",
                "properties": {
                    "step_id": {"type": ["string", "null"], "description": "Plan step id (preferred)."},
                    "step": {"type": ["string", "null"], "description": "Plan step text (fallback lookup if step_id omitted)."},
                    "policy": {
                        "type": "object",
                        "properties": {
                            "all_of": {
                                "type": "array",
                                "items": {
                                    "type": "object",
                                    "properties": {
                                        "any_of": {
                                            "type": "array",
                                            "items": {
                                                "type": "string",
                                                "enum": ["tool", "visual", "human"],
                                            },
                                        },
                                        "description": {"type": "string"},
                                    },
                                    "required": ["any_of"],
                                    "additionalProperties": False,
                                },
                            }
                        },
                        "required": ["all_of"],
                        "additionalProperties": False,
                    },
                    "notes": {
                        "type": ["string", "null"],
                        "description": "Optional note describing what this check covers or its limitations.",
                    },
                },
                "required": ["policy"],
                "additionalProperties": False,
            },
        }
    },
    "required": ["steps"],
    "additionalProperties": False,
}

VERIFICATION_RECORD_DESCRIPTION = (
    "Record a verification outcome for a plan step and mode.\n"
    "This appends deterministic evidence into the session events log and updates the current verification status.\n"
    "Use mode=tool for read-back checks, mode=visual for screenshot-based checks, and mode=human for explicit user confirmation."
)
VERIFICATION_RECORD_PARAMETERS: Dict[str, Any] = {
    "type": "object",
    "properties": {
        "step_id": {"type": ["string", "null"]},
        "step": {"type": ["string", "null"]},
        "mode": {"type": "string", "enum": ["tool", "visual", "human"]},
        "status": {"type": "string", "enum": ["pass", "fail", "unknown"]},
        "summary": {"type": "string", "description": "Short explanation of the outcome."},
        "ref": {
            "type": ["object", "null"],
            "description": "Optional reference to supporting evidence (paths/tool names/event ids).",
            "properties": {
                "tool": {"type": ["string", "null"]},
                "event_id": {"type": ["string", "null"]},
                "screenshot_path": {"type": ["string", "null"]},
            },
            "additionalProperties": False,
        },
    },
    "required": ["mode", "status", "summary"],
    "additionalProperties": False,
}

VERIFICATION_EVAL_PLAN_DESCRIPTION = "Evaluate whether each plan step's verification policy is satisfied by current verification statuses."
VERIFICATION_EVAL_PLAN_PARAMETERS: Dict[str, Any] = {
    "type": "object",
    "properties": {},
    "additionalProperties": False,
}


def _tool_handler(tool_name: str):
    def _call(args: dict[str, Any], ctx: ToolDispatchContext):
        return handle(tool_name, args, ctx)

    return _call


TOOLS: List[Tool] = [
    tool_from_schema(
        name="verification_get",
        description=VERIFICATION_GET_DESCRIPTION,
        parameters_schema=VERIFICATION_GET_PARAMETERS,
        handler=_tool_handler("verification_get"),
        preconditions=(require_session_store,),
    ),
    tool_from_schema(
        name="verification_set_requirements",
        description=VERIFICATION_SET_REQUIREMENTS_DESCRIPTION,
        parameters_schema=VERIFICATION_SET_REQUIREMENTS_PARAMETERS,
        handler=_tool_handler("verification_set_requirements"),
        preconditions=(require_session_store,),
    ),
    tool_from_schema(
        name="verification_record",
        description=VERIFICATION_RECORD_DESCRIPTION,
        parameters_schema=VERIFICATION_RECORD_PARAMETERS,
        handler=_tool_handler("verification_record"),
        preconditions=(require_session_store,),
    ),
    tool_from_schema(
        name="verification_eval_plan",
        description=VERIFICATION_EVAL_PLAN_DESCRIPTION,
        parameters_schema=VERIFICATION_EVAL_PLAN_PARAMETERS,
        handler=_tool_handler("verification_eval_plan"),
        preconditions=(require_session_store,),
    ),
]


def _resolve_step_id(*, ctx: ToolDispatchContext, step_id: Any, step: Any) -> str | None:
    sid = str(step_id or "").strip() if step_id is not None else ""
    if sid:
        return sid
    st = str(step or "").strip() if step is not None else ""
    if not st:
        return None
    # Look up by exact plan step text when possible.
    try:
        if ctx.session_store is not None:
            for it in ctx.session_store.get_plan() or []:
                if not isinstance(it, dict):
                    continue
                if str(it.get("step") or "").strip() == st:
                    got = str(it.get("step_id") or "").strip()
                    if got:
                        return got
    except Exception:
        pass
    return None


def handle(name: str, args: dict, ctx: ToolDispatchContext) -> str | None:
    ss = ctx.session_store
    if ss is None:
        return json.dumps({"ok": False, "error": "no session_store available"})

    if name == "verification_get":
        include_plan = bool(args.get("include_plan", True))
        try:
            payload: dict[str, Any] = {"ok": True, "verification": ss.get_verification()}
            if include_plan:
                payload["plan"] = ss.get_plan()
            return json.dumps(payload, ensure_ascii=False)
        except Exception as e:
            return json.dumps({"ok": False, "error": str(e)})

    if name == "verification_set_requirements":
        steps = args.get("steps")
        if not isinstance(steps, list):
            return json.dumps({"ok": False, "error": "steps must be an array"})

        updated: list[str] = []
        for ent in steps:
            if not isinstance(ent, dict):
                continue
            sid = _resolve_step_id(ctx=ctx, step_id=ent.get("step_id"), step=ent.get("step"))
            if not sid:
                return json.dumps(
                    {
                        "ok": False,
                        "error": "missing step_id (and could not resolve from step text)",
                    }
                )
            policy = ent.get("policy")
            notes = ent.get("notes")
            if not isinstance(policy, dict):
                return json.dumps({"ok": False, "error": "policy must be an object"})
            # Basic validation (defense-in-depth; full correctness is ensured by schema).
            all_of = policy.get("all_of")
            if not isinstance(all_of, list) or not all_of:
                return json.dumps({"ok": False, "error": "policy.all_of must be a non-empty array"})
            ss.set_verification_policy(
                step_id=sid,
                policy=policy,
                notes=str(notes).strip() if isinstance(notes, str) else None,
            )
            updated.append(sid)

        try:
            ss.save()
        except Exception:
            pass
        return json.dumps({"ok": True, "updated_step_ids": updated})

    if name == "verification_record":
        sid = _resolve_step_id(ctx=ctx, step_id=args.get("step_id"), step=args.get("step"))
        if not sid:
            return json.dumps({"ok": False, "error": "missing step_id (and could not resolve from step text)"})

        mode = str(args.get("mode") or "").strip().lower()
        status = str(args.get("status") or "").strip().lower()
        summary = str(args.get("summary") or "").strip()
        ref = args.get("ref")
        if not summary:
            return json.dumps({"ok": False, "error": "summary is required"})
        if mode not in ("tool", "visual", "human"):
            return json.dumps({"ok": False, "error": "mode must be tool|visual|human"})
        if status not in ("pass", "fail", "unknown"):
            return json.dumps({"ok": False, "error": "status must be pass|fail|unknown"})

        try:
            turn_id = None
            phase = None
            try:
                turn_id = ctx.runtime_state.get("turn_id")
                if not isinstance(turn_id, str) or not turn_id.strip():
                    turn_id = None
            except Exception:
                turn_id = None
            try:
                phase = ctx.runtime_state.get("phase")
                if not isinstance(phase, str) or not phase.strip():
                    phase = None
            except Exception:
                phase = None
            event_id = ss.record_verification_evidence(
                step_id=sid,
                mode=mode,  # type: ignore[arg-type]
                status=status,  # type: ignore[arg-type]
                summary=summary,
                ref=ref if isinstance(ref, dict) else None,
                turn_id=turn_id,
                phase=phase,
                source="llm",
            )
            ss.save()
            return json.dumps({"ok": True, "event_id": event_id, "step_id": sid})
        except Exception as e:
            return json.dumps({"ok": False, "error": str(e)})

    if name == "verification_eval_plan":
        plan = ss.get_plan() or []
        out: list[dict[str, Any]] = []
        for it in plan:
            if not isinstance(it, dict):
                continue
            sid = str(it.get("step_id") or "").strip()
            step = str(it.get("step") or "").strip()
            if not sid or not step:
                continue
            ev = ss.evaluate_verification_for_step(step_id=sid)
            out.append(
                {
                    "step_id": sid,
                    "step": step,
                    "plan_status": str(it.get("status") or ""),
                    "verification": ev,
                }
            )
        return json.dumps({"ok": True, "steps": out}, ensure_ascii=False)

    return None
