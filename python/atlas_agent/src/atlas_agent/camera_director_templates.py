"""Internal camera-director templates for first-person walkthrough planning.

This module intentionally provides a *small* vocabulary that normalizes common
motion intents ("enter", "turn right", "ascend", "pause") into the concrete
`segments=[...]` shape consumed by `animation_camera_walkthrough_apply`.

Design goals:
- Determinism: reduce ad-hoc numeric invention by LLMs.
- Portability: move distances are bbox-radius fractions (dataset-scale invariant).
- Minimalism: templates are optional and internal; callers may still provide
  explicit `move`/`rotate` segments.
"""

from __future__ import annotations

from typing import Any, Dict, List


MOVE_AMOUNT_DEFAULTS: dict[str, float] = {
    # Fractions of bbox enclosing-sphere radius
    "tiny": 0.05,
    "small": 0.15,
    "medium": 0.4,
    "large": 0.7,
    "deep": 1.0,
}

ROTATE_AMOUNT_DEFAULTS: dict[str, float] = {
    # Degrees
    "slight": 15.0,
    "small": 30.0,
    "medium": 45.0,
    "strong": 90.0,
    "large": 120.0,
}


def _norm_key(s: Any) -> str:
    return str(s or "").strip().lower().replace("-", "_").replace(" ", "_")


def _coerce_amount(value: Any, *, table: dict[str, float], field: str) -> float | None:
    """Coerce a template amount.

    Accepts:
    - None: return None (caller uses template default)
    - number: return float(number)
    - string: look up in `table` by normalized key
    """

    if value is None:
        return None
    if isinstance(value, (int, float)):
        return float(value)
    if isinstance(value, str):
        k = _norm_key(value)
        if k in table:
            return float(table[k])
        raise ValueError(f"unknown {field} {value!r}; allowed: {sorted(table.keys())}")
    raise ValueError(f"{field} must be a number or string label")


