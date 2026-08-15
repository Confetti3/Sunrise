# Homecoming activity/runtime evidence

This note joins the extracted Season of Arrivals activity graph with build-86657 runtime evidence.
It does not treat name-affinity graph edges as execution order. Runtime resolution now establishes
the registered build-86657 client indices without deriving them from static scenario tags.

## Static activity identity

The `DestinyResearch/reports/activity-logic` corpus identifies Homecoming directly:

- root tag `80F02003`, class `80808AAE`;
- scenario-client tag `80F0200E`, class `80809994`;
- package `w64_arcade_homecoming_activities_0381_5.pkg`;
- destination `arcade_homecoming`;
- main logic resource `80F0236B`;
- initial location PRV hash `F28FC859`;
- location/developer hash `3E5560C0`, localized as `Red Legion Command Ship`.

The main resource contains 266 unique entity definitions. The complete activity archive contains
325. Its five objective definitions are exact entity-name mappings:

| Tag | FNV-1 name hash | Name |
| --- | --- | --- |
| `80F02150` | `2B84AF64` | `obj_ghaul` |
| `80F02153` | `CDC14918` | `obj_damaged` |
| `80F02156` | `77D1C18C` | `obj_deck` |
| `80F02159` | `F4C2A75B` | `obj_shield_gen` |
| `80F0215C` | `73047EBB` | `obj_deck_ultra` |

All five use class pair `80808348/8080835A`. These tags and hashes establish content identity;
they do not establish an `objective.set` wire payload or consumer.

Each objective's serialized `80809C36` resource has six pointers in its trailing resource-pointer
array. Direct extraction from the user's package copy resolves all six back into fixed offsets
inside that same objective payload (`+0xA0`, `+0xB8`, `+0xF8`, `+0x110`, `+0x170`, and `+0x1D0`).
The layout is identical across all five objectives. None is an external trigger/action reference,
so this initially promising static lead is eliminated rather than promoted into a guessed edge.

The graph also contains 41 serialized WorldID-to-placement links. Those are strong direct links.
For example, `spawnrule_boss01` (`80F0206F`) points to WorldID `CA7EB3A7D45895FE`, and
`d_ship_door` (`80F020D2`) points to WorldID `D3FCE3E3838A7339`. The 21 name-affinity flow edges
in the report are candidates only and must not be used as proof of encounter execution order.

The separate A1 activity graph gives one additional typed boundary: Homecoming root `80F02003`
points at `arcade_homecoming:scenario_client` tag `80F0200E` at payload offset `+0x40`, and at the
still-unresolved shared resource `80FDB97F` at `+0x44`. This is a root-to-scenario relationship,
not an objective activation path. The graph explicitly does not resolve destinations, bubbles,
phases, entities, or execution order.

## Build-86657 descriptor boundary

The client stores its active activity descriptor in a wrapper at controller offset `+0x8E08`.
The wrapper's assignment entry is RVA `+0x17AD610`; its payload begins at wrapper offset `+0x148`
and is `0x118` bytes. The observer signature uniquely matched this entry in the reconstructed
build-86657 image. It records only the source descriptor's byte zero and 16-bit fields at offsets
two and four, then calls the native assignment unchanged.

A fresh Courtyard launch with the server destination override set to Homecoming produced:

| Sequence | Caller return RVA | Type | Primary | Override | Boundary |
| ---: | ---: | ---: | ---: | ---: | --- |
| 1 | `+0xC19EAC` | 2 | 0 | 0 | orbit/default setup |
| 2 | `+0xC19EAC` | 0 | 20 | 20 | destination selection |
| 3 | `+0xEE8546` | 0 | 20 | 20 | pre-request replication/update |

The activity-host request then named activity `0x0014 city_tower_social_d2`. The server correctly
forced destination `arcade_homecoming`, and the client reported that its world changed to that
destination. At in-world arrival, however, the client still started activity
`0x0014 city_tower_social_d2`. The rendered result was a black screen with cursor glow and no
world, HUD, objective, navigation text, prompt, or error.

This proves that activity index zero is the orbit/default descriptor, not a resolved Homecoming
definition. It also moves the decisive mismatch earlier than the host request: Tower's descriptor
is assigned at `+0xC19EAC` and copied again at `+0xEE8546` before the request at `+0xC0F61C`.
Changing only the server destination or the request slot cannot make Homecoming authoritative.

## Native activity-definition resolution

The descriptor normalizer at build-86657 RVA `+0xBFDFA0` calls the activity-definition lookup at
RVA `+0xDDECA0`. The lookup accepts a 16-bit activity index, obtains the registered activity record
through the manager's virtual `+0x460` entry, and resolves the relative string pointer stored at
record offset `+0x68`. The signature includes that final offset so it does not collide with the
adjacent lookup for the record's `+0x70` field.

On the first proven normalizer call, Sunrise exhaustively queried the wire-valid namespace
`0..4094` through that native read-only lookup. Exact string comparison produced:

| Activity name | Registered indices |
| --- | --- |
| `city_tower_social_d2` | `20`, `84` |
| `arcade_homecoming` | `37`, `38` |
| `mission_ember` | `54`, `281` |

The duplicate names explain why an observed UI selection can use an alias other than the first
registered match. Sunrise retains the first exact Homecoming match (`37`) only after this scan;
it does not guess an index from the static graph tag.

## Authoritative Homecoming launch

When—and only when—the active script-host override exactly names `arcade_homecoming`, the
descriptor hook recognizes the type-zero Courtyard payload (`primary=20`, `override=20`) at the
proven `+0xC19EAC` normalizer boundary. It copies the full `0x118`-byte payload into local storage,
replaces both indices with the runtime-resolved Homecoming index, writes the fixed `0x28`-byte name
field as `arcade_homecoming`, and gives the copy to the native assignment. It does not mutate the
caller's source memory or alter unrelated descriptors.

The retained run recorded:

```text
ev=activity_definition stage=scan result=complete searched=4095 homecoming=37/2 tower=20/2 ember=54/2
ev=activity_descriptor stage=override from=20 to=37 name=arcade_homecoming
world_controller:activity_manager: 'PRIVATE CURRENT' activity client requesting activity host startup [activity: 0x0025 ... (grognok: arcade_homecoming) ... (override: arcade_homecoming)]
world_controller:state:in_world: Starting activity '0x0025 ... (grognok: arcade_homecoming) ... (override: arcade_homecoming)'.
```

This is the first retained run in which both the client's authoritative activity and override are
Homecoming. The descriptor still retained Courtyard's destination hash `0x13e02331`; the server
destination override independently selected `arcade_homecoming`. The client reached its in-world
activity-start boundary and the script host advanced from `wait-for-destination` to
`open-objective`, but the rendered client remained a black frame with cursor glow. The scenario
then failed honestly at `objective.set` with `WireAdapterRequired`. Activity identity and arrival
are therefore verified; objective/entity activation and usable world rendering are not.

## Incident-source control

The exact build-86657 client incident source at RVA `+0xD82730` was observed during both the
authoritative Homecoming launch and a rendered Tower control. Homecoming reached the black
in-world frame without invoking the source. Tower invoked it once from return RVA `+0x4AA286`
with primary target `1121` and five ordered extras; Sunrise immediately accepted the corresponding
msg-19 incident. This negative/positive control shows that the Homecoming failure occurs before a
client scenario produces this class of activity incident. It does not make target `1121` an
objective id or establish an objective wire format.

A rendered EDZ control later produced the identical six-target list as Tower while changing the
application body. Therefore those targets are not activity-specific objective identifiers. The
server parser now retains four decoded application-payload prefix words for bounded diagnostics;
a follow-up Tower run proved its first decoded word exactly equals the client source-body first
word. This establishes the source-to-wire payload boundary, but the payload's semantics remain
unresolved.

## Objective-definition consumer

The old build-87221 diagnostic quest reader at `+0x1058690` provided a concrete dependency chain,
not merely an objective-named string. Its objective-definition resolver at `+0xC90980` ports
instruction-for-instruction to build-86657 RVA `+0xC923A0`; the only byte differences are relative
call displacements. The build-86657 resolver takes a manager, a two-pointer output pair, and a
16-bit objective index. Its validated long signature matches the reconstructed executable once.

A bounded read-only observer at that resolver completed a fresh runtime control without modifying
the output. Orbit/account UI resolved indices `1163`, `1193`, and `1361..1363` from three callers.
Opening the visibly empty Quests page (`0 / 0`), then Director, resolved this separate set from
caller `+0xDDD4DA`: `2914`, `2915..2917`, `2920..2922`, and `2929..2933`. Each index returned stable
primary and secondary definition prefixes. The same process then launched Tower with a genuine
hold and rendered Courtyard normally, showing the observer is compatible with the complete load.
The retained log is
`analysis/script-host-runtime/20260815-025700-objective-definition-consumer/sunrise.log`.
After repeated Director polling exhausted the first run's row budget, the observer was tightened to
emit each caller/index pair once. An exact-final-binary smoke run reached orbit and the empty
Quests page again; it retained all 12 `+0xDDD4DA` indices in only 17 total rows. That log is
`analysis/script-host-runtime/20260815-031000-objective-definition-dedup-smoke/sunrise.log`.

These values prove a client-visible objective definition namespace and consumer boundary. They do
not map extraction order `0..4` or Homecoming tags `80F02150..80F0215C` to runtime indices, and the
empty Quests page proves that definition lookup alone is not active objective state. Consequently
this narrows the next work to the already-ported completion/progress readers and their state input;
it does not yet justify advertising `objective.set`.

## Objective state read/application path

The build-87221 completion reader at `+0x524E30` and progress reader at `+0x522390` port with
instruction-identical bodies (apart from relative calls) to build-86657 RVAs `+0x5269D0` and
`+0x523F30`. Unique long signatures match each new entry exactly once. The progress ABI includes a
fifth stack argument: output, owner, definition, context, and activity state. Its returned output's
first dword is current progress; definition `+0x30` is the maximum. Completion additionally reads
definition flags at `+0x28/+0x2C` and an expression at `+0x38`.

