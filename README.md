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

The `sunrise-viewer` branch adds an editor-style, read-only world inspection workflow directly to
Sunrise. It builds on Sunrise's existing graphics hooks, runtime state, package/content systems, and
Viewer camera; it is not a separate map renderer or an Alkahest port.

Viewer Camera provides detached movement and mouse look with configurable movement speed, boost and
precision movement, persistent Viewer settings, focus-to-selection support, camera coordinates and
copy-position support. HUD and weapon presentation controls remain independent.

World Inspector provides World, Source, and Activity hierarchy modes, structured search, quick
filters, a captured live game viewport with projected spawn markers, selection/focus/hide/isolate
operations, a property inspector, and References/Data/Diagnostics bottom-dock views. The Entities
filter includes the live local controlled object when Sunrise has published its handle and physics
position, plus package-backed placed roster objects and their component-slot descriptors. Hide and
isolate affect inspector helpers only; inspection does not mutate Destiny world objects.

Current inspection coverage is evidence-backed and intentionally explicit:

| Capability | Status |
| --- | --- |
| Activity / destination / scenario / bubble context | Supported |
| Spawn set and spawn-point inspection | Supported |
| Live local controlled-object handle and physics position | Supported when the player observer is ready |
| Package-backed roster placement inspection | Supported for destination-wide and current-bubble groups |
| Roster component-slot metadata | Supported; bounded child preview with full declared counts |
| General live object-system / simulation-entity enumeration | Not currently enumerated |
| Volumes / trigger semantics | Not currently enumerated |
| Additional physics object enumeration | Not currently enumerated |
| Geometry / terrain enumeration | Not currently enumerated |
| Light enumeration | Not currently enumerated |
| Audio emitter enumeration | Not currently enumerated |

Package-backed placement nodes use the dedicated `Placed Object` kind and `Unknown semantic` status.
They represent authored catalog records and do not claim that a live object-system handle or
simulation entity exists. They intentionally have no transform, bounds, runtime identity, or
world-render mutation actions. The live local-player row is separate: it carries only the scalar
controlled-object handle and copied physics position that Sunrise already publishes without
retaining a gameplay pointer.

The Diagnostics view reports provider coverage and the exact world snapshot identity used to build
the graph, including package/map, activity session and revision, activity index, region, bubble,
map-bubble, scenario tag, spawn-set hash, local-player availability, placed-object and slot counts,
stale/deferred state, and catalog readiness. Unknown or unsupported runtime semantics are left
unknown rather than assigned speculative names.

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
