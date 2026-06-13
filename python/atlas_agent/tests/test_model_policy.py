from __future__ import annotations

import sys
import types
from pathlib import Path

# Add src layout to sys.path for local runs
ROOT_DIR = Path(__file__).resolve().parents[2]
SRC_DIR = ROOT_DIR / "src"
if str(SRC_DIR) not in sys.path:
    sys.path.insert(0, str(SRC_DIR))

from atlas_agent.model_policy import (  # type: ignore  # noqa: E402
    DEFAULT_MODEL,
    ImageTokenCostPatchModel,
    guess_model_effective_input_budget_tokens,
    image_token_cost_profile_for_model,
    model_context_window_tokens,
    model_effective_context_window_tokens,
    model_supports_reasoning_summaries,
    model_supports_text_verbosity,
    normalize_model_id,
)

import atlas_agent.model_policy as model_policy  # type: ignore  # noqa: E402


def test_normalize_model_id_handles_gateway_prefixes():
    assert normalize_model_id(f"openai/{DEFAULT_MODEL}") == DEFAULT_MODEL
    assert normalize_model_id(f"openai:{DEFAULT_MODEL}") == DEFAULT_MODEL


def test_default_model_supports_reasoning_and_text_controls():
    gateway_prefixed = f"openai/{DEFAULT_MODEL}"
    assert model_supports_reasoning_summaries(gateway_prefixed)
    assert model_supports_text_verbosity(gateway_prefixed)


def test_default_model_budget_and_image_profile_are_latest_family():
    assert model_context_window_tokens(DEFAULT_MODEL) == 272_000
    assert model_effective_context_window_tokens(DEFAULT_MODEL) == 258_400
    assert guess_model_effective_input_budget_tokens(DEFAULT_MODEL) == 258_400
    assert model_context_window_tokens("gpt-5.2") == 272_000
    assert model_effective_context_window_tokens("gpt-5.2") == 258_400

    profile = image_token_cost_profile_for_model(DEFAULT_MODEL)
    assert isinstance(profile, ImageTokenCostPatchModel)
    assert profile.patch_budget_for_detail("auto") >= profile.patch_budget_for_detail(
        "high"
    )
    assert profile.patch_budget_for_detail(
        "original"
    ) >= profile.patch_budget_for_detail("high")


def test_future_gpt5_minor_model_uses_current_family_policy():
    future_model = "gpt-5.7"

    assert model_supports_reasoning_summaries(future_model)
    assert model_supports_text_verbosity(future_model)
    assert model_context_window_tokens(future_model) == 272_000
    assert model_effective_context_window_tokens(future_model) == 258_400
    assert guess_model_effective_input_budget_tokens(future_model) == 258_400

    profile = image_token_cost_profile_for_model(future_model)
    assert isinstance(profile, ImageTokenCostPatchModel)
    assert profile.patch_budget_for_detail("auto") == 10_000
    assert profile.patch_budget_for_detail("original") == 10_000
    assert profile.patch_budget_for_detail("high") == 2_500


def test_future_default_model_update_only_requires_changing_default_slug():
    policy_path = Path(model_policy.__file__)
    source = policy_path.read_text(encoding="utf-8")
    future_source = source.replace(
        f'DEFAULT_MODEL = "{DEFAULT_MODEL}"',
        'DEFAULT_MODEL = "gpt-5.7"',
        1,
    )
    assert future_source != source

    module_name = "atlas_agent._future_default_model_policy_test"
    future_module = types.ModuleType(module_name)
    future_module.__file__ = str(policy_path)
    future_module.__package__ = "atlas_agent"
    sys.modules[module_name] = future_module
    try:
        exec(compile(future_source, str(policy_path), "exec"), future_module.__dict__)
    finally:
        sys.modules.pop(module_name, None)

    assert future_module.DEFAULT_MODEL == "gpt-5.7"
    assert (
        future_module.model_context_window_tokens(future_module.DEFAULT_MODEL)
        == 272_000
    )
    assert (
        future_module.model_effective_context_window_tokens(future_module.DEFAULT_MODEL)
        == 258_400
    )
    assert (
        future_module.guess_model_effective_input_budget_tokens(
            future_module.DEFAULT_MODEL
        )
        == 258_400
    )

    future_profile = future_module.image_token_cost_profile_for_model(
        future_module.DEFAULT_MODEL
    )
    assert isinstance(future_profile, future_module.ImageTokenCostPatchModel)
    assert future_profile.patch_budget_for_detail("auto") == 10_000
    assert future_profile.patch_budget_for_detail("high") == 2_500

    current_profile = future_module.image_token_cost_profile_for_model(DEFAULT_MODEL)
    assert isinstance(current_profile, future_module.ImageTokenCostPatchModel)
    assert current_profile.patch_budget_for_detail("auto") == 10_000
