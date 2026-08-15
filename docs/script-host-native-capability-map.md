# Script-host native capability map (build 86657)

This map separates implemented transport, runtime probes, and hypotheses. `available` means the
bridge advertises the capability. `probe required` means code exists but has not been exercised in
Destiny 2 build 86657. No row infers Bungie's absent host policy from client package metadata.

| C# capability | Sunrise state | Request / push | Native consumer or producer | Runtime evidence | Confidence / status |
| --- | --- | --- | --- | --- | --- |
| `host.ping` | none | pipe `host.ping` / `bridge.pong` | `client/script_host/runtime.h` | Deterministic bridge protocol | high / available |
| `world.phase.observe` | atomic `state::activity::WorldPhase` and transition tick | pipe `world.phase` | detoured build-86657 boot-step accessor maps steps 33-37 to transitioning and step 38 to arrived | A fresh Tower run emitted step 33, then step 38 after physics join; C# consumed arrived, advanced to `open-objective`, and the one-shot fade release produced a rendered Courtyard view | high / available |
| `activity.session.observe` | four copied `SessionRecord` values exist behind the State lock | none | `state/activity/activity_session_lookup.cpp` only exposes `contains` and `is_joined` | No stable copied session snapshot ABI | high / probe required |
| `activity.incident.observe` | bounded 64-row pointer-free queue; each row copies session/account scalars, target indices, flags, and payload length | client svc8 activity message type 19 -> native queue -> pipe `activity.incident` | build-86657 source `+0xD82730` -> retail encoder -> `activity_message_route.cpp` -> `incident::validate` -> `state::activity::incidents` -> script-host worker | A Tower control correlated one client source call containing target 1121 plus five ordered extras with the immediately accepted server incident. | high / available |
| `activity.incident.emit` | none | generic service-9 notification framing exists, but msg-19 selector/payload semantics and ordering are not recovered | client msg-19 handler table is only indirectly identified by the validator artifacts | No harmless target/payload has been replayed | low / wire adapter required |
| `objective.set` | native objective expressions consume an activity-state switch bank followed by progression rows; no writable Sunrise mirror exists | generic service-9 notification framing and progression banks are not yet a verified objective binding | definition resolver `+0xC923A0`, completion reader `+0x5269D0`, progress reader `+0x523F30`, retained-row dispatcher `+0x540320`, typed list applier `+0x52FD60`, and switch writer `+0x555EC0` are localized; their transport remains unwired | Original Red War `mission_towerfall` reached `phase=arrived`; definition lookups remained pre-selection, and the typed list applier and writer both recorded zero calls through arrival | high for read/application path; low / wire adapter required |
| `gameplay-switch.set` | native activity state begins with `0x5BCC` one-byte switch states; no writable Sunrise mirror exists | unknown | expression leaf `+0x5549C0` treats state value `2` as true; helper `+0x54BDF0` applies switches from definition-backed source rows; `+0x52FD60` supplies typed rows to definition-backed writer `+0x555EC0` | Fresh `mission_towerfall` launch-through-arrival control recorded zero list-applier and writer calls; no wire producer, reliable command drain, or harmless state transition is verified | high for writer identity; low / wire adapter required |
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
- A read-only observer now covers the exact build-86657 client source at RVA `+0xD82730`. The
  unique signature and offline disassembly establish a target count at source offset zero, target
  ids at `+0x0C` with `0x10` stride, and a pointer at `+0x420` to the fixed `0x400`-byte body copied
  into the retail `0x510`-byte request. The hook retains bounded scalars and a body hash only while
  the source call owns them; it never retains native pointers or changes the request.
- In the rendered Tower control, caller return RVA `+0x4AA286` supplied six ordered targets
  `1121,7117,3904,4913,850,2323`. The source body began
  `5E40AE5B,00000000,00000001,00000000`. Immediately afterward, Sunrise accepted msg 19 with
  primary target `1121`, five extras, and an 84-byte payload. This joins the client producer,
  retail encoder, server parser, queue, and managed observation path without guessing a payload.
- A rendered EDZ control emitted the same six targets as Tower, while the source body changed.
  This falsifies the apparent match between target `1121` and one Tower-related static
  activity-logic cluster: the target list is not an activity/objective identity. A follow-up Tower
  run decoded the first four application-payload words after the bit-packed msg-19 header. Its
  first word `3FBDF40B` exactly matched source-body word zero, proving that the source's fixed body
  becomes the msg-19 application payload. The remaining words are still opaque and no replay is
  authorized from this correlation alone.
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
that an objective is a msg-19 target or that a progression row drives the objective UI. An exact
build-87221 quest-reader dependency was ported to build 86657: objective-definition resolver
`+0xC90980` became `+0xC923A0` with an instruction-identical body apart from relative calls. Its
long signature matches the reconstructed executable exactly once.

