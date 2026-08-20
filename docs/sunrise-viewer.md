# Using and maintaining Sunrise Viewer

Sunrise Viewer is a read-only, in-game inspection workspace for the historical Destiny 2 client
targeted by this branch. It combines the existing Viewer camera, captured game frame, package data,
and bounded runtime observations; it is not a separate map renderer.

> **Target and safety:** the current native signatures target Destiny 2 build
> `86657.20.08.23.1800.d2_rc`. Inspection helpers never mutate Destiny world objects. Native
> gameplay, Wwise, and Havok pointers remain owned by their hook threads and are never stored in the
> inspection graph.

## Inspecting a destination

1. Load a destination and enable Viewer from the Sunrise UI.
2. Open the full-screen World Inspector workspace.
3. Choose **World**, **Source**, or **Activity** to change the hierarchy root.
4. Use the search field or the Geometry, Entities, Spawns, Triggers, and Audio filters to reduce the
   tree.
5. Select a tree row or viewport helper to inspect its identity, source, transform, relationships,
   and diagnostics.
6. Use **Focus** only for nodes with a copied position. **Hide** and **Isolate** affect helpers, not
   game rendering.

Runtime props may enter and leave the object system while the workspace is open. Selection is
remapped by the stable runtime handle when the graph refreshes, and the tree preserves its top
visible row and scroll offset. If the selected prop itself disappears, selection is cleared instead
of moving to a different object.

## Understanding the data flow

| Producer | Copied data | Inspection result |
| --- | --- | --- |
| Activity and package catalogs | Session, destination, scenario, bubble, spawn set, roster placements, component-slot metadata | Structural hierarchy, spawn points, placed objects, and component slots |
| Occupied object-datum iterator | Bounded handle and object-type rows | Runtime Entity nodes; StaticMesh and Speedtree types also appear as Geometry |
| Existing player physics sync | Controlled-object handle and copied position | Local controlled-object node and positions attached to observed live objects |
| Existing Wwise listener command | Primary listener position | One Audio listener node |
| Existing Havok simulation step | Bounded body-slot position and velocity rows | Physics nodes with explicitly non-durable slot identities |
| Native trigger event and post-simulation boundaries | Event state, runtime association, trigger-volume position, active state, and overlap count | Trigger nodes without invented shape bounds or authored semantics |
| Graphics frame capture | Copied back buffer | Live viewport behind projected helpers |

The provider rebuilds the inspection graph only from copied values. Runtime hooks publish bounded,
pointer-free snapshots under their own synchronization. The UI consumes those snapshots and never
walks native gameplay objects directly.

## Interpreting node kinds

| Kind | Meaning | Important limitation |
| --- | --- | --- |
| Spawn Point | Authored point from the selected spawn set | Not proof of a currently spawned object |
| Placed Object | Package-backed roster placement record | No runtime identity or transform is claimed |
| Component Slot | Bounded descriptor metadata for a placed object | Class/schema hashes are metadata, not decoded behavior |
| Runtime Entity | Occupied live object-system datum | Unknown object-type bytes remain explicit |
| Geometry | Live StaticMesh or Speedtree classification | Mesh resources, vertices, materials, and bounds are not enumerated |
| Trigger | Copied native event or Havok trigger-volume observation | Volume shapes, bounds, and authored trigger meaning are unavailable |
| Audio | Primary Wwise listener | Emitters, voices, buses, and the Wwise graph are unavailable |
| Physics | Copied Havok body-array slot | Slot numbers are not durable body, controller, or entity identities |

Viewport helpers use a distinct color for each `NodeKind`; selection and hover styling override the
kind color. Labels are optional because dense object groups can overlap heavily.

## Searching and navigating the tree

Free text matches normalized node names, types, status, package/map names, and recorded identifiers.
Structured terms can narrow common fields:

```text
type:trigger
type:geometry
tag:80806730
status:known
```

The tree is virtualized, supports horizontal scrolling for deeply nested or long labels, and shows
the complete label in a tooltip when it does not fit. Expand/collapse state for structural nodes is
kept during ordinary runtime observation refreshes.

## Coverage and limitations

Supported coverage:

- Activity, destination, scenario, region, and bubble context.
- Spawn sets, spawn points, package-backed placed objects, and component-slot metadata.
- Bounded general object-system enumeration and known object-type classification.
- Local-player position, primary audio listener, Havok body slots, native trigger events, and Havok
  trigger-volume positions.
- Read-only helper selection, focus, hide, isolate, copied identifiers/positions, references, raw
  data, and diagnostics.

Not currently supported:

- A separate simulation-entity registry with proven stable identity.
- Terrain patches, mesh resources, geometry bounds, materials, or depth-assisted surface picking.
- Light enumeration or light parameters.
- Audio emitters, voices, buses, occlusion objects, or graph relationships.
- Stable Havok body/controller identities, collision shapes, or trigger-volume bounds.
- Mutation of live game objects or rendering.

Unsupported semantics must remain unknown. Do not infer a terrain, light, emitter, trigger, or entity
type from a convenient class hash without separate layout and lifecycle evidence.

## Verifying a change

Run the signature checks against the exact build-86657 image before building:

```powershell
$Image86657 = Read-Host 'Path to the exact build-86657 Destiny 2 executable'
python .\verify_viewer_camera_signatures.py $Image86657
python .\verify_viewer_audio_signatures.py $Image86657
python .\verify_viewer_trigger_signature.py $Image86657
```

Build Release x64 serially to avoid MSVC memory exhaustion:

```powershell
$env:CL_MPCount = '1'
msbuild .\Sunrise\Sunrise.vcxproj /t:Build /p:Configuration=Release /p:Platform=x64 `
  /m:1 /p:MultiProcessorCompilation=false
```

The signature checks and a successful build are offline validation only. In-game acceptance remains
manual and should cover:

1. Camera movement and frame capture.
2. Selection from both the tree and viewport.
3. Runtime props spawning and despawning while a different prop remains selected.
4. Manual tree scrolling during the same object churn.
5. Trigger and object helper colors on more than one destination.
6. Clean viewer shutdown and destination transition.

## Adding another runtime producer

Use the existing lifecycle pattern:

1. Prove the exact build-86657 boundary with a unique signature or a verified migrated symbol.
2. Establish the native caller thread, object lifetime, and teardown order.
3. Copy only bounded scalar/value data while the native object is valid.
4. Publish through an owned snapshot API with explicit synchronization.
5. Convert the snapshot into inspection nodes without retaining native pointers.
6. Report truncation, readiness, and unknown semantics honestly in Diagnostics.
7. Add a deterministic offline signature check and perform a user-led in-game test.

The symbol evidence behind possible future entity, terrain, light, and audio work is recorded in
[Destiny 2 Viewer symbol research](research/2026-08-20-destiny2-viewer-symbols.md).
