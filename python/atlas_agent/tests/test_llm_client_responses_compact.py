from __future__ import annotations

import sys
from pathlib import Path

# Add src layout to sys.path for local runs
ROOT_DIR = Path(__file__).resolve().parents[2]
SRC_DIR = ROOT_DIR / "src"
if str(SRC_DIR) not in sys.path:
    sys.path.insert(0, str(SRC_DIR))

from atlas_agent.agent_team.base import LLMClient  # type: ignore  # noqa: E402


class _DummyResponses:
    def __init__(self):
        self.calls: list[dict] = []

    def compact(self, **kwargs):  # noqa: D401
        self.calls.append(dict(kwargs))
        return {
            "object": "response.compaction",
            "id": "rc_test",
            "output": [
                {
                    "type": "message",
                    "role": "user",
                    "content": [{"type": "input_text", "text": "hi"}],
                },
                {"type": "compaction", "encrypted_content": "opaque"},
            ],
        }


class _DummyOpenAIClient:
    def __init__(self, responses):
        self.responses = responses


def test_llm_client_responses_compact_uses_sdk_when_available():
    responses = _DummyResponses()
    llm = LLMClient(api_key="sk-test", model="gpt-test")
    llm._client = _DummyOpenAIClient(responses)

    items, resp = llm.compact_responses_input_items_with_response(
        input_items=[
            {
                "type": "message",
                "role": "user",
                "content": [{"type": "input_text", "text": "hi"}],
            }
        ]
    )
    assert isinstance(resp, dict)
    assert resp.get("object") == "response.compaction"
    assert items is not None and len(items) == 2
    assert items[1].get("type") == "compaction"
    assert len(responses.calls) == 1
    assert responses.calls[0].get("model") == "gpt-test"


def test_llm_client_responses_compact_returns_none_when_wire_api_chat():
    responses = _DummyResponses()
    llm = LLMClient(api_key="sk-test", model="gpt-test", wire_api="chat")
    llm._client = _DummyOpenAIClient(responses)

    items, resp = llm.compact_responses_input_items_with_response(
        input_items=[
            {
                "type": "message",
                "role": "user",
                "content": [{"type": "input_text", "text": "hi"}],
            }
        ]
    )
    assert items is None
    assert resp is None
    assert responses.calls == []


def test_llm_client_responses_compact_disables_on_unsupported_endpoint():
    class _UnsupportedError(RuntimeError):
        def __init__(self, msg: str):
            super().__init__(msg)
            self.status_code = 404

    class _UnsupportedResponses(_DummyResponses):
        def compact(self, **kwargs):  # noqa: D401
            self.calls.append(dict(kwargs))
            raise _UnsupportedError("404 not found: /v1/responses/compact")

    responses = _UnsupportedResponses()
    llm = LLMClient(api_key="sk-test", model="gpt-test")
    llm._client = _DummyOpenAIClient(responses)

    items1, resp1 = llm.compact_responses_input_items_with_response(
        input_items=[
            {
                "type": "message",
                "role": "user",
                "content": [{"type": "input_text", "text": "hi"}],
            }
        ]
    )
    assert items1 is None
    assert resp1 is None
    assert llm._responses_compact_disabled is True
    assert len(responses.calls) == 1

    # Once disabled, do not keep hitting the provider for the same unsupported endpoint.
    items2, resp2 = llm.compact_responses_input_items_with_response(
        input_items=[
            {
                "type": "message",
                "role": "user",
                "content": [{"type": "input_text", "text": "hi again"}],
            }
        ]
    )
    assert items2 is None
    assert resp2 is None
    assert len(responses.calls) == 1


def test_llm_client_responses_compact_falls_back_to_raw_http_when_sdk_missing():
    class _NoCompactResponses:
        pass

    llm = LLMClient(api_key="sk-test", model="gpt-test")
    llm._client = _DummyOpenAIClient(_NoCompactResponses())

    seen: dict = {}

    def _fake_http(*, input_items):  # noqa: ANN001
        seen["input_items"] = list(input_items or [])
        return {
            "object": "response.compaction",
            "id": "rc_http",
            "output": [{"type": "compaction", "encrypted_content": "opaque"}],
        }

    llm._responses_compact_via_http = _fake_http  # type: ignore[method-assign]

    items, resp = llm.compact_responses_input_items_with_response(
        input_items=[
            {
                "type": "message",
                "role": "user",
                "content": [{"type": "input_text", "text": "hi"}],
            }
        ]
    )
    assert isinstance(resp, dict)
    assert resp.get("id") == "rc_http"
    assert items is not None and items[0].get("type") == "compaction"
    assert seen["input_items"][0]["role"] == "user"