Both values are expression-derived. Inner evaluator `+0x554310` dispatches literals, nested
definitions, gameplay switches, progression values, boolean/comparison/arithmetic operators, and
FNV operations. Switch leaf `+0x5549C0` bounds its 16-bit index below `0x5BCC` and returns true when
the corresponding activity-state byte equals `2`. Progression leaf `+0x554B90` bounds its index
below `0x3C8C` and reads the bank following those switch bytes; observed rows are eight bytes with
a present byte and value dword at `+4`.

The matching application side is definition-driven. `+0x540C90` writes one progression row and
uses a definition type to select set/min/max/add behavior. `+0x54BDF0` resolves definition-backed
source rows and applies associated switches as state `2`; `+0x54BEE0` applies their direct and
derived progression targets, including a call to `+0x548980`; `+0x54D730` runs both paths for three
source sets. This is evidence for an activity/global-state source-row adapter, not permission to
invent one. No source wire shape or Homecoming objective-to-bank mapping is yet verified.

The paired observer then completed a fresh build-86657 runtime control. Both signatures attached,
the Hunter reached stable orbit, Director opened, and Quests visibly rendered `0 / 0` with the
empty-page message. Completion caller return RVA `+0xFA0F68` evaluated five definitions:
`6C1733AB` and `2F76637B` with maximum `1`, plus `93CDAE72`, `93CDAE71`, and `93CDAE70` with
maximum `1000`. All returned false with a non-null context. No progress invocation occurred during
the empty Quests control. This is positive ABI/consumer evidence and negative active-state
evidence; it neither identifies the Homecoming definitions nor supplies a mutation adapter.
The exact deployed DLL and retained log are in
`analysis/script-host-runtime/20260815-031801-objective-state-consumer/`; the DLL SHA-256 is
`33022AE70972726E6529591842A721E55F78AB3671D90811DA2BFF27E6F72CA9`.

## Retained activity-state source rows

The next static boundary is the build-86657 category dispatcher at `+0x540320`. Its category-zero
branch copies explicit retained rows into the rebuilt activity-state bank. The source layout is:

| Source offset | Shape | Meaning at the dispatcher |
| ---: | --- | --- |
| `+0xB3F4` | `int32` | switch-row count |
| `+0xB3F8` | 20 × 4 bytes | `int16 index`, `uint8 value`, `uint8 auxiliary` |
| `+0xB448` | `int32` | progression-row count |
| `+0xB44C` | 8-byte rows | `int16 index`, `uint16 auxiliary`, `uint32 value` |

A new observer snapshots those bounded values before calling the native dispatcher unchanged. An
orbit control recorded two enabled category-zero calls from return RVAs `+0x5492F1` and
`+0x54DA47`; both counts were zero. The same exact DLL then completed the authoritative
Homecoming launch: the client changed world to `arcade_homecoming`, entered `activity:in_world`,
and the script host persisted `open-objective`, while the dispatcher emitted no nonzero source
rows. The rendered result remained the black frame with cursor glow.

This connects the previously recovered application helpers to a concrete retained-source layout
and supplies a real Homecoming negative control. It also strengthens the boundary: the current
type-1 global-state message does not populate those explicit rows, and package extraction does not
provide a runtime objective index. A transport decoder or host-owned state producer still has to
be identified before `objective.set` can be implemented. The retained log, probe report,
checkpoint, and exact DLL are in
`analysis/script-host-runtime/20260815-034100-activity-state-source-homecoming/`; the DLL SHA-256 is
`D5714B55317EB4FAE657876BB6E0D2769A3EFCF92B6413A8DE8C14A616D1DAED`.

The `DestinyResearch` objective payloads also close one tempting but incorrect static route. Each
Homecoming objective has a six-entry pointer array, but `obj_ghaul`'s entries resolve internally to
payload offsets `+0xA0`, `+0xB8`, `+0xF8`, `+0x110`, `+0x170`, and `+0x1D0`; the other four
objectives share the same structural pattern. These are component payloads, not verified external
trigger/action edges. The archive's name-affinity arrows remain heuristic and cannot supply an
`objective.set` transition.

Static tracing also identifies the retained source more precisely. The activity-state rebuild at
`+0x5084C0` passes the value returned by activity-owner virtual method `+0x18` through `+0x54D730`
to `+0x540320`. A real-orbit identity control saw the same non-polymorphic prefix
`00100101,9EAA3001,00010000,00000000` at callers `+0x5492F1` and `+0x54DA47`, with both explicit
row counts still zero. The prefix has no literal match in the reconstructed executable, which
points toward a decoded/runtime producer. The exact log and DLL are retained in
`analysis/script-host-runtime/20260815-040931-activity-state-source-identity-orbit/`; DLL SHA-256
`FB5882E6F2C30360BC9E7E2FBD4E648A256200965C0F75801DC556D73489A5EA`.
