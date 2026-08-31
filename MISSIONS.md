# Bounded Lua Mission Programs

Sunrise can compile a fixed-budget Lua mission description into a pointer-free `MissionProgram` at
load time. Lua is an authoring parser only. Mission ticks execute the validated C++ program and do
not run Lua callbacks.

## Current scope

- Exact client/content scope: Windows PC build 86657.
- Current authored file: `Sunrise/missions/edz_freeroam.lua`.
- Current use: bounded Trostland Glimmer research, disabled unless its activation policy is enabled.
- This is not general mission support. Enemy waves are presently empty in the authored program,
  and several `ContentStepKind` names remain fail-closed placeholders without supported wire output.

The compiler rejects undeclared or over-budget rows, unknown events/actions/content-step names,
invalid transforms, duplicate identities, unresolved targets, and programs whose declared budgets do
not match their contents. Stable IDs and the final program hash are derived deterministically.

## Runtime boundary

The program contains bounded objectives, proximity interactions, enemy-wave intents, content-step
intents, content signals, timers, transitions, and up to four actions per transition. The compiled
policy deduplicates events and produces fixed-capacity intent queues. Separate gameplay and BAP code
owns any native effect; compilation alone does not prove that a content step was delivered or shown.

Mission files are external runtime content. Packaging must retain
`Sunrise/missions/<destination>.lua` beside the expected module layout.

## Validation

From the repository root, run:

```powershell
.\tests\run_mission_compiler_test.ps1
.\tests\run_compiled_mission_policy_test.ps1
.\tests\run_content_step_queue_test.ps1
.\tests\run_enemy_wave_queue_test.ps1
.\tests\run_mission_signal_queue_test.ps1
```

The compiler test receives the repository path explicitly so it remains valid when its executable
runs from a temporary directory.
