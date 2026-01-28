from __future__ import annotations

import json
import logging
from pathlib import Path
from typing import Any, Optional

from .chat_rpc_team import ChatTeam
from .defaults import (
    DEFAULT_AUTO_COMPACT_RATIO_DENOMINATOR,
    DEFAULT_AUTO_COMPACT_RATIO_NUMERATOR,
    DEFAULT_EXECUTOR_MAX_ROUNDS,
    DEFAULT_PLANNER_MAX_ROUNDS,
    DEFAULT_WEB_SEARCH_MODE,
    RESUME_SESSION_PICKER_PREVIEW_MAX_CHARS,
)
from .llm_usage import (
    LlmUsageDelta,
    LlmUsageTotals,
)
from .responses_tool_loop import ToolLoopCallbacks
from .session_resume import (
    format_tool_call_summary_line,
    format_web_search_summary_line,
    iter_resume_items,
    list_sessions,
)


def _try_parse_json(text: str) -> Any:
    try:
        return json.loads(text or "{}")
    except Exception:
        return None


def _render_plan(*, console: Any, team: ChatTeam) -> None:
    from rich.text import Text  # type: ignore

    try:
        plan = team.session_store.get_plan() or []
    except Exception:
        plan = []

    console.print("\n[bold]Plan[/bold]")
    if not plan:
        console.print("[dim](no plan)[/dim]")
        return

    for it in plan:
        if not isinstance(it, dict):
            continue
        step = str(it.get("step") or "").strip()
        status = str(it.get("status") or "").strip()
        if not step:
            continue
        if status == "completed":
            style = "green"
            mark = "✓"
        elif status == "in_progress":
            style = "yellow"
            mark = "…"
        else:
            style = "dim"
            mark = "·"
        console.print(Text(f"{mark} {step}", style=style))


def _render_llm_usage(*, console: Any, team: ChatTeam) -> None:
    """Show real token usage (last turn + session totals) when available."""

    from rich.text import Text  # type: ignore

    def _fmt(n: int | None) -> str:
        return f"{int(n):,}" if isinstance(n, int) else "?"

    try:
        meta = team.session_store.get_meta() or {}
    except Exception:
        meta = {}

    last_turn_usage: LlmUsageTotals | None = None
    if isinstance(meta, dict):
        last_turn_usage = LlmUsageTotals.from_dict(meta.get("llm_last_turn_usage"))
    if isinstance(last_turn_usage, LlmUsageTotals) and last_turn_usage.calls > 0:
        calls = last_turn_usage.calls
        calls_with_usage = last_turn_usage.calls_with_usage
        calls_with_cached = last_turn_usage.calls_with_cached_tokens
        sum_in: int | None = last_turn_usage.input_tokens
        sum_out: int | None = last_turn_usage.output_tokens
        sum_cached: int | None = last_turn_usage.cached_tokens
        sum_uncached_in: int | None = last_turn_usage.uncached_input_tokens

        coverage = f" (usage coverage: {calls_with_usage}/{calls} calls)"
        if calls_with_usage == 0 and calls > 0:
            sum_in = None
            sum_out = None
        cached_note = f" (cached coverage: {calls_with_cached}/{calls} calls)"
        if calls_with_cached == 0 and calls > 0:
            sum_cached = None
            sum_uncached_in = None

        console.print("\n[bold]Last Turn Usage[/bold]")
        console.print(Text(f"calls={calls}{coverage}{cached_note}", style="dim"))
        console.print(
            Text(
                f"input_tokens={_fmt(sum_in)}, output_tokens={_fmt(sum_out)}",
                style="dim",
            )
        )
        console.print(
            Text(
                f"cached_tokens={_fmt(sum_cached)}, uncached_input_tokens={_fmt(sum_uncached_in)}",
                style="dim",
            )
        )

    sess_usage: LlmUsageTotals | None = None
    if isinstance(meta, dict):
        sess_usage = LlmUsageTotals.from_dict(meta.get("llm_session_usage"))
    if isinstance(sess_usage, LlmUsageTotals) and sess_usage.calls > 0:
        calls = sess_usage.calls
        calls_with_usage = sess_usage.calls_with_usage
        calls_with_cached = sess_usage.calls_with_cached_tokens
        sum_in: int | None = sess_usage.input_tokens
        sum_out: int | None = sess_usage.output_tokens
        sum_cached: int | None = sess_usage.cached_tokens
        sum_uncached_in: int | None = sess_usage.uncached_input_tokens

        coverage = f" (usage coverage: {calls_with_usage}/{calls} calls)"
        if calls_with_usage == 0 and calls > 0:
            # Provider did not report usage for any calls yet; avoid printing 0
            # tokens which can be misread as "free". Show unknown instead.
            sum_in = None
            sum_out = None
        cached_note = f" (cached coverage: {calls_with_cached}/{calls} calls)"
        if calls_with_cached == 0 and calls > 0:
            sum_cached = None
            sum_uncached_in = None

        console.print("\n[bold]Session Usage[/bold]")
        console.print(Text(f"calls={calls}{coverage}{cached_note}", style="dim"))
        console.print(
            Text(
                f"input_tokens={_fmt(sum_in)}, output_tokens={_fmt(sum_out)}",
                style="dim",
            )
        )
        console.print(
            Text(
                f"cached_tokens={_fmt(sum_cached)}, uncached_input_tokens={_fmt(sum_uncached_in)}",
                style="dim",
            )
        )


