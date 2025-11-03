"""Typed Script API for Atlas agents.

This package contains developer-facing, typed helpers for writing Python
programs that orchestrate Atlas via RPC. It complements (and is distinct from)
the curated Agent Tooling used for LLM function-calling.

Key modules:
  - plan_types: dataclasses for scene/animation plans
  - fs: filesystem helpers (expand/check/glob/resolve/search)
  - scene: typed wrappers around SceneClient (raises exceptions)
  - camera: typed camera planning/validation helpers
  - runner: validate → apply → verify execution helpers
"""