A read-only detour at that resolver survived sign-in, orbit, Quests, Director, an 80-second Tower
load, and rendered Courtyard. It observed real definition indices without retaining native
pointers or changing the output pair. Account/orbit consumers at callers `+0xFA0F41`,
`+0x13E819D`, and `+0x13E7D99` resolved `1163`, `1193`, and `1361..1363`. The Quests/Director
consumer at `+0xDDD4DA` resolved `2914`, `2915..2917`, `2920..2922`, and `2929..2933`. Quests
visibly contained `0 / 0`, so these are definition queries, not proof of active quest state.

The corresponding build-87221 completion and progress readers also port exactly to build 86657 at
`+0x5269D0` and `+0x523F30`. The progress call has five arguments: output, owner, definition,
context, and activity state. Both readers evaluate the objective definition rather than reading a
standalone objective value. Their shared expression evaluator at `+0x554310` can consume literals,
nested expressions, gameplay switches, and progression values. Switch leaf `+0x5549C0` reads one
of `0x5BCC` state bytes and considers value `2` true. Progression leaf `+0x554B90` reads from the
following bank of `0x3C8C` rows, whose observed layout is a present byte plus a dword at `+4` in an
eight-byte row. Completion also consumes definition flags at `+0x28/+0x2C`, while progress returns
the current scalar and the definition stores its maximum at `+0x30`.

Static application helpers narrow the write side without proving a transport. `+0x540C90` applies
one progression row using definition-selected set/min/max/add behavior; `+0x54BDF0` applies switch
state `2` from definition-backed source rows; `+0x54BEE0` applies their progression indices; and
`+0x54D730` invokes the two application paths for three source sets. This makes an activity/global
state update the leading adapter boundary, but its source-row wire schema, Homecoming indices, and
safe host transition remain unverified. A real state change must be correlated before any narrow
writer is added, so `objective.set` stays unavailable.

A paired read-only detour subsequently attached to both build-86657 readers and survived sign-in,
orbit, Director, and the visibly empty Quests page. Quests invoked the completion reader five times
from return RVA `+0xFA0F68`, producing definition hashes `6C1733AB`, `2F76637B`, and
`93CDAE72..93CDAE70`. Their maxima were `1`, `1`, and `1000` for the remaining three; every
expression evaluated false with a non-null context. No progress read fired in the empty-page
control. This validates the completion ABI and expression-state consumer without manufacturing a
state transition. The Homecoming definition/bank mapping and write-side source remain unknown, so
the capability status does not change.

The source side of that application path is now bounded as well. Category dispatcher `+0x540320`
is called by the activity-state rebuild and accepts a retained source object. Category zero reads
an explicit switch-row count at source `+0xB3F4`, up to 20 four-byte rows at `+0xB3F8`
(`int16 index`, `uint8 value`, `uint8 auxiliary`), a progression-row count at `+0xB448`, and
eight-byte progression rows at `+0xB44C` (`int16 index`, `uint16 auxiliary`, `uint32 value`). Its
return value is ignored at all five direct call sites. A bounded read-only observer copied only
these scalars while the call owned the source and passed the native call through unchanged.

The observer attached in both an orbit control and an authoritative Homecoming launch. Calls from
return RVAs `+0x5492F1` and `+0x54DA47` had `enabled=1` but zero switch and progression rows. The
Homecoming client nevertheless reached `activity:in_world`, and the C# host advanced to the honest
`objective.set` boundary. No new category-zero rows appeared during activity transition or arrival.
This rules out the current 189-byte type-1 global-state payload as a source of explicit retained
rows in this run; it does not establish the missing wire fields or authorize writing the native
source object. Evidence is retained under
`analysis/script-host-runtime/20260815-034100-activity-state-source-homecoming/`; the exercised DLL
SHA-256 is `D5714B55317EB4FAE657876BB6E0D2769A3EFCF92B6413A8DE8C14A616D1DAED`.

Tracing the rebuild's arguments corrects the source ownership boundary. `+0x5084C0` obtains the
source passed through `+0x54D730` to the category dispatcher from activity-owner virtual method
`+0x18`, not method `+0x38`. The concrete owner vtable is at `+0x1C35DC8`; its `+0x18` method
(`+0x15A3C10`) returns owner field `+0x18`, which initialization at `+0xFA7600` fills from the
registry path rooted at `+0xBE1CB0`.

A follow-up bounded identity snapshot reached real orbit and observed the same raw source prefix
at both dispatcher callers: `00100101,9EAA3001,00010000,00000000`. Its first pointer-sized value
is not a main-image vtable, and the full 16-byte prefix is not present literally in the
reconstructed executable. The structure is therefore consistent with decoded/runtime state, not
a polymorphic object or a static package record. Both row counts remained zero. Evidence is in
`analysis/script-host-runtime/20260815-040931-activity-state-source-identity-orbit/`; the exact DLL
SHA-256 is `FB5882E6F2C30360BC9E7E2FBD4E648A256200965C0F75801DC556D73489A5EA`.

