# Sunrise Inspector Upgrade Plan

## Handoff status

- **Repository:** `F:/sunrise/Sunrise`
- **Branch:** `sunrise-inspect`
- **Planning baseline:** commit `a83a2393491373a781eaa40eb1da5126d7ce6c5d`
- **Scope:** world-render helper overlays, trigger/box visualization, safe wireframe-like debug drawing, node graph, and activity graph browsing.
- **Current state:** planning only; no implementation changes have been made.
- **Important:** do not modify the supplied PE, PDB, pdata, manifest ZIP, or existing IDA databases.

## Context and verified evidence

The current Inspector has a useful pointer-free model and shared selection behavior, but the center viewport is not a world renderer:

1. `Present` captures the completed D3D11 color backbuffer.
2. The Inspector clears/re-composites that image into an ImGui child viewport.
3. `world_inspector_viewport.cpp` projects copied node positions with the Viewer Camera and draws 2D markers, labels, and selection styling.
4. No depth texture, object-ID buffer, scene geometry stream, or game view-projection matrix is retained.

The inspection graph is a flat vector with parent/children links. `Node` already carries transforms, optional bounds, source/activity identity, runtime state, and actions. One `Selection` object is shared by the outliner, viewport, inspector, references, events, and compare panels.

Triggers already publish pointer-free event/volume observations. Volume observations currently contain a center position, enabled/active state, and overlap count, but not shape extents or collision geometry. `append_triggers` assigns positional actions and then overwrites them with `Action::copyId`, which suppresses volume markers because the viewport currently uses `Action::hide` as a renderability condition.

The supplied evidence provides useful research boundaries:

- `destiny2_dethread_pdata.bin` is a complete build-87221 x64 PE, not a raw `.pdata` slice.
- `tiger_release_final.pdb` matches it exactly by RSDS GUID/age.
- The PDB contains analysis-generated public anchors such as:
  - `debug_draw_aabb_transform`
  - `debug_draw_shape_by_type`
  - `physics_debug_draw_capsule`
  - `debug_draw_batch_submit`
  - `gfx_emit_debug_primitive`
  - `gfx_queue_debug_draw_command`
  - `trigger_volume_contains_point`
  - `world_compute_node_group_bounds`
  - `scene_graph_register_node`
  - `editor_mesh_wireframe`
- The depot executable at `F:/depot/depots/1085661/24238629/destiny2.exe` is build 86657 and does **not** match the supplied build-87221 addresses/PDB.
- The PDB has no reliable original source/type layouts. Treat these names as analysis anchors, not callable ABI contracts.
- `manifest.zip` has content version `87221.20.09.10.1506-2` and contains:
  - 26 activity graphs
  - 178 graph nodes
  - 445 node-to-activity references
  - 16 linked-graph references
  - empty `connections` arrays in the inspected archive
  - location releases with graph/node hashes, spawn points, and public four-lane positions
- The activity manifest is public activity-map metadata, not the runtime scene graph or collision/trigger geometry.
- `F:/DestinyResearch/tools/alkahest-prebl` contains useful conceptual references for D3D11 wireframe rasterizer states, immediate line/cube/AABB rendering, and package AABBs. Those implementations are not proof of Sunrise/native-runtime identity mappings.

## Product outcome

Deliver three synchronized center views:

1. **World** — captured game image with projected markers, trigger centers, and known AABB outlines.
2. **Node Graph** — deterministic graph view of the current inspection ownership tree.
3. **Activity Map** — optional browse-only view of the external activity catalog, using authored activity-graph positions.

The first implementation must be safe and honest:

- Call the visual overlay **Known bounds (x-ray)**, not engine wireframe.
- State visibly that helpers are not depth-tested.
- Draw a trigger center when only a center is known; never invent a volume shape.
- Do not call build-specific Destiny debug functions.
- Do not change the game's global rasterizer state.
- Do not retain game/Havok pointers.
- Do not treat activity-map positions as live world transforms.
- Do not bundle or commit the supplied manifest data.

## Recommended implementation stages

### Stage 1 — Fix trigger spatial behavior and formalize bounds

#### Files

