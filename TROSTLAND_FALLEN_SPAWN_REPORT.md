# Trostland Fallen Spawn Research Report

Status: reproduced successfully on PC build 86657 on 2026-08-28.

## Result

Sunrise activated an authentic Trostland Fallen encounter from a Lua-authored mission. The game created the enemies through its native activity and AI-spawner path; no enemy object was constructed directly and no client-side spawn function was called.

The live acceptance run produced all three required kinds of evidence:

- Sunrise compiled `Sunrise/missions/edz_freeroam.lua`, accepted an injected edge for the authored trigger, advanced `reach_trostland` from state 1 to state 2, and reserved wave `fallen_multipoint_core`.
- The build-86657 client decoded the authored component state as two requested-member slots `{1,0}`, generation 4, active true, and mode 0. Its native request builder then consumed that state.
- A hostile Fallen group appeared at the content-authored roadside placement. The player observed it, damaged it, and killed every enemy.

The important distinction is that `{1,0}` is a requested-member state, not a literal request for one enemy. The referenced squad and spawner content determine the resulting formation.

## What was activated

The working slice is deliberately restricted to one measured build and encounter:

| Field | Confirmed value |
| --- | --- |
| Destination | `edz_freeroam` |
| Bubble | 56 |
| Slice set | 448 |
| Spawner definition | `0x80C26B0A` |
| Activity group | `0x2986181D` |
| Spawner component | type 1, slot 271 |
| Squad reference | group `0x2986181D`, type 66, slot 565 |
| Requested members | `{1,0}` |
| Reserve members | `{0,0}` |
| Active / mode | `true` / `0` |
| Observed generation | 3 |
| Authored generation | 4 |
| Spawner auth schema | `0x80807EC9` |
| Client observation schema | `0x80807ECC` |

## How the path works

1. At world open, the restricted Lua compiler loads `edz_freeroam.lua`, validates its bounded mission graph, hashes the resulting native `MissionProgram`, and destroys the Lua VM.
2. A `trigger_enter` event for `trostland_wave_volume` executes two native actions: change the objective state and activate `fallen_multipoint_core`.
3. `CompiledMissionPolicy` emits an `EnemyWaveIntent`. The Activity Host queue accepts only the confirmed Trostland tuple: spawner `0x80C26B0A`, mode 0, requested state `{1,0}`.
4. The type-5 activity writer locates group `0x2986181D`, type 1, slot 271 and writes a full server-authored spawner state under schema `0x80807EC9`.
5. The client resolves the explicit squad reference, commits the decoded 0xC4-byte state to the native spawner instance, and builds its normal content-backed AI requests.
6. The intent remains reserved across sends. Sunrise settles it only after the native probe observes request construction with the expected `{1,0}` state, preventing a discarded frame from falsely completing the mission action.

This leaves Lua outside the protocol, physics, and native object layers. Lua describes the mission; fixed native code owns validation, state, transport, and execution.

## The wire-format correction that made it work

The unsuccessful approach treated the client's type-6 spawner report as a state body that could be reflected back through type 5. The captured type-6 value contained generation 3 and the 30-bit delta `0x220C3124`, but replaying it only changed a sense/acknowledgement mirror. It never constructed authoritative spawner state.

Disassembly of build 86657 showed that the native spawner apply function receives a `{schema,pointer}` wrapper, resolves it, and copies a decoded 0xC4-byte object into the spawner instance. Static schema records then identified two different contracts:

- `0x80807ECC`: the A4-byte client observation/update schema used by type 6.
- `0x80807EC9`: the C4-byte server-authored state schema required by the type-5 auth block.

The fix was to keep type 6 observation-only and encode the complete `0x80807EC9` state in the type-5 auth half. The accepted 278-bit component body is defined exactly as the following field sequence; signed i32 values use the build's `+0x80000000` wire bias:

```text
key_a_present:1=0
key_b_present:1=0
pad_list_present:1=0
requested_present:1=1
requested_count:4=2
requested_0:32=0x80000001
requested_1:32=0x80000000
reserve_present:1=1
reserve_count:4=2
reserve_0:32=0x80000000
reserve_1:32=0x80000000
flag_record_present:1=0
generation_present:1=1
generation:31=4
unknown_or_target_presence:4=0,0,0,0
squad_present:1=1
squad_group:32=0x2986181D
squad_type:7=67          # decoded type 66, bias 1
squad_slot:16=0x8235    # decoded slot 565, bias 0x8000
remaining_optional_presence:6=0,0,0,0,0,0
active:2=2              # decoded true, bias 1
mode:3=1                # decoded mode 0, bias 1
name_present:1=1
name_hash:32=0x811C9DC5
```

The containing object block has auth reset 1, auth root 1, a remainder of 281 bits, and a zero type-5 sense-presence bit. The complete accepted 6,361-byte type-5 body was captured in the runtime log as `ev=spawner_probe stage=auth_wire` before encryption.

## Live evidence

The successful run recorded this sequence:

```text
ev=mission stage=compile result=ok destination=edz_freeroam hash=0x0023F52615D08BC5
ev=mission stage=trigger_inject result=accepted request=1 trigger=0xE54C5271E2B0A821
ev=mission stage=wave_queue result=reserved ... spawner=0x80C26B0A ticket=1
ev=squad_reference_probe stage=decoded_state ... count=2 requested=1,0 generation=4 active=1 mode=0
ev=squad_reference_probe stage=build_requests ... requested=1,0 produced=1
ev=mission stage=wave_settle result=confirmed ... ticket=1 requests=18
```

The runtime console independently reported:

```text
objective_0=2
activated_waves=1
decoded_slot_count=2
decoded_requested_0=1
decoded_requested_1=0
decoded_generation=4
decoded_active=true
decoded_mode=0
```

The final visual and gameplay check was performed by the user: the Fallen group was present, hostile, damageable, and could be killed normally.

## Relevant implementation

- Mission sample: `Sunrise/missions/edz_freeroam.lua`
- Lua compiler and native runtime: `Sunrise/src/server/gameplay/mission/`
- Intent-to-Activity-Host adapter: `Sunrise/src/server/bap/encrypted/push/activity/activity_roster_push.cpp`
- Type-5 auth encoder: `Sunrise/src/middleware/bap/activity_message/activity_sensor_auth_bodies.cpp`
- Type-6 observation parser: `Sunrise/src/middleware/bap/activity_message/activity_sense_update_parser.cpp`
- Native acceptance probe: `Sunrise/src/client/hooks/squad_reference_probe/squad_reference_probe.cpp`
- Golden field test: `tests/mission_sensor_auth_test.cpp`

## Reproduction and verification

Enable only the explicit research gates in a local settings file:

```json
{
  "server": {
    "console_endpoint": { "enabled": true, "port": 30975 },
    "activation": {
      "gameplay_external_body": true,
      "trostland_spawner_probe": true,
      "mission_script_host": true,
      "trostland_mission_preset": true
    }
  }
}
```

Load build 86657 into the configured Trostland destination. Entering the authored proximity volume is the intended mission path. The successful acceptance run used the research console to inject the equivalent edge:

```text
mission.trigger trostland_wave_volume
```

Inspect `mission.status` and `mission.spawner_status` for the values listed above. The focused regression checks are:

```powershell
tests/run_mission_compiler_test.ps1
tests/run_compiled_mission_policy_test.ps1
tests/run_enemy_wave_queue_test.ps1
tests/run_mission_sensor_auth_test.ps1
tests/run_scenario_slot_classification_test.ps1
```

Debug and Release x64 builds passed, as did `git diff --check`.

## Natural-trigger follow-up

Static analysis after the accepted spawn run established the player-to-host identity chain but not yet a safe pose decoder:

- A bound gameplay view opens an external scheduler body after the two reliable queues. Its four lanes decode in fixed order; channel 2 is the entity stream.
- Type 2 `player_broadcast` uses baseline schema `0x80806ABD` and update schema `0x80806B1A`. The update is a 20-byte reflected structure behind one presence bit.
- Its literal `player` field is a type-25 runtime union. Build 86657 uses selector 18, a nullable 17-bit entity token, to identify the player's type-0 SObject.
- Position is not in `player_broadcast`. It is in the referenced SObject's `transform` component, root `0x80809F75`, followed by `parent`, `stream-source`, and every component selected by the live SObject definition.
- An ordinary type-0 update has no enclosing payload length. The selected component tail is sequential and has no unknown-field skip or resynchronization point. The Guardian RSAT and its live reflected component layout have not been identified statically. Reading only the known transform prefix would therefore be a guessed payload and is intentionally refused.

The local build now records at most eight authenticated, view-bound external scheduler bodies as opaque MSB-first bits under `ev=gameplay stage=external_observation mode=observation_only`. It does not decode or apply those bytes. The next live run can identify the Guardian RSAT/component layout from the authentic outbound body without replacing the path with a camera hook or debug movement injection.

### Measured Trostland volume

The build-86657 schema-4 Activity Logic cache contains four strong `fallen_multipoint_core` dropship placements under map table `0x80BE7AA0`:

```text
(341.954956, 379.310669, 84.226868)
(335.504883, 463.457520, 72.993042)
(416.611237, 420.238586, 82.190857)
(480.704193, 348.558594, 79.899902)
```

Their axis-aligned bounding-box center is `(408.104538, 406.008057, 78.609955)`. `edz_freeroam.lua` now uses that center with half-extents `(75, 60, 10)`. This is a measured spatial envelope associated with the encounter, not yet proof that the player can naturally enter it. The separate build-data cache proves that `edz_freeroam` bubble 56 publishes group `0x2986181D` from source object `0x80C26607`, including type 1 / slot 271 and type 66 / slot 565.

The cache inspection also found non-spatial `WorldID = UINT64_MAX` rows duplicated across every entity. Those rows are now rejected during collection, suppressed while loading existing schema-4 shards, and defensively excluded from graph materialization so they cannot pull the derived volume toward the origin.

### Same-session hygiene and validation

Mission reload now consumes a pre-reload research trigger request, clears published mission status, closes the old world, and recompiles/reopens it in the same admitted session. Full reset clears reload, trigger, and mission status atomics after the worker has joined and all worlds have closed. This rearms the native mission policy and trigger generations; authoritative native spawner deactivation remains unresolved and is not synthesized from an unmeasured payload.

The focused compiler, compiled-policy, enemy-wave queue, sensor-auth, scenario-slot, and Activity Logic cache tests pass. Debug x64 and Release x64 both build with zero warnings and zero errors, and `git diff --check` passes. The verified Release DLL/PDB and measured Lua script were deployed locally with `20260828-1935-static-safe` backups.

A live build-86657 run then confirmed two successful activations in the same Destiny process. The first world compiled hash `0x4C585291A4B509AA`, accepted research trigger request 1, reserved ticket 1, decoded generation 4 / requested `{1,0}` / active true / mode 0, and settled after 18 native request-builder calls. A later world in the same process accepted request 2, reserved ticket 2, decoded the same authoritative state, and settled after 54 observed request-builder calls. The user confirmed the encounter worked. Both edges were `stage=trigger_inject`, so this proves repeat/rearm and the native spawner path, not natural proximity. No `external_observation` body was emitted during the run; the authentic player-pose body remains unavailable. The preserved log is `G:\sunrise\analysis\sunrise.trostland-static-safe-live-20260828-195645.log`.

## Boundaries

This proves one authentic content-backed encounter on PC build 86657. It does not establish a generalized spawner schema, another destination, or compatibility with another client build. Unsupported tuples and schema mismatches fail closed. Both `mission_script_host` and `trostland_spawner_probe` remain disabled by default, and enabling the mission host does not implicitly enable unrelated experimental gates.
