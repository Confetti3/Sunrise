# Sunrise Agent Guide

Read the narrow document for the task before searching source:

- Product/build overview: [README.md](README.md)
- Viewer and Inspector: [INSPECTOR.md](INSPECTOR.md)
- Bounded mission programs: [MISSIONS.md](MISSIONS.md)
- Build-86657 research and probes: [RESEARCH.md](RESEARCH.md)

## Source routing

- `Sunrise/src/client`: game-facing hooks, copied inspection state, UI, and local controls.
- `Sunrise/src/core`: process settings, logging, console, UI foundation, and lifecycle.
- `Sunrise/src/middleware`: bounded wire codecs and installed-content readers.
- `Sunrise/src/server`: offline service routes, gameplay host, mission policy, and replication.
- `Sunrise/src/state`: authoritative fixed-capacity process state and persistence infrastructure.
- `Sunrise/missions`: external bounded Lua authoring files.
- `tests`: standalone Windows C++ contract tests and their PowerShell runners.

Do not search build output, vendored code, old worktrees, or the external analysis corpus before the
canonical document identifies a specific need. Keep static, build, deployment, runtime, and
cross-build evidence separate. Every RVA or native layout must name its exact client build.

## Change safety

- Keep product behavior, dormant infrastructure, and temporary research probes visibly distinct.
- Do not turn a research receipt into a supported feature claim.
- Do not transfer native layouts or payload meaning across client builds.
- Run `git diff --check`, the narrow tests named by the relevant document, and a Release x64 build.
- Do not commit generated `build/`, `out/`, or `tests/build/` output.
