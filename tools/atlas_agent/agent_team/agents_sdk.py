from __future__ import annotations

from dataclasses import dataclass
from typing import Any, Dict, List, Tuple

from .base import AgentMessage, LLMClient

# Require Agents SDK; no fallback path
import agents as openai_agents  # type: ignore


def _wrap_tools_for_sdk(tools: List[Dict[str, Any]], dispatch) -> List[Any]:
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
                result = dispatch(nm, args_json or "{}")
                try:
                    return __import__("json").loads(result)
                except Exception:
                    return result

            return FunctionTool(
                name=nm,
                description=desc or nm,
                params_json_schema=sch,
                on_invoke_tool=_on_invoke,
                # Use non-strict schema to avoid forcing every property into
                # the 'required' list; our schemas specify defaults and allow
                # optional fields like 'color'.
                strict_json_schema=False,
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
    wrapped_tools = _wrap_tools_for_sdk(tools, dispatch)
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

    # Build agent; allow a custom base_url by constructing an OpenAIProvider and
    # fetching a Model object to pass into the Agent.
    model_arg: Any = client.model
    # If LLMClient has a base_url, construct a provider-backed Model
    if getattr(client, "base_url", None) is not None or client.api_key:
        try:
            from agents.models.openai_provider import OpenAIProvider  # type: ignore
            provider = OpenAIProvider(api_key=client.api_key, base_url=getattr(client, "base_url", None))
            model_arg = provider.get_model(client.model)
        except Exception:
            model_arg = client.model

    try:
        agent = AgentCls(name=agent_name, instructions=composed_instructions, tools=wrapped_tools, model=model_arg)
    except TypeError:
        agent = AgentCls(name=agent_name, instructions=composed_instructions, tools=wrapped_tools)

    # Run a single turn, allow custom max_turns to avoid premature termination
    try:
        result = Runner.run_sync(agent, input=user_text, max_turns=max_turns)
    except TypeError:
        # Older SDKs may not support max_turns kwarg on run_sync; fall back
        result = Runner.run_sync(agent, input=user_text)
    # Runner returns an object with .final_output; fall back to .content/string if absent
    content = getattr(result, "final_output", None)
    if content is None:
        content = getattr(result, "content", None) if hasattr(result, "content") else str(result)
    return [AgentMessage(role="assistant", content=content)]
