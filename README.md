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
- Detached Viewer camera and World Inspector

## Runtime Console

Press **F10** to open the bounded in-game console. The shipped binding avoids the default gameplay
keys and can be changed with `client.ui.console_toggle_key`. The menu and console keep separate
visibility, but either surface captures input while open.

Use `console.find <text>` to search registered names, `console.help <name>` for one entry's typed
usage, and `console.clear` to clear scrollback. Commands and variables are registered by the module
that owns their state. Calls run through a bounded single-consumer queue, and queued work is canceled
when its registration or console lifetime ends.

The optional TCP console endpoint is disabled by default. When enabled, it binds only loopback with
exclusive port ownership and trusts local processes: it is a debug/automation surface, not an
authentication boundary. Connections have bounded request sizes and an idle timeout. A request ID
must be nonzero, first in its JSON object, and unique while a result is pending.

## World Browser

The **Worlds** page searches destinations extracted from the user's installed packages. Selecting a
world, bubble, slice set, and optional compatible spawn set configures Sunrise's existing validated
activity redirect for the next load. Spawn sets from packages the destination does not load remain
visible for diagnosis but cannot be selected.

An optional Activity Logic catalog can add browse-only authored names. Those annotations never
authorize or alter a launch selection, and a catalog for another content build remains explicitly
browse-only.

## Viewer and World Inspector

Viewer adds a detached camera, HUD/audio controls, reusable camera paths, and an editor-style World
Inspector. The Inspector presents copied runtime observations and optional authored catalogs as a
searchable graph with spatial helpers, history/compare tools, and schema-versioned JSON/CSV exports.
It does not modify game rendering when nodes are hidden or isolated.

See [INSPECTOR.md](INSPECTOR.md) for architecture, safety limits, supported-build constraints,
optional catalog generation, export interfaces, and the manual verification checklist.

## Bounded mission programs

The current research build includes a fixed-budget, load-time Lua mission compiler. See
[MISSIONS.md](MISSIONS.md) for its exact scope, fail-closed rules, packaging requirement, and tests.
It does not make missions generally supported.

## WIP

This mod is a work in progress. Things might break or work in unexpected ways. There is also currently
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
- **Clean Code** - Try to post readable high-quality code, follow the project's existing style of
  comment and add docs.
- **Provide Documentation** - Please explain what you changed, why you changed it and the effects it
  has in detail, it saves me a lot of work.
- **Follow Up** - If something with the PR is not right, I will reply and ask you to fix it.
- **One Feature** - Do not put multiple features into one PR.
- **Complete Implementations** - Do not PR features that are not completed and/or have non-functional parts.
- **Server Focus** - For features that are intended to be part of the server, don't abuse client patches. Sometimes it's needed but mostly everything should go through the right requests and pushes.

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
