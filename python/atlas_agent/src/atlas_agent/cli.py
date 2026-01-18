import sys

# Enforce minimum Python version early (fail-fast at import)
if sys.version_info < (3, 12):
    raise SystemExit(
        f"Atlas Agent requires Python 3.12+ (detected {sys.version.split()[0]}). "
        "Please upgrade your Python interpreter."
    )

import argparse
import logging
import os

from .chat_rpc_team import run_repl as run_team_repl
from .console_ui import run_console_repl
from .defaults import DEFAULT_EXECUTOR_MAX_ROUNDS


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
        default="gpt-5.2-pro",
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
        default="high",
        choices=["low", "medium", "high", "xhigh"],
        help="Reasoning effort for Responses API calls (when supported by the model/provider).",
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
    if args.cmd and args.cmd not in ("chat", "chat-rpc"):
        logging.error(
            "Unknown command; this CLI supports chat only. Usage: python -m atlas_agent"
        )
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
                max_rounds=int(args.max_rounds),
                session=args.session,
                session_dir=args.session_dir,
                enable_codegen=bool(args.enable_codegen),
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
            max_rounds=int(args.max_rounds),
            session=args.session,
            session_dir=args.session_dir,
            enable_codegen=bool(args.enable_codegen),
        )
    )
