from dataclasses import dataclass

from .base import LLMClient

INTENT_RESOLVER_SYSTEM = (
    "You are the Task Brief writer for Atlas Agent.\n"
    "Goal: Merge chat history + the latest user message into a short, self-contained TASK BRIEF so the next phase can act without extra history.\n\n"
    "Instructions:\n"
    "- Read the user message, history, and a compact scene context.\n"
    "- Proceed-first policy: prefer a Task Brief with explicit assumptions. Avoid asking questions.\n"
    "- Do NOT output CLARIFY. If something is ambiguous, pick a reasonable default and encode it as an Assumption.\n"
    "- Do NOT plan steps and do NOT name parameters/json_keys/options. Provide only high-level intent and targets. Do NOT ask for exact paths or option strings — tools will resolve them.\n"
    "- Treat any user-mentioned file or folder as a hint only; do NOT expand/guess absolute paths. Keep the hint as-is in Targets/Inputs and leave path resolution/verification to fs_* tools.\n"
    "- Assumptions MUST NOT prescribe additional parameter mutations; defaults remain unchanged unless explicitly requested by the user.\n"
    "- Classify intent (scene, animation, or mixed) and reflect user‑provided durations.\n"
    "- Hint direction only (e.g., update scene or update animation); avoid design details.\n"
    "- Output exactly a TASK BRIEF with these bullets (plain text, no JSON):\n"
    "      - Intent: scene | animation | mixed | playback | save | explain\n"
    "      - Targets/Inputs: ids/names if known; file hints or patterns\n"
    "      - Assumptions: defaults chosen (state them clearly)\n"
    "      - Signals: update scene or update animation (brief); duration if provided\n"
    "      - Verify: what success looks like (be concise)\n"
)

# Variant used by the runtime to recover from spurious CLARIFY outputs when the
# request is still safely actionable with defaults.
INTENT_RESOLVER_SYSTEM_FORCE_TASK_BRIEF = (
    INTENT_RESOLVER_SYSTEM
    + "\n\nAdditional rule: Output MUST be a TASK BRIEF (never CLARIFY)."
)


@dataclass
class IntentResolver:
    client: LLMClient
    temperature: float | None = None

    def resolve(self, user_text: str, *, scene_context: str) -> str:
        prompt = (
            "Scene context + history:\n" + scene_context + "\n\n" +
            "Latest user message:\n" + (user_text or "") + "\n\n" +
            "Produce a 'TASK BRIEF:' as specified."
        )
        return self.client.complete_text(
            system_prompt=INTENT_RESOLVER_SYSTEM,
            user_text=prompt,
            temperature=self.temperature,
        )

    def resolve_force_task_brief(self, user_text: str, *, scene_context: str) -> str:
        """Resolve a TASK BRIEF even when the model would otherwise ask CLARIFY.

        Used as a best-effort recovery path for providers/models that over-use
        clarifying questions. This keeps the agent "proceed-first" by encoding
        defaults as explicit assumptions.
        """

        prompt = (
            "Scene context + history:\n" + scene_context + "\n\n" +
            "Latest user message:\n" + (user_text or "") + "\n\n" +
            "Produce a 'TASK BRIEF:' as specified."
        )
        return self.client.complete_text(
            system_prompt=INTENT_RESOLVER_SYSTEM_FORCE_TASK_BRIEF,
            user_text=prompt,
            temperature=self.temperature,
        )
