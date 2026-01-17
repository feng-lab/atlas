from __future__ import annotations

from typing import Any, Optional

from .context import ToolDispatchContext


def require_engine_ready(_args: dict[str, Any], ctx: ToolDispatchContext) -> Optional[str]:
    client = getattr(ctx, "client", None)
    if client is None or not hasattr(client, "ensure_view"):
        return "engine-backed tool requires a SceneClient with ensure_view()"
    try:
        # require=True: fail fast with a clear message rather than letting
        # downstream RPCs fail with FAILED_PRECONDITION.
        ok = bool(client.ensure_view(require=True))
        return None if ok else "3D engine not ready"
    except Exception as e:
        return f"3D engine not ready: {e}"


def require_session_store(_args: dict[str, Any], ctx: ToolDispatchContext) -> Optional[str]:
    if getattr(ctx, "session_store", None) is None:
        return "no session_store available (run with --session to enable persistent memory/search)"
    return None


def require_screenshot_consent(_args: dict[str, Any], ctx: ToolDispatchContext) -> Optional[str]:
    ss = getattr(ctx, "session_store", None)
    allow = False
    try:
        if ss is not None:
            allow = (ss.get_consent("screenshots") is True)
    except Exception:
        allow = False
    if allow:
        return None
    return "screenshots not permitted for this session (toggle via :screenshots on)"


def require_codegen_enabled(_args: dict[str, Any], ctx: ToolDispatchContext) -> Optional[str]:
    if not bool(getattr(ctx, "codegen_enabled", False)):
        return "codegen disabled (enable with --enable-codegen)"
    return None


def require_animation_id(args: dict[str, Any], ctx: ToolDispatchContext) -> Optional[str]:
    """Require a concrete Animation3D id for deterministic timeline operations."""

    try:
        v = args.get("animation_id", None)
        if v is None or int(v) <= 0:
            cur = None
            try:
                cur = int(getattr(ctx, "runtime_state", {}).get("current_animation_id") or 0)
            except Exception:
                cur = None
            hint = f" (current_animation_id={cur})" if isinstance(cur, int) and cur > 0 else ""
            return f"animation_id is required (call animation_ensure_animation first){hint}."
    except Exception:
        return "animation_id is required (call animation_ensure_animation first)."
    return None

