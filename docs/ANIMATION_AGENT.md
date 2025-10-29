Atlas Animation Agent (Python)

Overview
- Multi‑agent system that designs animations live in Atlas via gRPC, with preview in the GUI timeline.
- Single entry point: a chat interface. The agent saves .animation3d via RPC and, when asked, exports MP4 by invoking Atlas headless in the background (no extra CLI commands for the user).

Quickstart
- Run chat (only command):
  - `python -m tools.atlas_agent chat-rpc --address localhost:50051 --model o4`

Schema Discovery
- Preferred: pass `--atlas-dir` (installation root). The tool derives both the Atlas binary and the schema under it.
- If `--atlas-dir` is not provided, it searches common defaults and errors if not found:
  - macOS: `/Applications/fenglab/Atlas.app`, `/Applications/Atlas.app`
  - Windows: `C:\\Program Files\\fenglab\\Atlas`, `C:\\Program Files (x86)\\fenglab\\Atlas`
  - Linux: `/opt/fenglab/Atlas`, `/opt/fenglab/atlas`, `/usr/local/fenglab/Atlas`
- Alternatively, provide `--schema-dir` or set `ATLAS_SCHEMA_DIR`.

Headless Rendering
- When the agent exports video on your behalf, it invokes Atlas with the headless exporter (`--run_export_3d_animation`), adding `-platform offscreen` on Windows/Linux.

Notes
- Headless export uses the Atlas binary; set `--atlas-dir` or install Atlas to standard locations.
- Requires the Agents SDK (pip package name `agents`) for multi‑agent chat. Install it to enable session management and future MCP tool support.

No Other CLI Commands
- All actions happen through chat. Ask to load data, set keys, play/pause, save, or export and the agent will call the right tools under the hood.

Natural Language Examples
- Filesystem selection (auto-expands into Doc):
  - "Use swc files in /data/neuron_traces"
  - "Use image files in ./slices with name starts with 'slice__'"
  - "Add file soma.ply and fibers.obj"
- Camera motion:
  - "Orbit around mesh 2 for 12 seconds with radius 250 on y axis"
  - "Orbit around soma.ply for 8s, then dolly in"
- Per-object effects:
  - "Toggle all swc files off at time 10 seconds"
  - "Increase the opacity of file fibers.obj from 0 to 1 during time 10-12 seconds"
  - "Color of file soma.ply from red to blue during time 5-8 seconds"

Multi‑Agent (live)
- Start: `python -m tools.atlas_agent chat-rpc --address localhost:50051 --model o4`
- The agents (Supervisor, Planner, Inspector) will:
  - Query scene state (objects, bbox, params) via tools
  - Propose ≥2 plans, critique, ask clarifying questions if inputs are ambiguous
  - Execute with atomic key updates, adjust, and explain rationale concisely