def _render_token_budget(*, console: Any, team: ChatTeam) -> None:
    """Show best-effort model token budget + recent usage stats."""

    from rich.text import Text  # type: ignore

    def _as_pos_int(v: Any) -> int | None:
        if isinstance(v, bool):
            return None
        if isinstance(v, int):
            return int(v) if int(v) > 0 else None
        if isinstance(v, float) and v.is_integer():
            n = int(v)
            return n if n > 0 else None
        return None

    def _fmt(n: int | None) -> str:
        return f"{int(n):,}" if isinstance(n, int) else "?"

    try:
        meta = team.session_store.get_meta() or {}
    except Exception:
        meta = {}

    requested_model = str(getattr(team, "model", "") or "").strip()
    gateway_model = str(meta.get("gateway_model_last") or "").strip()
    gateway_model_l = gateway_model.lower()
    model_key = (
        gateway_model
        if (gateway_model and "detect gateway model" not in gateway_model_l)
        else requested_model
    ).strip()

    total_by_model = (
        meta.get("model_total_context_window_tokens_by_model")
        if isinstance(meta, dict)
        else None
    )
    max_out_by_model = (
        meta.get("model_max_output_tokens_by_model") if isinstance(meta, dict) else None
    )
    eff_in_by_model = (
        meta.get("model_effective_input_budget_tokens_by_model")
        if isinstance(meta, dict)
        else None
    )

    total = (
        _as_pos_int(total_by_model.get(model_key))
        if isinstance(total_by_model, dict) and model_key
        else None
    )
    max_out = (
        _as_pos_int(max_out_by_model.get(model_key))
        if isinstance(max_out_by_model, dict) and model_key
        else None
    )
    eff_in = (
        _as_pos_int(eff_in_by_model.get(model_key))
        if isinstance(eff_in_by_model, dict) and model_key
        else None
    )
    if eff_in is None and total is not None and max_out is not None:
        try:
            v = int(total) - int(max_out)
            eff_in = v if v > 0 else None
        except Exception:
            eff_in = None
    auto_compact = (
        max(
            1,
            (int(eff_in) * DEFAULT_AUTO_COMPACT_RATIO_NUMERATOR)
            // DEFAULT_AUTO_COMPACT_RATIO_DENOMINATOR,
        )
        if eff_in is not None
        else None
    )

    console.print("\n[bold]Token Budget[/bold]")
    console.print(Text(f"requested_model={requested_model or '?'}", style="dim"))
    console.print(
        Text(f"gateway_model={gateway_model or '?'}", style="dim"), markup=False
    )
    if model_key:
        console.print(Text(f"model_key={model_key}", style="dim"), markup=False)
    if total is not None:
        console.print(Text(f"total_context_window_tokens={_fmt(total)}", style="dim"))
    if max_out is not None:
        console.print(Text(f"max_output_tokens={_fmt(max_out)}", style="dim"))
    if eff_in is not None:
        console.print(
            Text(f"effective_input_budget_tokens={_fmt(eff_in)}", style="dim")
        )
    if auto_compact is not None:
        pct = (
            DEFAULT_AUTO_COMPACT_RATIO_NUMERATOR * 100
        ) // DEFAULT_AUTO_COMPACT_RATIO_DENOMINATOR
        console.print(
            Text(f"auto_compact_tokens({pct}%)={_fmt(auto_compact)}", style="dim")
        )

    _render_llm_usage(console=console, team=team)


