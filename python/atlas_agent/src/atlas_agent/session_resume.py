from __future__ import annotations

import json
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Any, Iterable, Iterator

from .defaults import RESUME_SESSION_PICKER_PREVIEW_MAX_CHARS


@dataclass(frozen=True)
class ResumeItem:
    """A renderable item derived from a session.jsonl log.

    This is used only for *terminal replay* on resume (UX), not for model input.
    """

    kind: str  # "transcript" | "tool_call" | "web_search" | "plan"
    ts: float | None
    event: dict[str, Any]


@dataclass(frozen=True)
class SessionListItem:
    session_id: str
    root: Path
    log_path: Path
    updated_at: float | None
    first_user_preview: str

    def updated_local_time(self) -> str:
        if not isinstance(self.updated_at, (int, float)):
            return ""
        try:
            return datetime.fromtimestamp(float(self.updated_at)).strftime(
                "%Y-%m-%d %H:%M:%S"
            )
        except Exception:
            return ""


def _iter_log_events(log_path: Path) -> Iterator[dict[str, Any]]:
    if not isinstance(log_path, Path):
        return
        yield  # pragma: no cover
    if not log_path.exists():
        return
        yield  # pragma: no cover
    try:
        with open(log_path, "r", encoding="utf-8", errors="replace") as f:
            for ln in f:
                try:
                    ev = json.loads(ln)
                except Exception:
                    continue
                if isinstance(ev, dict):
                    yield ev
    except Exception:
        return


def list_sessions(
    *,
    sessions_root: Path,
    preview_max_chars: int = RESUME_SESSION_PICKER_PREVIEW_MAX_CHARS,
) -> list[SessionListItem]:
    """List available sessions under a sessions root dir.

    preview_max_chars applies only to the 1-line preview shown in pickers.
    The full transcript is still available via resume replay.
    """

    root = sessions_root if isinstance(sessions_root, Path) else None
    if root is None or not root.exists():
        return []

    items: list[SessionListItem] = []
    try:
        for p in root.iterdir():
            if not p.is_dir():
                continue
            lp = p / "session.jsonl"
            upd: float | None = None
            try:
                upd = (
                    float(lp.stat().st_mtime)
                    if lp.exists()
                    else float(p.stat().st_mtime)
                )
            except Exception:
                upd = None

            preview = ""
            if lp.exists():
                try:
                    for ev in _iter_log_events(lp):
                        if str(ev.get("type") or "") != "transcript":
                            continue
                        if str(ev.get("role") or "") != "user":
                            continue
                        content = str(ev.get("content") or "")
                        content = " ".join(content.split())
                        if not content:
                            continue
                        if int(preview_max_chars) > 0 and len(content) > int(
                            preview_max_chars
                        ):
                            preview = content[: int(preview_max_chars)].rstrip() + "…"
                        else:
                            preview = content
                        break
                except Exception:
                    preview = ""

            items.append(
                SessionListItem(
                    session_id=p.name,
                    root=p,
                    log_path=lp,
                    updated_at=upd,
                    first_user_preview=preview,
                )
            )
    except Exception:
        return []

    items.sort(key=lambda it: float(it.updated_at or 0.0), reverse=True)
    return items


def iter_resume_items(log_path: Path) -> Iterable[ResumeItem]:
    """Iterate session events for resume replay, in session-log order.

    Policy:
    - Includes all transcript entries (user + assistant).
    - Includes all tool calls as single entries (the UI prints one line per tool call).
    - Includes *only the latest* plan_updated payload (current plan), and inserts it at
      its natural position in the event stream (where it occurred in the log).
    - Skips task-brief events by design (they are internal-facing).
    """

    last_plan_event_id: str | None = None
    for ev in _iter_log_events(log_path):
        if str(ev.get("type") or "") == "plan_updated":
            eid = ev.get("event_id")
            if isinstance(eid, str) and eid.strip():
                last_plan_event_id = eid.strip()

    for ev in _iter_log_events(log_path):
        et = str(ev.get("type") or "")
        ts = ev.get("ts")
        ts_f = float(ts) if isinstance(ts, (int, float)) else None

        if et == "transcript":
            yield ResumeItem(kind="transcript", ts=ts_f, event=ev)
            continue

        if et == "tool_call":
            yield ResumeItem(kind="tool_call", ts=ts_f, event=ev)
            continue

        if et == "web_search":
            yield ResumeItem(kind="web_search", ts=ts_f, event=ev)
            continue

        if et == "plan_updated":
            eid = ev.get("event_id")
            if (
                last_plan_event_id is not None
                and isinstance(eid, str)
                and eid.strip() == last_plan_event_id
            ):
                yield ResumeItem(kind="plan", ts=ts_f, event=ev)
            continue

        # Skip internal-only items that are noisy in the terminal replay.
        if et in {"task_brief", "task_brief_clarify_suppressed"}:
            continue


def _extract_ok_error_from_tool_event(ev: dict[str, Any]) -> tuple[bool | None, str]:
    if not isinstance(ev, dict):
        return (None, "")
    policy = str(ev.get("result_policy") or "summary").strip().lower()
    result = ev.get("result") if policy == "full" else ev.get("result_summary")
    if not isinstance(result, dict):
        return (None, "")
    ok = result.get("ok")
    ok_b = bool(ok) if isinstance(ok, bool) else None
    err = str(result.get("error") or "")
    if not err:
        err = str(result.get("reason") or "")
    return (ok_b, err.strip())


def _format_tool_args_preview(args: Any) -> str:
    """Compact, one-line args preview for tool-call summaries.

    This intentionally does not attempt to print full args (which can be large).
    """

    if not isinstance(args, dict) or not args:
        return ""

    parts: list[str] = []
    for k in (
        "id",
        "animation_id",
        "seconds",
        "time",
        "duration",
        "path",
        "relative_path",
        "expected_name",
        "name",
        "type",
    ):
        v = args.get(k)
        if v is None:
            continue
        if isinstance(v, str):
            vv = v.strip()
            if not vv:
                continue
            parts.append(f"{k}={vv!r}")
            continue
        if isinstance(v, (int, float, bool)):
            parts.append(f"{k}={v!r}")
            continue

    # Count-only summaries for potentially large collections.
    for k in ("ids", "json_keys", "paths", "times"):
        v = args.get(k)
        if isinstance(v, list):
            parts.append(f"{k}={len(v)}")

    return (" " + " ".join(parts)) if parts else ""


def format_tool_call_summary_line(ev: dict[str, Any]) -> str:
    """Format a tool_call event as a single human-readable line."""

    tool = str(ev.get("tool") or "").strip() or "<unknown>"
    args = ev.get("args")
    ok, err = _extract_ok_error_from_tool_event(ev)
    args_preview = _format_tool_args_preview(args)

    if ok is True:
        return f"→ {tool}{args_preview}: ok"
    if ok is False:
        suffix = f": fail {err}" if err else ": fail"
        return f"→ {tool}{args_preview}{suffix}"
    # Unknown/neutral.
    return f"→ {tool}{args_preview}: done"


def format_web_search_summary_line(ev: dict[str, Any]) -> str:
    """Format a web_search event as a single human-readable line."""

    if not isinstance(ev, dict):
        return "→ web_search"
    query = ev.get("query")
    url = ev.get("url")
    pattern = ev.get("pattern")
    parts: list[str] = []
    for k, v in (("query", query), ("url", url), ("pattern", pattern)):
        if isinstance(v, str) and v.strip():
            parts.append(f"{k}={v.strip()!r}")
    suffix = (" " + " ".join(parts)) if parts else ""
    return f"→ web_search{suffix}"
