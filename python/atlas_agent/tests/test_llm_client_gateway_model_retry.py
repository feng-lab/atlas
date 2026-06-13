from __future__ import annotations

import sys
from pathlib import Path

# Add src layout to sys.path for local runs
ROOT_DIR = Path(__file__).resolve().parents[2]
SRC_DIR = ROOT_DIR / "src"
if str(SRC_DIR) not in sys.path:
    sys.path.insert(0, str(SRC_DIR))

from atlas_agent.agent_team.base import LLMClient  # type: ignore  # noqa: E402
from atlas_agent.model_policy import DEFAULT_MODEL  # type: ignore  # noqa: E402


class _DummyResponses:
    def __init__(self, scripted: list[dict]):
        self._scripted = list(scripted)
        self.calls = 0

    def create(self, **_kwargs):  # noqa: D401
        if self.calls >= len(self._scripted):
            raise AssertionError("DummyResponses scripted responses exhausted")
        out = self._scripted[self.calls]
        self.calls += 1
        return dict(out)


class _DummyOpenAIClient:
    def __init__(self, responses: _DummyResponses):
        self.responses = responses


def test_llm_client_retries_internal_responses_create_when_gateway_model_missing():
    import atlas_agent.agent_team.base as base_mod  # type: ignore

    prev_backoff = base_mod.TRANSIENT_NETWORK_BACKOFF_SECONDS
    base_mod.TRANSIENT_NETWORK_BACKOFF_SECONDS = 0.0
    try:
        responses = _DummyResponses(
            [
                {
                    # Missing model: should be discarded and retried.
                    "output": [
                        {
                            "type": "message",
                            "role": "assistant",
                            "content": [{"type": "output_text", "text": "Discard"}],
                        }
                    ]
                },
                {
                    "model": DEFAULT_MODEL,
                    "output": [
                        {
                            "type": "message",
                            "role": "assistant",
                            "content": [{"type": "output_text", "text": "Done."}],
                        }
                    ],
                },
            ]
        )
        llm = LLMClient(api_key="sk-test", model=DEFAULT_MODEL)
        llm._client = _DummyOpenAIClient(responses)

        text, resp = llm.complete_text_with_response(
            system_prompt="sys", user_text="hi"
        )
        assert text == "Done."
        assert isinstance(resp, dict)
        assert resp.get("model") == DEFAULT_MODEL
        assert responses.calls == 2
    finally:
        base_mod.TRANSIENT_NETWORK_BACKOFF_SECONDS = prev_backoff


def test_llm_client_retries_internal_responses_create_when_gateway_model_mismatched():
    import atlas_agent.agent_team.base as base_mod  # type: ignore

    prev_backoff = base_mod.TRANSIENT_NETWORK_BACKOFF_SECONDS
    base_mod.TRANSIENT_NETWORK_BACKOFF_SECONDS = 0.0
    try:
        responses = _DummyResponses(
            [
                {
                    # Wrong model: should be discarded and retried.
                    "model": "gpt-4o",
                    "output": [
                        {
                            "type": "message",
                            "role": "assistant",
                            "content": [{"type": "output_text", "text": "Discard"}],
                        }
                    ],
                },
                {
                    "model": DEFAULT_MODEL,
                    "output": [
                        {
                            "type": "message",
                            "role": "assistant",
                            "content": [{"type": "output_text", "text": "Done."}],
                        }
                    ],
                },
            ]
        )
        llm = LLMClient(api_key="sk-test", model=DEFAULT_MODEL)
        llm._client = _DummyOpenAIClient(responses)

        text, resp = llm.complete_text_with_response(
            system_prompt="sys", user_text="hi"
        )
        assert text == "Done."
        assert isinstance(resp, dict)
        assert resp.get("model") == DEFAULT_MODEL
        assert responses.calls == 2
    finally:
        base_mod.TRANSIENT_NETWORK_BACKOFF_SECONDS = prev_backoff
