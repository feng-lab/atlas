from __future__ import annotations

import json
import logging
from pathlib import Path
from typing import Any, Optional

from .chat_rpc_team import ChatTeam
from .defaults import DEFAULT_EXECUTOR_MAX_ROUNDS
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
        ans = console.input("Allow preview screenshots for this session? [Y/n] ").strip().lower()
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
        session=session,
        session_dir=session_dir,
        enable_codegen=bool(enable_codegen),
    )

    console.print(f"[bold]Atlas Agent[/bold]. Session=[cyan]{team.session_store.session_id()}[/cyan]")
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
                    "[cyan]:events [N][/cyan]          Show recent events\n"
                    "[cyan]:save <path>[/cyan]         Save current animation\n"
                    "[cyan]:time <seconds>[/cyan]      Set current time\n"
                    "[cyan]:objects[/cyan]             List objects"
                )
                continue
            if cmd == "session":
                console.print(f"session_id=[cyan]{team.session_store.session_id()}[/cyan]")
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
                        f"\nconsent.screenshots={c!r}\n"
                        "Usage: :screenshots on | off",
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
                    evs = team.session_store.tail_events(limit=1, event_type="task_brief")
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
                        console.print(f"[dim]{ts}[/dim]\t{et}\t{tool}\t[dim]{tid}[/dim]")
                    except Exception:
                        console.print(str(ev), markup=False)
                continue
            if cmd == "save" and rest:
                ok = False
                try:
                    resp = team.scene.ensure_animation(create_new=False, name=None)
                    aid = int(getattr(resp, "animation_id", 0) or 0)
                    if bool(getattr(resp, "ok", False)) and aid > 0:
                        ok = bool(team.scene.save_animation(animation_id=aid, path=Path(rest[0])))
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
                        ok = bool(team.scene.set_time(animation_id=aid, seconds=float(rest[0])))
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
                        dumped = json.dumps(extra, ensure_ascii=False, indent=2, sort_keys=True)
                        console.print(Syntax(dumped, "json", word_wrap=True))
            else:
                console.print(Text(f"← {name}: done", style="green"))

            if name == "update_plan" and ok is True:
                _render_plan(console=console, team=team)

        def _on_response_meta(resp: dict[str, Any], _call_index: int) -> None:
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
            suffix = f" (requested {requested})" if requested and model_name != requested else ""
            console.print(Text(f"[gateway model: {model_name}{suffix}]", style="dim"))

        callbacks = ToolLoopCallbacks(
            on_phase_start=_on_phase_start,
            on_phase_end=_on_phase_end,
            on_reasoning_summary_delta=_on_reasoning_delta,
            on_reasoning_summary_part_added=_on_reasoning_part_added,
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
