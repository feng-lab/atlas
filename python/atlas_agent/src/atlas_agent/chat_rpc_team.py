from __future__ import annotations

import logging
import os
from dataclasses import dataclass
from typing import Optional

from .agent_team.base import LLMClient
from .agent_team.supervisor import Supervisor
from .capabilities_prompt import build_capabilities_prompt
from .discovery import discover_schema_dir
from .scene_rpc import SceneClient


@dataclass
class ChatTeam:
    address: str
    api_key: str
    model: str
    temperature: float = 0.2
    atlas_dir: Optional[str] = None

    def __post_init__(self):
        # Disable Agents SDK tracing to avoid network calls to default OpenAI tracing backend.
        try:
            from agents.tracing import set_tracing_disabled  # type: ignore
            set_tracing_disabled(True)
        except Exception:
            pass
        self.scene = SceneClient(address=self.address)
        # LLMClient is the sole owner of base_url configuration
        self.llm = LLMClient(api_key=self.api_key, model=self.model)
        self.supervisor = Supervisor(client=self.llm, scene=self.scene, temperature=self.temperature, atlas_dir=self.atlas_dir)
        # Build capabilities context derived from atlas_dir or defaults
        self._context: Optional[str] = None
        # Maintain full-session chat history (list of (role, content)) for grounding future turns
        self._history: list[tuple[str, str]] = []
        # Optional: keep last timeline snapshot to produce a diff fact table
        self._last_snapshot: dict | None = None
        # Persist a session TODO ledger across turns
        self._todo_ledger: list[dict] = []
        # Configure a shared agents logger if not already configured
        agents_logger = logging.getLogger("atlas_agent.agents")
        if not agents_logger.handlers:
            h = logging.StreamHandler()
            fmt = logging.Formatter(fmt="%(asctime)s [%(levelname)s] %(name)s: %(message)s", datefmt="%H:%M:%S")
            h.setFormatter(fmt)
            agents_logger.addHandler(h)
            agents_logger.setLevel(logging.INFO)
            agents_logger.propagate = False
        try:
            sd, _ = discover_schema_dir(user_schema_dir=None, atlas_dir=self.atlas_dir)
            if sd:
                self._context = build_capabilities_prompt(sd)
        except Exception:
            self._context = None

    def turn(self, user_text: str, *, shared_context: Optional[str] = None) -> str:
        ctx = shared_context or self._context
        # Pass full conversation history to the Supervisor for this session
        # Synchronize current session TODOs to Supervisor
        try:
            setattr(self.supervisor, "_todo_ledger", list(self._todo_ledger))
        except Exception:
            pass
        msgs = self.supervisor.run_turn(user_text, shared_context=ctx, recent_history=self._history)
        # Prefer the facts-based description if present; otherwise fall back to the latest assistant content
        text = ""
        description = None
        for m in msgs:
            if m.role == "assistant" and m.content:
                c = m.content
                if c.strip().lower().startswith("description (facts-based):"):
                    description = c
        # Choose assistant text to return; never append ledger into it
        if description:
            text = description
        else:
            # Pick the last assistant message
            text = ""
            for m in reversed(msgs):
                if m.role == "assistant" and m.content:
                    text = m.content
                    break
        # Post-execution fact guard: append a fact table snapshot (and diff) to the final message
        try:
            snap = self.scene.scene_facts()
        except Exception:
            snap = None
        if snap:
            # Compute a diff vs. last snapshot, including lightweight value digests so value changes at same times are visible.
            def _flatten(s):
                flat = []
                keys = (s.get("keys") or {})
                # Per-object param times and value digests
                for oid, mp in keys.get("objects", {}).items():
                    for jk, times in mp.items():
                        flat.append((f"key:{oid}:{jk}", tuple(times)))
                        # Build a compact digest of values at those times (if available)
                        try:
                            lr = self.scene.list_keys(id=int(oid), json_key=jk, include_values=True)
                            dig_items = []
                            for k in getattr(lr, "keys", []):
                                try:
                                    t = float(getattr(k, "time", 0.0))
                                    vj = getattr(k, "value_json", "")
                                    # Truncate to keep logs compact
                                    LOG_VALUE_DIGEST_PREVIEW_CHARS = int(
                                        os.environ.get(
                                            "ATLAS_AGENT_VALUE_DIGEST_PREVIEW_CHARS",
                                            "96",
                                        )
                                    )
                                    if (
                                        isinstance(vj, str)
                                        and LOG_VALUE_DIGEST_PREVIEW_CHARS >= 0
                                        and len(vj) > LOG_VALUE_DIGEST_PREVIEW_CHARS
                                    ):
                                        vj = vj[:LOG_VALUE_DIGEST_PREVIEW_CHARS] + "…"
                                    dig_items.append(f"{t:.6g}:{vj}")
                                except Exception:
                                    continue
                            if dig_items:
                                flat.append((f"keyval:{oid}:{jk}", tuple(dig_items)))
                        except Exception:
                            pass
                # Camera times
                if keys.get("camera"):
                    flat.append(("key:camera", tuple(keys.get("camera") or [])))
                # Include objects list presence (ids+paths) for load operations
                objs = s.get("objects_list") or []
                ids_paths = tuple(sorted([f"{o.get('id')}:{o.get('path')}" for o in objs]))
                flat.append(("objects_list", ids_paths))
                return dict(flat)
            prev = _flatten(self._last_snapshot) if self._last_snapshot else {}
            cur = _flatten(snap)
            changes = []
            for k, v in cur.items():
                if prev.get(k) != v:
                    changes.append((k, list(v)))
            if changes:
                facts_lines = ["Facts (updated this turn):"]
                for k, times in changes:
                    facts_lines.append(f"- {k} times={times}")
                facts_text = "\n".join(facts_lines)
                text = (text + "\n\n" + facts_text) if text else facts_text
            else:
                # Override any success claims when no changes were applied
                facts_text = "Facts: no new keys compared to last snapshot."
                guidance = (
                    "No changes were applied this turn. If you intended to modify the scene, "
                    "please specify the target id, parameter name, time, and value."
                )
                text = facts_text if not text else facts_text + "\n" + guidance
            self._last_snapshot = snap

        # Update history (append the exact content returned to the user)
        if user_text:
            self._history.append(("user", user_text))
        if text:
            self._history.append(("assistant", text))
        # Persist updated TODO ledger state for next turn
        try:
            self._todo_ledger = list(self.supervisor.get_todo_ledger())
        except Exception:
            pass
        return text or "(no response)"


