# Homecoming activity/runtime evidence

This note joins the extracted Season of Arrivals activity graph with build-86657 runtime evidence.
It does not treat name-affinity graph edges as execution order and does not claim that the
Homecoming activity has a known client activity index.

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

The graph also contains 41 serialized WorldID-to-placement links. Those are strong direct links.
For example, `spawnrule_boss01` (`80F0206F`) points to WorldID `CA7EB3A7D45895FE`, and
`d_ship_door` (`80F020D2`) points to WorldID `D3FCE3E3838A7339`. The 21 name-affinity flow edges
in the report are candidates only and must not be used as proof of encounter execution order.

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

## Next evidence requirement

The next implementation must resolve a real Homecoming descriptor before the `+0xC19EAC` /
`+0xEE8546` boundary. The static scenario-client tag `80F0200E` is not interchangeable with a
client activity index. A mutation is justified only after tracing the native activity-definition
lookup that turns registered content into the `0x118`-byte descriptor.
