import hashlib
import json
import logging
from dataclasses import dataclass
from typing import Optional

from agents.tracing import set_tracing_disabled  # type: ignore

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
        set_tracing_disabled(True)
        self.scene = SceneClient(address=self.address)
        # LLMClient is the sole owner of base_url configuration
        self.llm = LLMClient(api_key=self.api_key, model=self.model)
        self.supervisor = Supervisor(client=self.llm, scene=self.scene, temperature=self.temperature, atlas_dir=self.atlas_dir)
        # Build capabilities context derived from atlas_dir or defaults
        self._context: Optional[str] = None
        # Maintain full-session chat history (list of (role, content)) for grounding future turns
        self._history: list[tuple[str, str]] = []
        # Optional: keep a lightweight last facts projection for post-turn diffs
        self._last_snapshot_flat: dict | None = None
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
        # Post-execution fact guard: append a facts diff, but never overwrite the agent's response.
        # This avoids masking CLARIFY/help/explain answers while still preventing false "success" claims.
        def _is_clarify(s: str) -> bool:
            return (s or "").lstrip().lower().startswith("clarify:")

        def _json_digest(value: object) -> str:
            try:
                payload = json.dumps(
                    value,
                    sort_keys=True,
                    ensure_ascii=False,
                    separators=(",", ":"),
                )
            except Exception:
                try:
                    payload = str(value)
                except Exception:
                    payload = f"<{type(value).__name__}>"
            return hashlib.sha256(payload.encode("utf-8")).hexdigest()

        def _project_facts(facts: dict) -> dict:
            # Keep only stable, comparable signals (no full value dumps):
            # - objects (id/type/name/path/visible)
            # - camera key times
            # - per-param key series (time + value digest)
            # - scene param value digests
            proj: dict[str, object] = {}
            # Objects
            objs_by_id: dict[str, dict] = {}
            for o in (facts.get("objects_list") or []) if isinstance(facts, dict) else []:
                try:
                    oid = str(o.get("id"))
                    objs_by_id[oid] = {
                        "type": str(o.get("type", "")),
                        "name": str(o.get("name", "")),
                        "path": str(o.get("path", "")),
                        "visible": bool(o.get("visible", False)),
                    }
                except Exception:
                    continue
            proj["objects_by_id"] = objs_by_id
            # Camera key times
            cam_times = []
            try:
                cam_times = (facts.get("keys", {}).get("camera") or [])
            except Exception:
                cam_times = []
            proj["camera_key_times"] = tuple(float(t) for t in cam_times)
            # Timeline keys (objects/groups only; camera values omitted)
            series: dict[str, tuple] = {}
            try:
                obj_keys = (facts.get("keys", {}).get("objects") or {})
                if isinstance(obj_keys, dict):
                    for oid, mp in obj_keys.items():
                        if not isinstance(mp, dict):
                            continue
                        for jk, entry in mp.items():
                            # entry is either [times...] or [{time,value}...]
                            ser = []
                            if isinstance(entry, list) and entry and isinstance(entry[0], dict):
                                for it in entry:
                                    try:
                                        t = float(it.get("time", 0.0))
                                        v = it.get("value", None)
                                        ser.append((t, _json_digest(v)))
                                    except Exception:
                                        continue
                            else:
                                # times-only fallback
                                for t in entry or []:
                                    try:
                                        ser.append((float(t), ""))
                                    except Exception:
                                        continue
                            ser.sort(key=lambda x: x[0])
                            series[f"{oid}:{jk}"] = tuple(ser)
            except Exception:
                pass
            proj["key_series"] = series
            # Scene values (digests only)
            sv_digests: dict[str, str] = {}
            try:
                sv = facts.get("scene_values") or {}
                if isinstance(sv, dict):
                    for sid, mp in sv.items():
                        if not isinstance(mp, dict):
                            continue
                        for jk, v in mp.items():
                            sv_digests[f"{sid}:{jk}"] = _json_digest(v)
            except Exception:
                pass
            proj["scene_value_digests"] = sv_digests
            return proj

        def _diff_facts(prev: dict | None, cur: dict) -> list[str]:
            if not prev:
                return []
            lines: list[str] = []
            prev_objs = (prev.get("objects_by_id") or {}) if isinstance(prev, dict) else {}
            cur_objs = (cur.get("objects_by_id") or {}) if isinstance(cur, dict) else {}
            if isinstance(prev_objs, dict) and isinstance(cur_objs, dict):
                prev_ids = set(prev_objs.keys())
                cur_ids = set(cur_objs.keys())
                added = sorted(cur_ids - prev_ids)
                removed = sorted(prev_ids - cur_ids)
                changed = sorted(
                    i for i in (prev_ids & cur_ids) if prev_objs.get(i) != cur_objs.get(i)
                )
                for oid in added:
                    o = cur_objs.get(oid) or {}
                    lines.append(
                        f"- object_added id={oid} type={o.get('type','')} name={o.get('name','')}"
                    )
                for oid in removed:
                    o = prev_objs.get(oid) or {}
                    lines.append(
                        f"- object_removed id={oid} type={o.get('type','')} name={o.get('name','')}"
                    )
                for oid in changed:
                    before = prev_objs.get(oid) or {}
                    after = cur_objs.get(oid) or {}
                    lines.append(
                        " - ".join(
                            [
                                f"- object_updated id={oid}",
                                f"visible {before.get('visible')}→{after.get('visible')}",
                                f"path {before.get('path','')}→{after.get('path','')}",
                            ]
                        )
                    )
            # Camera key times
            prev_cam = tuple(prev.get("camera_key_times") or ()) if isinstance(prev, dict) else ()
            cur_cam = tuple(cur.get("camera_key_times") or ()) if isinstance(cur, dict) else ()
            if prev_cam != cur_cam:
                lines.append(f"- camera_keys times={list(cur_cam)}")
            # Timeline key series
            prev_series = (prev.get("key_series") or {}) if isinstance(prev, dict) else {}
            cur_series = (cur.get("key_series") or {}) if isinstance(cur, dict) else {}
            if isinstance(prev_series, dict) and isinstance(cur_series, dict):
                keys = sorted(set(prev_series.keys()) | set(cur_series.keys()))
                for k in keys:
                    a = prev_series.get(k)
                    b = cur_series.get(k)
                    if a == b:
                        continue
                    if a is None:
                        times = [t for (t, _d) in (b or [])]
                        lines.append(f"- keys_added {k} times={times}")
                        continue
                    if b is None:
                        lines.append(f"- keys_removed {k}")
                        continue
                    prev_times = [t for (t, _d) in (a or [])]
                    cur_times = [t for (t, _d) in (b or [])]
                    if prev_times != cur_times:
                        lines.append(f"- keys_times_changed {k} times={cur_times}")
                    else:
                        lines.append(f"- keys_values_changed {k} times={cur_times}")
            # Scene values
            prev_sv = (prev.get("scene_value_digests") or {}) if isinstance(prev, dict) else {}
            cur_sv = (cur.get("scene_value_digests") or {}) if isinstance(cur, dict) else {}
            if isinstance(prev_sv, dict) and isinstance(cur_sv, dict):
                sv_keys = sorted(set(prev_sv.keys()) | set(cur_sv.keys()))
                for k in sv_keys:
                    if prev_sv.get(k) != cur_sv.get(k):
                        lines.append(f"- scene_value_changed {k} (digest)")
            return lines

        try:
            snap = self.scene.scene_facts(
                include_values=True, include_scene_values=True
            )
        except Exception:
            snap = None

        if snap and isinstance(snap, dict):
            prev = self._last_snapshot_flat
            cur = _project_facts(snap)
            diffs = _diff_facts(prev, cur)
            self._last_snapshot_flat = cur

            if prev is not None and not _is_clarify(text):
                if diffs:
                    facts_text = "\n".join(
                        [
                            "Facts (changed since last turn):",
                            *diffs,
                            "Note: timeline/scene values are compared via JSON digests; use scene_get_values or animation_list_keys(include_values=true) for full values.",
                        ]
                    )
                    text = (text + "\n\n" + facts_text) if text else facts_text
                else:
                    # Only warn about "no changes" when the system actually attempted a write.
                    write_tools = {
                        "scene_apply",
                        "scene_ensure_loaded",
                        "scene_load_files",
                        "scene_smart_load",
                        "scene_set_visibility",
                        "scene_make_alias",
                        "scene_cut_set_box",
                        "scene_cut_clear",
                        "animation_ensure_animation",
                        "animation_set_duration",
                        "animation_set_key_param",
                        "animation_replace_key_param",
                        "animation_remove_key_param_at_time",
                        "animation_replace_key_param_at_times",
                        "animation_remove_key",
                        "animation_replace_key_camera",
                        "animation_clear_keys",
                        "animation_batch",
                    }
                    ledger = getattr(self.supervisor, "_last_ledger", None)
                    write_entries = [
                        e
                        for e in (ledger or [])
                        if isinstance(e, dict) and e.get("tool") in write_tools
                    ]
                    write_attempted = bool(write_entries)
                    write_failed = any(
                        isinstance(e.get("result"), dict)
                        and not bool(e["result"].get("ok", True))
                        for e in write_entries
                    )
                    write_all_skipped = bool(write_entries) and all(
                        isinstance(e.get("result"), dict) and ("skipped" in e["result"])
                        for e in write_entries
                    )
                    if (
                        write_attempted
                        and not write_all_skipped
                    ):
                        suffix = (
                            " Some write calls failed; check the tool errors."
                            if write_failed
                            else ""
                        )
                        note = (
                            "Facts: no scene/timeline changes detected this turn."
                            + suffix
                        )
                        guidance = (
                            "If you intended to modify the scene, specify target id, parameter name/json_key, time (for animation), and value."
                        )
                        extra = note + "\n" + guidance
                        text = (text + "\n\n" + extra) if text else extra

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