- `Sunrise/src/client/inspection/providers/runtime_observation_inspection.h`
- `Sunrise/src/client/inspection/world_inspection_model.h`
- `Sunrise/src/client/inspection/world_inspection_model.cpp`
- `Sunrise/src/client/ui/world_inspector/world_inspector.cpp`
- `Sunrise/src/client/inspection/inspection_capture.h`
- `Sunrise/src/client/inspection/inspection_capture.cpp`

#### Changes

1. In `append_triggers` and `update_triggers`:
   - preserve `focus`, `hide`, `isolate`, and `copyPosition` when a trigger has a valid position;
   - retain `copyId` in addition to positional actions;
   - only grant positional actions while a valid position is present;
   - preserve event-to-live-object position matching.
2. Stop treating `Action::hide` as the condition for drawing a spatial helper. A node is drawable when it has valid spatial data and its category is enabled; actions only control available commands.
3. Define `Bounds` as a finite world-space AABB:
   - `minimum[i] <= maximum[i]` for every lane;
   - absence means unknown;
   - zero-size bounds are allowed only when actually supplied by a producer;
   - never synthesize a unit cube around a position.
4. Add small pure helpers for bounds validation, center, extents, and optionally corners.
5. Add bounds provenance so UI/export can distinguish derived, catalog, and runtime values.
6. Update properties, tooltips, JSON, CSV, and change capture to state:
   - known AABB with provenance; or
   - `shape unavailable; center observation only` for current trigger volumes.

#### Acceptance criteria

- Rebuilt trigger volume rows retain Focus/Hide/Isolate/Copy Position.
- Trigger centers are eligible for projection even though they have no bounds.
- Invalid/non-finite/inverted bounds are rejected.
- Position-only nodes never receive guessed bounds.

### Stage 2 — Add projected world-debug primitives

#### New files

- `Sunrise/src/client/ui/world_inspector/world_debug_primitives.h`
- `Sunrise/src/client/ui/world_inspector/world_debug_primitives.cpp`

#### Existing files

- `Sunrise/src/client/ui/world_inspector/world_inspector_viewport.h`
- `Sunrise/src/client/ui/world_inspector/world_inspector_viewport.cpp`

#### Design

Keep this module renderer-independent and resource-free. Submit primitives to the existing ImGui draw list after the captured frame is drawn.

Projection context:

- copied camera pose;
- fitted image rectangle;
- near-plane distance;
- clip rectangle.

Primitive types:

- world-space line segment;
- three-axis center cross;
- AABB outline (12 edges);
- generic eight-corner box outline for future OBB use;
- capsule outline infrastructure, only when a producer supplies validated endpoints/radius;
- optional dashed style for uncertain/non-current metadata.

Projection requirements:

- validate finite camera vectors and FOV;
- reject degenerate bases and invalid viewport sizes;
- preserve current FOV convention unless live evidence requires a change;
- clip line segments against the near plane before projection;
- use the existing ImGui clip rectangle for viewport-edge clipping;
- return projected segments/depth for edge picking and deterministic tie-breaking.

#### Viewport options

Extend `viewport::Options` with at least:

- `showKnownBounds`;
- `showTriggerCenters`;
- `showUnknownShapeMarkers`;
- retain existing category and label options.

Rendering policy:

- selected/hovered known bounds always render;
- known bounds render for enabled categories when the overlay is enabled;
- trigger centers render when trigger helpers are enabled;
- unknown trigger shapes get a small center cross and explicit unknown-shape styling;
- hidden nodes do not render except selected nodes;
- selected geometry is drawn last with stronger color/line width;
- bounds edges are pickable;
- bounds are used for camera focus framing; point-only nodes keep the existing fixed focus distance.

The viewport should include a small disclosure such as:

> X-ray overlay — no scene depth

#### Acceptance criteria

- A known AABB renders all 12 edges.
- A trigger with only a center renders a center marker and no guessed shape.
- Near-plane crossings do not make an entire box disappear.
- Edge picking selects the expected node.
- Letterboxed/fitted viewport clipping remains correct.

### Stage 3 — Add a dependency-free graph canvas

#### New files

- `Sunrise/src/client/ui/world_inspector/world_inspector_graph.h`
- `Sunrise/src/client/ui/world_inspector/world_inspector_graph.cpp`

#### Existing file

- `Sunrise/src/client/ui/world_inspector/world_inspector.cpp`

#### API

The graph canvas should return the same interaction shape as the viewport:

