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
DEFAULT_PLANNER_MAX_ROUNDS = 2400


# Responses-tool-loop defaults / guardrails
#
# These are "robustness knobs" rather than behavioral limits: they bound retry loops
# and help us recover from flaky OpenAI-compatible gateways without silently dropping
# work. When these need changes, update them here so magic numbers don't drift.
CONTEXT_TRIM_MAX_RETRIES = 32
TRANSIENT_NETWORK_MAX_RETRIES = 3
TRANSIENT_NETWORK_BACKOFF_SECONDS = 0.6
# Cap exponential backoff so extended retry loops don't sleep unboundedly.
TRANSIENT_NETWORK_BACKOFF_MAX_SECONDS = 600.0
FINAL_OUTPUT_CONTINUE_MAX_CALLS = 8

# Some OpenAI-compatible gateways occasionally return a response payload that is
# missing the routed model name (resp["model"]). For OpenAI models, this strongly
# suggests the request did not fully reach (or return from) the intended backend.
#
# We treat this (and related gateway/proxy errors) as recoverable and allow a long
# retry loop, bounded by TRANSIENT_NETWORK_BACKOFF_MAX_SECONDS per attempt.
#
# NOTE: This is a *retry budget*, not an exponential backoff exponent. Call sites
# must cap sleeps to avoid unbounded delays and integer→float overflow.
GATEWAY_MODEL_DETECTION_MAX_RETRIES = 4500

# Context checkpoint compaction (within-turn)
#
# When a provider rejects a request due to context window overflow, atlas_agent
# tries to "compact" older within-turn tool-loop history into a small checkpoint
# summary and retries, rather than blindly dropping old items. This preserves
# intent/progress across very long tool loops that span multiple model context
# windows.
CONTEXT_COMPACTION_KEEP_TAIL_ITEMS = 24
CONTEXT_COMPACTION_RECENT_TOOL_EVENTS = 24

# Proactive checkpoint compaction
#
# We try to compact BEFORE hitting a provider "context length exceeded" error.
# This is best-effort and uses an approximate token estimate; when estimates are
# wrong (or the provider has stricter limits), the reactive overflow path still
# applies.
PROACTIVE_CONTEXT_COMPACTION_TRIGGER_RATIO = 0.8
PROACTIVE_CONTEXT_COMPACTION_MAX_ATTEMPTS_PER_CALL = 3


# Model token budgeting
#
# Default provider-agnostic auto-compaction threshold when a provider does not
# expose an explicit "auto compact token limit" for a model. Expressed as a
# rational to avoid float rounding drift and keep integer math deterministic.
DEFAULT_AUTO_COMPACT_RATIO_NUMERATOR = 9
DEFAULT_AUTO_COMPACT_RATIO_DENOMINATOR = 10


# Console UX
#
# When selecting a session via `:resume`, we show a 1-line preview derived from the
# first user message in the session. This is a UI-only preview; the full transcript
# is replayed on resume.
RESUME_SESSION_PICKER_PREVIEW_MAX_CHARS = 200


# Screenshot / preview constraints
#
# Rationale: Some providers/models reject very large image payloads, and sending
# large images increases latency and cost. We therefore bound the bytes we attach
# to the model and ask for a smaller render if exceeded.
# Maximum size (bytes) for inlining preview images as base64 data URLs in the
# tool loop (function_call_output). Inline base64:
# - increases payload size (~33% overhead),
# - is re-sent in subsequent tool-loop rounds (history), and
# - can quickly blow up context/token usage.
#
# Prefer file_id attachments via the Files API when possible; inline base64 only
# as a fallback for small images.
MAX_PREVIEW_IMAGE_INLINE_BYTES_FOR_MODEL = 1_000_000

# Backwards-compatible alias (older code/tests may still import this name).
MAX_PREVIEW_IMAGE_BYTES_FOR_MODEL = MAX_PREVIEW_IMAGE_INLINE_BYTES_FOR_MODEL


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

# Subprocess output shaping (console)
#
# Some Atlas CLI helper operations (e.g. headless animation export/preview) can
# emit very verbose stdout/stderr, which is helpful for debugging but noisy for
# normal interactive use. We therefore show only a small head+tail summary by default.
#
# This affects only what we print to the console/logs; the tool JSON results
# returned to the model remain unchanged.
DEFAULT_SUBPROCESS_LOG_HEAD_LINES = 5
DEFAULT_SUBPROCESS_LOG_TAIL_LINES = 15


# Web search tool (Responses API built-in)
#
# Disabled by default for determinism and privacy. Enable via `--web-search`.
# When enabled:
# - "cached": uses provider cached content (no live internet access)
# - "live": allows live internet access (provider-controlled)
DEFAULT_WEB_SEARCH_MODE = "cached"  # "off" | "cached" | "live"
