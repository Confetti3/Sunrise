# SQLite account persistence

## Status

This change implements a SQLite-backed persistence path for existing Sunrise account state. It does
not change or remove `settings.json`.

## Feasibility verdict

SQLite is a good fit for mutable account state, but not for every file Sunrise currently owns.
The recommended architecture is hybrid:

- Keep `settings.json` as human-editable bootstrap and recovery configuration.
- Keep the generated build-data cache in its current versioned binary format. It is a dense,
  rebuildable, sequential-read cache and would gain little from relational storage.
- Store mutable account state in SQLite. Characters, inventory, equipment, sockets, profile
  stacks, account settings, and key bindings need atomic updates and durable restart behavior.

The existing runtime already validates complete `AccountState` snapshots before publishing them.
That makes a transactional snapshot boundary practical without changing the game-facing encoders.

## Implemented scope

The branch adds `Sunrise/state.sqlite3` under the existing module-relative artifact directory and
uses the Windows SDK WinSQLite API (`winsqlite/winsqlite3.h` and `winsqlite3.lib`). No SQLite
amalgamation or third-party binary is copied into the repository.

Initialization now follows this order:

1. Open or create `state.sqlite3`.
2. Create or validate schema v1 through `PRAGMA user_version`.
3. Read a complete account snapshot.
4. Reconstruct bounded C++ state and run the existing `account::valid` checks.
5. Initialize State from the restored account.
6. Fall back to the account in `settings.json` if the database is missing, newer than the code,
   malformed, or rejected by installed build data.

Shutdown checkpoints the account before Client hook teardown and writes the final account again
after Client, Server, and Middleware have quiesced. Each save replaces all SQLite rows inside one
`BEGIN IMMEDIATE` transaction. If hook teardown must be retried, the initial checkpoint remains
durable; the prior committed snapshot remains intact if any insert or commit fails.

## Schema v1

`account_state`
: Singleton root, format version, row counts, primary SOID, explicit settings payload, and update
  timestamp.

`dismantle_rewards`
: Ordered economy-policy rows.

`profile_items`
: Ordered account-wide currency, material, and action-source stacks.

`characters`
: Character identity, selection, presentation values, runtime masks, and inventory generation.

`character_items`
: Equipment and character-inventory items. `location` distinguishes semantic equipment slots from
  ordered unequipped inventory rows.

`item_plugs`
: Ordered optional socket-plug entries for each item.

The account settings leaf is encoded into a small versioned payload because it is a fixed scalar
record rather than a useful relational query surface. Every field and optional key binding is
written explicitly in little-endian order; C++ object bytes, padding, and `std::optional` internals
are never copied to disk.

Unsigned 64-bit identifiers and masks are bit-cast into SQLite's signed 64-bit integer domain and
bit-cast back on load. This preserves values such as an all-bits-set acquired-ability mask.

## Compatibility and recovery

- `PRAGMA user_version` versions the relational schema.
- `format_version` versions the account-row contract.
- The settings payload has an independent format version.
- A database with a higher `user_version` is inspected before writable pragmas are enabled, then
  left untouched and ignored by older code.
- A newer account-row or settings-payload format disables persistence writes for the session,
  even when its schema version is still recognized.
- A structurally valid account that cannot initialize against the installed game packages is
  rejected and the authored `settings.json` account is used instead.
- `settings.json` remains unchanged, so deleting or renaming `state.sqlite3`, `state.sqlite3-wal`,
  and `state.sqlite3-shm` restores the previous ephemeral behavior on the next launch.

## Durability behavior

The first implementation writes when Sunrise shutdown begins and again after clean teardown. A
failed pre-teardown checkpoint cancels shutdown while Client, Server, Middleware, and State remain
live, allowing a later attempt to retry the durable write. Once that checkpoint succeeds, normal
restarts remain persistent even when hook teardown needs a retry. WAL mode is requested for future
write-through support, `synchronous=FULL` is used, and a truncate checkpoint is attempted after a
successful final commit.

The public persistence wrapper also exposes `flush()`. It can be called by later top-level mutation
boundaries once the desired write cadence is selected.

## Known limitations

1. A process crash or forced termination can lose changes made after the previous clean shutdown.
   The previously committed database remains valid.
2. The existing State initializer deliberately reseeds character item mutation serials at startup.
   Inventory contents and ordering persist, but those runtime generations are canonicalized again.
3. WinSQLite follows the SQLite version serviced with Windows. A production release that requires
   a pinned SQLite version or extension set should vendor the official amalgamation instead.
4. No automatic SQLite-to-JSON export is included. `settings.json` is recovery/bootstrap input, not
   a live mirror of runtime inventory.
5. Live client preference changes are not yet copied back into `AccountState.settings`; SQLite
   persists the settings State currently owns, while live settings propagation remains separate.
6. The database is designed for one Sunrise process. Cross-process account sharing is not included.

## Validation performed in the implementation environment

- The complete header was syntax-checked as C++20 with strict Clang warnings using API and project
  type stubs.
- The settings payload was round-tripped and re-encoded byte-for-byte; the current payload is 438
  bytes.
- A complete two-character snapshot was saved and restored through SQLite, including high-bit SOIDs,
  profile stacks, equipment, inventory, nullable plugs, settings, and key bindings.
- Schema v1 was executed against SQLite, all six tables were inspected, and both integrity and
  foreign-key checks passed. Newer schema and nested payload versions remain untouched, while a
  deliberately missing plug row is rejected and repaired through the intended fallback path.
- The repository's 100-column formatting contract was checked.

The branch workflow builds and runs `tests/SQLitePersistenceTests.vcxproj` before the DLL build.
That test links the production State validators, compiles the persistence path against Windows SDK
WinSQLite, and repeats complete snapshot, integrity, compatibility, malformed-row, and repair
checks before the Windows Release x64 DLL build.

## Recommended next steps

1. Add debounced `flush()` calls after successful top-level account mutations so a crash loses at
   most a short interval rather than the whole session.
2. Add a build/package fingerprint to the root row and report exactly why a restored snapshot is
   rejected after an installed-build change.
3. Add fault-injection tests for interrupted transactions, malformed child rows, a newer
   `user_version`, and WAL recovery.
4. Add schema v2 only through an explicit migration transaction; never edit schema v1 in place once
   users have databases.
5. Reassess whether to vendor the official SQLite amalgamation before distribution. This is a
   dependency-policy decision, not a blocker for proving the persistence model.

## Non-goals

This change does not migrate the package-derived build cache, embedded JSON resources, activity
configuration, or logging output.
