# Script-host native capability map (build 86657)

This map separates implemented transport, runtime probes, and hypotheses. `available` means the
bridge advertises the capability. `probe required` means code exists but has not been exercised in
Destiny 2 build 86657. No row infers Bungie's absent host policy from client package metadata.

| C# capability | Sunrise state | Request / push | Native consumer or producer | Runtime evidence | Confidence / status |
| --- | --- | --- | --- | --- | --- |
| `host.ping` | none | pipe `host.ping` / `bridge.pong` | `client/script_host/runtime.h` | Deterministic bridge protocol | high / available |
| `world.phase.observe` | atomic `state::activity::WorldPhase` and transition tick | pipe `world.phase` | detoured build-86657 boot-step accessor maps steps 33-37 to transitioning and step 38 to arrived | A fresh Tower run emitted step 33, then step 38 after physics join; C# consumed arrived, advanced to `open-objective`, and the one-shot fade release produced a rendered Courtyard view | high / available |
| `activity.session.observe` | four copied `SessionRecord` values exist behind the State lock | none | `state/activity/activity_session_lookup.cpp` only exposes `contains` and `is_joined` | No stable copied session snapshot ABI | high / probe required |
| `activity.incident.observe` | bounded 64-row pointer-free queue; each row copies session/account scalars, target indices, flags, and payload length | client svc8 activity message type 19 -> native queue -> pipe `activity.incident` | `activity_message_route.cpp` -> `incident::validate` -> `state::activity::incidents` -> script-host worker | A fresh build-86657 Tower session accepted target 1121 with 5 extras and an 84-byte payload as sequence 2; the live C# host consumed it before disconnect. | high / available |
| `activity.incident.emit` | none | generic service-9 notification framing exists, but msg-19 selector/payload semantics and ordering are not recovered | client msg-19 handler table is only indirectly identified by the validator artifacts | No harmless target/payload has been replayed | low / wire adapter required |
| `objective.set` | no objective-specific Sunrise State found | generic service-9 notification framing and progression banks are not an objective binding | no objective-specific client consumer is localized in source | No definition-index, payload, state transition, or UI result is verified | low / wire adapter required |
| `gameplay-switch.set` | no gameplay-switch-specific Sunrise State found | unknown | no consumer localized | No wire or runtime evidence | low / wire adapter required |
| `entity.allocate` | 8192-bit lease mask per joined activity session | svc8 slot request -> svc9 slot notification | `state/activity/entity_slots` transaction code | Lease lifecycle is implemented, but it does not identify an actor or object | high for leases; low for actors / probe required |
| `entity.spawn` / `entity.update` / `entity.destroy` | no actor lifecycle State | unknown create/baseline/update/remove messages | no SObject, actor baseline, health, damage, death, or removal funnel localized | Spawn-set catalogs contain placement/package provenance only | low / probe required |
| `placed-content.authority` | bubble-authority tokens and membership state | svc9 roster/sensor-authority pushes | client bubble-authority decoder patch | Authority grant exists; placed-object activation has not been demonstrated for this feature | medium / client patch required |
| `dialogue.play` / `cinematic.play` | none | unknown | no verified host invocation | Package/client component presence is insufficient | low / probe required |
| `mission.sequence` | managed checkpoint and scenario graph | authored JSON policy | `MissionRuntime` | Deterministic self-test only; original host policy is not shipped | high / authored policy |
| `enemy.ai-policy` | none | unknown | no recovered authoritative AI directive path | Client packages do not prove host encounter policy | low / authored policy |

## Incident probe lifecycle

- The encrypted activity route validates the complete recoverable msg-19 shape before publication.
- The producer uses `TryAcquireSRWLockExclusive`; a game/network route never waits for C#.
- Queue overflow replaces the oldest row and increments a monotonic drop counter.
- A new pipe connection discards pre-connection rows so a restarted host cannot advance a mission
  from a stale incident.
- The pipe copies JSON scalars and arrays only. It exposes no State, packet, or gameplay pointer.
- Build 86657 has emitted a validated incident into the native queue. The reconnect test discarded
  that pre-connection row and incremented the drop counter as designed.
- A fresh Tower Courtyard launch reached `idle -> transitioning`, completed its 62-second initial
  slice load despite the nonfatal prologue-filler timeout, entered `activity:in_world`, and emitted
  target 1121. The connected C# host consumed it as sequence 2.
- A separate named-pipe integration test consumed the same target/scalar shape in C# and preserved
  the mission checkpoint boundary. It is protocol evidence, not a substitute for a fresh in-game
  end-to-end event.
- `activity.incident.observe` is advertised because the target-build producer, queue, pipe, and
  managed consumer have now been exercised in one fresh session.

## World-phase lifecycle

- The build-specific accessor decrypts the boot-flow manager and returns its copied integer step;
  no manager or world pointer crosses the hook boundary.
- The detour observes engine calls on the game thread. It maps steps 33 through 37 to
  `transitioning`, step 38 (`activity:in_world`) to `arrived`, and other steps to `idle`.
- The previous spawn-gate-only polling stopped before the first step-38 observation. A fresh
  build-86657 Tower run proved the accessor detour at steps 33 and 38.
- The step-38 edge invokes the existing narrow fade-channel operation once per load. Runtime logs
  recorded channel `0x57572DAC`, and the client rendered the Hunter in Tower Courtyard instead of
  remaining behind the black transition fade.
- The connected C# host consumed `arrived`, ran the destination-arrival action, and persisted
  `open-objective` with `objective.set` as the explicit unavailable boundary.

## Objective hypothesis boundary

Service 9 can carry activity notifications, and msg 19 is bidirectional, but neither fact proves
that an objective is a msg-19 target or that a progression row drives the objective UI. The next
objective experiment must start at a client-visible objective consumer or a captured retail-like
event, identify its definition/runtime index and state ordering, and then add one narrow host-side
adapter. Until that evidence exists, `objective.set` must stay unavailable.
