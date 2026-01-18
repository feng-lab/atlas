"""Runtime defaults for Atlas Agent.

These are intentionally centralized to avoid duplicating magic numbers across
modules (CLI, console UI, and the chat runtime).
"""

# Max tool-loop rounds for the Executor phase in a single user turn.
#
# Note: this is a default, not a behavior cap. Users can override it via
# `--max-rounds` (including `0` for unlimited).
DEFAULT_EXECUTOR_MAX_ROUNDS = 9600


# Max tool-loop rounds for the Planner phase in a single user turn.
#
# Planner is read-only but it can still use tools (docs_search, filesystem hints,
# update_plan, verification_set_requirements). Keep this high enough that the
# Planner can recover from minor provider/tool-calling hiccups, but not so high
# that a stuck model burns lots of budget before the Executor can take over.
#
# Note: this is a default, not a behavior cap. Users can override it via
# `--planner-max-rounds` (including `0` for unlimited).
DEFAULT_PLANNER_MAX_ROUNDS = 240


# Responses-tool-loop defaults / guardrails
#
# These are "robustness knobs" rather than behavioral limits: they bound retry loops
# and help us recover from flaky OpenAI-compatible gateways without silently dropping
# work. When these need changes, update them here so magic numbers don't drift.
CONTEXT_TRIM_MAX_RETRIES = 32
TRANSIENT_NETWORK_MAX_RETRIES = 3
TRANSIENT_NETWORK_BACKOFF_SECONDS = 0.6
FINAL_OUTPUT_CONTINUE_MAX_CALLS = 8


# Screenshot / preview constraints
#
# Rationale: Some providers/models reject very large image payloads, and sending
# large images increases latency and cost. We therefore bound the bytes we attach
# to the model and ask for a smaller render if exceeded.
MAX_PREVIEW_IMAGE_BYTES_FOR_MODEL = 3_000_000


# Agent context shaping (internal runtime policy)
#
# Note: these don't truncate *storage*; they bound what we include in prompts so
# long sessions remain stable and deterministic. Full session history is still on
# disk and retrievable via session tools.
SESSION_MEMORY_COMPACTION_MODE = "llm"  # "llm" | "heuristic" | "off"
SESSION_MAX_RECENT_MESSAGES = 24
SESSION_MEMORY_RECENT_WRITE_EVENTS = 12

AUTO_RETRIEVE_MODE = "auto"  # "off" | "auto" | "always"
AUTO_RETRIEVE_MAX_SNIPPETS = 6
AUTO_RETRIEVE_MAX_CHARS = 280
AUTO_RETRIEVE_RECENT_WRITE_EVENTS = 8
AUTO_RETRIEVE_NEEDLE_MAX_TOKENS = 4

# Prompt-budget guardrail for the Supervisor Task Brief step.
INTENT_RESOLVER_SCENE_SNAPSHOT_MAX_CHARS = 2400


# File/search tool defaults (correctness-first)
#
# 0 means "unlimited"; -1 for max_depth means "unlimited depth".
DEFAULT_FS_RESOLVE_MAX_RESULTS = 0
DEFAULT_FS_RESOLVE_MAX_DEPTH = -1
DEFAULT_FS_HINT_RESOLVE_MAX_RESULTS = 0
DEFAULT_FS_HINT_RESOLVE_MAX_DEPTH = -1


# Codegen / python_write_and_run defaults (dev-only tool)
#
# We avoid truncating stdout/stderr by writing full outputs to files and returning
# those paths, but we still provide small previews to keep the tool payload safe.
DEFAULT_CODEGEN_TIMEOUT_SEC = 120.0
DEFAULT_CODEGEN_MAX_ECHO_CHARS = 4000
DEFAULT_CODEGEN_STDIO_PREVIEW_CHARS = 8000
