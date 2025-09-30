# AGENTS.md — Atlas Repository Guide

Scope: Required instructions for anyone (human or automated agent) changing this repository. Read this before modifying code, docs, or build tooling.

## Reading Order
1. `readme.md` — build prerequisites, platform setup, high-level goals.
2. `docs/DEVELOPER_GUIDE.md` — architecture, threading rules, rendering pipeline.
3. `docs/VULKAN_MIGRATION_PLAN.md` — mandatory policy for backend work.
4. `docs/USER_GUIDE.md` — product behavior reference; keep user-facing flows intact.

## Intent & Principles
- High-performance 2D/3D visualization built in modern C++20 with Qt, OpenGL, and emerging Vulkan support.
- Preserve progressive rendering, deterministic invalidation, and RAII-managed GPU resources.
- Respect SOLID/KISS/YAGNI, but never at the expense of correctness or portability.
- Follow Atlas naming conventions (`Z3D*` for shared/OpenGL 3D code, `ZVulkan*` for Vulkan-only). Do not introduce alternate prefixes such as `ZGL*`; keep files feature-scoped.
- Security/privacy: no unexpected telemetry, no leaking user data in logs.

## Non-Negotiable Rules
- **Never run `git checkout`** inside automated workflows; use patch or `git restore --source=HEAD -- <path>` if you must discard *your own* work. Do not touch user changes.
- Do not modify submodules or vendored third-party code unless explicitly tasked.
- Respect the single-GL-context assumption: all rendering/glbinding work happens on the rendering thread.
- Cross threads via `QMetaObject::invokeMethod` (queued connections). Never mutate engine QObjects from the UI thread.
- When adding or altering Vulkan code, first validate the API-neutral façade per `docs/VULKAN_MIGRATION_PLAN.md`. Do not bypass the façade or duplicate OpenGL state.
- Update documentation (especially `docs/DEVELOPER_GUIDE.md` and migration docs) in the same change when behavior, architecture, or workflows move.
- **Never commit unless you have successfully compiled/tested the change or the user explicitly confirms it is safe to skip.** If the user instructs you to commit, freeze the work: make no additional edits or follow-up commits unless the user asks for more changes.

## Workflow Expectations
- Coordinate work through GitHub Issues (label P0/P1/P2). Document plan, progress, and hand-off notes in the issue thread.
- Keep changes focused. Prefer incremental PRs over sprawling refactors.
- Before committing to a major architectural change, draft the plan in the issue and link to the relevant design doc section.
- Always leave a session summary (What changed / What remains / Suggested next steps) if handing work to another agent.
- Complex changes should land with multi-line commit messages (subject + explanatory body) that call out rationale, risk, and validation.

## Build & Tooling Cheatsheet
- **Always read and keep this section handy during a session; it contains the canonical build/test commands.**
- First-time setup: follow `readme.md` for platform prerequisites (Qt, Intel oneAPI, Vulkan SDK, etc.) before running repo scripts.
- After external dependencies are in place, run `python3 util/build_ext_libs.py all` to stage bundled libraries.
- Configure with CMake presets or run `cmake -S . -B build/Release -DCMAKE_BUILD_TYPE=Release` (adjust preset as needed).
- Incremental build: `cmake --build build/Release` (or your chosen build directory/preset). Prefer running this to confirm edits compile; if you skip it, call out the gap in your handoff.
- Packaging helper: `python3 util/build_and_deploy_atlas.py [--skip-test]` (wraps build + optional smoke tests).
- Avoid editing generated files, build artifacts, or anything matched by `.gitignore` (bin/, obj/, artifacts/, logs/, win-mirror/).

## Testing & Verification
- Enable tests in your CMake configuration (`include(CTest)` is already wired). Run `ctest --output-on-failure` from the build directory for GoogleTest-based suites.
- GPU-dependent or UI features should be verified interactively when feasible; log findings in the issue if interactive validation is skipped.
- When touching numerics or serialization, add or extend tests in `test/` (GoogleTest). Use the `add_gtest_executable` macro defined in `test/test.cmake`.
- Benchmark additions go in `test/zbenchmark.cpp` and must be guarded to avoid shipping long-running jobs.

## Architecture Highlights
- **UI Thread**: owns `ZMainWindow`, `Z3DCanvas`, menus, docks. No direct renderer mutations.
- **Rendering Thread**: `Z3DRenderingEngine` + `Z3DCompositor`; holds global parameters (`Z3DGlobalParameters`) and scratch pool.
- **Network Evaluator**: `Z3DNetworkEvaluator` processes filter graphs and progressive invalidation.
- **Scratch Pool**: lease-based reuse of BlockID, entry/exit, layer arrays, temp 2D RTs. Slots grow but only shrink on `trim()`.
- **Transparency Modes**: Blend-delayed, dual depth peeling, weighted average, weighted blended. Ensure new passes request the correct pooled resources and restore GL state.
- **Alias model**: Documents own data; aliases share packs but have independent filters/render states.

## Coding Standards
- C++20 (`set(CMAKE_CXX_STANDARD 20)`); enable warnings and treat new warnings seriously.
- Prefer RAII, smart pointers, and move semantics. Keep GPU handle lifetime explicit.
- Add brief comments when control flow, state transitions, or variable roles are non-obvious so future readers understand intent quickly.
- One type per file; match filenames to classes (e.g., `z3dscratchresourcepool.cpp`/`.h`).
- Thread-safe design: avoid global state mutations without synchronization; keep renderer state local to the rendering thread.
- Structured logging through `ZLog`; avoid `std::cout`/`printf` in production paths.
- When you need GLM math utilities, include `zglmutils.h`; do not include `<glm/...>` headers directly.
- Maintain consistent naming: `CamelCase` types, `camelCase` locals, `_member` private fields where applicable.
- Remove dead code, unused includes, and stale feature flags. Do not leave TODOs without linked issues.

## Vulkan Migration Guardrails
- Follow the façade-first approach: filters talk to API-neutral renderers; backends plug in beneath `Z3DRendererBase`.
- Maintain feature parity: UI, parameters, and public APIs must not diverge between OpenGL and Vulkan.
- Document any backend limitations or temporary gaps in `docs/VULKAN_MIGRATION_PLAN.md` and the issue tracker.

## Debugging & Performance
- Use `--v=1` (or higher) for stage timing logs; wrap hotspots with `ZBenchTimer`.
- Diagnose GL state issues with `--atlas_debug_opengl` (expensive) or `--atlas_log_glbinding_context_switch` (for context audits).
- For GPU memory pressure, inspect scratch-pool logs and review recent trim activity.
- Keep `m_outputSize` and viewport sizes in sync across collaborating renderers to avoid redundant reallocations.

## Documentation & Handoff
- Any change that affects user workflows, CLI flags, or scene serialization must update `docs/USER_GUIDE.md` or appropriate reference sections.
- Record architectural shifts, threading changes, or resource policies in `docs/DEVELOPER_GUIDE.md`.
- When handing off work, note pending validation (tests not run, platforms not exercised) and blockers.

By following this guide, agents maintain Atlas’ stability while iterating quickly. Deviations require prior agreement in the relevant GitHub issue.
