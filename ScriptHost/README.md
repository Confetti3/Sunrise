# Sunrise C# script host foundation

This directory contains a standalone .NET 8 host for authored offline activity experiments. It is a
host framework, capability scanner, and deterministic mission state machine. It is **not** a claim
that Bungie's original Red War host scripts were recovered.

The native Sunrise bridge is deliberately small. It connects to the named pipe, publishes the
client's `WorldPhase`, answers pings, and rejects unimplemented mutation commands. The C# host owns
scenario state, checkpoints, directory probes, managed plugins, and command dispatch.

## Build

```powershell
dotnet build .\ScriptHost\Sunrise.ScriptHost.csproj -c Release
```

## Validate the included prototype

```powershell
dotnet run --project .\ScriptHost\Sunrise.ScriptHost.csproj -- `
  --validate `
  --scenario .\ScriptHost\scripts\red-war-first-mission.prototype.json `
  --binding .\ScriptHost\bindings\build-86657.json
```

## Run in the VM

```powershell
dotnet run --project .\ScriptHost\Sunrise.ScriptHost.csproj -c Release -- `
  --game-root 'F:\depot\depots\1085661\24238629' `
  --sunrise-root 'F:\depot\depots\1085661\24238629\bin\x64' `
  --scenario .\ScriptHost\scripts\red-war-first-mission.prototype.json `
  --binding .\ScriptHost\bindings\build-86657.json `
  --plugin-root .\ScriptHost\plugins `
  --state-root .\ScriptHost\state `
  --hash-binaries
```

Start the C# host before launching Destiny. Sunrise connects to
`\\.\pipe\sunrise-script-host-v1`. Set `SUNRISE_SCRIPT_HOST_DISABLED=1` to disable the bridge, or
set `SUNRISE_SCRIPT_HOST_PIPE` to a different full pipe path or leaf name.

## What works in this draft

- deterministic JSON scenario graph with checkpoints;
- world arrival/transition observation from Sunrise;
- versioned newline-delimited JSON named-pipe protocol;
- explicit build capability manifest;
- managed plugin loading without third-party packages;
- exact-root VM probe report with optional executable/DLL SHA-256;
- failure at the first unavailable native capability instead of silent mission progression;
- built-in `--self-test` and `--validate` modes.

## What is intentionally not faked

- actor allocation, spawn baselines, updates, migration, removal, or slot return;
- enemy navigation, combat policy, encounter population budgets, or encounter adjudication;
- incident schema/index mapping;
- objective/gameplay-switch wire adapters;
- placed-content bubble authority;
- dialogue, cinematic, completion, and reward bindings.

Each item is represented in `bindings/build-86657.json` with the evidence class required to make it
real. A plugin or native bridge revision should only advertise a capability after it has a verified
request/push or client integration, a teardown path, and a runtime test.

## Plugin contract

A plugin assembly in `plugins/` may implement `IHostPlugin`. Capabilities are exclusive: startup
rejects duplicate providers. Plugins receive expanded JSON payloads and the scenario variable
snapshot. Keep build-specific memory work in the native Sunrise module; managed plugins should
prefer protocol/state adapters and offline authoring logic.
