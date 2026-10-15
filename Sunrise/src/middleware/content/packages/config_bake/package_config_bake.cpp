#include <algorithm>
#include <array>

#include "../../../../core/filesystem/temporary_sibling.h"
#include "internal.h"

namespace sunrise::middleware::content::packages::config_bake {
namespace {

using internal::File;
using internal::Prepared;
namespace tables = internal::tables;
namespace layout = internal::layout;

/** Emitted spans cover the output once, in order, including zero padding. */
using Sink = bool (*)(void*, std::uint64_t, std::span<const std::byte>);
struct Output final {
    Sink sink{};
    void* context{};
    std::uint64_t blocksOffset{};
};

/** @param context Output sink. @param index New block ordinal. @param bytes Plaintext block. */
[[nodiscard]] bool
emit_block(void* context, std::uint32_t index, std::span<const std::byte> bytes) {
    const auto& output = *static_cast<Output*>(context);
    return output.sink(
        output.context, output.blocksOffset + std::uint64_t{index} * layout::kBlockSize, bytes);
}

/**
 * Source bytes after the header remain byte-exact; appended gaps are always zero.
 * @param plan Complete deterministic layout with held sources.
 * @param sink Writer or comparer receiving each output range.
 * @param context Borrowed sink state.
 * @return False on any source read or sink failure.
 */
[[nodiscard]] bool emit(const Prepared& plan, Sink sink, void* context) {
    if (!sink(context, 0, plan.header)) {
        return false;
    }
    std::vector<std::byte> buffer(layout::kBlockSize);
    std::uint64_t cursor = layout::kHeaderSize;
    while (cursor < plan.source.size) {
        const auto count = static_cast<std::size_t>(
            (std::min)(std::uint64_t{buffer.size()}, plan.source.size - cursor));
        auto bytes = std::span(buffer).first(count);
        if (!tables::read_exact(plan.source.file.get(), cursor, bytes)
            || !sink(context, cursor, bytes)) {
            return false;
        }
        cursor += count;
    }
    std::fill(buffer.begin(), buffer.end(), std::byte{});
    const auto pad = [&](std::uint64_t end) {
        while (cursor < end) {
            const auto count =
                static_cast<std::size_t>((std::min)(std::uint64_t{buffer.size()}, end - cursor));
            if (!sink(context, cursor, std::span(buffer).first(count))) {
                return false;
            }
            cursor += count;
        }
        return true;
    };
    if (!pad(plan.metadataOffset) || !sink(context, cursor, plan.metadata)) {
        return false;
    }
    cursor += plan.metadata.size();
    if (!pad(plan.blocksOffset)) {
        return false;
    }
    Output output{sink, context, plan.blocksOffset};
    if (!internal::blocks(plan, &emit_block, &output)) {
        return false;
    }
    std::array<std::byte, internal::kDiskAlignment> footer{};
    tables::store<std::uint32_t>(footer, 0, internal::kFooterMarker);
    return sink(context, plan.footer, footer);
}

struct Comparison final {
    HANDLE file{};
    std::vector<std::byte> bytes;
};

/** @param context Held cache and scratch. @param offset File position. @param bytes Expected data.
 */
[[nodiscard]] bool
compare_range(void* context, std::uint64_t offset, std::span<const std::byte> bytes) {
    auto& comparison = *static_cast<Comparison*>(context);
    comparison.bytes.resize(bytes.size());
    return tables::read_exact(comparison.file, offset, comparison.bytes)
           && std::equal(bytes.begin(), bytes.end(), comparison.bytes.begin());
}

/** @param plan Expected bytes. @param file Held cache. @return True on complete byte equality. */
[[nodiscard]] bool compare(const Prepared& plan, HANDLE file) {
    LARGE_INTEGER size{};
    if (GetFileSizeEx(file, &size) == FALSE || size.QuadPart < 0
        || static_cast<std::uint64_t>(size.QuadPart) != plan.fileSize) {
        return false;
    }
    Comparison comparison{file, {}};
    return emit(plan, &compare_range, &comparison);
}

/**
 * Existing hard links and path aliases must not let a cache replace an input file.
 * @param plan Held source identities.
 * @param output Candidate output path.
 * @return True only for an absent file or a distinct existing file.
 */
[[nodiscard]] bool separate_output(const Prepared& plan, const wchar_t* output) noexcept {
    File file(CreateFileW(output,
                          GENERIC_READ,
                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                          nullptr,
                          OPEN_EXISTING,
                          FILE_ATTRIBUTE_NORMAL,
                          nullptr));
    if (!file.valid()) {
        return GetLastError() == ERROR_FILE_NOT_FOUND;
    }
    BY_HANDLE_FILE_INFORMATION candidate{};
    if (GetFileInformationByHandle(file.get(), &candidate) == FALSE) {
        return false;
    }
    const auto different = [&](const internal::Input& input) {
        BY_HANDLE_FILE_INFORMATION source{};
        return GetFileInformationByHandle(input.file.get(), &source) != FALSE
               && (candidate.dwVolumeSerialNumber != source.dwVolumeSerialNumber
                   || candidate.nFileIndexHigh != source.nFileIndexHigh
                   || candidate.nFileIndexLow != source.nFileIndexLow);
    };
    return different(plan.source)
           && std::all_of(plan.parents.begin(), plan.parents.end(), different);
}

/**
 * The temporary remains private until flushed and compared against the complete plan.
 * @param plan Held inputs and exact output layout.
 * @param path Caller-owned final cache path.
 * @return ok only after atomic publication; failure preserves the previous cache.
 */
[[nodiscard]] Status publish(const Prepared& plan, const wchar_t* path) {
    const std::wstring output = core::path::extended(path);
    if (output.empty()) {
        return Status::writeFailed;
    }
    if (!separate_output(plan, output.c_str())) {
        return Status::identity;
    }
    core::path::Buffer temporary;
    if (!core::path::temporary_sibling(output, temporary)) {
        return Status::writeFailed;
    }
    File file(CreateFileW(temporary.chars.data(),
                          GENERIC_READ | GENERIC_WRITE,
                          0,
                          nullptr,
                          CREATE_NEW,
                          FILE_ATTRIBUTE_NORMAL,
                          nullptr));
    if (!file.valid()) {
        return Status::writeFailed;
    }
    bool complete = false;
    try {
        complete = emit(plan, &tables::write_at, file.get())
                   && FlushFileBuffers(file.get()) != FALSE && compare(plan, file.get());
    } catch (...) {
        complete = false;
    }
    complete = file.close() && complete;
    if (complete) {
        complete = core::path::publish_sibling(temporary.chars.data(), output.c_str());
    }
    if (!complete) {
        (void)DeleteFileW(temporary.chars.data());
    }
    return complete ? Status::ok : Status::writeFailed;
}

} // namespace

/** Validation holds raw sources until every entry, site and generated block has been checked. */
Status validate(const wchar_t* source,
                std::span<const Parent> parents,
                std::span<const Value> values) noexcept {
    try {
        Prepared plan;
        return internal::prepare(source, parents, values, {}, plan);
    } catch (...) {
        return Status::capacity;
    }
}

/** Configured values only; no entry is replaced. */
Status write(const wchar_t* source,
             std::span<const Parent> parents,
             std::span<const Value> values,
             const wchar_t* output) noexcept {
    return write(source, parents, values, {}, output);
}

/** A failed preparation never opens the destination for writing. */
Status write(const wchar_t* source,
             std::span<const Parent> parents,
             std::span<const Value> values,
             std::span<const Replacement> replacements,
             const wchar_t* output) noexcept {
    try {
        Prepared plan;
        const auto status = internal::prepare(source, parents, values, replacements, plan);
        return status == Status::ok ? publish(plan, output) : status;
    } catch (...) {
        return Status::capacity;
    }
}

/** Configured values only; no entry is replaced. */
bool matches(const wchar_t* source,
             std::span<const Parent> parents,
             std::span<const Value> values,
             const wchar_t* cached) noexcept {
    return matches(source, parents, values, {}, cached);
}

/** Matches compares the copied source prefix, padding, metadata and all packed block bytes. */
bool matches(const wchar_t* source,
             std::span<const Parent> parents,
             std::span<const Value> values,
             std::span<const Replacement> replacements,
             const wchar_t* cached) noexcept {
    try {
        Prepared plan;
        if (internal::prepare(source, parents, values, replacements, plan) != Status::ok
            || cached == nullptr) {
            return false;
        }
        File file(CreateFileW(core::path::extended(cached).c_str(),
                              GENERIC_READ,
                              FILE_SHARE_READ,
                              nullptr,
                              OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL,
                              nullptr));
        return file.valid() && compare(plan, file.get());
    } catch (...) {
        return false;
    }
}

/** One entry through the batch read. */
Status
read_entry(const wchar_t* path, std::uint32_t index, std::vector<std::byte>& bytes) noexcept {
    bytes.clear();
    std::vector<std::vector<std::byte>> entries;
    const Status status = read_entries(path, std::span(&index, 1), entries);
    if (status == Status::ok) {
        bytes = std::move(entries.front());
    }
    return status;
}

/** Opens the file alone, so only its own plaintext blocks can serve the entries. */
Status read_entries(const wchar_t* path,
                    std::span<const std::uint32_t> indices,
                    std::vector<std::vector<std::byte>>& entries) noexcept {
    try {
        entries.clear();
        Prepared plan;
        Status status = internal::read_inputs(path, {}, plan);
        entries.resize(indices.size());
        for (std::size_t i = 0; status == Status::ok && i < indices.size(); ++i) {
            status = internal::read_entry(plan, indices[i], entries[i]);
        }
        if (status != Status::ok) {
            entries.clear();
        }
        return status;
    } catch (...) {
        entries.clear();
        return Status::capacity;
    }
}

/** The mod list shows the first failed admission rule. */
const char* status_text(Status status) noexcept {
    switch (status) {
    case Status::ok:
        return "ok";
    case Status::unreadable:
        return "config source could not be read";
    case Status::format:
        return "config package layout is unsupported";
    case Status::identity:
        return "config package identity differs";
    case Status::source:
        return "config source bytes differ";
    case Status::layout:
        return "config scalar layout is unsupported";
    case Status::blocks:
        return "config entry needs complete plaintext mod blocks";
    case Status::capacity:
        return "configured package exceeds its limits";
    case Status::writeFailed:
        return "configured cache could not be published";
    }
    return "unknown config package failure";
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
    case Status::source:
        return "source";
    case Status::layout:
        return "layout";
    case Status::blocks:
        return "blocks";
    case Status::capacity:
        return "capacity";
    case Status::writeFailed:
        return "write_failed";
    }
    return "unknown";
}

} // namespace sunrise::middleware::content::packages::config_bake
