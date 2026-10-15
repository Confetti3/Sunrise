#pragma once

#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <span>

#include "../reader/layout.h"

namespace sunrise::middleware::content::packages::merge {

/** A tag handle indexes 13 bits of entries, so no package holds more. */
inline constexpr std::size_t kEntryLimit = std::size_t{1} << reader::layout::kTagEntryBits;

/** Why a patch cannot extend its base, or why a merge failed. */
enum class Status : std::uint8_t {
    ok,
    unreadable,
    format,
    identity,
    generation,
    entryTable,
    baseBlocks,
    names,
    capacity,
    writeFailed,
};

/** Header metadata digest; merge data names it to bind to one package file. */
using MetadataDigest = std::array<std::byte, reader::layout::kMetadataDigestSize>;

/** Public header fields of one package file. */
struct Identity final {
    std::uint16_t packageId{};
    std::uint16_t patchId{};
    std::uint64_t buildSignature{};
    MetadataDigest metadataDigest{};
};

/**
 * Reads the public header of any version-38 Windows package.
 * @param path Package file.
 * @param output Receives the id, generation, build signature and metadata digest.
 * @return ok, unreadable, or format.
 */
[[nodiscard]] Status identify(const wchar_t* path, Identity& output) noexcept;

/** One patch checked against the base generation it extends. */
struct PatchSummary final {
    std::uint16_t packageId{};
    std::uint16_t patchId{};
    std::uint64_t buildSignature{};
    MetadataDigest metadataDigest{};
    std::uint32_t entryCount{};
    std::uint32_t addedBlocks{};
    /** Entries whose record differs from the base. */
    std::bitset<kEntryLimit> changedEntries{};
};

/**
 * Checks that a patch is the next generation of its base and keeps every base block record.
 * Both files must use the version-38 table layout with variant byte 1.
 * @param basePath Newest generation the patch must extend.
 * @param patchPath Patch file.
 * @param output Receives the patch identity and the entries it changes.
 * @return ok, or the first rule the patch breaks.
 */
[[nodiscard]] Status
inspect(const wchar_t* basePath, const wchar_t* patchPath, PatchSummary& output) noexcept;

/**
 * Writes one generation that holds every patch's changes on top of the base.
 * Later patches win where two change the same entry. Entries patches add to dependency
 * records are unioned. The new blocks are copied in, so the output names no other patch file.
 * @param basePath Newest generation every patch extends.
 * @param patchPaths Patch files in load order. Each must pass `inspect` against the base.
 * @param outputPath Final file path. A sibling temporary is written first.
 * @return ok, or why the output was not written.
 */
[[nodiscard]] Status write(const wchar_t* basePath,
                           std::span<const wchar_t* const> patchPaths,
                           const wchar_t* outputPath) noexcept;

/** Checks cached metadata and copied blocks against the same plan used by the writer. */
[[nodiscard]] bool matches(const wchar_t* basePath,
                           std::span<const wchar_t* const> patchPaths,
                           const wchar_t* cachedPath) noexcept;

/** @return A short readable reason for a mod problem message. */
[[nodiscard]] const char* status_text(Status status) noexcept;

/** @return The status as one lowercase log token. */
[[nodiscard]] const char* status_name(Status status) noexcept;

} // namespace sunrise::middleware::content::packages::merge
