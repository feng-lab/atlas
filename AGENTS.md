# AGENTS.md — Atlas Repository Guide

Scope: Required instructions for anyone (human or automated agent) changing this repository. Read this before modifying code, docs, or build tooling.

## Reading Order
1. `readme.md` — build prerequisites, platform setup, high-level goals.
2. `docs/DEVELOPER_GUIDE.md` — architecture, threading rules, rendering pipeline.
3. `docs/USER_GUIDE.md` — product behavior reference; keep user-facing flows intact.

## Intent & Principles
- High-performance 2D/3D visualization built in modern C++20 with Qt, OpenGL, and emerging Vulkan support.
- Preserve progressive rendering, deterministic invalidation, and RAII-managed GPU resources.
- Respect SOLID/KISS/YAGNI, but never at the expense of correctness or portability.
- Correctness first: do not introduce arbitrary caps/limits that truncate, drop, or otherwise change behavior. When memory or latency matters, use streaming or block-wise processing that preserves full correctness. If a hard limit is unavoidable (e.g., user-specified), fail fast with a clear error instead of silently truncating.
- Optimize for post-change clarity: prefer designs that are simplest to read, reason about, and maintain after the refactor, even if they require a larger one-time change. Avoid partial measures or stateful gaps that trade clarity for smaller diffs.
- Follow Atlas naming conventions (`Z3D*` for shared/OpenGL 3D code, `ZVulkan*` for Vulkan-only). Do not introduce alternate prefixes such as `ZGL*`; keep files feature-scoped.
- Security/privacy: no unexpected telemetry, no leaking user data in logs.

## Non-Negotiable Rules
- **Never run `git checkout`**.
- Do not modify submodules or vendored third-party code unless explicitly tasked.
- Respect the single-GL-context assumption: all rendering/glbinding work happens on the rendering thread.
- Cross threads via `QMetaObject::invokeMethod` (queued connections). Never mutate engine QObjects from the UI thread.
- Update documentation (especially `docs/DEVELOPER_GUIDE.md` and migration docs) in the same change when behavior, architecture, or workflows move.
- **Never commit unless you have successfully compiled/tested the change or the user explicitly confirms it is safe to skip.** If the user instructs you to commit, freeze the work: make no additional edits or follow-up commits unless the user asks for more changes.
- Prefer hard `CHECK` assertions for every invariant we can satisfy within the engine across the entire code base (descriptor bindings, attachment counts, format contracts, non-null pointers, state machines, UI assumptions, etc.). Only downgrade to early returns when the condition genuinely depends on uncontrolled external inputs (e.g., user-provided files or network payloads), and emit a warning/error in those cases. If an early return is by design, document the rationale inline so reviewers understand why the invariant is soft.
- No magic numbers or silent truncation: do not hardcode list or text limits (e.g., `[:12]`) in engine or agent paths. Lists must be complete by default. If a limit is truly required (UI, transport), make it explicitly configurable, name the constant, document the rationale, and prefer paging/streaming over dropping data. When summarizing, state the policy in the output; never perform random cuts.

### Invariants & Error Handling Strictness
- Do not hide invariant violations with sentinel/default values. Never "return 0/{}" or similar fallbacks from internal engine APIs when an invariant should hold. Crash early with `CHECK`.
- Example: accessors that require an active frame (e.g., querying per‑frame dynamic offsets) must `CHECK(m_activeFrame)` and return the real value; do not return `0` when no frame is active.
- Optionals and soft error paths are allowed only at system boundaries (user I/O, files, network). Within the engine core and render pipeline, prefer `CHECK` to enforce contracts.
- Avoid defensive branches that mask bugs (e.g., silent `if (!ptr) return {};`). Either make the pointer nullable by contract and handle it explicitly with logging, or `CHECK`.

## Workflow Expectations
- Coordinate work through GitHub Issues (label P0/P1/P2). Document plan, progress, and hand-off notes in the issue thread.
- Keep changes focused. Prefer incremental PRs over sprawling refactors.
- Before committing to a major architectural change, draft the plan in the issue and link to the relevant design doc section.
- Always leave a session summary (What changed / What remains / Suggested next steps) if handing work to another agent.
- Complex changes should land with multi-line commit messages (subject + explanatory body) that call out rationale, risk, and validation.

## Build & Tooling Cheatsheet
- **Always read and keep this section handy during a session; it contains the canonical build/test commands.**
- Configure with CMake presets or run `cmake -S . -B build/Release -DCMAKE_BUILD_TYPE=Release` (DO NOT adjust preset).
- Incremental build: `cmake --build build/Release` (DO NOT use other build directory/preset, DO NOT add -j). Prefer running this to confirm edits compile; if you skip it, call out the gap in your handoff.
- Packaging helper: `python3 util/build_and_deploy_atlas.py [--skip-test]` (wraps build + optional smoke tests).
- Avoid editing generated files, build artifacts, or anything matched by `.gitignore` (bin/, obj/, artifacts/, logs/, win-mirror/).

## Testing & Verification
- Enable tests in your CMake configuration (`include(CTest)` is already wired). Run `ctest --output-on-failure` from the build directory for GoogleTest-based suites.
- GPU-dependent or UI features should be verified interactively when feasible; log findings in the issue if interactive validation is skipped.
- When touching numerics or serialization, add or extend tests in `test/` (GoogleTest). Use the `add_gtest_executable` macro defined in `test/test.cmake`.
- Benchmark additions go in `test/zbenchmark.cpp` and must be guarded to avoid shipping long-running jobs.