def _ensure_screenshot_consent(*, console: Any, team: ChatTeam) -> None:
    """Prompt once per session for screenshot-based visual verification consent.

    Default is allow (opt-out), but we persist the explicit decision so future
    runs do not prompt again.
    """
    try:
        decided = team.session_store.get_consent("screenshots")
    except Exception:
        decided = None
    if decided is not None:
        return

    console.print("\n[bold]Privacy consent[/bold]")
    console.print(
        "Atlas Agent can render a single-frame preview image for visual verification.\n"
        "- Used to confirm camera framing / visibility when tool-only checks are insufficient.\n"
        "- The image is generated locally (temporary file) and may be sent to the model for inspection.\n"
        "- If you deny, the agent will fall back to human-check steps for visual requirements.\n",
        markup=False,
    )

    allowed = True
    for _ in range(3):
        ans = (
            console.input("Allow preview screenshots for this session? [Y/n] ")
            .strip()
            .lower()
        )
        if ans in {"", "y", "yes"}:
            allowed = True
            break
        if ans in {"n", "no"}:
            allowed = False
            break
        console.print("[dim]Please answer y/yes or n/no.[/dim]")

    try:
        team.session_store.set_consent("screenshots", allowed)
        team.session_store.save()
    except Exception:
        # Consent must not break startup.
        pass

    if allowed:
        console.print("[green]Screenshots enabled for this session.[/green]")
    else:
        console.print("[yellow]Screenshots disabled for this session.[/yellow]")


def _render_session_replay(*, console: Any, team: ChatTeam) -> None:
    """Replay a saved session history to the terminal (UX for resume).

    Policy (matches user-facing expectations):
    - Prints all transcript messages (user + assistant).
    - Prints a one-line summary for every tool call (and web_search event).
    - Prints only the *current* plan (latest plan_updated), inserted at the point
      it occurred in the session log.
    - Skips internal-only task briefs.
    """

    from rich.text import Text  # type: ignore

    log_path = getattr(team.session_store, "log_path", None)
    if not isinstance(log_path, Path) or not log_path.exists():
        return

    items = list(iter_resume_items(log_path))
    if not any(
        it.kind in {"transcript", "tool_call", "web_search", "plan"} for it in items
    ):
        return

    # Only treat this as a "resume replay" when there is real user/assistant transcript
    # content (otherwise a brand-new session would print nothing but meta/consent noise).
    if not any(it.kind == "transcript" for it in items):
        return

    console.print("\n[dim]--- Resuming session history (replay) ---[/dim]")
    for it in items:
        ev = it.event
        kind = str(it.kind or "")

        if kind == "transcript":
            role = str(ev.get("role") or "").strip().lower()
            content = str(ev.get("content") or "")
            if not content:
                continue
            if role == "user":
                console.print(Text(f"\n>> {content}", style="bold cyan"))
            else:
                console.print("\n[bold]Answer[/bold]")
                console.print(content, markup=False)
            continue

        if kind == "tool_call":
            try:
                ok = None
                policy = str(ev.get("result_policy") or "summary").strip().lower()
                payload = (
                    ev.get("result") if policy == "full" else ev.get("result_summary")
                )
                if isinstance(payload, dict):
                    ok = payload.get("ok")
                style = "cyan"
                if ok is True:
                    style = "green"
                elif ok is False:
                    style = "red"
                console.print(Text(format_tool_call_summary_line(ev), style=style))
            except Exception:
                console.print(Text("→ <tool>: done", style="cyan"))
            continue

        if kind == "web_search":
            console.print(Text(format_web_search_summary_line(ev), style="cyan"))
            continue

        if kind == "plan":
            _render_plan(console=console, team=team)
            continue


