# Viewer and World Inspector

Viewer adds a detached camera and inspection workspace for the supported Destiny 2 build. Inspector
combines copied runtime observations and optional authored catalogs into one searchable,
pointer-free document. Each producer reports its own readiness, bounded copy counts, truncation, and
failure, so one unavailable source does not disable unrelated evidence.

## Workspace

The Viewer page shows Runtime, Authored, and Rendering readiness beside the camera, HUD, audio, and
camera-path controls. The World Inspector has four views:

| View | Purpose |
| --- | --- |
| World | Copied spatial observations and explicitly known bounds over the captured frame. |
| Node Graph | Ownership and filtered hierarchy navigation. |
| Relationships | Authored Activity Logic relations for the current selection. |
| Activity Map | Authored graph coordinates; these are never treated as live transforms. |

References, Data, Changes, Compare, and Diagnostics are separate bottom panels. Search, selection,
hide, isolate, and overlay detail are shared workspace state. Hide and isolate affect Inspector
helpers only; they do not hide game objects.

## Architecture

`client/inspection` owns the evidence model, `InspectionDocument`, provider, session, bounded
history, comparisons, settings, and exports. `client/ui/world_inspector` owns transient interaction
and presentation. Graphics code consumes an immutable `SceneFrame` through
`inspection_workspace_host.h`; renderer code does not depend on workspace state.

`InspectionDocument` is the single published state. It owns the graph, world context, diagnostics,
structure/value revisions, and reports from producers that actually ran. No public Inspector state
retains a gameplay pointer.

`NodeId` addresses one graph generation. `NodeKey` contains producer, producer epoch, kind, native
key, and deterministic discriminator. Selection, hidden/collapsed state, history, comparisons, and
exports use `NodeKey` across rebuilds. Duplicate keys are rejected and diagnosed.

Spatial evidence is explicit:

- Exact bounds are finite, non-inverted AABBs supplied by a producer.
- Runtime centers are point observations and never become volumes.
- Authored placements remain catalog evidence and never become live transforms.
- Helper markers are presentation only.

Provider-specific catalog details are typed properties on the world root and readiness/count/failure
data is published through `ProviderReport`. Unknown fields stay absent.

## Runtime and hook safety

Hook installation is independent: a failed optional Viewer/Inspector hook is logged and leaves the
remaining features available. Shutdown reverses installed hooks and clears copied publications.
Runtime producers publish bounded copied state rather than native pointers. Every synchronous action
revalidates the current candidate and ownership before reading or writing.

Player Hold resolves the existing player-position candidate on demand. Its Havok body pointer exists
only inside `StepContext` across the original synchronous Havok call and is revalidated afterward.
Teleport remains blocked during Viewer entry/active ownership. **Teleport Player Here** validates a
spatial target, exits detached Viewer ownership, and applies through the existing teleport physics
path.

World helpers render only through the Sunrise depth-aware D3D11 path after the current native view
and depth inputs have been proven. If those inputs are unavailable, stale, or rejected, helper
geometry is disabled for that frame and the viewport reports the failure explicitly; there is no
ImGui geometry substitute. Depth picking uses a private ID pass and discards stale results by
request sequence, engine frame, graph generation, and immutable scene ownership.

## Supported build and limitations

The native signatures, layout assumptions, image metadata, and optional catalogs target Destiny 2
content build 86657. They are fail-closed on a different or malformed image. This repository does
not include a game executable, extracted manifests, research corpora, generated catalogs, or other
copyrighted game data.

Current live coverage includes the local controlled object, bounded object-system observations,
triggers, the primary Wwise listener, and bounded Havok body slots. Physics slots are observations,
not durable body identities.

Package-backed coverage includes scenario/destination context, roster placements, spawn points,
static footprints, bubble bounds, activity maps, and Activity Logic when their source data is
available. Roster placement does not prove that a live entity exists. Terrain surfaces, trigger
volume shapes, audio emitters, lights, navigation, physics controllers, stable physics-body
identities, and general live entity bounds do not have proven producers and are not represented as
placeholder nodes or fake reports.

## Optional catalogs

The three converters accept user-supplied extracted data and write generated output outside source
control. Sunrise looks for these files beside the installed DLL, normally `bin\x64\Sunrise`:

```powershell
python tools/build_activity_graph_catalog.py `
  --input <activity-manifest.json> `
  --output bin\x64\Sunrise\activity-graph-catalog.bin

python tools/build_activity_logic_catalog.py `
  --input <activity-logic.json> `
  --output bin\x64\Sunrise\activity-logic-catalog.bin

python tools/build_bubble_bounds_catalog.py `
  --corpus <mesh-hillshade-bubbles-directory> `
  --output bin\x64\Sunrise\bubble-bounds-catalog.bin
```

Missing, malformed, build-mismatched, or capacity-limited catalogs fail locally and publish a
diagnostic. Browse-only authored data is labeled as such and never presented as current live state.

## Persistence

`viewer.json` owns Viewer camera/input settings. `viewer-paths.json` owns camera paths.
`inspector.json` owns Inspector layout, filters, overlay detail, and labels.

Inspector settings use `"schema_version": 1`. A missing file creates defaults. A malformed or
incompatible file is logged and defaults are used without importing or overwriting `viewer.json`.
Session-only diagnostic and interaction state is not persisted.

## Export schema 1

Existing commands and filenames are retained:

- `viewer-inspection.json`
- `viewer-inspection.csv`
- `viewer-events.json`
- `viewer-route-<process>-<session>-<sequence>-<tick>.json`

Files are written atomically beneath the installed DLL directory. JSON is serialized directly from
`InspectionDocument` and contains capture/image metadata, world context, revisions, actual provider
reports, diagnostics, and nodes. Every node includes its stable key, parent key, kind, producer,
status, provenance, source, spatial evidence, typed properties, relations, and authored metadata.

CSV remains one row per node with stable/parent keys, common source and spatial columns, and escaped
property, relation, and authored-metadata payloads. Route exports add route/camera metadata. Event
exports include schema version 1 and the current world context.

## Manual verification checklist

Run this checklist against both Debug and Release builds before committing:

- Launch into a destination, open/close Viewer and Inspector repeatedly, then shut down cleanly.
- Enter/exit detached Viewer camera; verify player anchoring and restored ordinary camera control.
- Exercise Viewer movement, speed/FOV, HUD visibility, audio listener/mute controls, and input focus.
- Create, edit, save, reload, play, interrupt, and delete camera paths.
- Browse every Inspector view and bottom panel; search quoted/bare terms; select, focus, hide,
  isolate, restore, and copy values.
- Exercise selected-only, nearby, adaptive, all, and camera-nearby detail modes.
- Verify depth helpers become ready and remain correctly occluded; confirm unavailable, stale, and
  rejected depth inputs show an explicit failure with no helper geometry. Resize/reset the device
  and change resolution/window state.
- Load each optional catalog, verify missing/malformed/build-mismatch diagnostics, and inspect
  browse-only labeling.
- Verify safe and rejected Teleport Player Here targets.
- Restart to verify `viewer.json`, `viewer-paths.json`, and schema-1 `inspector.json`
  persistence and rejection/default behavior.
- Export JSON, CSV, events, and route captures; validate schema version 1, escaping, optional fields,
  stable/parent keys, context, reports, properties, relations, and route metadata.
- Recheck Movement, Fly, Noclip, Sword Skate, ordinary Teleport, player-position publication, and
  camera behavior with Viewer closed.