def expand_walkthrough_segments(segments: List[Dict[str, Any]]) -> List[Dict[str, Any]]:
    """Expand internal template segments into concrete move/rotate/pause segments.

    Input: list of segment dicts. Each segment may optionally include:
      - template: string
      - amount: string label or number (template-dependent)
      - distance: numeric (move templates)
      - degrees: numeric (rotate templates)

    Output: list of segment dicts with templates expanded; unknown extra keys are preserved.
    """

    out: list[dict[str, Any]] = []

    for seg in segments or []:
        if not isinstance(seg, dict):
            raise ValueError("each segment must be an object")

        template_raw = seg.get("template")
        if template_raw is None:
            out.append(seg)
            continue

        template = _norm_key(template_raw)
        if not template:
            out.append(seg)
            continue

        # Start with a copy so we preserve timing fields (u0/u1/duration) and any
        # optional metadata (label/notes) while overwriting the motion fields.
        expanded: dict[str, Any] = dict(seg)
        expanded.pop("template", None)

        # Template-specific knobs (optional)
        amount = seg.get("amount")
        distance = seg.get("distance")
        degrees = seg.get("degrees")

        # Base motion produced by the template.
        base_move: dict[str, Any] | None = None
        base_rotate: dict[str, Any] | None = None
        base_pause = False

        # Pause
        if template in {"pause", "hold"}:
            base_pause = True

        # Move templates (bbox-radius fractions)
        elif template in {"enter", "move_forward", "forward", "advance"}:
            d = _coerce_amount(amount, table=MOVE_AMOUNT_DEFAULTS, field="move amount")
            if d is None:
                # "enter" is stronger than a generic forward nudge.
                d = 0.6 if template == "enter" else 0.3
            if distance is not None:
                if not isinstance(distance, (int, float)):
                    raise ValueError("distance must be a number")
                d = float(distance)
            base_move = {"forward": float(d)}

        elif template in {"step_forward"}:
            d = _coerce_amount(amount, table=MOVE_AMOUNT_DEFAULTS, field="move amount")
            if d is None:
                d = 0.1
            if distance is not None:
                if not isinstance(distance, (int, float)):
                    raise ValueError("distance must be a number")
                d = float(distance)
            base_move = {"forward": float(d)}

        elif template in {"strafe_right", "move_right", "right"}:
            d = _coerce_amount(amount, table=MOVE_AMOUNT_DEFAULTS, field="move amount")
            if d is None:
                d = 0.2
            if distance is not None:
                if not isinstance(distance, (int, float)):
                    raise ValueError("distance must be a number")
                d = float(distance)
            base_move = {"right": float(d)}

        elif template in {"strafe_left", "move_left", "left"}:
            d = _coerce_amount(amount, table=MOVE_AMOUNT_DEFAULTS, field="move amount")
            if d is None:
                d = 0.2
            if distance is not None:
                if not isinstance(distance, (int, float)):
                    raise ValueError("distance must be a number")
                d = float(distance)
            base_move = {"left": float(d)}

        elif template in {"ascend", "move_up", "up"}:
            d = _coerce_amount(amount, table=MOVE_AMOUNT_DEFAULTS, field="move amount")
            if d is None:
                d = 0.2
            if distance is not None:
                if not isinstance(distance, (int, float)):
                    raise ValueError("distance must be a number")
                d = float(distance)
            base_move = {"up": float(d)}

        elif template in {"descend", "move_down", "down"}:
            d = _coerce_amount(amount, table=MOVE_AMOUNT_DEFAULTS, field="move amount")
            if d is None:
                d = 0.2
            if distance is not None:
                if not isinstance(distance, (int, float)):
                    raise ValueError("distance must be a number")
                d = float(distance)
            base_move = {"down": float(d)}

        # Rotate templates (degrees)
        elif template in {"turn_right", "yaw_right"}:
            a = _coerce_amount(amount, table=ROTATE_AMOUNT_DEFAULTS, field="rotate amount")
            if a is None:
                a = 45.0
            if degrees is not None:
                if not isinstance(degrees, (int, float)):
                    raise ValueError("degrees must be a number")
                a = float(degrees)
            base_rotate = {"yaw": float(abs(a))}

        elif template in {"turn_left", "yaw_left"}:
            a = _coerce_amount(amount, table=ROTATE_AMOUNT_DEFAULTS, field="rotate amount")
            if a is None:
                a = 45.0
            if degrees is not None:
                if not isinstance(degrees, (int, float)):
                    raise ValueError("degrees must be a number")
                a = float(degrees)
            base_rotate = {"yaw": -float(abs(a))}

        elif template in {"look_up", "pitch_up"}:
            a = _coerce_amount(amount, table=ROTATE_AMOUNT_DEFAULTS, field="rotate amount")
            if a is None:
                a = 15.0
            if degrees is not None:
                if not isinstance(degrees, (int, float)):
                    raise ValueError("degrees must be a number")
                a = float(degrees)
            base_rotate = {"pitch": float(abs(a))}

        elif template in {"look_down", "pitch_down"}:
            a = _coerce_amount(amount, table=ROTATE_AMOUNT_DEFAULTS, field="rotate amount")
            if a is None:
                a = 15.0
            if degrees is not None:
                if not isinstance(degrees, (int, float)):
                    raise ValueError("degrees must be a number")
                a = float(degrees)
            base_rotate = {"pitch": -float(abs(a))}

        elif template in {"roll_right"}:
            a = _coerce_amount(amount, table=ROTATE_AMOUNT_DEFAULTS, field="rotate amount")
            if a is None:
                a = 15.0
            if degrees is not None:
                if not isinstance(degrees, (int, float)):
                    raise ValueError("degrees must be a number")
                a = float(degrees)
            base_rotate = {"roll": float(abs(a))}

        elif template in {"roll_left"}:
            a = _coerce_amount(amount, table=ROTATE_AMOUNT_DEFAULTS, field="rotate amount")
            if a is None:
                a = 15.0
            if degrees is not None:
                if not isinstance(degrees, (int, float)):
                    raise ValueError("degrees must be a number")
                a = float(degrees)
            base_rotate = {"roll": -float(abs(a))}

        else:
            raise ValueError(f"unknown segment template {template_raw!r}")

        # Merge policy:
        # - Template provides defaults.
        # - Explicit move/rotate in the segment override per-field (if present).
        if base_pause:
            expanded["pause"] = True

        if base_move is not None:
            merged = dict(base_move)
            if isinstance(seg.get("move"), dict):
                merged.update(seg["move"])
            expanded["move"] = merged
        elif "move" in expanded and expanded.get("move") is None:
            expanded.pop("move", None)

        if base_rotate is not None:
            merged = dict(base_rotate)
            if isinstance(seg.get("rotate"), dict):
                merged.update(seg["rotate"])
            expanded["rotate"] = merged
        elif "rotate" in expanded and expanded.get("rotate") is None:
            expanded.pop("rotate", None)

        # Remove template-only knobs to keep the saved session payload minimal.
        expanded.pop("amount", None)
        expanded.pop("distance", None)
        expanded.pop("degrees", None)

        # Provide a stable default label if the caller didn't supply one.
        if not isinstance(expanded.get("label"), str) or not str(expanded.get("label") or "").strip():
            expanded["label"] = str(template_raw)

        out.append(expanded)

    return out

