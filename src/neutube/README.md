# `src/neutube/` — neuTube 2.0 code (migration workspace)

This folder is the landing zone for the “neuTube 2.0” migration: modern C++ implementations of neuTube/NeuTu features
rebuilt on top of Atlas’ current infrastructure (zimg, modern SWC model, boost::json, etc.).

Project tracker and plan:

- `src/neutube/NEUTUBE2_MIGRATION.md`

Initial focus:

- CLI tracing runner (`nim::ZRunNeuTuCommand2`) as a modern replacement for the legacy `nim::ZRunNeuTuCommand`.

Design intent:

- Keep code here free of neurolabi C dependencies (`src/neurolabi/c`, `src/neurolabi/lib/genelib`).
- Prefer Atlas-standard libraries and patterns (see `AGENTS.md` at repo root).