def run_repl(address: str, api_key: str, model: str, temperature: float = 0.2, *, atlas_dir: Optional[str] = None) -> int:
    # Configure default logger if not already configured
    logger = logging.getLogger("atlas_agent.chat")
    if not logger.handlers:
        h = logging.StreamHandler()
        fmt = logging.Formatter(fmt="%(asctime)s [%(levelname)s] %(name)s: %(message)s", datefmt="%H:%M:%S")
        h.setFormatter(fmt)
        logger.addHandler(h)
        logger.setLevel(logging.INFO)
        logger.propagate = False

    # Instantiate chat team; LLMClient reads base_url centrally (env or later configuration)
    team = ChatTeam(address=address, api_key=api_key, model=model, temperature=temperature, atlas_dir=atlas_dir)
    logger.info("Atlas Multi-Agent Chat (RPC). Type :help for commands.")
    while True:
        try:
            line = input(">> ").strip()
        except (EOFError, KeyboardInterrupt):
            logger.info("")
            return 0
        if not line:
            continue
        if line.startswith(":"):
            cmd, *rest = line[1:].split()
            if cmd in {"q", "quit", "exit"}: return 0
            if cmd in {"h", "help"}:
                logger.info(
                    "Commands:\n"
                    ":help                This help\n"
                    ":save <path>        Save current animation\n"
                    ":time <seconds>     Set current time\n"
                    ":objects            List objects"
                )
                continue
            if cmd == "save" and rest:
                ok = team.scene.save_animation(rest[0])
                logger.info("%s", "ok" if ok else "fail")
                continue
            if cmd == "time" and rest:
                logger.info("%s", "ok" if team.scene.set_time(float(rest[0])) else "fail")
                continue
            if cmd == "objects":
                resp = team.scene.list_objects()
                for o in resp.objects:
                    logger.info("%s\t%s\t%s\t%s", o.id, o.type, o.name, o.visible)
                continue
            logger.info("Unknown command; :help")
            continue

        # Natural language turn
        try:
            rationale = team.turn(line, shared_context=None)
            logger.info("%s", rationale)
        except Exception as e:
            # Log full traceback to prove error origin
            logger.exception("Agent error: %s", e)
            continue
    return 0
