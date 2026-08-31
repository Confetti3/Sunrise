# Glimmer Extraction Research — build 86657

Status: active, bounded research. This is the authoritative agent-context document for the current Glimmer extraction work.

## Current conclusion

The authentic site-1 dropship contribution reaches the native request builder, but extraction presentation does not complete.

Direct build-86657 runtime hooks now prove the failure point:

- Every correlated `allocate_datum` call returned `0xFFFFFFFF` (`-1`).
- The initializer was never called.
- The failure is allocation failure before initialization, not initializer failure.
- The exact build-86657 pool routine at RVA `0x35EE10` scans an 8192-bit availability bitmap for the first set/free bit and returns `-1` when none exists.
- Eight direct before/after packets now prove the 1,024-byte bitmap at allocator context `+0xC118` is readable and contains zero set bits before and after every correlated attempt; it is unpopulated rather than exhausted.
- The newer entity-slot trace moves the missing lifecycle earlier: authentic type-0 delivery reaches handler RVA `0x4F4530`, which stores all 7,936 granted slots and requests application through the session manager.
- Runtime has a valid established session, slot record, and roster container, but the pending count remains `7936`, dirty remains `0`, and the sticky world-container bind receipt remains `0`.
- The build-86657 bind receipt setter is RVA `0x4F6480`; it is not reached. The next bounded task is tracing its callers, indirect dispatch, and guards. Do not write the allocator bitmap.

The live visual result remains a stationary ship with no drill. Native request production is not proof of a complete public-event presentation.

A manual authentic entity-slot type-0 re-publication was staged and delivered exactly once with no transaction error. It did not change the result: all 24 correlated allocations still returned `0xFFFFFFFF`, and initialization remained unreachable. A missed one-time join notification is therefore not a sufficient explanation for the empty simulation allocator; do not repeat this diagnostic unless new evidence changes the premise.

## Proven site-1 content facts

- `1/28 sq_dropship -> 66/324 sr_dropship` is an exact serialized population binding.
- Type `58/394` is `dropship_enter` and directly references path payload `80BE6A21`.
- Type `58/397` is `dropship_exit` and directly references path payload `80BE6A25`.
- No serialized edge joins `1/28` or `66/324` to `58/394` or `58/397`.
- The generic active-controller closure contains no site-1 roster endpoint.
- The active resource chain is exact:

  `80BE8D79 (5/14) -> 80C0124F -> 80FD6396 -> 80C0124D -> 80C0124B`

- Its expression is `gl_pla_public_event_active >= 0.25`, but this does not prove a site-1 command or lifecycle edge.

### Corrected defender relationship

The earlier `1/207 -> 66/321` claim is contradicted by the serialized record.

- Actual: `1/207 sq_defenders[0] -> 66/450 sr_device_dummy`.
- `66/321 sr_drill_near` has only a self identity in the inspected material.
- The rejected defender probe is disabled and is not part of the supported mission.

Do not recommend direct activation of `66/321`, type-58 emission, drill/device toggles, or an active-controller-to-site-1 edge without a decoded owner, value, direction, and runtime receipt.

## Current v5 chain

Version 5 is the supported bounded test chain:

1. Trigger or reload/re-arm the Glimmer mission in the admitted `edz_freeroam` session.
2. Stage and commit the proven `1/28 -> 66/324` ship contribution.
3. Observe the native request builder produce the request.
4. Correlate the exact retail `failed to create 'sobject' entity` wrapper at RVA `0x16EE3F7`.
5. Follow the build-86657 lower wrapper at RVA `0x170F190`: initialize output to `-1`, call `allocate_datum`, copy and compare the result, then call the initializer only on allocation success.
6. Current hooks stop the causal chain at allocation: `allocate_datum == 0xFFFFFFFF`; no initializer call follows.

The former defender-probe step is absent from v5. A successful test receipt must not claim drill motion, dropship path playback, or actor creation from request production alone.

## Runtime history that remains valid

- Two native ship build calls each had four exact retail SObject failures in the same millisecond.
- All eight failures used call-site RVA `0x16EE3F7` and the same coherent request snapshot.
- That timing proves deterministic request/failure correlation, but does not identify which internal object each failure represents.
- The lower wrapper disassembly establishes allocate-before-initialize control flow.
- The newer direct hooks resolve the remaining branch question: allocation fails and initialization is not reached.

## Live no-restart workflow

Keep Destiny open in `edz_freeroam`. Do not restart the game for ordinary status, trigger, reload/re-arm, or capture cycles.

Division of work:

- **Kate drives the game.** Kate moves the player and confirms the visible and gameplay result.
- **The agent commands the experiment.** The agent checks status, triggers, reloads/re-arms, arms captures, and collects evidence.

Three equivalent control surfaces are available:

- UI: **Inspector > Runtime > Glimmer Test**.
- Research console: status, trigger, reload/re-arm, and capture-arm operations.
- PowerShell:

```powershell
G:\DestinyResearch\tmp\glimmer-test.ps1 -Action status
G:\DestinyResearch\tmp\arm-glimmer-capture.ps1
G:\DestinyResearch\tmp\capture-sunrise-function.ps1 -Rva 0x35EE10
G:\DestinyResearch\tmp\reload-glimmer-mission.ps1
G:\DestinyResearch\tmp\collect-glimmer-live-evidence.ps1 -Label my-test
G:\DestinyResearch\tmp\run-glimmer-test.ps1 -Label my-test
```

`run-glimmer-test.ps1` arms the wide capture, triggers once, polls until state 3, waits for evidence, and snapshots the live log without closing Destiny.

For each attempt, retain the mission trigger/commit facts, native build facts, SObject wrapper correlation, allocation result, initializer-call count, and Kate's visual result.

## Platform and evidence boundaries

All current function RVAs, hook results, bitmap behavior, and runtime conclusions above are Windows PC build 86657 facts only.

The supplied PS4 image is Destiny 2 `CUSA05042` v1.0. Its RTTI names include:

- `c_simulation_sobject_entity_definition`
- `c_simulation_queue_activity_client_entity_slots_allocation_definition`

These names are useful cross-build vocabulary only. They do not identify the build-86657 allocator, prove the 8192-entry capacity on PS4, or transfer any RVA, layout, or behavior between builds.

## Source reports

- `/mnt/g/DestinyResearch/tmp/glimmer-site1-drill-command-candidates-build86657.md`
- `/mnt/g/DestinyResearch/tmp/glimmer-active-sequence-command-list-build86657.md`
- `/mnt/g/DestinyResearch/tmp/glimmer-defender-probe-retraction-build86657.md`
- `/mnt/g/DestinyResearch/tmp/sobject-final-request-correlation-build86657-20260830.md`
- `/mnt/g/DestinyResearch/tmp/sobject-lower-function-build86657-20260830.md`
- `/mnt/g/DestinyResearch/tmp/allocator-bitmap-runtime-result-build86657-20260830.md`
- `/mnt/g/DestinyResearch/tmp/entity-slot-bind-handoff-build86657-20260831.md`
- `/mnt/g/DestinyResearch/tmp/user-eboot-sobject-audit.md`
- `/mnt/g/DestinyResearch/tmp/d2-preservation-snapshot-audit-20260830.md`
- `/mnt/g/DestinyResearch/tmp/glimmer-test-tools.md`
- `/mnt/g/sunrise/Sunrise/TROSTLAND_FALLEN_SPAWN_REPORT.md`

The compact evidence and testing index is `/mnt/g/DestinyResearch/reports/glimmer-extraction/README.md`.
