from __future__ import annotations

import json
import logging
from pathlib import Path
from typing import Any, Optional

from .chat_rpc_team import ChatTeam
from .defaults import DEFAULT_EXECUTOR_MAX_ROUNDS, DEFAULT_PLANNER_MAX_ROUNDS
from .responses_tool_loop import ToolLoopCallbacks


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


def _render_token_budget(*, console: Any, team: ChatTeam) -> None:
    """Show best-effort model token budget + recent usage stats."""

    from rich.text import Text  # type: ignore

    def _as_nonneg_int(v: Any) -> int | None:
        if isinstance(v, bool):
            return None
        if isinstance(v, int):
            return int(v) if int(v) >= 0 else None
        if isinstance(v, float) and v.is_integer():
            n = int(v)
            return n if n >= 0 else None
        return None

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
    auto_compact = max(1, (int(eff_in) * 9) // 10) if eff_in is not None else None

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
        console.print(
            Text(f"auto_compact_tokens(90%)={_fmt(auto_compact)}", style="dim")
        )

    last_turn_usage = (
        meta.get("llm_last_turn_usage") if isinstance(meta, dict) else None
    )
    if isinstance(last_turn_usage, dict):
        calls = _as_nonneg_int(last_turn_usage.get("calls"))
        calls_with_usage = _as_nonneg_int(last_turn_usage.get("calls_with_usage"))
        sum_in = _as_nonneg_int(last_turn_usage.get("input_tokens"))
        sum_out = _as_nonneg_int(last_turn_usage.get("output_tokens"))
        sum_total = _as_nonneg_int(last_turn_usage.get("total_tokens"))
        if any(
            v is not None for v in (calls, calls_with_usage, sum_in, sum_out, sum_total)
        ):
            coverage = ""
            if calls is not None and calls_with_usage is not None:
                coverage = f" (usage coverage: {calls_with_usage}/{calls} calls)"
            console.print("\n[bold]Last Turn Usage[/bold]")
            console.print(
                Text(
                    f"calls={calls if calls is not None else '?'}"
                    f", input_tokens={_fmt(sum_in)}"
                    f", output_tokens={_fmt(sum_out)}"
                    f", total_tokens={_fmt(sum_total)}{coverage}",
                    style="dim",
                )
            )

    try:
        recent = team.session_store.tail_events(limit=5, event_type="llm_call_stats")
    except Exception:
        recent = []
    if not recent:
        console.print("[dim](no llm_call_stats recorded yet)[/dim]")
        return

    console.print("\n[bold]Recent LLM Call Stats[/bold]")
    for ev in recent:
        if not isinstance(ev, dict):
            continue
        ph = str(ev.get("phase") or "").strip()
        try:
            call_index = int(ev.get("call_index", 0) or 0)
        except Exception:
            call_index = 0
        req = ev.get("request") if isinstance(ev.get("request"), dict) else {}
        usage = ev.get("usage") if isinstance(ev.get("usage"), dict) else {}
        est_in = (
            _as_pos_int(req.get("estimated_input_tokens"))
            if isinstance(req, dict)
            else None
        )
        eff = (
            _as_pos_int(req.get("effective_input_budget_tokens"))
            if isinstance(req, dict)
            else None
        )
        inp = (
            _as_pos_int(usage.get("input_tokens")) if isinstance(usage, dict) else None
        )
        out = (
            _as_pos_int(usage.get("output_tokens")) if isinstance(usage, dict) else None
        )

        used = inp if inp is not None else est_in
        used_label = "in" if inp is not None else "in≈"
        ratio = ""
        if used is not None and eff is not None and eff > 0:
            try:
                pct = (float(used) / float(eff)) * 100.0
                ratio = f" ({pct:.1f}%)"
            except Exception:
                ratio = ""
        tail = ""
        if out is not None:
            tail = f", out={_fmt(out)}"
        console.print(
            Text(
                f"- {ph}#{call_index}: {used_label}{_fmt(used)}/{_fmt(eff)}{ratio}{tail}",
                style="dim",
            )
        )


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


def run_console_repl(
    *,
    address: str,
    api_key: str,
    model: str,
    wire_api: str = "auto",
    temperature: float | None = None,
    reasoning_effort: str | None = "high",
    max_rounds: int = DEFAULT_EXECUTOR_MAX_ROUNDS,
    max_rounds_planner: int = DEFAULT_PLANNER_MAX_ROUNDS,
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
        temperature=temperature,
        reasoning_effort=reasoning_effort,
        max_rounds_executor=int(max_rounds),
        max_rounds_planner=int(max_rounds_planner),
        session=session,
        session_dir=session_dir,
        enable_codegen=bool(enable_codegen),
    )

    console.print(
        f"[bold]Atlas Agent[/bold]. Session=[cyan]{team.session_store.session_id()}[/cyan]"
    )
    console.print(f"[dim]Atlas app:[/dim] {team.atlas_dir}", markup=False)
    console.print("[dim]Type :help for commands. Ctrl+C to exit.[/dim]")
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
        last_gateway_model: str | None = None
        request_meta_by_call: dict[int, dict[str, Any]] = {}

        def _on_phase_start(phase: str) -> None:
            nonlocal printed_reasoning, current_phase, last_gateway_model
            current_phase = str(phase or "").strip() or "Phase"
            printed_reasoning = False
            last_gateway_model = None
            console.print(Text(f"\n# {current_phase}", style="bold magenta"))

        def _on_phase_end(_phase: str) -> None:
            # The tool loop streams output; add a small separator between phases.
            nonlocal printed_reasoning
            if printed_reasoning:
                console.print("\n")
            printed_reasoning = False

        def _on_request_meta(req_meta: dict[str, Any], call_index: int) -> None:
            nonlocal printed_reasoning, request_meta_by_call
            if not isinstance(req_meta, dict):
                return

            def _pos_int(v: Any) -> int | None:
                if isinstance(v, bool):
                    return None
                if isinstance(v, int):
                    return int(v) if int(v) >= 0 else None
                if isinstance(v, float) and v.is_integer():
                    n = int(v)
                    return n if n >= 0 else None
                return None

            idx = int(call_index or 0)
            request_meta_by_call[idx] = dict(req_meta)
            est = _pos_int(req_meta.get("estimated_input_tokens"))
            eff = _pos_int(req_meta.get("effective_input_budget_tokens"))
            auto = _pos_int(req_meta.get("auto_compact_tokens"))

            used = est
            pct = None
            if used is not None and eff is not None and eff > 0:
                try:
                    pct = (float(used) / float(eff)) * 100.0
                except Exception:
                    pct = None

            style = "dim"
            if pct is not None:
                if pct >= 90.0:
                    style = "red"
                elif pct >= 75.0:
                    style = "yellow"

            def _fmt(n: int | None) -> str:
                return f"{int(n):,}" if isinstance(n, int) else "?"

            parts: list[str] = []
            if used is not None:
                parts.append(f"in≈{_fmt(used)}/{_fmt(eff)}")
            else:
                parts.append(f"in≈?/{_fmt(eff)}")
            if pct is not None:
                parts.append(f"({pct:.1f}%)")
            if auto is not None:
                parts.append(f"auto={_fmt(auto)}")

            # Avoid gluing onto a streaming reasoning line.
            if printed_reasoning:
                console.print()
            console.print(
                Text(f"[context {current_phase}#{idx}] " + " ".join(parts), style=style)
            )

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

        def _on_response_meta(resp: dict[str, Any], call_index: int) -> None:
            nonlocal last_gateway_model, printed_reasoning
            model_name = None
            if isinstance(resp, dict):
                model_name = resp.get("model")
            if not isinstance(model_name, str) or not model_name.strip():
                model_name = "can not detect gateway model"
            model_name = model_name.strip()
            if last_gateway_model == model_name:
                return
            last_gateway_model = model_name
            # If we were streaming reasoning without a trailing newline, ensure the
            # meta line doesn't glue onto the previous output.
            if printed_reasoning:
                console.print()
            requested = str(model or "").strip()
            suffix = (
                f" (requested {requested})"
                if requested and model_name != requested
                else ""
            )
            tok_suffix = ""
            try:
                usage = resp.get("usage") if isinstance(resp, dict) else None
                if isinstance(usage, dict):
                    inp = usage.get("input_tokens")
                    if not isinstance(inp, int):
                        inp = (
                            usage.get("prompt_tokens")
                            if isinstance(usage.get("prompt_tokens"), int)
                            else None
                        )
                    out = usage.get("output_tokens")
                    if not isinstance(out, int):
                        out = (
                            usage.get("completion_tokens")
                            if isinstance(usage.get("completion_tokens"), int)
                            else None
                        )
                else:
                    inp = None
                    out = None
                req_meta = request_meta_by_call.get(int(call_index or 0), {})
                eff = (
                    req_meta.get("effective_input_budget_tokens")
                    if isinstance(req_meta, dict)
                    else None
                )
                if isinstance(eff, bool) or not isinstance(eff, int) or eff <= 0:
                    eff = None
                used = inp if isinstance(inp, int) and inp >= 0 else None
                pct = None
                if used is not None and eff is not None:
                    try:
                        pct = (float(used) / float(eff)) * 100.0
                    except Exception:
                        pct = None
                if used is not None or out is not None:
                    toks = []
                    if used is not None:
                        if eff is not None and pct is not None:
                            toks.append(f"in={used:,}/{eff:,} ({pct:.1f}%)")
                        else:
                            toks.append(f"in={used:,}")
                    if isinstance(out, int) and out >= 0:
                        toks.append(f"out={out:,}")
                    if toks:
                        tok_suffix = " [" + ", ".join(toks) + "]"
            except Exception:
                tok_suffix = ""

            console.print(
                Text(f"[gateway model: {model_name}{suffix}]{tok_suffix}", style="dim")
            )

        callbacks = ToolLoopCallbacks(
            on_phase_start=_on_phase_start,
            on_phase_end=_on_phase_end,
            on_reasoning_summary_delta=_on_reasoning_delta,
            on_reasoning_summary_part_added=_on_reasoning_part_added,
            on_request_meta=_on_request_meta,
            on_response_meta=_on_response_meta,
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
