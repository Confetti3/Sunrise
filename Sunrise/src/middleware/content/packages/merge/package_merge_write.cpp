#include <Windows.h>

#include <algorithm>
#include <bit>
#include <limits>
#include <string>
#include <utility>

#include "../../../../core/filesystem/temporary_sibling.h"
#include "../../../crypto/sha1.h"
#include "dependency_records.h"
#include "internal.h"

namespace sunrise::middleware::content::packages::merge {
namespace {

using internal::align;
using internal::HeaderField;
using internal::kDiskAlignment;
using internal::kFooterMarker;
using internal::kStartBlockMask;
using internal::load;
using internal::MetadataField;
using internal::Package;
using internal::store;
using internal::write_at;
using reader::layout::kBlockRecordSize;
using reader::layout::kEntryRecordSize;

/** Stock packages keep a 0x800-byte signature region here; the merge leaves it empty. */
constexpr std::uint32_t kSignatureOffset = 0x800;
/** Stock packages place the misc region here. */
constexpr std::uint32_t kMiscOffset = 0x1000;
/** Stock packages place metadata no earlier than here. */
constexpr std::uint32_t kMetadataMinimumOffset = 0x1800;
/** The temporary output is a sibling with this suffix, removed on failure. */
constexpr std::wstring_view kTemporarySuffix = L".merge.tmp";

/** One patch read for the merge, with its first merged block index. */
struct Input final {
    Package package;
    std::bitset<kEntryLimit> changed{};
    std::uint32_t firstBlock{};
};

/**
 * Replaces the base misc region with the union of every patch's dependency records.
 * @return ok, `names` when a patch changes more than added record entries, or capacity.
 */
[[nodiscard]] Status union_misc(Package& base, std::span<const Input> inputs) noexcept {
    std::vector<std::span<const std::byte>> patches;
    std::vector<std::byte> merged;
    try {
        patches.reserve(inputs.size());
        for (const Input& input : inputs) {
            patches.emplace_back(input.package.misc);
        }
    } catch (...) {
        return Status::capacity;
    }
    const Status status = dependency_records::merge(base.misc, patches, merged);
    if (status == Status::ok) {
        base.misc = std::move(merged);
    }
    return status;
}

/**
 * Reads the base and every patch, and checks each patch against the base.
 * On success the base misc region holds every patch's dependency records.
 * @return ok, or the first failure. `names` when a patch changes the misc region otherwise.
 */
[[nodiscard]] Status read_inputs(const wchar_t* basePath,
                                 std::span<const wchar_t* const> patchPaths,
                                 Package& base,
                                 std::vector<Input>& inputs) noexcept {
    Status status = internal::read_package(basePath, base);
    if (status != Status::ok) {
        return status;
    }
    try {
        inputs.resize(patchPaths.size());
    } catch (...) {
        return Status::unreadable;
    }
    std::uint64_t blocks = base.blockCount;
    for (std::size_t index = 0; index < patchPaths.size(); ++index) {
        Input& input = inputs[index];
        status = internal::read_package(patchPaths[index], input.package);
        if (status == Status::ok) {
            status = internal::check(base, input.package, input.changed);
        }
        if (status != Status::ok) {
            return status;
        }
        input.firstBlock = static_cast<std::uint32_t>(blocks);
        blocks += input.package.blockCount - base.blockCount;
    }
    if (blocks > internal::kBlockLimit) {
        return Status::capacity;
    }
    return union_misc(base, inputs);
}

/**
 * Builds the merged metadata: base tables, grown block table, every patch's entries and blocks.
 * Block file offsets are filled later, once the layout is known.
 * @return ok, or format when the base places its tag pairs where they cannot move.
 */
[[nodiscard]] Status build_metadata(const Package& base,
                                    std::span<const Input> inputs,
                                    std::uint32_t blockCount,
                                    std::vector<std::byte>& output) noexcept {
    try {
        if (!internal::grow_block_table(base, blockCount, output)) {
            return Status::format;
        }
    } catch (...) {
        return Status::unreadable;
    }
    const auto mergedPatch = static_cast<std::uint16_t>(base.patchId + 1U);
    for (const Input& input : inputs) {
        const std::uint32_t move = input.firstBlock - base.blockCount;
        for (std::uint32_t entry = 0; entry < input.package.entryCount; ++entry) {
            if (!input.changed.test(entry)) {
                continue;
            }
            const std::size_t target = MetadataField::kEntryTable + entry * kEntryRecordSize;
            const std::span<const std::byte> record = internal::entry_bytes(input.package, entry);
            std::copy(
                record.begin(), record.end(), output.begin() + static_cast<std::ptrdiff_t>(target));
            auto placement =
                load<std::uint64_t>(output, target + internal::EntryRecordField::kPlacement);
            const std::uint64_t start = placement & kStartBlockMask;
            // Only blocks the patch added move; inherited blocks keep their index.
            if (start >= base.blockCount) {
                placement = (placement & ~kStartBlockMask) | (start + move);
                store<std::uint64_t>(
                    output, target + internal::EntryRecordField::kPlacement, placement);
            }
        }
        for (std::uint32_t block = base.blockCount; block < input.package.blockCount; ++block) {
            const std::size_t target = base.blockTable + (block + move) * kBlockRecordSize;
            const std::span<const std::byte> record = internal::block_bytes(input.package, block);
            std::copy(
                record.begin(), record.end(), output.begin() + static_cast<std::ptrdiff_t>(target));
            store<std::uint16_t>(
                output, target + internal::BlockRecordField::kPatchId, mergedPatch);
        }
    }
    return Status::ok;
}

/** One new block's source and its place in the output. */
struct BlockCopy final {
    const wchar_t* source{};
    std::uint32_t sourceOffset{};
    std::uint32_t size{};
    std::uint64_t targetOffset{};
};

/**
 * Places every new block after the metadata and writes its file offset into its record.
 * @return The footer offset.
 */
[[nodiscard]] std::uint64_t place_blocks(const Package& base,
                                         std::span<const Input> inputs,
                                         std::span<const wchar_t* const> patchPaths,
                                         std::uint64_t firstOffset,
                                         std::vector<std::byte>& metadata,
                                         std::vector<BlockCopy>& copies) noexcept {
    std::uint64_t cursor = firstOffset;
    for (std::size_t index = 0; index < inputs.size(); ++index) {
        const Input& input = inputs[index];
        for (std::uint32_t block = base.blockCount; block < input.package.blockCount; ++block) {
            const std::span<const std::byte> record = internal::block_bytes(input.package, block);
            const std::size_t target =
                base.blockTable + (block - base.blockCount + input.firstBlock) * kBlockRecordSize;
            const auto size = load<std::uint32_t>(record, internal::BlockRecordField::kSize);
            copies.push_back(
                BlockCopy{patchPaths[index],
                          load<std::uint32_t>(record, internal::BlockRecordField::kOffset),
                          size,
                          cursor});
            store<std::uint32_t>(metadata,
                                 target + internal::BlockRecordField::kOffset,
                                 static_cast<std::uint32_t>(cursor));
            cursor = align(cursor + size);
        }
    }
    return cursor;
}

/**
 * Copies every new block from its patch file into the output.
 * @return False when a source cannot be read or the output cannot be written.
 */
[[nodiscard]] bool copy_blocks(HANDLE output, std::span<const BlockCopy> copies) noexcept {
    std::vector<std::byte> buffer;
    for (const BlockCopy& copy : copies) {
        const HANDLE source = CreateFileW(core::path::extended(copy.source).c_str(),
                                          GENERIC_READ,
                                          FILE_SHARE_READ,
                                          nullptr,
                                          OPEN_EXISTING,
                                          FILE_ATTRIBUTE_NORMAL,
                                          nullptr);
        if (source == INVALID_HANDLE_VALUE) {
            return false;
        }
        bool copied = false;
        try {
            buffer.resize(copy.size);
            copied = internal::read_exact(source, copy.sourceOffset, buffer)
                     && write_at(output, copy.targetOffset, buffer);
        } catch (...) {
            copied = false;
        }
        CloseHandle(source);
        if (!copied) {
            return false;
        }
    }
    return true;
}

/**
 * Writes the whole output file to an open, empty handle.
 * @return False on any short write or failed flush.
 */
[[nodiscard]] bool write_file(HANDLE file,
                              std::span<const std::byte> header,
                              std::span<const std::byte> misc,
                              std::uint64_t metadataOffset,
                              std::span<const std::byte> metadata,
                              std::span<const BlockCopy> copies,
                              std::uint64_t footer,
                              std::uint64_t fileSize) noexcept {
    LARGE_INTEGER size{};
    size.QuadPart = static_cast<LONGLONG>(fileSize);
    const std::array<std::byte, sizeof kFooterMarker> marker =
        std::bit_cast<std::array<std::byte, sizeof kFooterMarker>>(kFooterMarker);
    return SetFilePointerEx(file, size, nullptr, FILE_BEGIN) != FALSE && SetEndOfFile(file) != FALSE
           && write_at(file, 0, header) && write_at(file, kMiscOffset, misc)
           && write_at(file, metadataOffset, metadata) && copy_blocks(file, copies)
           && write_at(file, footer, marker) && FlushFileBuffers(file) != FALSE;
}

/** One deterministic merge layout reused by publication and cache validation. */
struct Prepared final {
    std::array<std::byte, reader::layout::kHeaderSize> header{};
    std::vector<std::byte> misc;
    std::vector<std::byte> metadata;
    std::vector<BlockCopy> copies;
    std::uint64_t metadataOffset{};
    std::uint64_t footer{};
    std::uint64_t fileSize{};
};

/** Builds the exact output layout without touching an existing cached file. */
[[nodiscard]] Status prepare(const wchar_t* basePath,
                             std::span<const wchar_t* const> patchPaths,
                             Prepared& output) noexcept {
    Package base;
    std::vector<Input> inputs;
    Status status = read_inputs(basePath, patchPaths, base, inputs);
    if (status != Status::ok) {
        return status;
    }
    const std::uint32_t blockCount =
        inputs.empty()
            ? base.blockCount
            : inputs.back().firstBlock + inputs.back().package.blockCount - base.blockCount;
    std::vector<std::byte> metadata;
    status = build_metadata(base, inputs, blockCount, metadata);
    if (status != Status::ok) {
        return status;
    }
    const std::uint64_t metadataOffset =
        (std::max)(std::uint64_t{kMetadataMinimumOffset}, align(kMiscOffset + base.misc.size()));
    std::vector<BlockCopy> copies;
    std::uint64_t footer = 0;
    try {
        copies.reserve(blockCount - base.blockCount);
        footer = place_blocks(
            base, inputs, patchPaths, align(metadataOffset + metadata.size()), metadata, copies);
    } catch (...) {
        return Status::unreadable;
    }
    const std::uint64_t fileSize = footer + kDiskAlignment;
    if (fileSize > (std::numeric_limits<std::uint32_t>::max)()) {
        return Status::capacity;
    }

    auto& header = output.header;
    header = base.header;
    crypto::sha1::Digest miscDigest{};
    crypto::sha1::Digest metadataDigest{};
    if (!crypto::sha1::hash(base.misc, miscDigest)
        || !crypto::sha1::hash(metadata, metadataDigest)) {
        return Status::writeFailed;
    }
    store<std::uint16_t>(
        header, HeaderField::kPatchId, static_cast<std::uint16_t>(base.patchId + 1U));
    store<std::uint32_t>(header, HeaderField::kSignatureOffset, kSignatureOffset);
    store<std::uint32_t>(header, HeaderField::kBlockCount, blockCount);
    store<std::uint32_t>(header, HeaderField::kMiscOffset, kMiscOffset);
    store<std::uint32_t>(
        header, HeaderField::kMiscSize, static_cast<std::uint32_t>(base.misc.size()));
    std::copy(miscDigest.begin(), miscDigest.end(), header.begin() + HeaderField::kMiscDigest);
    store<std::uint32_t>(
        header, HeaderField::kMetadataOffset, static_cast<std::uint32_t>(metadataOffset));
    store<std::uint32_t>(
        header, HeaderField::kMetadataSize, static_cast<std::uint32_t>(metadata.size()));
    std::copy(metadataDigest.begin(),
              metadataDigest.end(),
              header.begin() + HeaderField::kMetadataDigest);
    store<std::uint32_t>(header, HeaderField::kFooterOffset, static_cast<std::uint32_t>(footer));
    store<std::uint32_t>(header, HeaderField::kFileSize, static_cast<std::uint32_t>(fileSize));

    output.misc = std::move(base.misc);
    output.metadata = std::move(metadata);
    output.copies = std::move(copies);
    output.metadataOffset = metadataOffset;
    output.footer = footer;
    output.fileSize = fileSize;
    return Status::ok;
}

/** Compares stored block bytes with the exact patch ranges selected by the merge. */
[[nodiscard]] bool same_blocks(HANDLE file, std::span<const BlockCopy> copies) noexcept {
    try {
        std::vector<std::byte> expected;
        std::vector<std::byte> actual;
        for (const BlockCopy& copy : copies) {
            expected.resize(copy.size);
            actual.resize(copy.size);
            const HANDLE source = CreateFileW(core::path::extended(copy.source).c_str(),
                                              GENERIC_READ,
                                              FILE_SHARE_READ,
                                              nullptr,
                                              OPEN_EXISTING,
                                              FILE_ATTRIBUTE_NORMAL,
                                              nullptr);
            if (source == INVALID_HANDLE_VALUE) {
                return false;
            }
            const bool read = internal::read_exact(source, copy.sourceOffset, expected);
            const bool closed = CloseHandle(source) != FALSE;
            if (!read || !closed || !internal::read_exact(file, copy.targetOffset, actual)
                || actual != expected) {
                return false;
            }
        }
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace

/** Validates cached structure and copied data against the current merge inputs. */
bool matches(const wchar_t* basePath,
             std::span<const wchar_t* const> patchPaths,
             const wchar_t* cachedPath) noexcept {
    Prepared expected;
    Package actual;
    if (prepare(basePath, patchPaths, expected) != Status::ok) {
        return false;
    }
    const HANDLE file = CreateFileW(core::path::extended(cachedPath).c_str(),
                                    GENERIC_READ,
                                    FILE_SHARE_READ,
                                    nullptr,
                                    OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    LARGE_INTEGER length{};
    std::array<std::byte, sizeof kFooterMarker> marker{};
    const bool valid = internal::read_package(file, actual) == Status::ok
                       && actual.header == expected.header && actual.misc == expected.misc
                       && actual.metadata == expected.metadata
                       && GetFileSizeEx(file, &length) != FALSE && length.QuadPart >= 0
                       && static_cast<std::uint64_t>(length.QuadPart) == expected.fileSize
                       && internal::read_exact(file, expected.footer, marker)
                       && std::bit_cast<std::uint32_t>(marker) == kFooterMarker
                       && same_blocks(file, expected.copies);
    return CloseHandle(file) != FALSE && valid;
}

/** Writes one generation that holds every patch's changes on top of the base. */
Status write(const wchar_t* basePath,
             std::span<const wchar_t* const> patchPaths,
             const wchar_t* outputPath) noexcept {
    Prepared prepared;
    const Status status = prepare(basePath, patchPaths, prepared);
    if (status != Status::ok) {
        return status;
    }

    const std::wstring output = core::path::extended(outputPath);
    if (output.empty()) {
        return Status::writeFailed;
    }
    std::wstring temporary;
    try {
        temporary = output + std::wstring(kTemporarySuffix);
    } catch (...) {
        return Status::writeFailed;
    }
    const HANDLE file = CreateFileW(temporary.c_str(),
                                    GENERIC_WRITE,
                                    0,
                                    nullptr,
                                    CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return Status::writeFailed;
    }
    const bool written = write_file(file,
                                    prepared.header,
                                    prepared.misc,
                                    prepared.metadataOffset,
                                    prepared.metadata,
                                    prepared.copies,
                                    prepared.footer,
                                    prepared.fileSize);
    const bool closed = CloseHandle(file) != FALSE;
    if (!written || !closed || !core::path::publish_sibling(temporary.c_str(), output.c_str())) {
        DeleteFileW(temporary.c_str());
        return Status::writeFailed;
    }
    return Status::ok;
}

} // namespace sunrise::middleware::content::packages::merge
