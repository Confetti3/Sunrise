#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
#include <vector>

#include "../reader/layout.h"
#include "table_merge.h"

namespace sunrise::middleware::content::packages::table_merge {

/** The bytes 'S','M','R','G' read as a little-endian u32. */
inline constexpr std::uint32_t kSidecarMagic = 0x47524D53U;
/** Any layout change bumps this; other versions are ignored. */
inline constexpr std::uint16_t kSidecarVersion = 2;
/** Bound on one sidecar file, so a damaged file cannot exhaust memory. */
inline constexpr std::size_t kSidecarByteLimit = std::size_t{64} << 20;
/** OpRecord::parentOp value of an op that sits directly in the entry. */
inline constexpr std::uint32_t kTopLevelOp = 0xFFFFFFFFU;
/** ShapeField::ref value of an owned array whose elements hold no pointers. */
inline constexpr std::uint16_t kNoElementShape = 0xFFFF;
/** SiteRecord::target bit that names an op of the same entry instead of a target. */
inline constexpr std::uint16_t kOpLocalTarget = 0x8000;
/** Low bits of an op-local site target: the op index. */
inline constexpr std::uint16_t kOpLocalMask = kOpLocalTarget - 1U;
/** Bytes of a typed array descriptor {u64 count, i64 rel}. */
inline constexpr std::size_t kDescriptorSize = 16;
/** Bytes of a row or object pointer, an i64 counted from its own field. */
inline constexpr std::size_t kRowPointerSize = 8;
/** OpRecord::flags bit: no two merged rows may share a key. */
inline constexpr std::uint8_t kUniqueKey = 0x01;
/** OpRecord::flags bit: merged keys must stay strictly ascending. */
inline constexpr std::uint8_t kOrdered = 0x02;
/** SiteRecord::flags bit: the site value is signed. */
inline constexpr std::uint8_t kSignedSite = 0x01;
/** Bytes of the package header digest a sidecar is bound to. */
using reader::layout::kMetadataDigestSize;
/** Bytes of one SHA-256 digest. */
inline constexpr std::size_t kStockDigestSize = 32;

/** How the runtime may merge one entry. */
enum class EntryKind : std::uint8_t {
    opaque = 0,
    table = 1,
    manifest = 2,
    added = 3,
};

/** What one shape field holds. */
enum class FieldKind : std::uint8_t {
    /** A 16-byte typed array descriptor {i64 count, i64 rel}. */
    ownedArray = 1,
    /** An 8-byte self-relative pointer to a row start. */
    rowPointer = 2,
    /** An 8-byte self-relative pointer to an object outside every array. */
    objectPointer = 3,
};

/** @return The bytes one shape field covers in its row. */
[[nodiscard]] constexpr std::size_t field_size(FieldKind kind) noexcept {
    return kind == FieldKind::ownedArray ? kDescriptorSize : kRowPointerSize;
}

/** How one op's rows merge. */
enum class OpKind : std::uint8_t {
    append = 1,
    sortedMap = 2,
};

/** Sidecar file header. */
struct FileHeader final {
    std::uint32_t magic{};
    std::uint16_t version{};
    std::uint16_t flags{};
    std::uint16_t packageId{};
    std::uint16_t patchId{};
    /** Copy of the package header's metadata digest. */
    std::array<std::byte, kMetadataDigestSize> metadataDigest{};
    std::uint32_t targetCount{};
    std::uint32_t shapeCount{};
    std::uint32_t entryCount{};
    std::uint32_t reserved{};
};

/** One index space: a top-level append op of an installed table entry. */
struct TargetRecord final {
    std::uint32_t tableTag{};
    std::uint32_t descriptorOffset{};
    std::uint32_t stockCount{};
    /** This mod's final row count. */
    std::uint32_t modCount{};
};

/** Row layout header; its fields follow it. */
struct ShapeHeader final {
    std::uint32_t stride{};
    std::uint32_t fieldCount{};
};

/** One pointer-bearing field of a row. */
struct ShapeField final {
    std::uint32_t rowOffset{};
    FieldKind kind{};
    std::uint8_t reserved{};
    /** Element shape of an owned array, op of a row pointer, kNoElementShape for an object. */
    std::uint16_t ref{};
};

/** One entry's merge description; diffs, ops and sites follow it. */
struct EntryHeader final {
    std::uint32_t entryIndex{};
    EntryKind kind{};
    std::array<std::uint8_t, 3> reserved{};
    std::uint32_t stockSize{};
    /** Must equal the entry's size in the package. */
    std::uint32_t builtSize{};
    std::array<std::byte, kStockDigestSize> stockSha256{};
    std::uint32_t diffCount{};
    std::uint32_t opCount{};
    std::uint32_t siteCount{};
    std::uint32_t reserved2{};
};

/** One changed stock range; its old bytes follow it. */
struct DiffHeader final {
    std::uint32_t offset{};
    std::uint32_t length{};
};

/** One typed array the merge rebuilds. */
struct OpRecord final {
    OpKind kind{};
    std::uint8_t flags{};
    /** 0, or 4 for a u32 key. */
    std::uint8_t keyWidth{};
    std::uint8_t reserved{};
    std::uint32_t keyOffset{};
    /** kTopLevelOp, or the op whose row holds this descriptor. */
    std::uint32_t parentOp{};
    std::uint32_t parentRow{};
    /** Entry offset at top level, else offset inside the parent row. */
    std::uint32_t descriptorOffset{};
    std::uint32_t shape{};
    std::uint32_t stockCount{};
    std::uint32_t modCount{};
    /** Largest allowed merged count. */
    std::uint32_t limit{};
    /** 0, or the id shared by tables whose rows are added in step. */
    std::uint32_t pairGroup{};
};

/** One field that holds the number of a row this mod added. */
struct SiteRecord final {
    std::uint32_t offset{};
    /** 1, 2 or 4 bytes. */
    std::uint8_t width{};
    std::uint8_t flags{};
    /** A targets[] index, or kOpLocalTarget | op. */
    std::uint16_t target{};
};

/** Wire sizes of the version 2 records. */
struct WireSize final {
    // The file is packed with no padding, so each record must match its size here.
    static constexpr std::size_t kFileHeader = 48;
    static constexpr std::size_t kTargetRecord = 16;
    static constexpr std::size_t kShapeHeader = 8;
    static constexpr std::size_t kShapeField = 8;
    static constexpr std::size_t kEntryHeader = 64;
    static constexpr std::size_t kDiffHeader = 8;
    static constexpr std::size_t kOpRecord = 40;
    static constexpr std::size_t kSiteRecord = 8;
};
static_assert(sizeof(FileHeader) == WireSize::kFileHeader);
static_assert(sizeof(TargetRecord) == WireSize::kTargetRecord);
static_assert(sizeof(ShapeHeader) == WireSize::kShapeHeader);
static_assert(sizeof(ShapeField) == WireSize::kShapeField);
static_assert(sizeof(EntryHeader) == WireSize::kEntryHeader);
static_assert(sizeof(DiffHeader) == WireSize::kDiffHeader);
static_assert(sizeof(OpRecord) == WireSize::kOpRecord);
static_assert(sizeof(SiteRecord) == WireSize::kSiteRecord);
static_assert(std::is_trivially_copyable_v<FileHeader> && std::is_trivially_copyable_v<EntryHeader>
              && std::is_trivially_copyable_v<OpRecord>);

/** One row layout. */
struct Shape final {
    std::uint32_t stride{};
    std::vector<ShapeField> fields;
};

/** One stock range a mod changed. `old` points into the owning sidecar's file bytes. */
struct Diff final {
    std::uint32_t offset{};
    std::span<const std::byte> old;
};

/** One parsed entry description. */
struct Entry final {
    EntryHeader header{};
    std::vector<Diff> diffs;
    std::vector<OpRecord> ops;
    std::vector<SiteRecord> sites;
};

/** One parsed sidecar. It owns the file bytes its diffs point into, so it can move but not copy. */
class Sidecar final {
public:
    Sidecar() = default;
    Sidecar(const Sidecar&) = delete;
    Sidecar& operator=(const Sidecar&) = delete;
    Sidecar(Sidecar&&) noexcept = default;
    Sidecar& operator=(Sidecar&&) noexcept = default;

