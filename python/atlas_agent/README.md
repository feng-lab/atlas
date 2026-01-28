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

- Some steps are best verified visually. The agent can render a screenshot image for verification.
  - For current scene state (preferred): `scene_screenshot`
  - For animation-at-time verification: `animation_render_preview`
- On startup, the CLI asks once per session for consent to use preview screenshots for verification.
  - Default is allow (press Enter), but you can deny and the agent will fall back to human-check steps for visual requirements.
  - You can toggle later in the REPL with `:screenshots on` / `:screenshots off`.

Common options:

- `--model` to choose the LLM model
- `--reasoning-effort low|medium|high|xhigh` to control how much deliberate reasoning the model uses (when supported by your model/provider)
- `--reasoning-summary auto|concise|detailed` to control whether/how a high-level reasoning summary is streamed (when supported by your model/provider)
- `--text-verbosity low|medium|high` to control assistant output verbosity (when supported by your model/provider)
- `--max-rounds N` to control how many tool-loop rounds the Executor is allowed to run in one turn (`0` = unlimited)
- `--web-search off|cached|live` to expose the Responses API built-in `web_search` tool
  - `cached`: provider cached content only (no live internet access)
  - `live`: allow live internet access (provider-controlled)
  - Requires the Responses API. If you force `--wire-api chat` (or your provider forces a fallback), web search is not available.

Notes:

- Atlas install location is discovered from the running Atlas RPC server. If Atlas isn't running, the CLI attempts to launch it from common install paths, then re-tries RPC.

## Docs + Long Sessions

- Atlas ships markdown docs inside the app bundle. The agent can search and read them at runtime via `docs_search` / `docs_read` / `docs_list`.
- Each user turn starts with a small Supervisor step that produces a short `TASK BRIEF` (stored in the session log). Downstream phases follow this brief to reduce intent drift in long sessions.
- The chat runtime maintains a compact “Session Memory” summary so long conversations remain stable even when raw history exceeds the model context window.
  - Memory compaction is built-in and not tuned via CLI flags or environment variables.
  - In the REPL: `:memory` shows the current memory summary.
- If a provider rejects a request due to context length, the runtime performs **checkpoint compaction**:
  - It compacts older within-turn tool-loop context into a short “CONTEXT CHECKPOINT” summary and retries.
  - It may also compact proactively when the estimated prompt budget is approaching the model’s effective input budget.
  - Model token budgeting prefers provider model metadata when available (total context window and max output tokens) and derives an effective input budget; it falls back to conservative model-name guesses only when the provider does not expose token limits.
  - If needed, it then falls back to trimming the oldest non-essential items.
  - The full session log on disk (`session.jsonl`) is never truncated; the checkpoint is only for prompt-budget resilience.
- Sessions are persisted on disk as a single append-only JSONL log (`session.jsonl`) containing:
  - domain events (plan updates, memory updates, verification policy/evidence, consent/meta),
  - transcript entries (user/assistant),
  - tool call events (args + results/summaries),
  - per-call LLM stats (prompt budget estimates + provider-reported token usage when available),
  - reasoning summaries (phase-level).
  - `--session <id-or-path>` to resume a previous session
  - `--session-dir <path>` to choose where sessions live
  - In the REPL: `:session`, `:resume`, `:brief`, `:plan`, `:memory`, `:budget`
- Default session location when `--session-dir` is omitted:
  - macOS/Linux: `$XDG_STATE_HOME/atlas_agent/sessions` if set, otherwise `~/.atlas_agent/sessions`
  - Windows: `%APPDATA%\\atlas_agent\\sessions`
- How to resume if you didn’t set anything explicitly:
  - Use the session id printed at startup: `atlas-agent --session <session_id>`
  - Or copy/paste the on-disk path from the REPL command `:session` (you can pass a session dir or a `session.jsonl` path)
  - Or use `:resume` to pick from existing sessions interactively (no copy/paste)
- Resume UX: when resuming an existing session (via `--session` or `:resume`), the CLI replays the saved session history to the terminal:
  - all transcript messages (user + assistant),
  - a one-line summary of each tool call,
  - the current plan (latest `update_plan`).
- Auto-retrieval (context-window resilience): when the user says “resume/continue/last time”, the runtime injects a small “Auto-retrieved context” block derived from the session log (recent tool calls + matching transcript entries).
  - This is intentionally a small excerpt; when more detail is needed, the agent can call `session_search_transcript` or `session_search_events`.
  - `session_search_transcript` / `session_search_events` support paging via `offset` + `max_results` and can return newest-first with `reverse=true`.
- The runtime streams a first-person “Reasoning summary” while the model thinks. This is a high-level summary (not chain-of-thought).

## Camera walkthroughs and waypoint splines

Atlas camera animation supports both:

- **First-person walkthroughs** (“fly/drone inside the object”): the agent turns natural-language motion into a small set of motion segments (local moves + yaw/pitch/roll) and writes camera keys.
- **Guided waypoint splines** (explicit waypoints): the agent solves keys from bbox/world waypoints and evaluates them as a spline.

Prompt patterns that work well:

- First-person walkthrough:
  - “Create a 12s first-person walkthrough: start outside the volume, fly forward into it, then yaw right while slowly ascending. Keep it smooth; no snap turns.”
  - “Do an interior fly-through; it’s OK if the object goes out of frame.”
- Guided waypoint spline:
  - “Make a 10s guided fly-through with 3 waypoints: outside the front face → inside the center → near the top-right corner. Look at the bbox center throughout. Use bbox fractions for waypoints so it works across datasets.”

Implementation notes:

- Camera interpolation method selection is currently disabled for RPC/agent use. Camera path tools rely on the default `Center` mode and achieve smoothness by writing appropriate camera keys.
- For interior shots, the agent disables the “keep object fully visible” constraint (`keep_visible=false`) so the camera can move inside.
- When the user provides explicit waypoints, the agent uses waypoint tools; when the user describes motion in words, the agent uses walkthrough segments.

Help:

- Console: `atlas-agent --help`
- Module: `python -m atlas_agent --help`

## Development (monorepo)

If you are working inside the Atlas repo:

```bash
pip install -e python/atlas_agent
```

Or run from source by setting `PYTHONPATH` to include `python/atlas_agent/src`.
