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
