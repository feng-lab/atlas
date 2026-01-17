import json
from typing import Any, Dict, List, Optional

from ...tool_registry import Tool, tool_from_schema
from .context import ToolDispatchContext
from .preconditions import require_session_store

SESSION_INFO_DESCRIPTION = "Return current session identity + storage path (single session.jsonl log) and saved meta (atlas_dir, address, model)."
SESSION_INFO_PARAMETERS: Dict[str, Any] = {"type": "object", "properties": {}}

SESSION_GET_PLAN_DESCRIPTION = "Return the current task plan from the persistent session store (or runtime plan if no session is available)."
SESSION_GET_PLAN_PARAMETERS: Dict[str, Any] = {
    "type": "object",
    "properties": {
        "include_explanation": {
            "type": "boolean",
            "default": True,
            "description": "When true, include plan_source and plan_explanation when available.",
        }
    },
}

SESSION_GET_MEMORY_DESCRIPTION = "Return the current Session Memory summary string (durable cross-turn context)."
SESSION_GET_MEMORY_PARAMETERS: Dict[str, Any] = {"type": "object", "properties": {}}

SESSION_SEARCH_TRANSCRIPT_DESCRIPTION = "Search the full append-only session transcript (no truncation by default). Returns matching entries with timestamps/roles. For large transcripts, scope the query or set max_results to bound output."
SESSION_SEARCH_TRANSCRIPT_PARAMETERS: Dict[str, Any] = {
    "type": "object",
    "properties": {
        "query": {"type": "string", "description": "Substring or regex to match."},
        "regex": {
            "type": "boolean",
            "default": False,
            "description": "When true, treat query as a regex pattern.",
        },
        "case_sensitive": {
            "type": "boolean",
            "default": False,
            "description": "Case sensitive match when true.",
        },
        "role": {
            "type": ["string", "null"],
            "description": "Optional role filter (e.g., 'user' or 'assistant').",
        },
        "max_results": {
            "type": "integer",
            "default": 0,
            "description": "0=unlimited (correctness-first). If >0, returns only a window and sets limit_reached=true.",
        },
        "offset": {
            "type": "integer",
            "default": 0,
            "description": "Skip the first N matches (for paging). Use with max_results to page without truncation.",
        },
        "reverse": {
            "type": "boolean",
            "default": False,
            "description": "When true, return newest-first rather than oldest-first.",
        },
    },
    "required": ["query"],
}

SESSION_SEARCH_EVENTS_DESCRIPTION = "Search the structured events log (tool calls + facts snapshots). Useful for recalling exact previous tool calls beyond the current model context window."
SESSION_SEARCH_EVENTS_PARAMETERS: Dict[str, Any] = {
    "type": "object",
    "properties": {
        "query": {
            "type": "string",
            "description": "Substring or regex to match (searches serialized event JSON).",
        },
        "regex": {
            "type": "boolean",
            "default": False,
            "description": "When true, treat query as a regex pattern.",
        },
        "case_sensitive": {
            "type": "boolean",
            "default": False,
            "description": "Case sensitive match when true.",
        },
        "event_type": {
            "type": ["string", "null"],
            "description": "Optional event type filter (e.g., 'tool_call', 'facts_snapshot').",
        },
        "tool": {
            "type": ["string", "null"],
            "description": "Optional tool filter for tool_call events (exact match).",
        },
        "max_results": {
            "type": "integer",
            "default": 0,
            "description": "0=unlimited (correctness-first). If >0, returns only a window and sets limit_reached=true.",
        },
        "offset": {
            "type": "integer",
            "default": 0,
            "description": "Skip the first N matches (for paging). Use with max_results to page without truncation.",
        },
        "reverse": {
            "type": "boolean",
            "default": False,
            "description": "When true, return newest-first rather than oldest-first.",
        },
    },
    "required": ["query"],
}

SESSION_TAIL_EVENTS_DESCRIPTION = "Return the last N matching events (default excludes transcript). Useful for quick 'what just happened' recall without crafting a search query."
SESSION_TAIL_EVENTS_PARAMETERS: Dict[str, Any] = {
    "type": "object",
    "properties": {
        "limit": {
            "type": "integer",
            "default": 20,
            "description": "Number of events to return from the end of the log.",
        },
        "event_type": {
            "type": ["string", "null"],
            "description": "Optional event type filter (e.g., 'tool_call', 'plan_updated'). Use 'transcript' to include transcript entries explicitly.",
        },
        "tool": {
            "type": ["string", "null"],
            "description": "Optional tool filter for tool_call events (exact match).",
        },
        "reverse": {
            "type": "boolean",
            "default": False,
            "description": "When true, return newest-first.",
        },
    },
}


def _tool_handler(tool_name: str):
    def _call(args: dict[str, Any], ctx: ToolDispatchContext):
        return handle(tool_name, args, ctx)

    return _call


