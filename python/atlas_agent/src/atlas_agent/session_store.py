from __future__ import annotations

import json
import os
import time
import uuid
import hashlib
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Literal, Optional

PlanStatus = Literal["pending", "in_progress", "completed"]
VerificationMode = Literal["tool", "visual", "human"]
VerificationStatus = Literal["unknown", "pass", "fail"]
ConsentName = Literal["screenshots"]


def _now_unix() -> float:
    return float(time.time())


def _expand_path(s: str) -> Path:
    return Path(os.path.expanduser(os.path.expandvars(str(s)))).resolve()


def default_sessions_root() -> Path:
    # Prefer a state dir when available; fall back to a dotfolder.
    if os.name == "nt":
        base = os.environ.get("APPDATA")
        if base:
            return _expand_path(os.path.join(base, "atlas_agent", "sessions"))
        return _expand_path(str(Path.home() / "AppData" / "Roaming" / "atlas_agent" / "sessions"))
    xdg_state = os.environ.get("XDG_STATE_HOME")
    if xdg_state:
        return _expand_path(os.path.join(xdg_state, "atlas_agent", "sessions"))
    return _expand_path(str(Path.home() / ".atlas_agent" / "sessions"))


def _default_verification_state() -> dict[str, Any]:
    # Keep this small and stable; detailed evidence is stored in the append-only session log.
    return {"version": 1, "steps": {}}


def _make_step_id(step_text: str) -> str:
    # Deterministic id derived from step text (stable across turns). This is not a security
    # boundary; it is only for linking plan steps to verification metadata.
    h = hashlib.sha256(str(step_text or "").encode("utf-8", errors="replace")).hexdigest()
    return "p_" + h[:16]


