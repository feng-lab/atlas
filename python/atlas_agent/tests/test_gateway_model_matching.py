from __future__ import annotations

import sys
from pathlib import Path

# Add src layout to sys.path for local runs
ROOT_DIR = Path(__file__).resolve().parents[2]
SRC_DIR = ROOT_DIR / "src"
if str(SRC_DIR) not in sys.path:
    sys.path.insert(0, str(SRC_DIR))

from atlas_agent.gateway_model import (  # type: ignore  # noqa: E402
    gateway_model_matches_requested,
)


def test_gateway_model_matching_accepts_date_suffix_for_alias():
    assert gateway_model_matches_requested("gpt-4o", "gpt-4o-2024-08-06")
    assert gateway_model_matches_requested("openai/gpt-4o", "gpt-4o-2024-08-06")


def test_gateway_model_matching_rejects_mini_variant_mismatch():
    assert not gateway_model_matches_requested("gpt-4o", "gpt-4o-mini")
    assert not gateway_model_matches_requested("gpt-4o", "gpt-4o-mini-2024-07-18")


def test_gateway_model_matching_requires_exact_when_request_is_dated():
    assert gateway_model_matches_requested("gpt-4o-2024-08-06", "gpt-4o-2024-08-06")
    assert not gateway_model_matches_requested("gpt-4o-2024-08-06", "gpt-4o-2024-09-01")
    assert not gateway_model_matches_requested("gpt-4o-2024-08-06", "gpt-4o")
