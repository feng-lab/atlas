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


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="atlas-agent",
        description="Atlas animation agent (chat only): control Atlas GUI via RPC",
    )
    # Single entry; accept an optional first positional (e.g., 'chat' or 'chat-rpc')
    parser.add_argument("cmd", nargs="?", help=argparse.SUPPRESS)
    parser.add_argument(
        "--address", default=os.environ.get("ATLAS_RPC_ADDR", "localhost:50051")
    )
    parser.add_argument(
        "--model",
        default=os.environ.get("ATLAS_LLM_MODEL", "gpt-5.1"),
    )
    parser.add_argument(
        "--temperature",
        type=float,
        default=float(os.environ.get("ATLAS_LLM_TEMPERATURE", "0.2")),
    )
    parser.add_argument("--api-key", default=os.environ.get("OPENAI_API_KEY"))
    parser.add_argument(
        "--atlas-dir",
        default=None,
        help="Atlas installation root (optional; used to derive exporter path)",
    )
    parser.add_argument(
        "--allow-screenshots",
        action="store_true",
        help="Enable preview screenshots for Inspector (sets ATLAS_AGENT_ALLOW_SCREENSHOTS=1)",
    )
    parser.add_argument(
        "--enable-codegen",
        action="store_true",
        help="Enable code generation tools (sets ATLAS_AGENT_ENABLE_CODEGEN=1)",
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
            "Unknown command; this CLI supports chat only. Usage: python -m atlas_agent --address localhost:50051"
        )
        return 2

    if not args.api_key:
        logging.error("OPENAI_API_KEY is required (set --api-key).")
        return 2

    # Set screenshot env gate from flag if requested
    if args.allow_screenshots:
        os.environ["ATLAS_AGENT_ALLOW_SCREENSHOTS"] = "1"
    if args.enable_codegen:
        os.environ["ATLAS_AGENT_ENABLE_CODEGEN"] = "1"

    return int(
        run_team_repl(
            address=args.address,
            api_key=args.api_key,
            model=args.model,
            temperature=args.temperature,
            atlas_dir=args.atlas_dir,
        )
    )