The same registry source has a persistent switch-state bank at `+0x9348`: direct readers at
`+0xE06090` and `+0xE08D40` test its bytes for state `2`. Generic post-decode dispatcher
`+0xE05DA0` routes category 4/subtype 1 to comparison callback `+0xE07BA0`, which compares old and
new source blobs and performs a 4 KiB comparison of that bank. A second read-only detour attached
to this unique callback alongside the retained-row observer. It survived sign-in, real orbit, a
genuine Courtyard hold/launch, and `activity:in_world` in `nadir_endgame`, but never fired. The
downstream `+0x540320` observer did fire with zero rows. This excludes that post-decode callback
from the current type-1 offline update path; it does not identify a writer. Evidence and the exact
DLL are in `analysis/script-host-runtime/20260815-043300-activity-state-postdecode-control/`; DLL
SHA-256 is `0064B5F7B3502D254717979664E82C543F8E6E36D303F2CB6BB7509A4FEE9108`.

Instrumenting the generic dispatcher itself resolves that negative control. A fresh orbit decode
called `+0xE05DA0` from return RVA `+0xE018FA` with category `4`, index `0`, and matching argument
prefixes `00100100,9EAA3001`; the retained source seen immediately afterward at `+0x540320`
remained `00100101,9EAA3001`. Category 4/index 0 jumps to comparator `+0xE078F0`, not the
index-1 comparator `+0xE07BA0`. Static inspection shows that the index-0 object is at least
`0x175E4` bytes and its comparator visits fields at `+0x728`, `+0x1108`, `+0x68B0`, `+0x6978`,
`+0x6C3C`, `+0x742C`, `+0x1249C`, and `+0x17508`; it does not compare the index-1 `+0x9348`
switch bank. The different low-byte identity and disjoint layouts prevent treating this orbit
decode as a retained activity-state update. Evidence and the exact DLL are in
`analysis/script-host-runtime/20260815-044100-decode-dispatch-orbit/`; DLL SHA-256 is
`A6B02EA7A5871C16925A4D1226667073AAB472ADCFE85F4DB5CD7E0363EFA4F8`.

DestinyResearch graph correlation corrects the activity target: original Red War Homecoming is
`mission_towerfall` (root `80B500AC`, scenario `80B500BC`), while `arcade_homecoming` is a later
copy. Build 86657 resolved `mission_towerfall` uniquely at activity index `266`. A fresh genuine
Launch hold selected that destination server-side, changed the client world to
`mission_towerfall`, observed step 38/`phase=arrived`, and advanced the host to `open-objective`.
The run contained 60 objective-definition lookups, all before activity selection, and zero calls
to the unique definition-backed switch writer at `+0x555EC0`. No post-selection objective lookup
occurred. This proves the corrected activity runtime without manufacturing an objective
transition. Evidence is in
`analysis/script-host-runtime/20260815-052700-mission-towerfall-runtime/`; exercised DLL SHA-256
`6F9FC7AA3EA5456786FF8C01B39983F2C630A4BFC35C71A449C0539724082AD3`.

The unique switch writer's direct callers include `+0x52FD60`, a native list applier whose
four-byte switch rows are `uint16 definition`, `int8 requested`, and `uint8 auxiliary`. It owns a
valid state-owner argument on its calling thread, but only for that event-driven invocation. A
bounded read-only detour at this entry completed another genuine `mission_towerfall` launch and
first-person Tower North arrival while recording zero list-applier calls and zero writer calls.
It therefore cannot supply the missing startup transition or a reliable general command-drain
safe point. Evidence is in
`analysis/script-host-runtime/20260815-060353-apply-lists-towerfall-runtime/`; exercised DLL
SHA-256 `BA0768FEA0BF909338DB2E499F68F3E3A8A3AE7E1E5DC7FDE65178C5996F172B`.

DestinyResearch identifies four original objective-group payloads under resource `80B508F4` and
their named later-copy counterparts. Their serialized low ordinals (`0..3` original versus
`0x3D..0x41` arcade) join the package copies but are not typed as the writer's definition input.
Likewise, the six trailing relative pointers are internal component links, not external graph
edges. These fields remain discriminators until a native consumer proves their runtime meaning.

A bounded live classification of the writer's registry base table now separates those serialized
references from writable definitions. Original `mission_towerfall` candidates `0x19D..0x1A2`
resolve to type `0`, bank index `-1`, and are ignored by the observed `+0x555EC0` writer path. In
the later `arcade_homecoming` copy, `0xF3..0xF5` are likewise type `0`, but `0xF6` resolves to
type `3`, retained bank index `1`. This materially narrows the first real transition target without
making it available: a valid transient activity owner, read-before-write observation, and bounded
game-thread command drain are still required. The classification is explicitly for the base table;
no active hot-patch overlay was observed. Evidence is in
`analysis/script-host-runtime/20260815-063239-definition-candidates-orbit/`; exercised DLL SHA-256
`E7CAC8A88B984B9916507B4567213DC171BC9DACEA4AB0A7BD1EF3E27545B9E1`.
