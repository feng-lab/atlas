"""Runtime defaults for Atlas Agent.

These are intentionally centralized to avoid duplicating magic numbers across
modules (CLI, console UI, and the chat runtime).
"""

# Max tool-loop rounds for the Executor phase in a single user turn.
#
# Note: this is a default, not a behavior cap. Users can override it via
# `--max-rounds` (including `0` for unlimited).
DEFAULT_EXECUTOR_MAX_ROUNDS = 9600