def _pick_session_interactive(
    *, console: Any, session_dir: Optional[str]
) -> str | None:
    """Return a session id/path chosen interactively, or None if cancelled."""

    from rich.table import Table  # type: ignore

    from .session_store import default_sessions_root

    sessions_root = (
        Path(session_dir).expanduser().resolve()
        if isinstance(session_dir, str) and session_dir.strip()
        else default_sessions_root()
    )
    if not sessions_root.exists():
        console.print("[dim](no sessions directory)[/dim]")
        return None

    preview_max_chars = int(RESUME_SESSION_PICKER_PREVIEW_MAX_CHARS)
    sessions = list_sessions(
        sessions_root=sessions_root, preview_max_chars=int(preview_max_chars)
    )
    if not sessions:
        console.print("[dim](no sessions found)[/dim]")
        return None

    table = Table(title="Resume a previous session", show_lines=False)
    table.add_column("#", style="dim", justify="right")
    table.add_column("updated", style="dim")
    table.add_column("preview", style="dim")

    rows: list[str] = []
    for i, s in enumerate(sessions, start=1):
        table.add_row(
            str(i),
            s.updated_local_time(),
            str(s.first_user_preview or ""),
        )
        rows.append(str(s.session_id))

    console.print(table)
    console.print(
        f"[dim]Preview is the first user message (truncated to {preview_max_chars} chars).[/dim]"
    )
    console.print(
        "[dim]Enter a number to resume, a session id/path, or blank to cancel.[/dim]"
    )
    ans = console.input("[bold cyan]resume>[/bold cyan] ").strip()
    if not ans:
        return None
    try:
        idx = int(ans)
        if 1 <= idx <= len(rows):
            return rows[idx - 1]
    except Exception:
        pass

    # Allow direct id/path entry.
    return ans