- hovered node;
- selected node;
- focused/double-clicked node;
- context target;
- clear-selection request.

Use Dear ImGui draw lists. Do not add ImNodes, ImPlot, Graphviz, or another graph dependency.

#### Workspace state

Add a center mode:

- `World`;
- `Node Graph`;
- `Activity Map`.

Keep UI-only state in `WorkspaceState`:

- pan offset;
- zoom;
- fit request;
- cached graph generation;
- cached filter/admission revision;
- automatic layout positions;
- selected activity graph hash.

Do not store pan/zoom in the inspection model.

#### Node Graph layout

The current model has ownership edges only. Use deterministic layered tree layout:

1. derive depth from the selected hierarchy root;
2. assign leaves in stable child order;
3. center parents over admitted children;
4. place depth horizontally and leaf order vertically;
5. recompute only when graph generation or admitted membership changes.

Render parent-child edges behind cards. Cards should include a kind color strip, concise name/kind, and status/provenance where space allows. Cull off-screen cards and edges.

#### Shared behavior

Refactor filter admission only enough to produce one admitted `NodeId` set consumed by both outliner and graph. Route all graph interactions through existing selection/reveal/context/focus functions so:

- graph selection updates the outliner;
- outliner selection highlights the graph card;
- the right inspector, references, events, and compare follow the same selection;
- hiding/isolation/search semantics stay consistent.

Do not add a generic `Edge` model yet. Parent/children is sufficient for the current graph.

### Stage 4 — Add optional compact activity catalog

#### New files

- `Sunrise/src/client/inspection/activity_graph_catalog.h`
- `Sunrise/src/client/inspection/activity_graph_catalog.cpp`
- `Sunrise/src/client/inspection/providers/activity_graph_inspection.h`
- `Sunrise/src/client/inspection/providers/activity_graph_inspection.cpp`
- `tools/build_activity_graph_catalog.py`

#### Offline converter

Use only Python standard-library modules:

- `zipfile`;
- `json`;
- `hashlib`;
- `struct`;
- `argparse`.

Read only the required manifest tables from `manifest.zip`:

- `DestinyActivityDefinition.json`;
- `DestinyActivityGraphDefinition.json`;
- `DestinyLocationDefinition.json`;
- optionally `DestinyDestinationDefinition.json` for display/navigation metadata.

Do not extract or retain the nested SQLite database. Do not commit source or generated data.

Retain only:

- full manifest version and numeric content build;
- source-table SHA-256 digests;
- activity index/hash/name mapping;
- graph hash/index;
- graph node IDs and authored positions;
- node states/style hashes;
- node-to-activity references;
- linked graph IDs/targets;
- summarized location-release keys, spawn point, and public four-lane position;
- deduplicated UTF-8 strings.

Output a compact explicit little-endian binary under the normal generated-artifact/cache directory.

#### Binary validation

Do not read packed C++ structs with `reinterpret_cast`. The header should carry magic, schema version, content build, version string location, source digests, section offsets/counts, string-table size, and total file size.

Validate:

- every section range;
- integer addition/multiplication overflow;
- record child ranges;
- string offsets and termination;
- finite authored positions;
- unique graph hashes;
- unique `(graphHash, nodeHash)` keys;
- valid linked graph/activity references;
- known schema version;
- fixed maximum counts and file size.

Missing/malformed catalog behavior: keep the normal Inspector working and add one diagnostic. Never fail client startup.

#### Runtime loading

Use `core::path::artifact_directory` to resolve the optional generated artifact. Initialize through the normal client runtime lifecycle and shut down in reverse order. Do not continuously watch/reload the file.

#### Model/provider representation

Add only metadata needed by UI/search/export/stable selection. If the existing model needs expansion, add:

- `NodeKind::activityGraph`;
- `NodeKind::activityGraphNode`;
- `NodeKind::activityReference`;
- an `ActivityMetadata` value containing graph/node/activity hashes, authored position, state/style hashes, release/reference counts, and catalog identity;
- `Producer::activityCatalog`.

Use deterministic keys:

- graph: graph hash;
- graph node: `(graphHash, nodeHash)`;
- activity reference: `(graphNodeHash, activityHash)`.

Keep catalog hierarchy as ownership:

```text
Current activity/session
└── Activity catalog
    └── Activity graph
        └── Activity graph node
            └── Activity reference
```

Do not invent activity connections; the supplied archive has empty connection arrays.

#### Build compatibility

Keep one declared target-build constant rather than duplicating literals. The supplied catalog is build 87221 while the depot/current branch is build 86657. Therefore:

- show catalog version and target version;
- mark it `Browse only`;
- disable current-session correlation;
- do not map public location positions to live transforms/bounds;
- do not claim that public activity hashes identify current runtime nodes.

If a matching catalog is later supplied, map current activity index to public activity hash and expose all matching graph hashes rather than silently choosing one without showing the match set.

#### Activity Map

- use authored graph-node UI positions;
- normalize positions into canvas coordinates;
- preserve coincident authored positions rather than perturbing them;
- show activity references in cards/details;
- expose linked graphs as navigation controls;
- draw no edges when `connections` is empty;
- display `Authored positions only; no node connections in this catalog`.

Location release positions remain metadata only and never become world viewport geometry.

### Stage 5 — Integrate workspace, capture, project metadata

#### `world_inspector.cpp`

Add:

- center-view selector;
- known-bounds and trigger-center overlay toggles;
- fit/recenter graph control;
- common filter admission for tree and graph;
- common application of viewport/graph interaction results;
- activity catalog build/compatibility indicators;
- activity metadata details;
- bounds provenance/center/extents details;
- explicit unknown trigger-shape state.

Keep transient graph pan/zoom session-local. Persist only controls that fit existing viewer settings conventions.

#### `inspection_capture.{h,cpp}`

Export:

- bounds and provenance;
- activity metadata;
- catalog version/build;
- compatibility and correlation state.

Update comparisons without reporting static catalog metadata as a change every frame. Increment snapshot schema version only if the external JSON contract changes.

#### Project files

- Add each new source/header to `Sunrise/Sunrise.vcxproj`.
- Existing root `CMakeLists.txt` uses recursive source globs and should discover production files, but test targets must be explicit.
- Update `README.md` with catalog generation, placement, mismatch behavior, x-ray/no-depth disclosure, and unsupported native wireframe/shape limitations.

Do not include `manifest.zip`, extracted JSON, nested SQLite, or generated catalogs in source control or release artifacts.

## Research-gated follow-up (not part of first implementation)

### Native trigger shape recovery

Only pursue after build-specific validation of:

- trigger ownership/lifetime;
- Havok rigid-body and shape layout;
- shape type discrimination;
- safe scalar copying at event-tick/post-simulation boundaries;
- callback thread and synchronization.

Extend the existing pointer-free trigger snapshot with copied shape data. Never retain Havok pointers.

### Native debug draw

The supplied symbols (`debug_draw_aabb_transform`, `debug_draw_shape_by_type`, `debug_draw_batch_submit`, `gfx_emit_debug_primitive`) are research anchors only. Before calling any one:

- prove exact executable/PDB identity;
- validate calling convention and parameter layout;
- identify owner and render-thread requirements;
- prove queue lifetime and state restoration;
- provide unload-safe behavior;
- gate by exact build with projected-overlay fallback.

### True wireframe

Technique strings such as `editor_mesh_wireframe` prove assets exist, not a safe toggle. A future feature must target a validated selected-draw/technique boundary, preserve D3D11 state, be disabled by default, and never use a global Present-time rasterizer override.

### Depth-aware helpers

Depth-aware occlusion requires a separately validated depth-resource capture path, including typeless/MSAA handling and state restoration. Do not complicate the first overlay with this.

## Critical files

### Existing

- `Sunrise/src/client/ui/world_inspector/world_inspector.cpp`
- `Sunrise/src/client/ui/world_inspector/world_inspector_viewport.h`
- `Sunrise/src/client/ui/world_inspector/world_inspector_viewport.cpp`
- `Sunrise/src/client/inspection/world_inspection_model.h`
- `Sunrise/src/client/inspection/world_inspection_model.cpp`
- `Sunrise/src/client/inspection/providers/runtime_observation_inspection.h`
- `Sunrise/src/client/inspection/providers/spawn_inspection_provider.h`
- `Sunrise/src/client/inspection/providers/spawn_inspection_provider.cpp`
- `Sunrise/src/client/inspection/providers/spawn_inspection_provider_part*.inc`
- `Sunrise/src/client/inspection/inspection_capture.h`
- `Sunrise/src/client/inspection/inspection_capture.cpp`
- `Sunrise/src/core/filesystem/path.h`
- `Sunrise/src/core/filesystem/path.cpp`
- `Sunrise/src/client/viewer/viewer_camera_settings_store.h`
- `Sunrise/src/client/viewer/viewer_camera_settings_store.cpp`
- `Sunrise/Sunrise.vcxproj`
- `CMakeLists.txt`
- `README.md`

