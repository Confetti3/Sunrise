#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace sunrise::middleware::content::packages::table_merge {

/** Why a sidecar was ignored, or why an entry or the row layer fell back. */
enum class Status : std::uint8_t {
    ok,
    format,
    version,
    binding,
    stock,
    sites,
    mismatch,
    pointer,
    order,
    duplicate,
    pairs,
    limit,
    capacity,
    unsupported,
};

class Sidecar;

/** One mod's copy of one entry. */
struct Input final {
    /** Identifies the mod in warnings and shifts; input order, not this value, is load order. */
    std::size_t mod{};
    /** Parsed merge data of the mod's package file, or null when it has none. */
    const Sidecar* sidecar{};
    /** The entry bytes from the mod's own package file. */
    std::span<const std::byte> bytes;
};

/** Every mod copy of one entry, in load order. */
struct EntryInputs final {
    std::uint16_t packageId{};
    std::uint32_t entryIndex{};
    std::vector<Input> inputs;
};

/** New bytes for one entry of the served package. */
struct Replacement final {
    std::uint16_t packageId{};
    std::uint32_t entryIndex{};
    std::vector<std::byte> bytes;
};

/** An entry served later-wins. */
struct Fallback final {
    std::uint16_t packageId{};
    std::uint32_t entryIndex{};
    Status reason{};
};

/** Bytes two mods changed differently; the later mod's values were kept. */
struct Conflict final {
    std::uint16_t packageId{};
    std::uint32_t entryIndex{};
    std::size_t earlierMod{};
    std::size_t laterMod{};
    std::uint32_t bytes{};
};

/** A mod whose added rows in one table were lost to a later mod. */
struct Dropped final {
    std::size_t mod{};
    std::uint32_t tableTag{};
};

/** A sidecar the merge did not use. */
struct Ignored final {
    std::size_t mod{};
    std::uint16_t packageId{};
    Status reason{};
};

/** Everything one merge run decided. */
struct Outcome final {
    /** Sorted by package id, then entry index. */
    std::vector<Replacement> replacements;
    std::vector<Fallback> fallbacks;
    std::vector<Conflict> conflicts;
    std::vector<Dropped> dropped;
    std::vector<Ignored> ignored;
    /** ok while row tables merge; otherwise why every row table fell back. */
    Status rows{Status::ok};
    /** Tag of the table that turned the row layer off. */
    std::uint32_t rowsTable{};
};

/**
 * Merges every entry that several mods change, and shifts the row numbers mods added.
 * Pure: reads only the given spans and sidecars.
 * @param entries Entries to merge, each with its mod copies in load order.
 * @return Replacements, fallbacks and the warnings to raise.
 */
[[nodiscard]] Outcome merge(std::span<const EntryInputs> entries) noexcept;

/** @return The status as one lowercase log token. */
[[nodiscard]] const char* status_name(Status status) noexcept;

/** @return A short readable reason for a mod warning. */
[[nodiscard]] const char* status_text(Status status) noexcept;

} // namespace sunrise::middleware::content::packages::table_merge