TOOLS: List[Tool] = [
    tool_from_schema(
        name="session_info",
        description=SESSION_INFO_DESCRIPTION,
        parameters_schema=SESSION_INFO_PARAMETERS,
        handler=_tool_handler("session_info"),
        preconditions=(require_session_store,),
    ),
    tool_from_schema(
        name="session_get_plan",
        description=SESSION_GET_PLAN_DESCRIPTION,
        parameters_schema=SESSION_GET_PLAN_PARAMETERS,
        handler=_tool_handler("session_get_plan"),
    ),
    tool_from_schema(
        name="session_get_memory",
        description=SESSION_GET_MEMORY_DESCRIPTION,
        parameters_schema=SESSION_GET_MEMORY_PARAMETERS,
        handler=_tool_handler("session_get_memory"),
    ),
    tool_from_schema(
        name="session_search_transcript",
        description=SESSION_SEARCH_TRANSCRIPT_DESCRIPTION,
        parameters_schema=SESSION_SEARCH_TRANSCRIPT_PARAMETERS,
        handler=_tool_handler("session_search_transcript"),
        preconditions=(require_session_store,),
    ),
    tool_from_schema(
        name="session_search_events",
        description=SESSION_SEARCH_EVENTS_DESCRIPTION,
        parameters_schema=SESSION_SEARCH_EVENTS_PARAMETERS,
        handler=_tool_handler("session_search_events"),
        preconditions=(require_session_store,),
    ),
    tool_from_schema(
        name="session_tail_events",
        description=SESSION_TAIL_EVENTS_DESCRIPTION,
        parameters_schema=SESSION_TAIL_EVENTS_PARAMETERS,
        handler=_tool_handler("session_tail_events"),
        preconditions=(require_session_store,),
    ),
]


def _runtime_plan(ctx: ToolDispatchContext) -> list[dict]:
    try:
        v = ctx.runtime_state.get("plan")
        return list(v) if isinstance(v, list) else []
    except Exception:
        return []


def _runtime_plan_explanation(ctx: ToolDispatchContext) -> str:
    try:
        s = ctx.runtime_state.get("plan_explanation")
        return str(s) if isinstance(s, str) else ""
    except Exception:
        return ""


def handle(name: str, args: dict, ctx: ToolDispatchContext) -> str | None:
    ss = ctx.session_store

    if name == "session_info":
        if ss is None:
            return json.dumps({"ok": False, "error": "no session_store available"})
        try:
            payload = {
                "ok": True,
                "session_id": ss.session_id(),
                "root": str(ss.root),
                "log_path": str(ss.log_path),
                "meta": ss.get_meta(),
            }
            return json.dumps(payload, ensure_ascii=False)
        except Exception as e:
            return json.dumps({"ok": False, "error": str(e)})

    if name == "session_get_plan":
        include_explanation = bool(args.get("include_explanation", True))
        try:
            if ss is not None:
                payload: dict[str, Any] = {"ok": True, "plan": ss.get_plan()}
                if include_explanation:
                    payload["plan_source"] = ss.plan_source()
                    payload["plan_explanation"] = ss.plan_explanation()
                return json.dumps(payload, ensure_ascii=False)
            # No session store: fall back to runtime state
            payload = {"ok": True, "plan": _runtime_plan(ctx)}
            if include_explanation:
                payload["plan_source"] = "runtime"
                payload["plan_explanation"] = _runtime_plan_explanation(ctx)
            return json.dumps(payload, ensure_ascii=False)
        except Exception as e:
            return json.dumps({"ok": False, "error": str(e)})

    if name == "session_get_memory":
        try:
            mem = ss.get_memory_summary() if ss is not None else ""
            return json.dumps({"ok": True, "memory_summary": mem}, ensure_ascii=False)
        except Exception as e:
            return json.dumps({"ok": False, "error": str(e)})

    if name == "session_search_transcript":
        if ss is None:
            return json.dumps({"ok": False, "error": "no session_store available"})
        payload = ss.search_transcript(
            query=str(args.get("query") or ""),
            regex=bool(args.get("regex", False)),
            case_sensitive=bool(args.get("case_sensitive", False)),
            role=args.get("role"),
            max_results=int(args.get("max_results", 0) or 0),
            offset=int(args.get("offset", 0) or 0),
            reverse=bool(args.get("reverse", False)),
        )
        return json.dumps(payload, ensure_ascii=False)

    if name == "session_search_events":
        if ss is None:
            return json.dumps({"ok": False, "error": "no session_store available"})
        query = str(args.get("query") or "")
        if not query:
            return json.dumps({"ok": False, "error": "query is required"})
        payload = ss.search_events(
            query=query,
            regex=bool(args.get("regex", False)),
            case_sensitive=bool(args.get("case_sensitive", False)),
            event_type=args.get("event_type"),
            tool=args.get("tool"),
            max_results=int(args.get("max_results", 0) or 0),
            offset=int(args.get("offset", 0) or 0),
            reverse=bool(args.get("reverse", False)),
        )
        return json.dumps(payload, ensure_ascii=False)

    if name == "session_tail_events":
        if ss is None:
            return json.dumps({"ok": False, "error": "no session_store available"})
        try:
            limit = int(args.get("limit", 20) or 0)
        except Exception:
            limit = 20
        try:
            reverse = bool(args.get("reverse", False))
        except Exception:
            reverse = False
        events = ss.tail_events(
            limit=max(0, limit),
            event_type=args.get("event_type"),
            tool=args.get("tool"),
        )
        if reverse:
            events = list(reversed(events))
        return json.dumps(
            {
                "ok": True,
                "session_id": ss.session_id(),
                "log_path": str(ss.log_path),
                "results": events,
            },
            ensure_ascii=False,
        )

    return None
