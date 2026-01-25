from __future__ import annotations

import sys
from pathlib import Path

# Add src layout to sys.path for local runs
ROOT_DIR = Path(__file__).resolve().parents[2]
SRC_DIR = ROOT_DIR / "src"
if str(SRC_DIR) not in sys.path:
    sys.path.insert(0, str(SRC_DIR))

from atlas_agent.responses_tool_loop import _estimate_request_tokens  # type: ignore  # noqa: E402


def test_estimate_request_tokens_does_not_scale_with_inline_base64_image_bytes() -> (
    None
):
    # A 1MB base64 payload would previously explode the JSON-size estimate.
    big_b64 = "A" * (1024 * 1024)
    small_b64 = "A" * 16

    items_big = [
        {
            "type": "function_call",
            "name": "scene_screenshot",
            "arguments": '{"width": 1600, "height": 900}',
            "call_id": "call_1",
        },
        {
            "type": "function_call_output",
            "call_id": "call_1",
            "output": [
                {"type": "input_text", "text": "preview"},
                {
                    "type": "input_image",
                    "detail": "auto",
                    "image_url": f"data:image/png;base64,{big_b64}",
                },
            ],
        },
    ]
    items_small = [
        {
            "type": "function_call",
            "name": "scene_screenshot",
            "arguments": '{"width": 1600, "height": 900}',
            "call_id": "call_1",
        },
        {
            "type": "function_call_output",
            "call_id": "call_1",
            "output": [
                {"type": "input_text", "text": "preview"},
                {
                    "type": "input_image",
                    "detail": "auto",
                    "image_url": f"data:image/png;base64,{small_b64}",
                },
            ],
        },
    ]

    est_big = _estimate_request_tokens(
        model_name="gpt-5.2",
        instructions="system",
        input_items=items_big,
        tools=[],
    )
    est_small = _estimate_request_tokens(
        model_name="gpt-5.2",
        instructions="system",
        input_items=items_small,
        tools=[],
    )

    # The estimate should be dominated by the model's image token cost (O(10^3)),
    # not by the base64 text length (O(10^6)).
    assert est_big < 10_000
    assert est_small < 10_000
    assert abs(est_big - est_small) < 500


def test_estimate_request_tokens_patch_model_is_reasonable_for_screenshots() -> None:
    # Patch-based models (e.g. gpt-5-mini) should land in the ~2k range for 1600x900.
    items = [
        {
            "type": "function_call",
            "name": "scene_screenshot",
            "arguments": '{"width": 1600, "height": 900}',
            "call_id": "call_1",
        },
        {
            "type": "function_call_output",
            "call_id": "call_1",
            "output": [
                {"type": "input_text", "text": "preview"},
                {
                    "type": "input_image",
                    "detail": "auto",
                    "image_url": "data:image/png;base64,AAAA",
                },
            ],
        },
    ]

    est = _estimate_request_tokens(
        model_name="gpt-5-mini",
        instructions="system",
        input_items=items,
        tools=[],
    )

    assert 1_500 <= est <= 6_000


def test_estimate_request_tokens_prefers_tool_output_dimensions_when_available() -> (
    None
):
    # If tool args omit width/height, we should still estimate from the tool JSON result,
    # which reflects the actual output dimensions.
    items = [
        {
            "type": "function_call",
            "name": "animation_render_preview",
            "arguments": "{}",
            "call_id": "call_1",
        },
        {
            "type": "function_call_output",
            "call_id": "call_1",
            "output": [
                {"type": "input_text", "text": "preview"},
                {
                    "type": "input_text",
                    "text": 'Tool JSON result:\n{"ok":true,"width":512,"height":512}',
                },
                {
                    "type": "input_image",
                    "detail": "auto",
                    "image_url": "data:image/png;base64,AAAA",
                },
            ],
        },
    ]

    # For patch-based models, unknown dimensions default to the max patch budget and
    # would be far larger (~2.5k tokens) than a 512x512 preview (~400–500 tokens).
    est = _estimate_request_tokens(
        model_name="gpt-5-mini",
        instructions="system",
        input_items=items,
        tools=[],
    )
    assert est < 1_500