    /** @return The entry description for `index`, or null when the file has none. */
    [[nodiscard]] const Entry* entry(std::uint32_t index) const noexcept;
    [[nodiscard]] std::span<const TargetRecord> targets() const noexcept {
        return targets_;
    }
    /** @return Shape `index`, or null when out of range. */
    [[nodiscard]] const Shape* shape(std::uint32_t index) const noexcept {
        return index < shapes_.size() ? &shapes_[index] : nullptr;
    }
    [[nodiscard]] std::span<const Entry> entries() const noexcept {
        return entries_;
    }
    [[nodiscard]] const FileHeader& header() const noexcept {
        return header_;
    }

private:
    friend Status parse(std::vector<std::byte> file,
                        std::uint16_t packageId,
                        std::uint16_t patchId,
                        std::span<const std::byte, kMetadataDigestSize> metadataDigest,
                        Sidecar& output) noexcept;

    std::vector<std::byte> file_;
    FileHeader header_{};
    std::vector<TargetRecord> targets_;
    std::vector<Shape> shapes_;
    std::vector<Entry> entries_;
};

/**
 * Parses and checks one sidecar against the package it sits next to.
 * @param file Whole file, moved into the output.
 * @param packageId Id of the package file.
 * @param patchId Generation of the package file.
 * @param metadataDigest The package header's metadata digest.
 * @param output Receives the parsed sidecar; left empty on failure.
 * @return ok, or why the file must be ignored.
 */
[[nodiscard]] Status parse(std::vector<std::byte> file,
                           std::uint16_t packageId,
                           std::uint16_t patchId,
                           std::span<const std::byte, kMetadataDigestSize> metadataDigest,
                           Sidecar& output) noexcept;

} // namespace sunrise::middleware::content::packages::table_merge
