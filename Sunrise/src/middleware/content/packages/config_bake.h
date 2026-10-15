#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "../../../state/mods/definition.h"

namespace sunrise::middleware::content::packages::config_bake {

/** A raw older generation supplied by a mod, never an installed encrypted source. */
struct Parent final {
    std::uint16_t patchId{};
    std::wstring path;
};

/** One verified scalar recipe and the exact startup value to apply. */
struct Value final {
    state::mods::ConfigBinding site{};
    state::mods::ConfigValue value{};
};

/** One whole entry the output replaces. The record keeps its type and gets a new placement. */
struct Replacement final {
    std::uint32_t index{};
    std::span<const std::byte> bytes;
};

/** A failed required binding prevents publication of the staged overlay. */
enum class Status : std::uint8_t {
    ok,
    unreadable,
    format,
    identity,
    source,
    layout,
    blocks,
    capacity,
    writeFailed,
};

/**
 * Validates current logical entries and their complete plaintext mod-owned block ranges.
 * @param sourcePath Raw newest package file.
 * @param parents Mod-supplied older generations of this same package family.
 * @param values Typed sites for entries in this package.
 * @return ok only when every source digest and scalar site matches.
 */
[[nodiscard]] Status validate(const wchar_t* sourcePath,
                              std::span<const Parent> parents,
                              std::span<const Value> values) noexcept;

/** Same as the overload below, with no replacements. */
[[nodiscard]] Status write(const wchar_t* sourcePath,
                           std::span<const Parent> parents,
                           std::span<const Value> values,
                           const wchar_t* outputPath) noexcept;

/**
 * Packs replaced and configured entry copies and publishes a flushed sibling.
 * Values on a replaced entry must name the replacement's SHA-256 and size.
 * @param sourcePath Raw newest package file.
 * @param parents Mod-supplied older generations.
 * @param values Exact startup recipes and values; may be empty when there are replacements.
 * @param replacements Whole entries, sorted by unique index, each at most the entry byte limit.
 * @param outputPath Final cache file.
 * @return ok only after the complete output is verified and published.
 */
[[nodiscard]] Status write(const wchar_t* sourcePath,
                           std::span<const Parent> parents,
                           std::span<const Value> values,
                           std::span<const Replacement> replacements,
                           const wchar_t* outputPath) noexcept;

/** Same as the overload below, with no replacements. */
[[nodiscard]] bool matches(const wchar_t* sourcePath,
                           std::span<const Parent> parents,
                           std::span<const Value> values,
                           const wchar_t* cachedPath) noexcept;

/**
 * Checks a cached file against the same deterministic preparation used by write.
 * @param sourcePath Raw newest package file.
 * @param parents Mod-supplied older generations.
 * @param values Exact startup recipes and values.
 * @param replacements Whole entries, as given to write.
 * @param cachedPath Existing cache file.
 * @return True when metadata and bytes match the complete current plan.
 */
[[nodiscard]] bool matches(const wchar_t* sourcePath,
                           std::span<const Parent> parents,
                           std::span<const Value> values,
                           std::span<const Replacement> replacements,
                           const wchar_t* cachedPath) noexcept;

/**
 * Reads one entry that lies wholly in plaintext blocks the file itself holds.
 * @param path Raw package file.
 * @param index Entry index.
 * @param bytes Receives the entry bytes.
 * @return ok, or blocks when any fragment is encrypted, compressed or in another file.
 */
[[nodiscard]] Status
read_entry(const wchar_t* path, std::uint32_t index, std::vector<std::byte>& bytes) noexcept;

/**
 * Reads several entries after opening and verifying the file once.
 * @param path Raw package file.
 * @param indices Entry indices.
 * @param entries Receives one entry per index, in order; empty on failure.
 * @return ok, or the first failure read_entry would return.
 */
[[nodiscard]] Status read_entries(const wchar_t* path,
                                  std::span<const std::uint32_t> indices,
                                  std::vector<std::vector<std::byte>>& entries) noexcept;

/** @param status Failed operation. @return A short mod-list diagnostic. */
[[nodiscard]] const char* status_text(Status status) noexcept;

/** @return The status as one lowercase log token. */
[[nodiscard]] const char* status_name(Status status) noexcept;

} // namespace sunrise::middleware::content::packages::config_bake