### New

- `Sunrise/src/client/ui/world_inspector/world_debug_primitives.h`
- `Sunrise/src/client/ui/world_inspector/world_debug_primitives.cpp`
- `Sunrise/src/client/ui/world_inspector/world_inspector_graph.h`
- `Sunrise/src/client/ui/world_inspector/world_inspector_graph.cpp`
- `Sunrise/src/client/inspection/activity_graph_catalog.h`
- `Sunrise/src/client/inspection/activity_graph_catalog.cpp`
- `Sunrise/src/client/inspection/providers/activity_graph_inspection.h`
- `Sunrise/src/client/inspection/providers/activity_graph_inspection.cpp`
- `tools/build_activity_graph_catalog.py`
- focused tests under `tests/`

## Focused tests

Follow the existing standalone test style in:

- `tests/viewer_camera_path_store_tests.cpp`
- `tests/run_viewer_camera_path_store_tests.ps1`

Add focused tests for:

### Model and trigger behavior

- generation-aware `NodeId` behavior;
- selection reconciliation;
- trigger action preservation;
- spatial eligibility independent of actions;
- valid/inverted/non-finite AABBs;
- bounds provenance;
- no synthesized bounds.

### Projection/primitives

- camera-center projection;
- behind-camera rejection;
- one endpoint behind near plane;
- invalid FOV/basis;
- all 12 AABB edges;
- finite capsule tessellation;
- aspect-ratio/letterbox fitting.

### Graph layout

- deterministic positions;
- parents centered over children;
- stable child ordering;
- filtered subtree handling;
- empty/deep graphs;
- authored activity positions remain unchanged;
- zero connections yield zero inferred edges.

### Catalog parser/converter

Use synthetic byte buffers and synthetic JSON-in-ZIP fixtures, not real manifest data. Cover:

- minimal valid catalog;
- multiple graphs/references;
- truncated headers/sections;
- overflowed counts/offsets;
- invalid strings;
- unknown schema;
- duplicate graph/node keys;
- non-finite authored positions;
- build match/mismatch;
- multiple graph matches for an activity.

## Verification sequence

1. Run the focused native Windows x64 tests.
2. Run the converter against `C:/Users/ptwar/Downloads/manifest.zip` and verify:
   - content build 87221;
   - 26 graphs;
   - 178 nodes;
   - 445 node-to-activity references;
   - 16 linked graphs;
   - empty connections;
   - expected location counts.
3. Confirm generated output is small, optional, and not tracked.
4. Build `Release|x64` with `Sunrise.sln`.
5. Configure/build through the existing CMake/xwin path.
6. Test startup with no catalog, malformed catalog, and build-mismatched catalog.
7. Live test on the supported offline build:
   - trigger centers appear;
   - volume actions survive refresh;
   - active state/overlap count update;
   - unknown shape is labeled center-only;
   - known AABBs render, pick, tooltip, and focus correctly when a proven producer exists;
   - near-plane, letterbox, DPI, and resize behavior remain stable;
   - World, Node Graph, and Activity Map share selection/filtering/properties/references/events/compare;
   - browse-only 87221 catalog never becomes live world geometry;
   - linked graph navigation works;
   - no invented activity edges appear;
   - no D3D11 resource/state leak or frame-capture regression.
8. Compare frame time with overlays disabled, trigger centers enabled, all known bounds enabled, and a large node graph visible.

## Non-goals for takeover

- No native engine debug-function calls in the first pass.
- No global game wireframe mode.
- No guessed trigger extents.
- No depth buffer integration.
- No generic arbitrary-edge graph model until real non-hierarchical edges are captured.
- No checked-in public manifest data or generated catalog.
- No unrelated refactor of the existing inspection provider or UI architecture.
