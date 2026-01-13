# Atlas Agent (Python)

`atlas-agent` is a CLI that connects to a running Atlas instance over gRPC and lets you control it via an LLM-powered streaming tool loop (OpenAI Responses API + tool-calling).

## Requirements

- Python 3.12+
- Atlas is controlled via a local gRPC server at `localhost:50051`
  - If Atlas is not running, the CLI will try to launch it from common install locations and then retry RPC discovery.
  - The agent compiles gRPC client stubs at runtime from the running Atlas installation’s `Resources/protos/scene.proto` (single source of truth; no monorepo fallback) to avoid proto drift.

## Installation

```bash
pip install atlas-agent
```

## Configuration

The agent requires an OpenAI-compatible API key:

- `OPENAI_API_KEY` (required)
- `OPENAI_BASE_URL` (optional) if you use a non-default endpoint (OpenAI-compatible providers)

Examples:

```bash
export OPENAI_API_KEY="..."
```

```bash
export OPENAI_API_KEY="..."
export OPENAI_BASE_URL="https://your-openai-compatible-endpoint/v1"
```

## Basic usage

Run the CLI (it starts a simple console UI by default):

```bash
atlas-agent
```

Optional: use the plain REPL (no styling; helpful for debugging or very limited terminals):

```bash
atlas-agent --plain
```

Phases (adaptive, default):

- Planner: may run first to produce/refresh the plan (read-only tools + `update_plan` only).
- Executor: performs the actual work (full tool access).
- Verifier: runs only if Executor made Atlas changes (read-only verification + `update_plan`), and produces the final answer.

Screenshots (optional)

- Some steps are best verified visually. The agent can render a preview frame to an image via `animation_render_preview`.
- On startup, the CLI asks once per session for consent to use preview screenshots for verification.
  - Default is allow (press Enter), but you can deny and the agent will fall back to human-check steps for visual requirements.
  - You can toggle later in the REPL with `:screenshots on` / `:screenshots off`.

Common options:

- `--model` to choose the LLM model
  - Atlas install location is discovered from the running Atlas RPC server. If Atlas isn't running, the CLI attempts to launch it from common install paths, then re-tries RPC.

## Docs + Long Sessions

- Atlas ships markdown docs inside the app bundle. The agent can search and read them at runtime via `docs_search` / `docs_read` / `docs_list`.
- The chat runtime maintains a compact “Session Memory” summary so long conversations remain stable even when raw history exceeds the model context window.
  - Memory compaction is built-in and not tuned via CLI flags or environment variables.
  - In the REPL: `:memory` shows the current memory summary.
- Sessions are persisted on disk as a single append-only JSONL log (`session.jsonl`) containing:
  - domain events (plan updates, memory updates, verification policy/evidence, consent/meta),
  - transcript entries (user/assistant),
  - tool call events (args + results/summaries),
  - reasoning summaries (phase-level).
  - `--session <id-or-path>` to resume a previous session
  - `--session-dir <path>` to choose where sessions live
  - In the REPL: `:session`, `:plan`, `:memory`
- Default session location when `--session-dir` is omitted:
  - macOS/Linux: `$XDG_STATE_HOME/atlas_agent/sessions` if set, otherwise `~/.atlas_agent/sessions`
  - Windows: `%APPDATA%\\atlas_agent\\sessions`
- How to resume if you didn’t set anything explicitly:
  - Use the session id printed at startup: `atlas-agent --session <session_id>`
  - Or copy/paste the on-disk path from the REPL command `:session` (you can pass a session dir or a `session.jsonl` path)
- Auto-retrieval (context-window resilience): when the user says “resume/continue/last time”, the runtime injects a small “Auto-retrieved context” block derived from the session log (recent tool calls + matching transcript entries).
  - This is intentionally a small excerpt; when more detail is needed, the agent can call `session_search_transcript` or `session_search_events`.
- The runtime streams a first-person “Reasoning summary” while the model thinks. This is a high-level summary (not chain-of-thought).

Help:

- Console: `atlas-agent --help`
- Module: `python -m atlas_agent --help`

## Development (monorepo)

If you are working inside the Atlas repo:

```bash
pip install -e python/atlas_agent
```

Or run from source by setting `PYTHONPATH` to include `python/atlas_agent/src`.
