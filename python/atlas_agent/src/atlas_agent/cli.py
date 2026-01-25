import sys

# Enforce minimum Python version early (fail-fast at import)
if sys.version_info < (3, 12):
    raise SystemExit(
        f"Atlas Agent requires Python 3.12+ (detected {sys.version.split()[0]}). "
        "Please upgrade your Python interpreter."
    )

import argparse
import json
import logging
import os

from .chat_rpc_team import run_repl as run_team_repl
from .console_ui import run_console_repl
from .defaults import (
    DEFAULT_EXECUTOR_MAX_ROUNDS,
    DEFAULT_PLANNER_MAX_ROUNDS,
    DEFAULT_WEB_SEARCH_MODE,
)


def main(argv: list[str] | None = None) -> int:
    # Atlas runs a local gRPC server; by convention we connect to localhost.
    address = "localhost:50051"

    parser = argparse.ArgumentParser(
        prog="atlas-agent",
        description="Atlas animation agent (chat only): control Atlas GUI via RPC",
    )
    # Single entry; accept an optional first positional (e.g., 'chat' or 'chat-rpc')
    parser.add_argument("cmd", nargs="?", help=argparse.SUPPRESS)
    parser.add_argument(
        "--model",
        default="gpt-5.2",
    )
    parser.add_argument(
        "--wire-api",
        default="auto",
        choices=["auto", "responses", "chat"],
        help=(
            "Which OpenAI-compatible wire API to use for tool-calling. "
            "'auto' prefers Responses API and falls back to Chat Completions when unsupported."
        ),
    )
    parser.add_argument(
        "--reasoning-effort",
        default="xhigh",
        choices=["low", "medium", "high", "xhigh"],
        help="Reasoning effort for Responses API calls (when supported by the model/provider).",
    )
    parser.add_argument(
        "--reasoning-summary",
        default="detailed",
        choices=["auto", "concise", "detailed"],
        help=(
            "Reasoning summary control for Responses API calls (when supported by the model/provider).\n"
            "- auto|concise|detailed: forwarded to the provider as reasoning.summary"
        ),
    )
    parser.add_argument(
        "--text-verbosity",
        default="high",
        choices=["low", "medium", "high"],
        help="Text verbosity control for Responses API calls (when supported by the model/provider).",
    )
    parser.add_argument(
        "--temperature",
        type=float,
        default=None,
        help=(
            "Sampling temperature. By default it is omitted (provider/model default). "
            "Some models/providers reject temperature; the agent will retry without it."
        ),
    )
    parser.add_argument(
        "--max-rounds",
        type=int,
        default=DEFAULT_EXECUTOR_MAX_ROUNDS,
        help=(
            "Maximum tool-loop rounds for the Executor phase (0 = unlimited). "
            "Increase for very complex tasks that require many tool calls."
        ),
    )
    parser.add_argument(
        "--planner-max-rounds",
        type=int,
        default=DEFAULT_PLANNER_MAX_ROUNDS,
        help=(
            "Maximum tool-loop rounds for the Planner phase (0 = unlimited). "
            "Increase if the Planner sometimes fails to emit update_plan/verification_set_requirements "
            "before handing off to the Executor."
        ),
    )
    parser.add_argument(
        "--session",
        default=None,
        help="Session id or path to a session dir. Persists plan/memory across restarts.",
    )
    parser.add_argument(
        "--session-dir",
        default=None,
        help="Root directory for sessions (defaults to ~/.atlas_agent/sessions or XDG/APPDATA).",
    )
    parser.add_argument(
        "--enable-codegen",
        action="store_true",
        help="Enable code generation tools (python_write_and_run).",
    )
    parser.add_argument(
        "--web-search",
        default=DEFAULT_WEB_SEARCH_MODE,
        choices=["off", "cached", "live"],
        help=(
            "Enable the Responses API built-in web_search tool.\n"
            "- off: disable web search\n"
            "- cached: cached content only (no live internet)\n"
            "- live: allow live internet access (provider-controlled)"
        ),
    )
    parser.add_argument(
        "--ephemeral-inline-images",
        action="store_true",
        help=(
            "When enabled, inline base64 preview images (tool outputs) are only included in the *next* model call, "
            "then removed from prompt history.\n"
            "This reduces token usage, but the model cannot refer to older screenshots after a few tool-loop rounds."
        ),
    )
    parser.add_argument(
        "--live-subprocess-tail",
        default=True,
        action=argparse.BooleanOptionalAction,
        help=(
            "Show a live-updating tail for long-running subprocess output (e.g. animation export/preview).\n"
            "This is only used in the rich console UI; it is ignored in --plain mode."
        ),
    )
    parser.add_argument(
        "--dump-tools",
        action="store_true",
        help=(
            "Print the tool JSON that would be sent to the model (after provider-border schema tightening) and exit.\n"
            "This is useful for debugging tool schemas (e.g., array item definitions like waypoints/segments)."
        ),
    )
    parser.add_argument(
        "--dump-tools-phase",
        default="executor",
        choices=["executor", "planner", "both"],
        help="Which phase tool list to dump when using --dump-tools.",
    )
    parser.add_argument(
        "--dump-tools-wire",
        default="responses",
        choices=["responses", "chat", "both"],
        help=(
            "Which wire format to dump when using --dump-tools.\n"
            "- responses: Responses API tool list (supports built-ins like web_search)\n"
            "- chat: Chat Completions tool list (function tools only)\n"
            "- both: include both shapes"
        ),
    )
    parser.add_argument(
        "--dump-tools-format",
        default="pretty",
        choices=["pretty", "compact"],
        help="Output formatting for --dump-tools JSON.",
    )
    parser.add_argument(
        "--plain",
        action="store_true",
        help="Disable styling and use the plain REPL (debugging/limited terminals).",
    )
    args = parser.parse_args(argv)
    # Ignore deprecated positional subcommands like 'chat' or 'chat-rpc'
    if not logging.getLogger().handlers:
        logging.basicConfig(
            level=logging.INFO,
            format="%(asctime)s [%(levelname)s] %(name)s: %(message)s",
            datefmt="%H:%M:%S",
        )

    # Back-compat / convenience positional commands (hidden from argparse help).
    if args.cmd:
        cmd = str(args.cmd).strip().lower()
        if cmd in {"dump-tools", "dump_tools"}:
            args.dump_tools = True
            args.cmd = None

    if args.cmd and args.cmd not in ("chat", "chat-rpc"):
        logging.error(
            "Unknown command; this CLI supports chat only. Usage: python -m atlas_agent"
        )
        return 2

    # Global runtime knobs (best-effort). Keep these centralized in the CLI so
    # tool modules don't need to thread flags through many layers.
    try:
        from .subprocess_utils import set_live_subprocess_tail

        # In --plain mode there is no rich UI, so live-updating output isn't used.
        if bool(getattr(args, "plain", False)):
            set_live_subprocess_tail(False)
        else:
            set_live_subprocess_tail(bool(getattr(args, "live_subprocess_tail", True)))
    except Exception:
        pass

    if bool(getattr(args, "dump_tools", False)):
        try:
            # Build the same tool list the chat runtime advertises to the model.
            from .agent_team.tool_modules import build_tools
            from .chat_rpc_team import (
                ATLAS_OUTPUT_TOOLS,
                ATLAS_STATE_MUTATION_TOOLS,
                CODEGEN_TOOLS,
                SESSION_MUTATION_TOOLS,
            )
            from .provider_tool_schema import (
                normalize_tools_for_chat_completions_api,
                normalize_tools_for_responses_api,
            )

            tool_objects = build_tools()

            # Mirror scene_tools_and_dispatcher: hide codegen tools unless explicitly enabled.
            if not bool(getattr(args, "enable_codegen", False)):
                codegen_tool_names = {"python_write_and_run", "codegen_allowed_imports"}
                tool_objects = [
                    t for t in tool_objects if t.name not in codegen_tool_names
                ]

            tools = [t.to_chat_tool_spec() for t in tool_objects]

            def _tool_name(tool_spec: dict) -> str:
                if not isinstance(tool_spec, dict):
                    return ""
                if str(tool_spec.get("type") or "") != "function":
                    return ""
                fn = tool_spec.get("function")
                if not isinstance(fn, dict):
                    return ""
                return str(fn.get("name") or "")

            all_tool_names = {n for n in (_tool_name(t) for t in tools) if n}
            read_only_tool_names = (
                all_tool_names
                - set(ATLAS_STATE_MUTATION_TOOLS)
                - set(ATLAS_OUTPUT_TOOLS)
                - set(CODEGEN_TOOLS)
            )
            planner_allowed = set(read_only_tool_names) | set(SESSION_MUTATION_TOOLS)
            planner_tools = [t for t in tools if _tool_name(t) in planner_allowed]
            executor_tools = list(tools)

            # Optional: Responses API built-in web_search tool.
            web_search_tool = None
            try:
                wsm = str(args.web_search or DEFAULT_WEB_SEARCH_MODE).strip().lower()
            except Exception:
                wsm = str(DEFAULT_WEB_SEARCH_MODE).strip().lower()
            if wsm == "cached":
                web_search_tool = {"type": "web_search", "external_web_access": False}
            elif wsm == "live":
                web_search_tool = {"type": "web_search", "external_web_access": True}

            def _phase_tools(phase: str) -> list[dict]:
                if phase == "planner":
                    base = list(planner_tools)
                elif phase == "executor":
                    base = list(executor_tools)
                else:
                    raise ValueError(f"unknown phase {phase!r}")
                if web_search_tool is not None:
                    base.append(dict(web_search_tool))
                return base

            phases: list[str]
            if str(args.dump_tools_phase) == "both":
                phases = ["planner", "executor"]
            else:
                phases = [str(args.dump_tools_phase)]

            wire = (
                str(getattr(args, "dump_tools_wire", "responses") or "responses")
                .strip()
                .lower()
            )
            fmt = (
                str(getattr(args, "dump_tools_format", "pretty") or "pretty")
                .strip()
                .lower()
            )
            pretty = fmt == "pretty"

            out: dict[str, object] = {
                "meta": {
                    "wire_api_flag": str(getattr(args, "wire_api", "auto")),
                    "web_search_mode": str(
                        getattr(args, "web_search", DEFAULT_WEB_SEARCH_MODE)
                    ),
                    "enable_codegen": bool(getattr(args, "enable_codegen", False)),
                },
                "tools": {},
            }

            def _normalize_chat(tools_in: list[dict]) -> list[dict]:
                # Chat Completions does not reliably accept non-function tool specs.
                fn_only = [
                    t
                    for t in (tools_in or [])
                    if isinstance(t, dict) and str(t.get("type") or "") == "function"
                ]
                return normalize_tools_for_chat_completions_api(fn_only) or []

            for ph in phases:
                phase_tools = _phase_tools(ph)
                entry: dict[str, object] = {}
                if wire in {"chat", "both"}:
                    entry["chat"] = _normalize_chat(phase_tools)
                if wire in {"responses", "both"}:
                    entry["responses"] = (
                        normalize_tools_for_responses_api(phase_tools) or []
                    )
                out["tools"][ph] = entry

            json_kwargs = {"ensure_ascii": False, "sort_keys": True}
            if pretty:
                json_kwargs["indent"] = 2
            else:
                json_kwargs["separators"] = (",", ":")
            sys.stdout.write(json.dumps(out, **json_kwargs) + "\n")
            return 0
        except Exception as e:
            logging.error("Failed to dump tools: %s", e)
            return 2

    api_key = os.environ.get("OPENAI_API_KEY")
    if not api_key:
        logging.error("OPENAI_API_KEY is required.")
        return 2

    if args.plain:
        return int(
            run_team_repl(
                address=address,
                api_key=api_key,
                model=args.model,
                wire_api=args.wire_api,
                temperature=args.temperature,
                reasoning_effort=args.reasoning_effort,
                reasoning_summary=args.reasoning_summary,
                text_verbosity=args.text_verbosity,
                max_rounds=int(args.max_rounds),
                max_rounds_planner=int(args.planner_max_rounds),
                ephemeral_inline_images=bool(args.ephemeral_inline_images),
                session=args.session,
                session_dir=args.session_dir,
                enable_codegen=bool(args.enable_codegen),
                web_search_mode=args.web_search,
            )
        )

    return int(
        run_console_repl(
            address=address,
            api_key=api_key,
            model=args.model,
            wire_api=args.wire_api,
            temperature=args.temperature,
            reasoning_effort=args.reasoning_effort,
            reasoning_summary=args.reasoning_summary,
            text_verbosity=args.text_verbosity,
            max_rounds=int(args.max_rounds),
            max_rounds_planner=int(args.planner_max_rounds),
            ephemeral_inline_images=bool(args.ephemeral_inline_images),
            session=args.session,
            session_dir=args.session_dir,
            enable_codegen=bool(args.enable_codegen),
            web_search_mode=args.web_search,
        )
    )
