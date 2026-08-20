# Sunrise

Destiny 2 Offline Exploration Mod

> This mod installs onto an old build of the game and allows you to play it offline, loading into
> destinations and exploring them.
>
> Most gameplay features are not currently supported. (Missions, Enemies, NPCs, Quests, Persistent Saves, ...)

- [Install Instructions](https://github.com/stanuwu/Sunrise/wiki/Installing)
- [FAQ](https://github.com/stanuwu/Sunrise/wiki/FAQ)
- [Common Issues](https://github.com/stanuwu/Sunrise/wiki/Common-Issues)
- [Discord](https://discord.gg/22JS6et5k9)

## Features
- Load into any Destination (matchmade activities are currently broken)
- Exploration Features (Fly, Noclip, Activity Override, ...)
- Basic Inventory Management

## Sunrise Viewer

> **Current branch:** `sunrise-inspect`. Viewer targets the exact Destiny 2 build 86657 image. It is
> read-only with respect to game-world objects: detached camera movement and inspector-helper
> rendering are allowed, but inspection never moves the player or mutates an entity. Offline builds
> and signature checks do not replace in-game validation.

Sunrise Viewer is an editor-style investigation workspace built on Sunrise's existing graphics
hooks, runtime snapshots, package catalogs, and detached camera. It is not a separate map renderer
or an Alkahest port.

### Inspecting a destination

1. Enable **Viewer Camera** from the Viewer panel.
2. Open **Workspace** and choose the World, Source, or Activity hierarchy.
3. Filter or search the scene tree, then select a row or a viewport marker.
4. Use **References** and **Data** for copied fields, **Events** for recording changes, **Compare**
   for a point-in-time diff, and **Diagnostics** for producer readiness and truncation.

The captured game viewport projects runtime and spawn helpers over the current frame. Selection,
focus, copy, hide, and isolate operate on Viewer state only. Labels are kept inside the viewport and
are placed without covering higher-priority selected or hovered labels.

### Recording and exporting inspection data

The Changes tab tracks only after **Start tracking** is pressed. Interactive tracking defaults to runtime nodes and suppresses transform noise unless **Track transforms** is enabled. It retains at most 4,096 added,
removed, and field-level changed events in memory; recording can be paused, cleared, filtered, and
exported. Selecting an event attempts to reveal the related node in the current graph. The Compare
tab captures an in-memory baseline and compares it with a later complete snapshot.

Snapshot nodes carry explicit producer and provenance values. Runtime continuity uses
`{producer, producer_epoch, native_key}` so a recycled native handle is not treated as the same
object after an activity transition or producer reset. Havok array slots are exported but excluded
from change continuity because they are observations rather than proven durable identities.

Exports are written atomically beneath the installed DLL directory, normally `bin\x64\Sunrise`:

| File | Contents |
| --- | --- |
| `viewer-inspection.json` | Versioned pointer-free graph, world identity, image hash, producer status, provenance, and truncation metadata |
| `viewer-inspection.csv` | Flattened node table for spreadsheet or data-tool analysis |
| `viewer-events.json` | Current bounded change history plus capture metadata |
| `viewer-route-*.json` | Uniquely named snapshot captured at a camera-path keyframe |
| `viewer-paths.json` | Camera paths persisted separately from ordinary Viewer settings |

The UI reports the final export path or a precise failure. Nothing is written continuously merely
because Viewer is open.

### Replaying camera paths

Camera Paths supports up to 32 named paths and 64 keyframes per path. A keyframe stores the camera
pose, FOV, travel time, dwell time, label, optional selected-node identity, and optional snapshot
capture. Paths can be reordered, duplicated, deleted, looped, paused, stopped, and scrubbed.

Playback interpolates position and FOV smoothly and takes the shortest yaw arc. Manual movement or
mouse input cancels playback immediately while preserving that input. Viewer exit, destination or
session changes, player/camera identity changes, and invalid path data also stop playback and clear
its FOV override. Camera playback never moves the player or a world object.

### Current inspection coverage

Coverage is evidence-backed and intentionally explicit:

| Capability | Status |
| --- | --- |
| Activity, destination, scenario, and bubble context | Supported |
| Spawn sets and spawn points | Supported |
| Local controlled-object handle and copied physics position | Supported when the player observer is ready |
| General live object-system enumeration | Supported as a bounded copy from the occupied-datum iterator |
| Live object-system type classification | Supported; unknown byte values remain explicit |
| Package-backed roster placements and component-slot metadata | Supported with declared and bounded copied counts |
| Primary Wwise listener position | Supported as a copied runtime observation |
| Havok positions and velocities | Supported as bounded copied slot observations; slots are not durable identities |
| Havok trigger observations | Supported with copied positions and overlap counts; authored shape semantics remain unavailable |
| StaticMesh and Speedtree classification | Supported; mesh resources, materials, LODs, and bounds are unavailable |
| Complete entity quaternion, parent/owner handles, world identifier, and bounds | Unavailable; the required ownership and copied layout are not proven |
| Simulation-entity enumeration | Not enumerated separately from object-system handles |
| Audio emitters, voices, buses, and graph edges | Unavailable; only the primary listener producer is enabled |
| Navigation tiles, polygons, and queries | Unavailable |
| Lights and render-owned light parameters | Unavailable |
| Terrain tiles, residency, and LOD | Unavailable |

Package-backed placement nodes describe authored catalog records and do not claim that a live
object-system handle or simulation entity exists. Runtime rows contain copied scalar data only;
Viewer does not retain Wwise, Havok, object, component, or other native gameplay pointers.
Unsupported fields remain absent instead of receiving synthesized values.

Each optional producer reports `installed`, `ready`, `sequence`, `declared_count`, `copied_count`,
`truncated`, and failure state. One unavailable or failed producer does not prevent Viewer from
starting or disable the rest of the inspection graph.

### Using the center views

The center workspace has three modes:

| Mode | Behavior |
| --- | --- |
| **World** | Draws the captured game image with projected pointer-free helpers. |
| **Node Graph** | Shows a selected-node neighborhood by default, with an option for all filtered ownership nodes. Double-click centers a card instead of moving the game camera. |
| **Activity Map** | Browses exactly one optional authored activity graph at a time, with graph navigation and browse-only build disclosure. |

World helpers are an **X-ray overlay — no scene depth** only when validated AABBs actually exist; otherwise the UI reports position helpers and disables the unavailable bounds toggle. Known AABBs are drawn only when a producer
supplies a finite, non-inverted bound. Trigger observations currently provide a center and state only;
the UI labels them **Shape unavailable; center observation only** and never invents extents. Native
engine debug-draw calls, global wireframe mode, depth-aware occlusion, and Havok shape retention are
not enabled.

### Building an optional activity catalog

The converter reads only the required tables from a supplied `manifest.zip` and uses Python's
standard library:

```powershell
python tools/build_activity_graph_catalog.py `
  --input C:\path\to\manifest.zip `
  --output bin\x64\Sunrise\activity-graph-catalog.bin
```

The output is optional and must remain outside source control. Sunrise loads it once from the normal
artifact directory during client initialization. Missing or malformed files produce an Inspector
diagnostic and do not block startup.

Catalogs carry their content build, manifest version, source-table SHA-256 digests, graph/node
references, authored positions, and location-release metadata. The current client target is build
86657. A catalog from another build, including the supplied build-87221 research archive, remains
**Browse only**: it cannot correlate to the current session and its authored positions never become
World viewport transforms, bounds, or edges. Empty `connections` arrays stay empty; linked graphs are
navigation metadata rather than invented node connections.

The converter and native focused tests use synthetic fixtures:

```powershell
python tests/test_build_activity_graph_catalog.py
powershell -ExecutionPolicy Bypass -File tests/run_inspector_upgrade_tests.ps1
```

## WIP

This mod is work in progress. Things might break or work in unexpected ways. There is also currently
a lack of documentation. This will improve over the coming weeks.

## Support Me

Leave a star on this repo.

If you want to support my open source work you can find the means on my
[profile](https://github.com/stanuwu). Also consider donating to charity instead.

All content released under this project is free and open source. If someone is trying to sell you
something you are getting scammed.

## Rules
Issues are for bug reports only.

PRs are for pull requests only.

Do not go and argue/chat there, you can do that on the discord.

## Building

### Windows

Install Visual Studio 2026 with the **Desktop development with C++** workload. The project builds
against the v145 toolset and the 10.0.26100 Windows SDK, so check that both are selected in the
installer.

The easiest route is to open `Sunrise.sln`, select the `Release` `x64` configuration and build.

To build from a command line, use the Developer PowerShell for VS 2026:

1. Clone the repository
```powershell
git clone https://github.com/stanuwu/Sunrise
cd Sunrise
```

2. Build the solution
```powershell
msbuild Sunrise.sln /m /p:Configuration=Release /p:Platform=x64
```

### Linux

Make sure you have `git`, `cmake`, `clang`, `ninja`, `llvm`, and `xwin` installed.

1. Clone the repository
```bash
$ git clone https://github.com/stanuwu/Sunrise
$ cd Sunrise
```

2. Download Windows headers:
```bash
$ xwin --accept-license splat --include-debug-libs --sdk-version 10.0.26100 --output .xwin-cache
```

3. Configure and build the project
```bash
$ cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=$(pwd)/linux-to-win-toolchain.cmake -DCMAKE_BUILD_TYPE=Release
$ cmake --build build --config Release
```

## Contributing

Pull Requests are welcome. Please follow these rules:

- **No Copyrighted Data** - All game data should be extracted at runtime.
- **Code Formatting** - Stick to the provided clang-format and clang-tidy configs.
- **Clean Code** - Try to post readable high quality code, follow the projects existing style of
  comment and add docs.
- **Provide Documentation** - Please explain what you changed, why you changed it and the effects it
  has in detail, it saves me a lot of work.
- **Follow Up** - If something with the PR is not right, I will reply and ask you to fix it.
- **One Feature** - Do not put multiple features into one PR.
- **Complete Implementations** - Do not PR features that are not completed and/or have non functional parts.
- **Server Focus** - For features that are intended to be part of the server, don't abuse client patches. Sometimes its needed but mostly everything should go through the right requests and pushes.

## Credits

### All Contributors

### Dependencies:

- https://github.com/ocornut/imgui
- https://github.com/microsoft/detours

### Artwork:

- [Solus](https://www.youtube.com/@Solus-yt)

### Testing:

- [Ferr](https://x.com/light_fades_awy)
- [gage](https://x.com/_Quolu_)
- [Jenka](https://youtube.com/@jenkad2oob?si=OQpCGeBCEJBS0zHx)
- [Katie](https://github.com/Confetti3)
- [Kody Ivie](https://x.com/Kody_Ivie)
- [Solus](https://www.youtube.com/@Solus-yt)
- Breshi
- [Deltadog55](https://www.youtube.com/@deltadog55)
- Moosh
- [MoveableFormula](https://youtube.com/@movableformula)
- Z
- The Cube17

### Inspiration/Helpful Repos

- https://github.com/v4nguard/tiger-pkg
- https://github.com/cohaereo/alkahest
- https://codeberg.org/V4NGUARD/tachyscope
- https://github.com/MontagueM/D2TagParser
- https://github.com/MontagueM/DestinyUnpackerCPP
- https://github.com/nblockbuster/D2TextureRipper
- https://github.com/v4nguard/tiger-parse
- https://github.com/Demonware-Custom-Server/demonware-cod4
- https://github.com/hosseinpourziyaie/demonware-companion
- https://github.com/jordam/demonbugger
- https://github.com/project-bo4/shield-development
- https://github.com/MontagueM/Charm
- https://github.com/v4nguard/quicktag
- https://github.com/nblockbuster/D2StaticDocs
- https://github.com/MontagueM/D2Maps
- https://github.com/MontagueM/DestinyMapmining
- https://github.com/nblockbuster/tachyscope
- https://github.com/cohaereo/destinydocs
- https://github.com/MontagueM/DestinyUnpacker
- https://github.com/nblockbuster/bungie-lua-decompiler

### Other:

- [Ginsor](https://x.com/GinsorKR) - Gave me some useful pointers

> Want to be added to or removed from the credits? Let me know.

## Content Disclaimer

Sunrise is not:

- A Crack
- A Cheat
- A Custom Server

Everyone needs to provide their own copy of the game, no piracy is happening. The mod does not
connect to any servers, it runs completely locally. We do not offer any servers or services.

## Legal Disclaimer

This project is not for profit. It does not affect live servers or newer versions of the game where
research like this could pose a security risk. No game data will be included in the release so this
is not a copyright violation. This is also not a circumvention of protective measures. Please do not
file any DMCA or other copyright claims against this. Legal action will be taken for abuse of the
copyright system to censor this work.

## AI Disclaimer

AI was used in the creation of this project. If you are not comfortable with the use of AI in
programming projects beware.

AI was NOT used to create any art or creative writing. Only for RE, development and documentation
purposes. All AI work that is publicly released is reviewed by a human. AI is a tool and the user is
responsible for the results it produces.

## Affiliation Disclaimer

This project is not affiliated with Bungie or Sony in any way.
