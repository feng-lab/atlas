from __future__ import annotations

import sys
from pathlib import Path

import pytest

# Add src layout to sys.path for local runs
ROOT_DIR = Path(__file__).resolve().parents[2]
SRC_DIR = ROOT_DIR / "src"
if str(SRC_DIR) not in sys.path:
    sys.path.insert(0, str(SRC_DIR))

from atlas_agent.camera_director_templates import (  # type: ignore  # noqa: E402
    expand_walkthrough_segments,
)


def test_expand_walkthrough_segments_basic_templates():
    segments = [
        {"template": "enter", "amount": "medium"},
        {"template": "turn_right", "degrees": 90},
        {"template": "pause", "duration": 1.0},
    ]

    out = expand_walkthrough_segments(segments)
    assert len(out) == 3

    assert out[0]["move"]["forward"] == pytest.approx(0.4)
    assert out[0]["label"] == "enter"
    assert "template" not in out[0]
    assert "amount" not in out[0]

    assert out[1]["rotate"]["yaw"] == pytest.approx(90.0)
    assert out[1]["label"] == "turn_right"
    assert "degrees" not in out[1]

    assert out[2]["pause"] is True
    assert out[2]["duration"] == pytest.approx(1.0)


def test_expand_walkthrough_segments_merge_overrides_move_rotate():
    segments = [
        {
            "template": "forward",
            "amount": "small",
            "move": {"forward": 0.9, "up": 0.1},
            "rotate": {"yaw": 12.0},
            "label": "custom",
        }
    ]

    out = expand_walkthrough_segments(segments)
    assert len(out) == 1
    seg0 = out[0]

    # move.merge: explicit move fields override template defaults
    assert seg0["move"]["forward"] == pytest.approx(0.9)
    assert seg0["move"]["up"] == pytest.approx(0.1)

    # rotate preserved (template doesn't define it)
    assert seg0["rotate"]["yaw"] == pytest.approx(12.0)

    # label preserved
    assert seg0["label"] == "custom"


def test_expand_walkthrough_segments_unknown_template_errors():
    with pytest.raises(ValueError, match="unknown segment template"):
        expand_walkthrough_segments([{"template": "this_is_not_a_template"}])