def run_console_repl(
    *,
    address: str,
    api_key: str,
    model: str,
    reasoning_effort: str | None,
    reasoning_summary: str | None,
    text_verbosity: str | None,
    wire_api: str = "auto",
    web_search_mode: str = DEFAULT_WEB_SEARCH_MODE,
    temperature: float | None = None,
    max_rounds: int = DEFAULT_EXECUTOR_MAX_ROUNDS,
    max_rounds_planner: int = DEFAULT_PLANNER_MAX_ROUNDS,
    ephemeral_inline_images: bool = False,
    session: Optional[str] = None,
    session_dir: Optional[str] = None,
    enable_codegen: bool = False,
) -> int:
    """Run a simple streaming CLI (non-fullscreen).

    This is intentionally minimal: a single scrolling terminal view with a
    prompt and styled sections for reasoning summary, tools, plan, and the final
    assistant message.
    """

    try:
        from rich.console import Console  # type: ignore
        from rich.syntax import Syntax  # type: ignore
        from rich.text import Text  # type: ignore
    except Exception as e:
        raise SystemExit(
            "Console UI dependencies are missing. Install with: `pip install rich`.\n"
            f"Import error: {e}"
        ) from e

    logger = logging.getLogger("atlas_agent.console")
    if not logger.handlers:
        logger.addHandler(logging.NullHandler())

    console = Console()
    team = ChatTeam(
        address=address,
        api_key=api_key,
        model=model,
        wire_api=wire_api,
        web_search_mode=str(web_search_mode or DEFAULT_WEB_SEARCH_MODE),
        temperature=temperature,
        max_rounds_executor=int(max_rounds),
        max_rounds_planner=int(max_rounds_planner),
        ephemeral_inline_images=bool(ephemeral_inline_images),
        session=session,
        session_dir=session_dir,
        enable_codegen=bool(enable_codegen),
        reasoning_effort=reasoning_effort,
        reasoning_summary=reasoning_summary,
        text_verbosity=text_verbosity,
    )

    console.print(
        f"[bold]Atlas Agent[/bold]. Session=[cyan]{team.session_store.session_id()}[/cyan]"
    )
    console.print(f"[dim]Atlas app:[/dim] {team.atlas_dir}", markup=False)
    console.print("[dim]Type :help for commands. Ctrl+C to exit.[/dim]")
    _render_session_replay(console=console, team=team)
    _ensure_screenshot_consent(console=console, team=team)

    while True:
        try:
            line = console.input("\n[bold cyan]>>[/bold cyan] ").strip()
        except (EOFError, KeyboardInterrupt):
            console.print("")
            return 0

        if not line:
            continue

        if line.startswith(":"):
            cmd, *rest = line[1:].split()
            if cmd in {"q", "quit", "exit"}:
                return 0
            if cmd in {"h", "help"}:
                console.print(
                    "\n[bold]Commands[/bold]\n"
                    "[cyan]:help[/cyan]                This help\n"
                    "[cyan]:session[/cyan]             Show session paths\n"
                    "[cyan]:resume[/cyan]              Switch to another session (interactive)\n"
                    "[cyan]:screenshots on|off[/cyan]  Toggle preview screenshots for this session\n"
                    "[cyan]:brief[/cyan]               Show the latest Task Brief\n"
                    "[cyan]:plan[/cyan]                Show current plan\n"
                    "[cyan]:memory[/cyan]              Show session memory summary\n"
                    "[cyan]:budget[/cyan]              Show token budget and recent usage\n"
                    "[cyan]:events [N][/cyan]          Show recent events\n"
                    "[cyan]:save <path>[/cyan]         Save current animation\n"
                    "[cyan]:time <seconds>[/cyan]      Set current time\n"
                    "[cyan]:objects[/cyan]             List objects"
                )
                continue
            if cmd == "resume":
                # :resume [<session-id-or-path>]
                target = ""
                if rest:
                    target = str(rest[0] or "").strip()
                else:
                    picked = _pick_session_interactive(
                        console=console, session_dir=session_dir
                    )
                    target = str(picked or "").strip()
                if not target:
                    continue
                try:
                    team = ChatTeam(
                        address=address,
                        api_key=api_key,
                        model=model,
                        wire_api=wire_api,
                        web_search_mode=str(web_search_mode or DEFAULT_WEB_SEARCH_MODE),
                        temperature=temperature,
                        max_rounds_executor=int(max_rounds),
                        max_rounds_planner=int(max_rounds_planner),
                        ephemeral_inline_images=bool(ephemeral_inline_images),
                        session=target,
                        session_dir=session_dir,
                        enable_codegen=bool(enable_codegen),
                        reasoning_effort=reasoning_effort,
                        reasoning_summary=reasoning_summary,
                        text_verbosity=text_verbosity,
                    )
                except Exception as e:
                    console.print(f"[red]fail:[/red] {e}")
                    continue
                console.print(
                    f"[bold]Atlas Agent[/bold]. Session=[cyan]{team.session_store.session_id()}[/cyan]"
                )
                console.print(f"[dim]Atlas app:[/dim] {team.atlas_dir}", markup=False)
                _render_session_replay(console=console, team=team)
                _ensure_screenshot_consent(console=console, team=team)
                continue
            if cmd == "session":
                console.print(
                    f"session_id=[cyan]{team.session_store.session_id()}[/cyan]"
                )
                console.print(f"log={team.session_store.log_path}", markup=False)
                try:
                    c = team.session_store.get_consent("screenshots")
                except Exception:
                    c = None
                console.print(f"consent.screenshots={c!r}", markup=False)
                continue
            if cmd == "screenshots":
                if not rest:
                    try:
                        c = team.session_store.get_consent("screenshots")
                    except Exception:
                        c = None
                    console.print(
                        f"\nconsent.screenshots={c!r}\nUsage: :screenshots on | off",
                        markup=False,
                    )
                    continue
                v = (rest[0] or "").strip().lower()
                if v in {"on", "1", "true", "yes", "y"}:
                    allowed = True
                elif v in {"off", "0", "false", "no", "n"}:
                    allowed = False
                else:
                    console.print("[red]Usage:[/red] :screenshots on | off")
                    continue
                try:
                    team.session_store.set_consent("screenshots", allowed)
                    team.session_store.save()
                except Exception:
                    pass
                console.print("[green]ok[/green]" if allowed else "[yellow]ok[/yellow]")
                continue
            if cmd == "brief":
                try:
                    evs = team.session_store.tail_events(
                        limit=1, event_type="task_brief"
                    )
                except Exception as e:
                    console.print(f"[red]fail:[/red] {e}")
                    continue
                if not evs:
                    console.print("\n[bold]Task Brief[/bold]")
                    console.print("[dim](no task brief recorded yet)[/dim]")
                    continue
                ev = evs[-1]
                text = str(ev.get("text") or "").strip()
                console.print("\n[bold]Task Brief[/bold]")
                if text:
                    console.print(text, markup=False)
                else:
                    console.print("[dim](empty)[/dim]")
                continue
            if cmd == "plan":
                _render_plan(console=console, team=team)
                continue
            if cmd == "memory":
                try:
                    mem = team.session_store.get_memory_summary()
                except Exception:
                    mem = ""
                console.print("\n[bold]Session Memory[/bold]")
                if mem:
                    console.print(mem, markup=False)
                else:
                    console.print("[dim](empty)[/dim]")
                continue
            if cmd == "budget":
                _render_token_budget(console=console, team=team)
                continue
            if cmd == "events":
                try:
                    n = int(rest[0]) if rest else 20
                except Exception:
                    n = 20
                try:
                    evs = team.session_store.tail_events(limit=max(1, n))
                except Exception as e:
                    console.print(f"[red]fail:[/red] {e}")
                    continue
                if not evs:
                    console.print("[dim](no events)[/dim]")
                    continue
                console.print("\n[bold]Recent Events[/bold]")
                for ev in evs:
                    try:
                        ts = ev.get("ts")
                        et = ev.get("type")
                        tool = ev.get("tool")
                        tid = ev.get("turn_id")
                        console.print(
                            f"[dim]{ts}[/dim]\t{et}\t{tool}\t[dim]{tid}[/dim]"
                        )
                    except Exception:
                        console.print(str(ev), markup=False)
                continue
            if cmd == "save" and rest:
                ok = False
                try:
                    resp = team.scene.ensure_animation(create_new=False, name=None)
                    aid = int(getattr(resp, "animation_id", 0) or 0)
                    if bool(getattr(resp, "ok", False)) and aid > 0:
                        ok = bool(
                            team.scene.save_animation(
                                animation_id=aid, path=Path(rest[0])
                            )
                        )
                except Exception:
                    ok = False
                console.print("[green]ok[/green]" if ok else "[red]fail[/red]")
                continue
            if cmd == "time" and rest:
                try:
                    ok = False
                    resp = team.scene.ensure_animation(create_new=False, name=None)
                    aid = int(getattr(resp, "animation_id", 0) or 0)
                    if bool(getattr(resp, "ok", False)) and aid > 0:
                        ok = bool(
                            team.scene.set_time(
                                animation_id=aid, seconds=float(rest[0])
                            )
                        )
                except Exception:
                    ok = False
                console.print("[green]ok[/green]" if ok else "[red]fail[/red]")
                continue
            if cmd == "objects":
                resp = team.scene.list_objects()
                console.print("\n[bold]Objects[/bold]")
                for obj in resp.objects:
                    console.print(
                        f"{obj.id}\t{obj.type}\t{obj.name}\t{obj.visible}",
                        markup=False,
                    )
                continue
            console.print("[red]Unknown command[/red]; try :help")
            continue

        # Natural language turn (stream reasoning summary, show tools, then final answer).
        printed_reasoning = False
        current_phase = "Executor"

        def _on_phase_start(phase: str) -> None:
            nonlocal printed_reasoning, current_phase
            current_phase = str(phase or "").strip() or "Phase"
            printed_reasoning = False
            console.print(Text(f"\n# {current_phase}", style="bold magenta"))

        def _on_phase_end(_phase: str) -> None:
            # The tool loop streams output; add a small separator between phases.
            nonlocal printed_reasoning
            if printed_reasoning:
                console.print("\n")
            printed_reasoning = False

        def _on_reasoning_delta(delta: str, _summary_index: int) -> None:
            nonlocal printed_reasoning
            if not delta:
                return
            if not printed_reasoning:
                console.print(
                    f"\n[bold]Reasoning summary[/bold] [dim](streaming; {current_phase})[/dim]"
                )
                printed_reasoning = True
            console.print(Text(delta, style="dim"), end="")

        def _on_reasoning_part_added(_summary_index: int) -> None:
            if printed_reasoning:
                console.print("\n")

        def _on_tool_call(name: str, args_json: str, _call_id: str) -> None:
            console.print(Text(f"\n→ {name}", style="cyan"))
            parsed = _try_parse_json(args_json)
            if parsed is None:
                console.print("[dim](args not valid JSON)[/dim]")
                if args_json:
                    console.print(args_json, markup=False)
                return
            dumped = json.dumps(parsed, ensure_ascii=False, indent=2, sort_keys=True)
            console.print(Syntax(dumped, "json", word_wrap=True))

        def _on_tool_result(name: str, _call_id: str, result_json: str) -> None:
            parsed = _try_parse_json(result_json)
            ok = None
            err = ""
            if isinstance(parsed, dict):
                ok = parsed.get("ok")
                err = str(parsed.get("error") or "")
            if ok is True:
                console.print(Text(f"← {name}: ok", style="green"))
            elif ok is False:
                msg = Text(f"← {name}: fail", style="red")
                if err:
                    msg.append(" ")
                    msg.append(err)
                console.print(msg)

                # For filesystem resolution tools, failures are often "soft":
                # they still return ranked candidates and the searched roots.
                # Show that context so users (and developers) can understand why
                # a resolve did not return ok=true.
                if isinstance(parsed, dict) and any(
                    k in parsed
                    for k in (
                        "hint",
                        "path",
                        "match",
                        "expected_name",
                        "candidates",
                        "tried",
                        "searched_dirs",
                        "missing_dirs",
                    )
                ):
                    extra: dict[str, Any] = {}
                    for k in (
                        "hint",
                        "match",
                        "expected_name",
                        "path",
                        "candidates",
                        "tried",
                        "searched_dirs",
                        "missing_dirs",
                    ):
                        if k in parsed:
                            extra[k] = parsed.get(k)
                    if extra:
                        dumped = json.dumps(
                            extra, ensure_ascii=False, indent=2, sort_keys=True
                        )
                        console.print(Syntax(dumped, "json", word_wrap=True))
            else:
                console.print(Text(f"← {name}: done", style="green"))

            if name == "update_plan" and ok is True:
                _render_plan(console=console, team=team)

        def _on_web_search_call(call: dict[str, Any], _call_index: int) -> None:
            action = call.get("action") if isinstance(call, dict) else None
            if not isinstance(action, dict) or not action:
                console.print(Text("\n→ web_search", style="cyan"))
                return
            at = str(action.get("type") or "").strip()
            parts: list[str] = []
            if at:
                parts.append(at)
            for k in ("query", "url", "pattern"):
                v = action.get(k)
                if isinstance(v, str) and v.strip():
                    parts.append(f"{k}={v.strip()!r}")
            suffix = (" " + " ".join(parts)) if parts else ""
            console.print(Text(f"\n→ web_search{suffix}", style="cyan"))

        def _on_response_meta(resp: dict[str, Any], call_index: int) -> None:
            nonlocal printed_reasoning

            model_name = None
            if isinstance(resp, dict):
                model_name = resp.get("model")
            if not isinstance(model_name, str) or not model_name.strip():
                model_name = "can not detect gateway model"
            model_name = model_name.strip()

            requested = str(model or "").strip()
            suffix = (
                f" (requested {requested})"
                if requested and model_name != requested
                else ""
            )

            # If we were streaming reasoning without a trailing newline, ensure the
            # meta line doesn't glue onto the previous output.
            if printed_reasoning:
                console.print()

            def _fmt(n: int | None) -> str:
                return f"{int(n):,}" if isinstance(n, int) else "?"

            usage = team._llm_last_call_usage
            if team._llm_last_call_index != call_index:
                usage = None

            turn_usage = team._llm_turn_usage_current
            sess_usage = team._llm_session_usage

            call_in = usage.input_tokens if usage is not None else None
            call_out = usage.output_tokens if usage is not None else None
            call_cached = usage.cached_tokens if usage is not None else None
            call_uncached = usage.uncached_input_tokens if usage is not None else None

            turn_in = turn_usage.input_tokens
            turn_out = turn_usage.output_tokens
            turn_cached = turn_usage.cached_tokens
            turn_uncached = turn_usage.uncached_input_tokens

            sess_in = sess_usage.input_tokens
            sess_out = sess_usage.output_tokens
            sess_cached = sess_usage.cached_tokens
            sess_uncached = sess_usage.uncached_input_tokens

            # Avoid showing misleading zeros when coverage is missing.
            if turn_usage.calls > 0 and turn_usage.calls_with_usage == 0:
                turn_in = None
                turn_out = None
            if turn_usage.calls > 0 and turn_usage.calls_with_cached_tokens == 0:
                turn_cached = None
                turn_uncached = None

            if sess_usage.calls > 0 and sess_usage.calls_with_usage == 0:
                sess_in = None
                sess_out = None
            if sess_usage.calls > 0 and sess_usage.calls_with_cached_tokens == 0:
                sess_cached = None
                sess_uncached = None

            prefix = (
                f"[llm {current_phase}#{int(call_index or 0)}] {model_name}{suffix}"
            )
            if usage is None:
                console.print(Text(prefix + " (usage unavailable)", style="dim"))
                return

            call_part = f"call(in={_fmt(call_in)} cached={_fmt(call_cached)} uncached={_fmt(call_uncached)} out={_fmt(call_out)})"
            turn_part = f"turn(in={_fmt(turn_in)} cached={_fmt(turn_cached)} uncached={_fmt(turn_uncached)} out={_fmt(turn_out)})"
            sess_part = f"session(in={_fmt(sess_in)} cached={_fmt(sess_cached)} uncached={_fmt(sess_uncached)} out={_fmt(sess_out)})"

            console.print(
                Text(" | ".join([prefix, call_part, turn_part, sess_part]), style="dim")
            )

        callbacks = ToolLoopCallbacks(
            on_phase_start=_on_phase_start,
            on_phase_end=_on_phase_end,
            on_reasoning_summary_delta=_on_reasoning_delta,
            on_reasoning_summary_part_added=_on_reasoning_part_added,
            on_response_meta=_on_response_meta,
            on_web_search_call=_on_web_search_call,
            on_tool_call=_on_tool_call,
            on_tool_result=_on_tool_result,
        )

        try:
            answer = team.turn(
                line,
                shared_context=None,
                callbacks=callbacks,
                emit_to_stdout=False,
            )
        except Exception as e:
            console.print(f"\n[red]Agent error:[/red] {e}")
            continue

        console.print("\n[bold]Answer[/bold]")
        console.print(answer, markup=False)
