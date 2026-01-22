from __future__ import annotations

import sys
from pathlib import Path

# Add src layout to sys.path for local runs
ROOT_DIR = Path(__file__).resolve().parents[2]
SRC_DIR = ROOT_DIR / "src"
if str(SRC_DIR) not in sys.path:
    sys.path.insert(0, str(SRC_DIR))

from atlas_agent.agent_team.base import LLMClient  # type: ignore  # noqa: E402


class _DummyModels:
    def __init__(self, model_data: dict, *, raises: bool = False):
        self._data = dict(model_data)
        self._raises = bool(raises)
        self.calls = 0

    def retrieve(self, _model_id: str):
        self.calls += 1
        if self._raises:
            raise RuntimeError("models.retrieve not supported")
        return dict(self._data)


class _DummyOpenAIClient:
    def __init__(self, model_data: dict, *, raises: bool = False):
        self.models = _DummyModels(model_data, raises=raises)


def test_llm_client_model_token_budgets_from_provider_metadata():
    client = _DummyOpenAIClient(
        {
            "id": "gpt-test",
            "context_window": 2000,
            "auto_compact_token_limit": 1500,
        }
    )
    llm = LLMClient(api_key="sk-test", model="gpt-test")
    llm._client = client

    b = llm.get_model_token_budgets("gpt-test")
    assert b.total_context_window_tokens == 2000
    assert b.auto_compact_tokens == 1500
    assert b.max_output_tokens is None
    assert b.effective_input_budget_tokens is None

    # Cached: should not re-fetch.
    b2 = llm.get_model_token_budgets("gpt-test")
    assert b2 == b
    assert client.models.calls == 1


def test_llm_client_model_token_budgets_computes_effective_from_total_minus_max_output():
    client = _DummyOpenAIClient(
        {
            "context_window": 400_000,
            "max_output_tokens": 128_000,
        }
    )
    llm = LLMClient(api_key="sk-test", model="gpt-test")
    llm._client = client

    b = llm.get_model_token_budgets("gpt-test")
    assert b.total_context_window_tokens == 400_000
    assert b.max_output_tokens == 128_000
    assert b.effective_input_budget_tokens == 272_000
    assert b.auto_compact_tokens == 244_800


def test_llm_client_model_token_budgets_supports_nested_and_string_values():
    client = _DummyOpenAIClient({"capabilities": {"context_window": "128k"}})
    llm = LLMClient(api_key="sk-test", model="gpt-test")
    llm._client = client

    b = llm.get_model_token_budgets("gpt-test")
    assert b.total_context_window_tokens == 128_000
    assert b.max_output_tokens is None
    assert b.effective_input_budget_tokens is None
    assert b.auto_compact_tokens is None


def test_llm_client_model_token_budgets_returns_unknown_when_provider_errors():
    client = _DummyOpenAIClient({}, raises=True)
    llm = LLMClient(api_key="sk-test", model="gpt-test")
    llm._client = client

    b = llm.get_model_token_budgets("gpt-test")
    assert b.total_context_window_tokens is None
    assert b.max_output_tokens is None
    assert b.effective_input_budget_tokens is None
    assert b.auto_compact_tokens is None
    # Cached unknown: still only one provider call.
    b2 = llm.get_model_token_budgets("gpt-test")
    assert b2 == b
    assert client.models.calls == 1
