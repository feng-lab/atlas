from __future__ import annotations

from typing import Any


# Atlas uses Qt's QEasingCurve names on the wire (SetKeyRequest.easing).
#
# We intentionally do NOT attempt to enumerate every QEasingCurve type here.
# That list can vary slightly across Qt versions, and advertising the wrong set
# is worse than providing a small, correct, high-signal subset.
#
# Instead, we:
# - Normalize common LLM/user aliases (e.g. "EaseInOut") to a canonical value.


_CANONICAL_BY_KEY: dict[str, str] = {
    # Canonical case normalization for common Qt/QEasingCurve names.
    "linear": "Linear",
    "switch": "Switch",
    "inquad": "InQuad",
    "outquad": "OutQuad",
    "inoutquad": "InOutQuad",
}

_ALIAS_BY_KEY: dict[str, str] = {
    # Common agent/UX aliases (not Qt names).
    # We map these to conservative, widely-supported Qt curves.
    "easein": "InQuad",
    "easeout": "OutQuad",
    "easeinout": "InOutQuad",
}


def normalize_easing_name(raw: Any) -> str:
    """Normalize common easing aliases to Atlas/Qt canonical option strings."""

    s = str(raw or "").strip()
    if not s:
        return "Linear"

    key = s.lower().replace("_", "").replace("-", "").replace(" ", "")

    if key in _CANONICAL_BY_KEY:
        return _CANONICAL_BY_KEY[key]

    if key in _ALIAS_BY_KEY:
        return _ALIAS_BY_KEY[key]

    # Unknown: pass through (server will validate).
    return s
