import logging
import os
import time
from typing import Any, Dict, List

# No provider-specific schema adaptation; pass canonical schemas to the SDK
# Require Agents SDK; no fallback path
import agents as openai_agents  # type: ignore
from agents import set_default_openai_api  # type: ignore
from agents.models.openai_provider import OpenAIProvider  # type: ignore

from .base import AgentMessage, LLMClient


def _wrap_tools_for_sdk(tools: List[Dict[str, Any]], dispatch, *, strict_json_schema: bool | None = None) -> List[Any]:
    # Pass canonical JSON Schemas through as-is for Chat Completions via the Agents SDK.
    wrapped: List[Any] = []
    # Prefer constructing FunctionTool directly with a params_json_schema and an
    # on_invoke_tool callback; this matches the SDK public API in 0.4.x.
    FunctionTool = getattr(openai_agents, "FunctionTool", None)
    if FunctionTool is None:
        raise RuntimeError("openai-agents-python FunctionTool class not found")
    for t in tools:
        fn = t.get("function", {})
        name = fn.get("name")
        schema = fn.get("parameters", {"type": "object"})
        desc = fn.get("description", name)
        if not name:
            continue

        def make_tool(nm: str, sch: Dict[str, Any], desc: str):
            # on_invoke_tool must be awaitable for the Agents SDK (returns Awaitable[Any])
            async def _on_invoke(ctx, args_json: str):
                # Pass through to our dispatcher; return string or JSON
                try:
                    logging.getLogger("atlas_agent.agents").info("[SDK] on_invoke tool=%s args_json=%s", nm, args_json or "")
                except Exception:
                    pass
                result = dispatch(nm, args_json or "{}")
                try:
                    return __import__("json").loads(result)
                except Exception:
                    return result

            # Strict JSON by default; allow override via env ATLAS_AGENT_SDK_STRICT_JSON=0 or caller strict_json_schema
            if strict_json_schema is None:
                strict_env = os.environ.get("ATLAS_AGENT_SDK_STRICT_JSON", "1").strip().lower()
                strict = strict_env not in ("0", "false", "no")
            else:
                strict = bool(strict_json_schema)
            # logging.getLogger("atlas_agent.agents").info("[SDK] register tool=%s strict_json_schema=%s", nm, strict)
            return FunctionTool(
                name=nm,
                description=desc or nm,
                params_json_schema=sch,
                on_invoke_tool=_on_invoke,
                strict_json_schema=strict,
            )

        wrapped.append(make_tool(name, schema, desc))
    return wrapped


def act_with_agents_sdk(
    *,
    client: LLMClient,
    system_prompt: str,
    user_text: str,
    shared_context: str | None,
    tools: List[Dict[str, Any]],
    dispatch,
    memory_texts: List[str] | None = None,
    temperature: float = 0.2,
    agent_name: str = "Agent",
    max_turns: int = 24,
) -> List[AgentMessage]:
    def _build_agent_with(strict: bool):
        wrapped = _wrap_tools_for_sdk(tools, dispatch, strict_json_schema=strict)
        AgentCls = getattr(openai_agents, "Agent", None)
        Runner = getattr(openai_agents, "Runner", None)
        if AgentCls is None or Runner is None:
            raise RuntimeError("openai-agents-python Agent/Runner classes not found")
        # Compose per-run instructions by embedding shared context and prior notes,
        # since some SDK versions do not expose message injection helpers.
        composed_instructions = system_prompt
        extras: List[str] = []
        if shared_context:
            extras.append(f"Shared context:\n{shared_context}")
        if memory_texts:
            extras.extend(memory_texts)
        if extras:
            composed_instructions = system_prompt + "\n\n" + "\n\n".join(extras)
        # Build agent; allow a custom base_url by constructing an OpenAIProvider and fetching a Model
        model_arg: Any = client.model
        if getattr(client, "base_url", None) is not None or client.api_key:
            try:
                provider = OpenAIProvider(api_key=client.api_key, base_url=getattr(client, "base_url", None))
                model_arg = provider.get_model(client.model)
            except Exception:
                model_arg = client.model
        try:
            agent = AgentCls(name=agent_name, instructions=composed_instructions, tools=wrapped, model=model_arg)
        except TypeError:
            agent = AgentCls(name=agent_name, instructions=composed_instructions, tools=wrapped)
        return agent, Runner

    # Force Agents SDK to use Chat Completions backend (avoid Responses API)
    try:
        set_default_openai_api("chat_completions")
        logging.getLogger("atlas_agent.agents").info(
            "[SDK] Default API set to chat_completions"
        )
    except Exception as e:
        logging.getLogger("atlas_agent.agents").warning(
            "[SDK] Could not set default API to chat_completions: %s", e
        )

    # Start with strict setting from env (default True); fallback to non-strict if provider conversion fails
    strict_env = os.environ.get("ATLAS_AGENT_SDK_STRICT_JSON", "1").strip().lower()
    strict_default = strict_env not in ("0", "false", "no")
    agent, Runner = _build_agent_with(strict_default)
    AgentCls = getattr(openai_agents, "Agent", None)
    Runner = getattr(openai_agents, "Runner", None)
    if AgentCls is None or Runner is None:
        raise RuntimeError("openai-agents-python Agent/Runner classes not found")

    # Run a single turn, allow custom max_turns to avoid premature termination
    # Retry wrapper for transient provider failures (e.g., OpenAI Responses 'Item with id ... not found')
    attempts = 3
    last_exc: Exception | None = None
    used_non_strict_fallback = False
    for i in range(attempts):
        try:
            try:
                result = Runner.run_sync(agent, input=user_text, max_turns=max_turns)
            except TypeError:
                # Older SDKs may not support max_turns kwarg on run_sync; fall back
                result = Runner.run_sync(agent, input=user_text)
            break
        except Exception as e:
            last_exc = e
            msg = str(e)
            logging.getLogger("atlas_agent.agents").error("Runner.run_sync failed (attempt %d/%d): %s", i + 1, attempts, msg)
            # Retry only for likely-transient resource id errors
            if "Item with id" in msg and "not found" in msg:
                time.sleep(0.25 * (i + 1))
                continue
            # Fallback: if provider can't convert request (schema conversion), rebuild tools with non-strict schemas once
            if ("convert_request_failed" in msg or "not implemented" in msg) and not used_non_strict_fallback:
                try:
                    logging.getLogger("atlas_agent.agents").warning("[SDK] Falling back to non-strict tool schemas after conversion failure")
                    agent, Runner = _build_agent_with(False)
                    used_non_strict_fallback = True
                    time.sleep(0.2)
                    continue
                except Exception:
                    pass
            # Non-retryable error
            return [AgentMessage(role="assistant", content=f"LLM provider error: {msg}")]
    else:
        # Exhausted attempts
        return [
            AgentMessage(
                role="assistant", content=f"LLM provider error: {str(last_exc)}"
            )
        ]
    # Runner returns an object with .final_output; fall back to .content/string if absent
    content = getattr(result, "final_output", None)
    if content is None:
        content = getattr(result, "content", None) if hasattr(result, "content") else str(result)
    return [AgentMessage(role="assistant", content=content)]
