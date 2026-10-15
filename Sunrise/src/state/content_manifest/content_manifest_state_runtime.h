#pragma once

#include <span>
#include <string_view>

#include "definition.h"

namespace sunrise::state::content_manifest {

/**
 * Loads a current cache or extracts one complete installed-package manifest.
 * @param module Loaded Sunrise module, or null to disable cache persistence.
 * @param packagesDirectory Caller-selected installed packages directory.
 * @return True when a checked nonempty catalog is ready in State.
 */
[[nodiscard]] bool initialize(void* module, std::wstring_view packagesDirectory) noexcept;

/**
 * Adds rows for package files outside the installed folder, and gives the catalog a new GUID
 * so the client does not replay a header cache for the old row set. The build fingerprint
 * becomes the combined identity, so server data follows the mods. The installed one stays.
 * @param rows Sorted rows whose names no installed row uses.
 * @param overlayFingerprint Identity of the files behind the rows.
 * @return False when State is not ready or the combined rows are not a valid catalog.
 */
[[nodiscard]] bool apply_overlay(std::span<const Row> rows,
                                 const Fingerprint& overlayFingerprint) noexcept;

/** Clears the generated manifest, fingerprints, GUID, and extraction scratch. */
void shutdown() noexcept;

/**
 * Visits one immutable catalog without leaking State-owned pointers after the call.
 * @param visitor Non-reentrant callback that consumes the borrowed catalog.
 * @param context Caller context passed to the callback.
 * @return False when State is not ready or the callback rejects the view.
 */
[[nodiscard]] bool visit_snapshot(SnapshotVisitor visitor, void* context) noexcept;

/**
 * Copies the identity of the installed rows alone; mods never change it.
 * @param output Receives the fingerprint; unchanged on failure.
 * @return False when State is not ready.
 */
[[nodiscard]] bool installed_fingerprint(Fingerprint& output) noexcept;

/**
 * Copies the catalog identity, which includes the mod rows.
 * @param output Receives the fingerprint; unchanged on failure.
 * @return False when State is not ready.
 */
[[nodiscard]] bool build_fingerprint(Fingerprint& output) noexcept;

} // namespace sunrise::state::content_manifest
