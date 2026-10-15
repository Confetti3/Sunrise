#pragma once

#include <cstddef>
#include <span>
#include <string_view>

#include "../definition.h"

namespace sunrise::state::mods::discovery {

/** Every mod folder holds its manifest under this name. */
inline constexpr std::wstring_view kManifestFileName = L"mod.json";

/** What one directory listing found. */
struct Result final {
    std::size_t count{};
    /** Folders past `kModCapacity` in name order, not read. */
    std::size_t skipped{};
    /** False on path, enumeration or close failure; no subset may be admitted. */
    bool complete{};
};

/**
 * Lists mod folders, sorted by name, and reads each `mod.json`.
 * Folders whose name starts with a dot are staging folders and are skipped.
 * A parsed manifest leaves its record loaded; every other outcome records a refusal.
 * @param modsDirectory Absolute mods directory. A missing directory is an empty list.
 * @param records Receives one record per folder.
 * @return The number of records written and the number of folders past capacity.
 */
[[nodiscard]] Result discover(std::wstring_view modsDirectory, std::span<Record> records) noexcept;

} // namespace sunrise::state::mods::discovery