@dataclass
class SessionStore:
    """On-disk session store for Atlas Agent.

    Layout:
      <root>/
        session.jsonl         (single append-only JSONL log, event-sourced "rollout")
    """

    root: Path
    log_path: Path
    _state: dict[str, Any]

    @classmethod
    def open(
        cls,
        *,
        session: str | None,
        session_dir: str | None = None,
    ) -> "SessionStore":
        # Resolve sessions root
        sessions_root = _expand_path(session_dir) if session_dir else default_sessions_root()
        sessions_root.mkdir(parents=True, exist_ok=True)

        def _looks_like_path(value: str) -> bool:
            if not value:
                return False
            if value.startswith(("/", "\\", ".")):
                return True
            return (os.sep in value) or ("/" in value) or ("\\" in value)

        # Resolve session root
        if session and _looks_like_path(session):
            root = _expand_path(session)
            # Allow passing a direct session.jsonl path
            if root.is_file() and root.name.lower().endswith(".jsonl"):
                root = root.parent
        elif session and session.strip():
            # Treat as a stable id (directory name)
            root = sessions_root / session.strip()
        else:
            root = sessions_root / uuid.uuid4().hex

        root.mkdir(parents=True, exist_ok=True)
        log_path = root / "session.jsonl"
        store = cls(
            root=root,
            log_path=log_path,
            _state={},
        )
        store.load()
        return store

    def _init_empty_state(self) -> None:
        self._state = {
            "version": 1,
            "session_id": self.root.name,
            "created_at": None,
            "updated_at": None,
            "meta": {},
            "memory_summary": "",
            "todo_ledger": [],
            "plan": [],
            "plan_source": "todos",  # "todos" | "llm" | "runtime"
            "plan_explanation": "",
            "verification": _default_verification_state(),
        }

    def _apply_meta_fields(self, fields: dict[str, Any]) -> None:
        meta = self._state.get("meta")
        if not isinstance(meta, dict):
            meta = {}
            self._state["meta"] = meta
        for k, v in (fields or {}).items():
            if v is None:
                continue
            meta[str(k)] = v

    def _apply_consent(self, name: str, allowed: Any) -> None:
        meta = self._state.get("meta")
        if not isinstance(meta, dict):
            meta = {}
            self._state["meta"] = meta
        consent = meta.get("consent")
        if not isinstance(consent, dict):
            consent = {}
            meta["consent"] = consent
        if name:
            consent[str(name)] = bool(allowed)

    def _apply_plan_from_event(self, plan: Any, *, explanation: Any, source: Any) -> None:
        raw = plan if isinstance(plan, list) else []
        normalized: list[dict[str, Any]] = []
        in_progress = 0
        used_ids: set[str] = set()
        for it in raw:
            if not isinstance(it, dict):
                continue
            step = str(it.get("step") or "").strip()
            status = str(it.get("status") or "pending").strip()
            step_id = str(it.get("step_id") or "").strip()
            if not step:
                continue
            if not step_id:
                step_id = _make_step_id(step)
            if step_id in used_ids:
                suffix = 2
                while f"{step_id}_{suffix}" in used_ids:
                    suffix += 1
                step_id = f"{step_id}_{suffix}"
            if status not in ("pending", "in_progress", "completed"):
                status = "pending"
            if status == "in_progress":
                in_progress += 1
            normalized.append({"step_id": step_id, "step": step, "status": status})
            used_ids.add(step_id)

        # Enforce plan invariant: at most one in_progress.
        if in_progress > 1:
            seen = 0
            for it in normalized:
                if it["status"] == "in_progress":
                    seen += 1
                    if seen > 1:
                        it["status"] = "pending"

        self._state["plan"] = normalized
        self._state["plan_source"] = str(source or "llm")
        if explanation is not None:
            self._state["plan_explanation"] = str(explanation or "")
        # Ensure verification entries exist (do not overwrite existing evidence).
        self._ensure_verification_entries_for_plan(normalized)

    def _apply_verification_policy_set(self, *, step_id: str, policy: Any, notes: Any) -> None:
        sid = str(step_id or "").strip()
        if not sid:
            return
        ver = self._state.get("verification")
        if not isinstance(ver, dict):
            ver = _default_verification_state()
            self._state["verification"] = ver
        steps = ver.get("steps")
        if not isinstance(steps, dict):
            steps = {}
            ver["steps"] = steps
        entry = steps.get(sid)
        if not isinstance(entry, dict):
            entry = {}
            steps[sid] = entry
        entry["policy"] = policy if isinstance(policy, dict) else policy
        if isinstance(notes, str) and notes.strip():
            entry["notes"] = notes.strip()
        entry.setdefault("status", {"tool": "unknown", "visual": "unknown", "human": "unknown"})
        entry.setdefault("last_evidence_event_id", {"tool": None, "visual": None, "human": None})

    def _apply_verification_evidence_event(self, ev: dict[str, Any]) -> None:
        sid = str(ev.get("step_id") or "").strip()
        mode = str(ev.get("mode") or "").strip().lower()
        status = str(ev.get("status") or "").strip().lower()
        if not sid or mode not in ("tool", "visual", "human") or status not in ("unknown", "pass", "fail"):
            return

        ver = self._state.get("verification")
        if not isinstance(ver, dict):
            ver = _default_verification_state()
            self._state["verification"] = ver
        steps = ver.get("steps")
        if not isinstance(steps, dict):
            steps = {}
            ver["steps"] = steps
        entry = steps.get(sid)
        if not isinstance(entry, dict):
            entry = {}
            steps[sid] = entry
        st_map = entry.get("status")
        if not isinstance(st_map, dict):
            st_map = {"tool": "unknown", "visual": "unknown", "human": "unknown"}
            entry["status"] = st_map
        st_map[mode] = status

        last = entry.get("last_evidence_event_id")
        if not isinstance(last, dict):
            last = {"tool": None, "visual": None, "human": None}
            entry["last_evidence_event_id"] = last
        last[mode] = ev.get("event_id")

    def _apply_event_to_state(self, ev: dict[str, Any]) -> None:
        et = str(ev.get("type") or "")
        if et == "meta_set":
            fields = ev.get("fields")
            if isinstance(fields, dict):
                self._apply_meta_fields(fields)
            return
        if et == "consent":
            self._apply_consent(str(ev.get("consent") or ""), ev.get("allowed"))
            return
        if et == "memory_summary_set":
            self._state["memory_summary"] = str(ev.get("memory_summary") or "")
            return
        if et == "todo_ledger_set":
            ledger = ev.get("ledger")
            if isinstance(ledger, list):
                self._state["todo_ledger"] = list(ledger)
                # Keep derived plan behavior consistent with runtime.
                if str(self._state.get("plan_source") or "") != "llm":
                    self._derive_plan_from_todos()
                else:
                    try:
                        self._sync_llm_plan_with_todos()
                    except Exception:
                        pass
            return
        if et == "plan_updated":
            self._apply_plan_from_event(
                ev.get("plan"),
                explanation=ev.get("explanation"),
                source=ev.get("source"),
            )
            return
        if et == "verification_policy_set":
            self._apply_verification_policy_set(
                step_id=str(ev.get("step_id") or ""),
                policy=ev.get("policy"),
                notes=ev.get("notes"),
            )
            return
        if et == "verification_evidence":
            self._apply_verification_evidence_event(ev)
            return

    def load(self) -> dict[str, Any]:
        # Pure rollout replay: reconstruct durable state by replaying domain events
        # from the append-only session log.
        self._init_empty_state()

        min_ts: float | None = None
        max_ts: float | None = None

        if self.log_path.exists():
            try:
                with open(self.log_path, "r", encoding="utf-8", errors="replace") as f:
                    for ln in f:
                        try:
                            ev = json.loads(ln)
                        except Exception:
                            continue
                        if not isinstance(ev, dict):
                            continue
                        ts = ev.get("ts")
                        if isinstance(ts, (int, float)):
                            tsv = float(ts)
                            if min_ts is None or tsv < min_ts:
                                min_ts = tsv
                            if max_ts is None or tsv > max_ts:
                                max_ts = tsv
                        self._apply_event_to_state(ev)
            except Exception:
                # Corrupted log must not brick the session; start fresh.
                self._init_empty_state()

        now = _now_unix()
        self._state["created_at"] = float(min_ts) if min_ts is not None else float(now)
        self._state["updated_at"] = float(max_ts) if max_ts is not None else float(now)

        # Ensure verification has entries for the current plan (defense-in-depth).
        try:
            self._ensure_verification_entries_for_plan(self.get_plan() or [])
        except Exception:
            pass

        return dict(self._state)

    def save(self) -> None:
        # No-op for persistence: the session is event-sourced and every mutation
        # appends a domain event immediately.
        self._state["updated_at"] = _now_unix()

    def session_id(self) -> str:
        return str(self._state.get("session_id") or self.root.name)

    def get_meta(self) -> dict[str, Any]:
        meta = self._state.get("meta")
        return dict(meta) if isinstance(meta, dict) else {}

    def set_meta(self, **kwargs: Any) -> None:
        fields: dict[str, Any] = {}
        for k, v in kwargs.items():
            if v is None:
                continue
            fields[str(k)] = v
        if not fields:
            return
        self._apply_meta_fields(fields)
        try:
            self.append_event({"type": "meta_set", "fields": fields})
        except Exception:
            pass

    def get_consent(self, name: ConsentName) -> bool | None:
        """Return the user's persisted consent decision for a capability.

        This is used for privacy/consent-gated actions like screenshot-based
        visual verification. None means "not decided yet for this session".
        """
        meta = self._state.get("meta")
        if not isinstance(meta, dict):
            return None
        consent = meta.get("consent")
        if not isinstance(consent, dict):
            return None
        v = consent.get(str(name))
        return bool(v) if isinstance(v, bool) else None

    def set_consent(self, name: ConsentName, allowed: bool) -> None:
        nm = str(name or "").strip()
        if not nm:
            return
        self._apply_consent(nm, allowed)
        try:
            self.append_event({"type": "consent", "consent": nm, "allowed": bool(allowed)})
        except Exception:
            pass

    def append_transcript(self, *, role: str, content: str, turn_id: str | None = None) -> None:
        entry: dict[str, Any] = {"type": "transcript", "role": str(role), "content": str(content)}
        if isinstance(turn_id, str) and turn_id.strip():
            entry["turn_id"] = turn_id.strip()
        self.append_event(entry)

    def search_transcript(
        self,
        *,
        query: str,
        regex: bool = False,
        case_sensitive: bool = False,
        role: str | None = None,
        max_results: int = 0,
        offset: int = 0,
        reverse: bool = False,
    ) -> dict[str, Any]:
        """Search transcript entries in the append-only session log.

        This is correctness-first: max_results=0 means unlimited (may be large).
        """
        import re

        q = str(query or "")
        if not q:
            return {"ok": False, "error": "query is required"}
        role_filter = str(role).strip() if isinstance(role, str) and str(role).strip() else None
        max_results = int(max_results or 0)
        if max_results < 0:
            max_results = 0
        offset = int(offset or 0)
        if offset < 0:
            offset = 0
        reverse = bool(reverse)

        flags = 0 if case_sensitive else re.IGNORECASE
        pattern: re.Pattern[str] | None = None
        if regex:
            try:
                pattern = re.compile(q, flags=flags)
            except re.error as e:
                return {"ok": False, "error": f"invalid regex: {e}"}

        def _match(text: str) -> bool:
            if pattern is not None:
                return bool(pattern.search(text))
            if case_sensitive:
                return q in text
            return q.lower() in text.lower()

        if not self.log_path.exists():
            return {
                "ok": True,
                "query": q,
                "regex": bool(regex),
                "case_sensitive": bool(case_sensitive),
                "role": role_filter,
                "reverse": reverse,
                "total_matches": 0,
                "results": [],
                "result_window": {"offset": offset, "max_results": max_results},
                "limit_reached": False,
            }

        total = 0
        limit_reached = False
        results: list[dict[str, Any]] = []
        window_size = (offset + max_results) if max_results else 0
        buf = None
        if reverse and window_size > 0:
            from collections import deque

            buf = deque(maxlen=window_size)
        try:
            with open(self.log_path, "r", encoding="utf-8", errors="replace") as f:
                for ln in f:
                    try:
                        entry = json.loads(ln)
                    except Exception:
                        continue
                    if not isinstance(entry, dict):
                        continue
                    if str(entry.get("type") or "") != "transcript":
                        continue
                    erole = str(entry.get("role") or "")
                    if role_filter is not None and erole != role_filter:
                        continue
                    content = str(entry.get("content") or "")
                    if not content:
                        continue
                    if not _match(content):
                        continue
                    total += 1
                    entry_out = {
                        "ts": entry.get("ts"),
                        "turn_id": entry.get("turn_id"),
                        "role": erole,
                        "content": content,
                    }
                    if reverse:
                        if buf is not None:
                            buf.append(entry_out)
                        else:
                            results.append(entry_out)
                        continue
                    if total <= offset:
                        continue
                    if max_results and len(results) >= max_results:
                        limit_reached = True
                        continue
                    results.append(
                        {
                            "ts": entry.get("ts"),
                            "turn_id": entry.get("turn_id"),
                            "role": erole,
                            "content": content,
                        }
                    )
        except Exception as e:
            return {"ok": False, "error": str(e)}

        if reverse:
            seq = list(buf) if buf is not None else list(results)
            seq.reverse()  # newest-first
            if offset:
                seq = seq[offset:]
            if max_results:
                seq = seq[:max_results]
            results = seq
            limit_reached = bool(max_results and total > (offset + max_results))

        return {
            "ok": True,
            "query": q,
            "regex": bool(regex),
            "case_sensitive": bool(case_sensitive),
            "role": role_filter,
            "reverse": reverse,
            "session_id": self.session_id(),
            "log_path": str(self.log_path),
            "total_matches": total,
            "results": results,
            "result_window": {"offset": offset, "max_results": max_results},
            "limit_reached": limit_reached,
        }

    def append_event(self, event: dict[str, Any]) -> str:
        """Append a structured event to the session log.

        Events are intended for deterministic resume/debug. They are append-only.
        Returns the generated event_id (hex string).
        """
        event_id = uuid.uuid4().hex
        payload = dict(event or {})
        payload.setdefault("ts", _now_unix())
        payload.setdefault("event_id", event_id)
        line = json.dumps(payload, ensure_ascii=False)
        self.log_path.parent.mkdir(parents=True, exist_ok=True)
        with open(self.log_path, "a", encoding="utf-8") as f:
            f.write(line + "\n")
        return event_id

    def append_tool_event(
        self,
        *,
        turn_id: str | None,
        phase: str | None = None,
        tool: str,
        args: Any,
        result: Any,
        note: str | None = None,
        policy: str = "summary",
    ) -> str:
        """Append a tool-call event.

        policy:
          - "full": store full args + full result (may be large)
          - "summary": store full args + summarized result + sha256 digest of full result JSON
        """
        tool_name = str(tool or "")
        if not tool_name:
            tool_name = "<unknown>"

        def _stable_json(v: Any) -> str:
            try:
                return json.dumps(v, ensure_ascii=False, sort_keys=True)
            except Exception:
                return json.dumps({"__non_json__": str(v)}, ensure_ascii=False, sort_keys=True)

        full_result_json = _stable_json(result)
        digest = hashlib.sha256(full_result_json.encode("utf-8", errors="replace")).hexdigest()

        stored: dict[str, Any] = {
            "type": "tool_call",
            "tool": tool_name,
            "turn_id": str(turn_id) if isinstance(turn_id, str) and turn_id.strip() else None,
            "phase": str(phase) if isinstance(phase, str) and phase.strip() else None,
            "args": args,
            "result_digest_sha256": digest,
            "result_policy": str(policy or "summary"),
        }
        if isinstance(note, str) and note.strip():
            stored["note"] = note.strip()

        if str(policy) == "full":
            stored["result"] = result
        else:
            # Summary: keep only key fields, plus the digest for exact reproduction/debug.
            summary: dict[str, Any] = {}
            if isinstance(result, dict):
                for k in (
                    "ok",
                    "error",
                    "reason",
                    "skipped",
                    "path",
                    "candidates",
                    "total_matches",
                    "limit_reached",
                    "exit_code",
                ):
                    if k in result:
                        summary[k] = result.get(k)
            stored["result_summary"] = summary if summary else {"kind": type(result).__name__}
        return self.append_event(stored)

    def search_events(
        self,
        *,
        query: str,
        regex: bool = False,
        case_sensitive: bool = False,
        event_type: str | None = None,
        tool: str | None = None,
        max_results: int = 0,
        offset: int = 0,
        reverse: bool = False,
    ) -> dict[str, Any]:
        """Search event entries in the session log and return matching event dicts.

        This is correctness-first: max_results=0 means unlimited (may be large).
        """
        import re

        q = str(query or "")
        if not q:
            return {"ok": False, "error": "query is required"}
        max_results = int(max_results or 0)
        if max_results < 0:
            max_results = 0
        offset = int(offset or 0)
        if offset < 0:
            offset = 0
        reverse = bool(reverse)
        et = str(event_type).strip() if isinstance(event_type, str) and str(event_type).strip() else None
        tool_filter = str(tool).strip() if isinstance(tool, str) and str(tool).strip() else None

        flags = 0 if case_sensitive else re.IGNORECASE
        pattern: re.Pattern[str] | None = None
        if regex:
            try:
                pattern = re.compile(q, flags=flags)
            except re.error as e:
                return {"ok": False, "error": f"invalid regex: {e}"}

        def _match(text: str) -> bool:
            if pattern is not None:
                return bool(pattern.search(text))
            if case_sensitive:
                return q in text
            return q.lower() in text.lower()

        if not self.log_path.exists():
            return {
                "ok": True,
                "query": q,
                "regex": bool(regex),
                "case_sensitive": bool(case_sensitive),
                "event_type": et,
                "tool": tool_filter,
                "reverse": reverse,
                "total_matches": 0,
                "results": [],
                "result_window": {"offset": offset, "max_results": max_results},
                "limit_reached": False,
            }

        total = 0
        limit_reached = False
        results: list[dict[str, Any]] = []
        window_size = (offset + max_results) if max_results else 0
        buf = None
        if reverse and window_size > 0:
            from collections import deque

            buf = deque(maxlen=window_size)
        try:
            with open(self.log_path, "r", encoding="utf-8", errors="replace") as f:
                for ln in f:
                    try:
                        ev = json.loads(ln)
                    except Exception:
                        continue
                    if not isinstance(ev, dict):
                        continue
                    ev_type = str(ev.get("type") or "")
                    if et is not None:
                        if ev_type != et:
                            continue
                    else:
                        # By default, "events" exclude the chat transcript; it has a dedicated
                        # access pattern (search_transcript) and can be noisy.
                        if ev_type == "transcript":
                            continue
                    if tool_filter is not None and str(ev.get("tool") or "") != tool_filter:
                        continue
                    # Match against the serialized event (contains tool, args, summaries, etc.)
                    try:
                        hay = json.dumps(ev, ensure_ascii=False)
                    except Exception:
                        hay = str(ev)
                    if not _match(hay):
                        continue
                    total += 1
                    if reverse:
                        if buf is not None:
                            buf.append(ev)
                        else:
                            results.append(ev)
                        continue
                    if total <= offset:
                        continue
                    if max_results and len(results) >= max_results:
                        limit_reached = True
                        continue
                    results.append(ev)
        except Exception as e:
            return {"ok": False, "error": str(e)}

        if reverse:
            seq = list(buf) if buf is not None else list(results)
            seq.reverse()  # newest-first
            if offset:
                seq = seq[offset:]
            if max_results:
                seq = seq[:max_results]
            results = seq
            limit_reached = bool(max_results and total > (offset + max_results))

        return {
            "ok": True,
            "query": q,
            "regex": bool(regex),
            "case_sensitive": bool(case_sensitive),
            "event_type": et,
            "tool": tool_filter,
            "reverse": reverse,
            "total_matches": total,
            "results": results,
            "result_window": {"offset": offset, "max_results": max_results},
            "limit_reached": limit_reached,
        }

    def tail_events(
        self,
        *,
        limit: int,
        event_type: str | None = None,
        tool: str | None = None,
    ) -> list[dict[str, Any]]:
        """Return the last N matching events.

        This is for "recent context" retrieval; it does not truncate storage.
        """
        lim = int(limit or 0)
        if lim <= 0:
            return []
        et = str(event_type).strip() if isinstance(event_type, str) and str(event_type).strip() else None
        tool_filter = str(tool).strip() if isinstance(tool, str) and str(tool).strip() else None
        if not self.log_path.exists():
            return []
        from collections import deque

        out: deque[dict[str, Any]] = deque(maxlen=lim)
        try:
            with open(self.log_path, "r", encoding="utf-8", errors="replace") as f:
                for ln in f:
                    try:
                        ev = json.loads(ln)
                    except Exception:
                        continue
                    if not isinstance(ev, dict):
                        continue
                    ev_type = str(ev.get("type") or "")
                    if et is not None:
                        if ev_type != et:
                            continue
                    else:
                        if ev_type == "transcript":
                            continue
                    if tool_filter is not None and str(ev.get("tool") or "") != tool_filter:
                        continue
                    out.append(ev)
        except Exception:
            return []
        return list(out)

    def get_memory_summary(self) -> str:
        s = self._state.get("memory_summary")
        return str(s) if isinstance(s, str) else ""

    def set_memory_summary(self, text: str) -> None:
        new = str(text or "")
        prev = self.get_memory_summary()
        self._state["memory_summary"] = new
        if new == prev:
            return
        try:
            self.append_event({"type": "memory_summary_set", "memory_summary": new})
        except Exception:
            pass

    def get_todo_ledger(self) -> list[dict]:
        tl = self._state.get("todo_ledger")
        return list(tl) if isinstance(tl, list) else []

    def set_todo_ledger(self, ledger: list[dict]) -> None:
        new_ledger = list(ledger or [])
        prev_ledger = self.get_todo_ledger()
        self._state["todo_ledger"] = new_ledger
        if new_ledger != prev_ledger:
            try:
                self.append_event({"type": "todo_ledger_set", "ledger": new_ledger})
            except Exception:
                pass
        # Keep an auto-derived plan in sync unless the LLM has explicitly
        # overridden it using update_plan.
        if str(self._state.get("plan_source") or "") != "llm":
            self._derive_plan_from_todos()
        else:
            # Best-effort sync: keep LLM-authored plan statuses aligned with TODO completion
            # when step text matches TODO text exactly (TODO ledger is the verification source).
            try:
                self._sync_llm_plan_with_todos()
            except Exception:
                pass

    def get_plan(self) -> list[dict]:
        plan = self._state.get("plan")
        return list(plan) if isinstance(plan, list) else []

    def update_plan(self, plan: list[dict], *, explanation: str | None = None, source: str = "llm") -> None:
        # Normalize + validate at store boundary as a defense-in-depth.
        normalized: list[dict[str, Any]] = []
        in_progress = 0

        # Preserve stable ids when possible.
        existing_by_step: dict[str, str] = {}
        for it in self.get_plan():
            if not isinstance(it, dict):
                continue
            step = str(it.get("step") or "").strip()
            step_id = str(it.get("step_id") or "").strip()
            if step and step_id:
                existing_by_step[step] = step_id
        used_ids: set[str] = set()

        for it in plan or []:
            if not isinstance(it, dict):
                continue
            step = str(it.get("step") or "").strip()
            status = str(it.get("status") or "").strip()
            step_id = str(it.get("step_id") or "").strip()
            if not step:
                continue
            if not step_id:
                step_id = existing_by_step.get(step) or _make_step_id(step)
            # Avoid accidental duplicates (rare; only when duplicate step text or collisions).
            if step_id in used_ids:
                suffix = 2
                while f"{step_id}_{suffix}" in used_ids:
                    suffix += 1
                step_id = f"{step_id}_{suffix}"
            if status not in ("pending", "in_progress", "completed"):
                status = "pending"
            if status == "in_progress":
                in_progress += 1
            normalized.append({"step_id": step_id, "step": step, "status": status})
            used_ids.add(step_id)
        if in_progress > 1:
            # Store is strict: enforce plan invariant (at most one in_progress).
            raise ValueError("plan contains more than one in_progress item")
        self._state["plan"] = normalized
        self._state["plan_source"] = str(source or "llm")
        if explanation is not None:
            self._state["plan_explanation"] = str(explanation or "")

        # Ensure there is a verification entry for each step id, but do not
        # overwrite existing policies/statuses.
        self._ensure_verification_entries_for_plan(normalized)

        # Persist as a domain event (pure rollout / event sourcing).
        try:
            self.append_event(
                {
                    "type": "plan_updated",
                    "plan": normalized,
                    "explanation": str(explanation) if isinstance(explanation, str) else None,
                    "source": str(source or "llm"),
                }
            )
        except Exception:
            pass

    def _derive_plan_from_todos(self) -> None:
        ledger = self.get_todo_ledger()
        plan: list[dict[str, Any]] = []
        for it in ledger:
            if not isinstance(it, dict):
                continue
            step = str(it.get("text") or "").strip()
            if not step:
                continue
            st = str(it.get("status") or "pending")
            status: PlanStatus = "completed" if st in ("applied", "done", "finished", "completed") else "pending"
            plan.append({"step_id": _make_step_id(step), "step": step, "status": status})
        # Mark first pending as in_progress (plan invariant: at most one).
        for it in plan:
            if it.get("status") == "pending":
                it["status"] = "in_progress"
                break
        self._state["plan"] = plan
        self._state["plan_source"] = "todos"

        self._ensure_verification_entries_for_plan(plan)

    def _sync_llm_plan_with_todos(self) -> None:
        """Sync an LLM-authored plan against the TODO ledger (status only).

        This never rewrites plan steps. It updates status for exact text matches
        so the displayed plan doesn't drift from verified TODO completion.
        """
        ledger = self.get_todo_ledger()
        todo_status: dict[str, str] = {}
        for it in ledger:
            if not isinstance(it, dict):
                continue
            text = str(it.get("text") or "").strip()
            if not text:
                continue
            todo_status[text] = str(it.get("status") or "pending")

        plan = self.get_plan()
        if not plan:
            return

        normalized: list[dict[str, Any]] = []
        in_progress = 0

        used_ids: set[str] = set()
        for it in plan:
            if not isinstance(it, dict):
                continue
            step_id = str(it.get("step_id") or "").strip()
            step = str(it.get("step") or "").strip()
            status = str(it.get("status") or "pending").strip()
            if not step:
                continue
            if not step_id:
                step_id = _make_step_id(step)
            if step_id in used_ids:
                suffix = 2
                while f"{step_id}_{suffix}" in used_ids:
                    suffix += 1
                step_id = f"{step_id}_{suffix}"
            if step in todo_status:
                st = todo_status[step]
                want_completed = st in ("applied", "done", "finished", "completed")
                if want_completed:
                    status = "completed"
                else:
                    # TODO ledger is authoritative; don't allow plan to claim completion.
                    if status == "completed":
                        status = "pending"
            if status not in ("pending", "in_progress", "completed"):
                status = "pending"
            if status == "in_progress":
                in_progress += 1
            normalized.append({"step_id": step_id, "step": step, "status": status})
            used_ids.add(step_id)

        # Enforce plan invariant: at most one in_progress.
        if in_progress > 1:
            seen = 0
            for it in normalized:
                if it["status"] == "in_progress":
                    seen += 1
                    if seen > 1:
                        it["status"] = "pending"
            in_progress = 1

        # If there are pending steps but none in_progress, pick the first pending.
        if in_progress == 0:
            for it in normalized:
                if it["status"] == "pending":
                    it["status"] = "in_progress"
                    break

        self._state["plan"] = normalized
        self._ensure_verification_entries_for_plan(normalized)

    def get_verification(self) -> dict[str, Any]:
        ver = self._state.get("verification")
        if not isinstance(ver, dict):
            return _default_verification_state()
        # Shallow copy for safety.
        return dict(ver)

    def _ensure_verification_entries_for_plan(self, plan: list[dict[str, Any]]) -> None:
        ver = self._state.get("verification")
        if not isinstance(ver, dict):
            ver = _default_verification_state()
            self._state["verification"] = ver
        steps = ver.get("steps")
        if not isinstance(steps, dict):
            steps = {}
            ver["steps"] = steps

        for it in plan or []:
            if not isinstance(it, dict):
                continue
            step_id = str(it.get("step_id") or "").strip()
            if not step_id:
                continue
            if step_id not in steps or not isinstance(steps.get(step_id), dict):
                steps[step_id] = {}
            entry = steps[step_id]
            if not isinstance(entry, dict):
                entry = {}
                steps[step_id] = entry
            entry.setdefault(
                "policy",
                {
                    "all_of": [
                        {
                            "any_of": ["tool"],
                            "description": "Verify via read-back tools (default).",
                        }
                    ]
                },
            )
            entry.setdefault(
                "status",
                {"tool": "unknown", "visual": "unknown", "human": "unknown"},
            )
            entry.setdefault("last_evidence_event_id", {"tool": None, "visual": None, "human": None})

    def set_verification_policy(self, *, step_id: str, policy: dict[str, Any] | None, notes: str | None = None) -> None:
        sid = str(step_id or "").strip()
        if not sid:
            raise ValueError("step_id is required")
        self._apply_verification_policy_set(step_id=sid, policy=policy, notes=notes)
        try:
            self.append_event(
                {
                    "type": "verification_policy_set",
                    "step_id": sid,
                    "policy": policy,
                    "notes": notes.strip() if isinstance(notes, str) and notes.strip() else None,
                }
            )
        except Exception:
            pass

    def record_verification_evidence(
        self,
        *,
        step_id: str,
        mode: VerificationMode,
        status: VerificationStatus,
        summary: str,
        ref: dict[str, Any] | None = None,
        turn_id: str | None = None,
        phase: str | None = None,
        source: str | None = None,
    ) -> str:
        sid = str(step_id or "").strip()
        if not sid:
            raise ValueError("step_id is required")
        m = str(mode or "").strip().lower()
        if m not in ("tool", "visual", "human"):
            raise ValueError("mode must be one of: tool, visual, human")
        st = str(status or "").strip().lower()
        if st not in ("unknown", "pass", "fail"):
            raise ValueError("status must be one of: unknown, pass, fail")
        sm = str(summary or "").strip()
        if not sm:
            raise ValueError("summary is required")

        event_id = self.append_event(
            {
                "type": "verification_evidence",
                "turn_id": str(turn_id) if isinstance(turn_id, str) and turn_id.strip() else None,
                "phase": str(phase) if isinstance(phase, str) and phase.strip() else None,
                "step_id": sid,
                "mode": m,
                "status": st,
                "summary": sm,
                "ref": ref if isinstance(ref, dict) else None,
                "source": str(source) if isinstance(source, str) and source.strip() else None,
            }
        )
        # Apply the same reducer logic that load() uses.
        try:
            self._apply_verification_evidence_event(
                {
                    "type": "verification_evidence",
                    "event_id": event_id,
                    "step_id": sid,
                    "mode": m,
                    "status": st,
                }
            )
        except Exception:
            pass

        return event_id

    def evaluate_verification_for_step(self, *, step_id: str) -> dict[str, Any]:
        sid = str(step_id or "").strip()
        if not sid:
            return {"ok": False, "error": "step_id is required"}
        ver = self._state.get("verification")
        if not isinstance(ver, dict):
            return {"ok": True, "step_id": sid, "policy": None, "status": {}, "satisfied": False, "missing": []}
        steps = ver.get("steps")
        if not isinstance(steps, dict):
            return {"ok": True, "step_id": sid, "policy": None, "status": {}, "satisfied": False, "missing": []}
        entry = steps.get(sid)
        if not isinstance(entry, dict):
            return {"ok": True, "step_id": sid, "policy": None, "status": {}, "satisfied": False, "missing": []}

        policy = entry.get("policy")
        status = entry.get("status") if isinstance(entry.get("status"), dict) else {}

        # Policy format:
        #   {"all_of": [{"any_of": ["tool", "visual"], "description": "..."} ...]}
        satisfied = False
        missing: list[dict[str, Any]] = []
        if isinstance(policy, dict) and isinstance(policy.get("all_of"), list):
            groups = policy.get("all_of") or []
            all_ok = True
            for g in groups:
                if not isinstance(g, dict):
                    continue
                any_of = g.get("any_of")
                if not isinstance(any_of, list) or not any_of:
                    continue
                ok = False
                for m in any_of:
                    mm = str(m or "").strip().lower()
                    if mm not in ("tool", "visual", "human"):
                        continue
                    if str(status.get(mm) or "unknown") == "pass":
                        ok = True
                        break
                if not ok:
                    all_ok = False
                    missing.append(
                        {
                            "any_of": [str(m or "") for m in any_of],
                            "description": str(g.get("description") or "").strip(),
                        }
                    )
            satisfied = all_ok
        else:
            # No policy means "not verified".
            satisfied = False

        return {
            "ok": True,
            "step_id": sid,
            "policy": policy,
            "status": status,
            "satisfied": bool(satisfied),
            "missing": missing,
        }

    def plan_explanation(self) -> str:
        s = self._state.get("plan_explanation")
        return str(s) if isinstance(s, str) else ""

    def plan_source(self) -> str:
        s = self._state.get("plan_source")
        return str(s) if isinstance(s, str) else ""