## C++ Conventions
- Avoid `friend` declarations. Do not use `friend` classes or functions unless there is a compelling, well-documented reason that cannot be achieved with proper encapsulation (interfaces, accessors, or helper functions). If you must use `friend`, document the rationale inline at the declaration site and in the PR description.

## Architecture Highlights
- **UI Thread**: owns `ZMainWindow`, `Z3DCanvas`, menus, docks. No direct renderer mutations.
- **Rendering Thread**: `Z3DRenderingEngine` + `Z3DCompositor`; holds global parameters (`Z3DGlobalParameters`) and scratch pool.
- **Engine Scheduler**: `Z3DRenderingEngine` owns the filter pipeline and drives progressive invalidation for all object filters feeding the compositor.
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
- Enum logging rules (zlog.h):
  - Prefer `enumOrUnderlying(e, 16)` when logging Vulkan (or other) enums to ensure a readable name if available, or a hex underlying value as a fallback without allocations or exceptions.
  - When you specifically want a literal fallback string, use `enumToStringOr(e, "<unknown>")` (returns `std::string_view`).
  - For Qt surfaces/UI text, use `enumToQStringOr(e, u"<unknown>")`.
  - Avoid `static_cast<int>(e)` and ad‑hoc string munging. Do not call the throwing `enumToString` in logs.
  - Prefer `std::string_view` results in formatting; don’t materialize `std::string` unless ownership is required.
- When you need GLM math utilities, include `zglmutils.h`; do not include `<glm/...>` headers directly.
- Maintain consistent naming: `CamelCase` types, `camelCase` locals, `_member` private fields where applicable.
- Remove dead code, unused includes, and stale feature flags. Do not leave TODOs without linked issues.
- Pointer nullability contract: treat all pointer and smart-pointer parameters as non-null by default. At function entry, `CHECK(ptr)` (or `CHECK(ptr != nullptr)`) once, then use directly without additional null branches. Only parameters explicitly marked as nullable (e.g., `/*nullable*/ T*` or documented as such) may be null; handle those paths deliberately. Unmarked pointers must be considered required and validated with a hard `CHECK`.

## Vulkan Migration Guardrails
- Follow the façade-first approach: filters talk to API-neutral renderers; backends plug in beneath `Z3DRendererBase`.
- Maintain feature parity: UI, parameters, and public APIs must not diverge between OpenGL and Vulkan.
- Document any backend limitations or temporary gaps in `docs/DEVELOPER_GUIDE.md` (Vulkan sections) and the issue tracker.
 - Invariant policy (Vulkan):
   - Match GL for benign skips (empty payloads, paging not ready, missing picking colors where GL also skips). Use early return.
   - For “should never happen” states, fail fast with `CHECK` (not silent return): null `renderer` on non-empty payloads; array size mismatches; missing/failed descriptor or buffer allocations; resolve/composite format contract violations.
   - Prefer debug-only `DCHECK` for expensive assertions (e.g., index range checks in hot loops).
- Do not introduce CPU-upload or GL-bridging fallbacks in Vulkan paths; if a Vulkan resource is required and unavailable, `CHECK` instead of silently substituting.
- Volatile inputs must be bound via per-draw override descriptor sets; allocation failure is fatal.

### Vulkan-Hpp/RAII Conventions
- Prefer Vulkan-Hpp and RAII types throughout (`vk::`, `vk::raii::`) instead of C API. Use strong enums/flags (`vk::Result`, `vk::ImageLayout`, etc.).
- Avoid `VK_TRUE`/`VK_FALSE`; use `bool` fields on Hpp structs. Avoid `VK_NULL_HANDLE`; prefer default-constructed Hpp handles (e.g., `vk::Buffer{}`).
- Result checks: compare against `vk::Result` (cast when interoping with VMA or C APIs) rather than `VK_SUCCESS`.
- Descriptor writes during recording are forbidden. Prime descriptor sets and write bindings before command buffer recording begins; during recording, only update buffer contents (host-visible) and bind pre-written sets.
- Samplers/descriptors: prefer immutable samplers when feasible to avoid platform-specific sampler class issues.
- Exceptions where C API/macros are acceptable:
  - VMA (vk_mem_alloc): uses C functions and raw handles; pass Hpp data via `reinterpret_cast` where necessary.
  - `VK_QUEUE_FAMILY_IGNORED` has no Hpp equivalent; keep using the macro for queue-family-ignored barriers.
  - Extension function pointers (e.g., debug utils) may be called via dispatcher guards when RAII wrappers are insufficiently portable.

## Debugging & Performance
- Use `--v=1` (or higher) for stage timing logs; wrap hotspots with `ZBenchTimer`.
- Diagnose GL state issues with `--atlas_debug_opengl` (expensive) or `--atlas_log_glbinding_context_switch` (for context audits).
- For GPU memory pressure, inspect scratch-pool logs and review recent trim activity.
- Keep `m_outputSize` and viewport sizes in sync across collaborating renderers to avoid redundant reallocations.

## Documentation & Handoff
- Any change that affects user workflows, CLI flags, or scene serialization must update `docs/USER_GUIDE.md` or appropriate reference sections.
- Record architectural shifts, threading changes, or resource policies in `docs/DEVELOPER_GUIDE.md`.
- When handing off work, note pending validation (tests not run, platforms not exercised) and blockers.

## Communication Expectations
- Final responses must be thorough and explicitly describe the root cause of the issue, the rationale for the chosen solution, and how the patch addresses the problem.
- Call out the relevant files or code regions that motivated the change so reviewers understand the context behind the fix.
- Highlight any best practices, trade-offs, or alternative approaches considered when explaining new code paths.

By following this guide, agents maintain Atlas’ stability while iterating quickly. Deviations require prior agreement in the relevant GitHub issue.
