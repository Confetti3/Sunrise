# Script host architecture and recovery plan

## Goal

Provide Sunrise with an out-of-process C# host that can run deterministic authored activity logic
while the reverse-engineering work discovers the native protocol and entity plugs needed by the
target Destiny 2 build. The first useful outcome is not a guessed full mission. It is a host that
stops at the exact missing capability and records what must be recovered next.

## Evidence boundary

The client contains scenario data, activity/session state, spawn-set catalogs, component factories,
and consumers for multiple activity messages. The distributed build does not include the original
host-side scenario policy, enemy encounter policy, or Bungie's authoring source. References to
BungieScript do not establish a recoverable VM in this client. Therefore:

- runtime-extracted hashes, package relationships, descriptors, and verified wire layouts are
  recovered data;
- mission sequencing, encounter composition, AI policy, and fallback behavior are authored policy;
- the included Red War file is always labeled `authored-prototype`;
- no copyrighted activity tables or original game script content are committed.

## Process split

```text
Destiny 2 process                         Sunrise.ScriptHost.exe
-----------------                        ----------------------
Sunrise State WorldPhase  ----pipe---->  event normalizer
optional native adapters   <---pipe---->  command broker
no retained game pointers                deterministic scenario graph
                                          checkpoints and variables
                                          capability manifest
                                          managed policy plugins
```

The native bridge runs on a dedicated worker thread. Game and hook threads never wait for the C#
process. Disconnects only remove bridge-provided capabilities; they do not demote Sunrise's normal
boot or destination support.

## Protocol v1

The pipe uses one UTF-8 JSON object per line. Current messages:

- `bridge.hello`: bridge/build identity and actually available capabilities;
- `world.phase`: `idle`, `transitioning`, or `arrived`, plus transition age;
- `activity.incident`: copied scalars from one validated client msg-19 incident;
- `host.capabilities`: asks the bridge to repeat its hello;
- `host.ping` / `bridge.pong`: liveness;
- `command.request`: request id, capability id, and JSON payload;
- `command.result`: `ok`, `unsupported`, `error`, or another explicit status.

The bridge currently advertises `host.ping`, `world.phase.observe`, and
`activity.incident.observe`. It returns `unsupported` for every mutation request. This is
intentional and testable.

World phase comes from the client’s build-specific boot-step accessor. The native detour copies the
returned integer on the engine call path, maps loading steps 33-37 and in-world step 38, and stores
only an atomic enum plus transition tick. This avoids retaining the decrypted boot-flow manager.
The first step-38 edge also invokes the already-localized world-transition fade channel once; the
operation is re-armed only after the client leaves a destination.

The managed server reads the native hello and initial phase before it writes anything. Windows
zero-buffer named pipes can otherwise deadlock when both peers synchronously write their startup
messages before either peer reads.

Incident observations use a fixed 64-row native queue. The activity-message route uses a
nonblocking lock attempt and never waits for C#. Overflow drops the oldest copy. Reconnect discards
pre-connection rows so an old incident cannot advance a newly restarted mission. Build 86657
exercised a fresh Tower target-1121 event from the native route through managed consumption.

## AI spawning decomposition

Enemy spawning is not equivalent to selecting a player spawn point. A complete adapter needs all of
the following before `entity.spawn` can be advertised:

1. **Definition binding** — identify the actor/archetype definition and every package it requires.
2. **Placement binding** — resolve a bubble-local point, transform, or placed SObject from
   runtime-extracted content.
3. **Authority and identity** — acquire an activity entity slot/lease and establish the host as the
   authoritative owner.
4. **Creation funnel** — emit the exact creation/baseline state expected by the client before any
   delta update.
5. **Simulation binding** — connect the object, physics, navigation, and AI components without
   bypassing their normal lifecycle.
6. **Replication updates** — provide movement, health, combat, and incident state in the correct
   ordering domain.
7. **Removal** — destroy the object, release all component/SObject state, and return the entity
   lease on death, unload, retry, or host shutdown.
8. **Validation** — spawn, damage, death, destination unload, restart, and repeated encounter tests
   must pass without retained gameplay pointers or leaked entity slots.

The existing spawn-set catalog remains useful for placement discovery and bubble/package filtering,
but does not prove actor construction or AI ownership.

## Mission/activity behavior decomposition

A minimal playable mission slice needs separate adapters for:

- activity/session snapshot and current destination identity;
- incident emission with recovered definition-index schema and target ordering;
- objective and gameplay-switch mutation;
- placed-content/bubble authority;
- actor lifecycle and health/death observation;
- dialogue/cinematic invocation;
- completion and retry behavior.

Do not combine these into one signature-heavy hook. Each adapter gets an independent capability,
lifecycle, build binding, and failure report, following the same local-failure principle used by the
PR #8 noclip module.

## Recommended recovery order

1. Exercise the copied activity-incident observation probe in a loaded destination and identify a
   stable target.
2. Trace one client-visible objective consumer backward to a verified host message/state change.
3. Add a copied, read-only activity/session snapshot to associate later events with a destination.
4. Capture and replay one harmless typed incident only after its target/payload semantics are known.
5. Prove placed-content authority for one known local object.
6. Recover entity allocation plus a no-AI static actor baseline and clean removal.
7. Add health/death state and a deterministic stationary target.
8. Add navigation/combat policy as authored host behavior.
9. Bind dialogue and mission transitions only after the underlying event/state adapters are stable.

Each step should add one capability to the hello only after runtime validation on the target build.

## VM workflow

The host probes only roots explicitly provided on the command line. It writes `probe-report.json`
with discovered paths, package count, and optional binary hashes. Keep per-build native findings in
a new binding manifest; never silently reuse offsets or schemas after the executable hash changes.

For native work, keep the game closed while replacing `steam_api64.dll`, archive the previous DLL,
record the tested hash, and test disconnect/reconnect, orbit transitions, destination load, death,
and process shutdown. A successful build is not runtime validation.
