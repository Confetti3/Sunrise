#include <Windows.h>

#include <algorithm>
#include <limits>

#include "../../../../core/filesystem/path.h"
#include "internal.h"

namespace sunrise::middleware::content::packages::merge {
namespace internal {
namespace {

using reader::layout::kBlockRecordSize;
using reader::layout::kBlockSize;
using reader::layout::kBlockTableGap;
using reader::layout::kEntryRecordSize;

/**
 * Checks the typed array prefix in front of one table.
 * @param metadata Metadata region.
 * @param table Offset of the first record.
 * @param elementClass Class the prefix must name.
 * @return True when the marker and class match.
 */
[[nodiscard]] bool valid_prefix(std::span<const std::byte> metadata,
                                std::size_t table,
                                std::uint32_t elementClass) noexcept {
    return table >= ArrayPrefixField::kMarker && table <= metadata.size()
           && load<std::uint32_t>(metadata, table - ArrayPrefixField::kMarker)
                  == reader::layout::kTableArrayMarker
           && load<std::uint32_t>(metadata, table - ArrayPrefixField::kElementClass)
                  == elementClass;
}

/**
 * Finds the entry and block tables in a read metadata region.
 * @return ok, or format when a prefix or a capacity does not fit.
 */
[[nodiscard]] Status locate_tables(Package& output) noexcept {
    const std::span<const std::byte> metadata(output.metadata);
    // The entry table sits at a fixed offset; the block table follows its capacity.
    constexpr std::size_t entryTable = MetadataField::kEntryTable;
    if (!valid_prefix(metadata, entryTable, reader::layout::kEntryTableElementClass)) {
        return Status::format;
    }
    const auto entryCapacity =
        load<std::uint64_t>(metadata, entryTable - ArrayPrefixField::kCapacity);
    if (entryCapacity < output.entryCount || entryCapacity > kEntryLimit) {
        return Status::format;
    }
    const std::size_t blockTable =
        entryTable + static_cast<std::size_t>(entryCapacity) * kEntryRecordSize + kBlockTableGap;
    if (!valid_prefix(metadata, blockTable, reader::layout::kBlockTableElementClass)) {
        return Status::format;
    }
    const auto blockCapacity =
        load<std::uint64_t>(metadata, blockTable - ArrayPrefixField::kCapacity);
    if (blockCapacity < output.blockCount || blockCapacity > kBlockLimit
        || blockTable + static_cast<std::size_t>(blockCapacity) * kBlockRecordSize
               > metadata.size()) {
        return Status::format;
    }
    output.blockTable = blockTable;
    output.blockCapacity = static_cast<std::size_t>(blockCapacity);
    return Status::ok;
}

/**
 * Reads the header, misc and metadata regions of one open package.
 * @return ok, unreadable, or format.
 */
[[nodiscard]] Status read_regions(HANDLE file, Package& output) noexcept {
    if (!read_exact(file, 0, output.header)) {
        return Status::unreadable;
    }
    const std::span<const std::byte> header(output.header);
    if (load<std::uint16_t>(header, HeaderField::kVersion) != reader::layout::kSupportedVersion
        || load<std::uint16_t>(header, HeaderField::kPlatform) != kSupportedPlatform
        || load<std::uint8_t>(header, HeaderField::kVariant) != kSupportedVariant) {
        return Status::format;
    }
    output.packageId = load<std::uint16_t>(header, HeaderField::kPackageId);
    output.patchId = load<std::uint16_t>(header, HeaderField::kPatchId);
    output.entryCount = load<std::uint32_t>(header, HeaderField::kEntryCount);
    output.blockCount = load<std::uint32_t>(header, HeaderField::kBlockCount);
    const auto miscSize = load<std::uint32_t>(header, HeaderField::kMiscSize);
    const auto metadataSize = load<std::uint32_t>(header, HeaderField::kMetadataSize);
    if (output.entryCount == 0 || output.entryCount > kEntryLimit || output.blockCount == 0
        || output.blockCount > kBlockLimit || output.patchId > kPatchLimit
        || miscSize > kRegionLimit || metadataSize > kRegionLimit
        || metadataSize < MetadataField::kEntryTable) {
        return Status::format;
    }
    try {
        output.misc.resize(miscSize);
        output.metadata.resize(metadataSize);
    } catch (...) {
        return Status::unreadable;
    }
    if (!read_exact(file, load<std::uint32_t>(header, HeaderField::kMiscOffset), output.misc)
        || !read_exact(
            file, load<std::uint32_t>(header, HeaderField::kMetadataOffset), output.metadata)) {
        return Status::unreadable;
    }
    if (load<std::uint64_t>(output.metadata, MetadataField::kLayout) != kSupportedMetadataLayout) {
        return Status::format;
    }
    return locate_tables(output);
}

} // namespace

/**
 * Checks one patch against its base and marks the entries it changes.
 * @return ok, or the first rule the patch breaks.
 */
Status
check(const Package& base, const Package& patch, std::bitset<kEntryLimit>& changed) noexcept {
    changed.reset();
    if (patch.packageId != base.packageId) {
        return Status::identity;
    }
    if (patch.patchId != base.patchId + 1U) {
        return Status::generation;
    }
    if (patch.entryCount != base.entryCount) {
        return Status::entryTable;
    }
    const std::size_t inherited = static_cast<std::size_t>(base.blockCount) * kBlockRecordSize;
    if (patch.blockCount < base.blockCount
        || !std::equal(base.metadata.begin() + static_cast<std::ptrdiff_t>(base.blockTable),
                       base.metadata.begin()
                           + static_cast<std::ptrdiff_t>(base.blockTable + inherited),
                       patch.metadata.begin() + static_cast<std::ptrdiff_t>(patch.blockTable))) {
        return Status::baseBlocks;
    }
    for (std::uint32_t block = base.blockCount; block < patch.blockCount; ++block) {
        // An added block's bytes must live in the patch file itself.
        const std::span<const std::byte> record = block_bytes(patch, block);
        const auto size = load<std::uint32_t>(record, BlockRecordField::kSize);
        if (size == 0 || size > kBlockSize
            || load<std::uint16_t>(record, BlockRecordField::kPatchId) != patch.patchId) {
            return Status::format;
        }
    }
    for (std::uint32_t index = 0; index < patch.entryCount; ++index) {
        const std::span<const std::byte> before = entry_bytes(base, index);
        const std::span<const std::byte> after = entry_bytes(patch, index);
        if (std::equal(before.begin(), before.end(), after.begin())) {
            continue;
        }
        changed.set(index);
        reader::layout::EntryRecord record{};
        std::memcpy(&record, after.data(), sizeof record);
        const reader::layout::EntryPlacement place = reader::layout::placement(record);
        const std::uint64_t end = place.startOffset + static_cast<std::uint64_t>(place.size);
        const std::uint64_t lastBlock = place.startBlock + (end == 0 ? 0 : (end - 1) / kBlockSize);
        // A merge moves only added blocks, so an entry may not span inherited and added ones.
        const bool startsInherited = place.startBlock < base.blockCount;
        if (lastBlock >= patch.blockCount || (startsInherited && lastBlock >= base.blockCount)) {
            return Status::entryTable;
        }
    }
    return Status::ok;
}

/** Reads an exact byte range from one file. */
bool read_exact(void* file, std::uint64_t offset, std::span<std::byte> output) noexcept {
    std::size_t done = 0;
    while (done < output.size()) {
        ULARGE_INTEGER position{};
        position.QuadPart = offset + done;
        OVERLAPPED overlapped{};
        overlapped.Offset = position.LowPart;
        overlapped.OffsetHigh = position.HighPart;
        const DWORD request = static_cast<DWORD>(
            (std::min)(output.size() - done, std::size_t{(std::numeric_limits<DWORD>::max)()}));
        DWORD received = 0;
        if (ReadFile(
                static_cast<HANDLE>(file), output.data() + done, request, &received, &overlapped)
                == FALSE
            || received == 0) {
            return false;
        }
        done += received;
    }
    return true;
}

/** Writes one byte range at an absolute file offset. */
bool write_at(void* file, std::uint64_t offset, std::span<const std::byte> bytes) noexcept {
    if (bytes.empty()) {
        return true;
    }
    if (bytes.size() > (std::numeric_limits<DWORD>::max)()) {
        return false;
    }
    ULARGE_INTEGER position{};
    position.QuadPart = offset;
    OVERLAPPED range{};
    range.Offset = position.LowPart;
    range.OffsetHigh = position.HighPart;
    DWORD written{};
    return WriteFile(static_cast<HANDLE>(file),
                     bytes.data(),
                     static_cast<DWORD>(bytes.size()),
                     &written,
                     &range)
               != FALSE
           && written == bytes.size();
}

/** Copies the base metadata with a grown block table and a moved tag-pair pointer. */
bool grow_block_table(const Package& base, std::size_t blockCount, std::vector<std::byte>& output) {
    const std::size_t oldTail = base.blockTable + base.blockCapacity * kBlockRecordSize;
    const std::size_t capacity = (std::max)(base.blockCapacity, blockCount);
    const std::size_t shift = (capacity - base.blockCapacity) * kBlockRecordSize;
    const bool tagPairs = load<std::uint64_t>(base.metadata, MetadataField::kTagPairCount) != 0;
    const auto relative = load<std::uint64_t>(base.metadata, MetadataField::kTagPairPointer);
    if (tagPairs) {
        // The pointer is relative to its own field and must land past the old block table.
        const std::uint64_t data =
            relative + MetadataField::kTagPairPointer + ArrayPrefixField::kCapacity;
        if (data < oldTail || data > base.metadata.size()) {
            return false;
        }
    }
    output.assign(base.metadata.size() + shift, std::byte{0});
    std::copy_n(base.metadata.begin(), oldTail, output.begin());
    std::copy(base.metadata.begin() + static_cast<std::ptrdiff_t>(oldTail),
              base.metadata.end(),
              output.begin() + static_cast<std::ptrdiff_t>(oldTail + shift));
    store<std::uint64_t>(output, MetadataField::kTotalSize, output.size());
    store<std::uint64_t>(output, MetadataField::kBlockCount, blockCount);
    store<std::uint64_t>(output, base.blockTable - ArrayPrefixField::kCapacity, capacity);
    if (tagPairs) {
        store<std::uint64_t>(output, MetadataField::kTagPairPointer, relative + shift);
    }
    return true;
}

/** Reads and checks one variant-1 package's header, misc and metadata regions. */
Status read_package(void* file, Package& output) noexcept {
    output = Package{};
    return read_regions(static_cast<HANDLE>(file), output);
}

/** Reads and checks one variant-1 package's header, misc and metadata regions. */
Status read_package(const wchar_t* path, Package& output) noexcept {
    output = Package{};
    const HANDLE file = CreateFileW(core::path::extended(path).c_str(),
                                    GENERIC_READ,
                                    FILE_SHARE_READ,
                                    nullptr,
                                    OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return Status::unreadable;
    }
    const Status status = read_regions(file, output);
    CloseHandle(file);
    return status;
}

/** @return The 16-byte entry record `index`. */
std::span<const std::byte> entry_bytes(const Package& package, std::uint32_t index) noexcept {
    return std::span(package.metadata)
        .subspan(MetadataField::kEntryTable + static_cast<std::size_t>(index) * kEntryRecordSize,
                 kEntryRecordSize);
}

/** @return The 48-byte block record `index`. */
std::span<const std::byte> block_bytes(const Package& package, std::uint32_t index) noexcept {
    return std::span(package.metadata)
        .subspan(package.blockTable + static_cast<std::size_t>(index) * kBlockRecordSize,
                 kBlockRecordSize);
}

} // namespace internal

/** Reads the public header of any version-38 Windows package. */
Status identify(const wchar_t* path, Identity& output) noexcept {
    output = {};
    const HANDLE file = CreateFileW(core::path::extended(path).c_str(),
                                    GENERIC_READ,
                                    FILE_SHARE_READ,
                                    nullptr,
                                    OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return Status::unreadable;
    }
    std::array<std::byte, reader::layout::kHeaderSize> header{};
    const bool read = internal::read_exact(file, 0, header);
    CloseHandle(file);
    if (!read) {
        return Status::unreadable;
    }
    const std::span<const std::byte> bytes(header);
    using internal::HeaderField;
    using internal::load;
    if (load<std::uint16_t>(bytes, HeaderField::kVersion) != reader::layout::kSupportedVersion
        || load<std::uint16_t>(bytes, HeaderField::kPlatform) != internal::kSupportedPlatform) {
        return Status::format;
    }
    output.packageId = load<std::uint16_t>(bytes, HeaderField::kPackageId);
    output.patchId = load<std::uint16_t>(bytes, HeaderField::kPatchId);
    output.buildSignature = load<std::uint64_t>(bytes, HeaderField::kBuildSignature);
    std::copy_n(header.begin() + HeaderField::kMetadataDigest,
                output.metadataDigest.size(),
                output.metadataDigest.begin());
    return Status::ok;
}

/** Checks that a patch is the next generation of its base. */
Status inspect(const wchar_t* basePath, const wchar_t* patchPath, PatchSummary& output) noexcept {
    output = {};
    internal::Package base;
    internal::Package patch;
    Status status = internal::read_package(basePath, base);
    if (status == Status::ok) {
        status = internal::read_package(patchPath, patch);
    }
    if (status == Status::ok) {
        status = internal::check(base, patch, output.changedEntries);
    }
    if (status != Status::ok) {
        return status;
    }
    output.packageId = patch.packageId;
    output.patchId = patch.patchId;
    output.buildSignature =
        internal::load<std::uint64_t>(patch.header, internal::HeaderField::kBuildSignature);
    std::copy_n(patch.header.begin() + internal::HeaderField::kMetadataDigest,
                output.metadataDigest.size(),
                output.metadataDigest.begin());
    output.entryCount = patch.entryCount;
    output.addedBlocks = patch.blockCount - base.blockCount;
    return Status::ok;
}

/** @return A short readable reason for a mod problem message. */
const char* status_text(Status status) noexcept {
    switch (status) {
    case Status::ok:
        return "ok";
    case Status::unreadable:
        return "the file could not be read";
    case Status::format:
        return "it is not a supported package file";
    case Status::identity:
        return "it belongs to another package id";
    case Status::generation:
        return "it was built for another patch generation";
    case Status::entryTable:
        return "its entry table does not match the base package";
    case Status::baseBlocks:
        return "it was built against a different base package";
    case Status::names:
        return "it changes package names or dependency records in a way that cannot be merged";
    case Status::capacity:
        return "the merged package is too large";
    case Status::writeFailed:
        return "the merged file could not be written";
    }
    return "unknown";
}

/** @return The status as one lowercase log token. */
const char* status_name(Status status) noexcept {
    switch (status) {
    case Status::ok:
        return "ok";
    case Status::unreadable:
        return "unreadable";
    case Status::format:
        return "format";
    case Status::identity:
        return "identity";
    case Status::generation:
        return "generation";
    case Status::entryTable:
        return "entry_table";
    case Status::baseBlocks:
        return "base_blocks";
    case Status::names:
        return "names";
    case Status::capacity:
        return "capacity";
    case Status::writeFailed:
        return "write_failed";
    }
    return "unknown";
}

} // namespace sunrise::middleware::content::packages::merge
