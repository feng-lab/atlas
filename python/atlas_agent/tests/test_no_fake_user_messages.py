from __future__ import annotations

import ast
import sys
from pathlib import Path

# Add src layout to sys.path for local runs
PKG_DIR = Path(__file__).resolve().parents[1]
SRC_DIR = PKG_DIR / "src"
if str(SRC_DIR) not in sys.path:
    sys.path.insert(0, str(SRC_DIR))


def test_chat_rpc_team_only_uses_role_user_for_real_user_text() -> None:
    """Guardrail: do not inject internal artifacts as role=user messages.

    The Responses API requires at least one user message, but within atlas_agent's
    main tool loop we only want the *real* end-user prompt to be labeled role=user.
    Internal repair/compaction directives should be passed via instructions or as
    assistant/history/tool outputs, not fake user messages.
    """

    path = SRC_DIR / "atlas_agent" / "chat_rpc_team.py"
    src = path.read_text(encoding="utf-8")
    tree = ast.parse(src, filename=str(path))

    user_message_calls: list[ast.Call] = []
    for node in ast.walk(tree):
        if not isinstance(node, ast.Call):
            continue
        # We only guard the local helper in this module: _message(role=..., text=...).
        if not isinstance(node.func, ast.Name) or node.func.id != "_message":
            continue

        role_kw = next((kw for kw in node.keywords if kw.arg == "role"), None)
        if role_kw is None:
            continue
        if not isinstance(role_kw.value, ast.Constant):
            continue
        if role_kw.value.value != "user":
            continue
        user_message_calls.append(node)

    assert len(user_message_calls) == 1, (
        f"Expected exactly one _message(role='user', ...) call, found {len(user_message_calls)}"
    )

    # The remaining call should be for the actual end-user input.
    call = user_message_calls[0]
    text_kw = next((kw for kw in call.keywords if kw.arg == "text"), None)
    assert text_kw is not None, "_message(role='user', ...) must specify text=..."
    assert isinstance(text_kw.value, ast.Name) and text_kw.value.id == "user_text", (
        "role=user messages must only wrap the current end-user prompt (user_text); "
        "internal directives must not be labeled as user messages."
    )
