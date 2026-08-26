#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <vector>

#include "internal.h"

namespace sunrise::middleware::content::packages::reader {
namespace {

constexpr std::size_t kMiscOffsetField = 0xF0;
constexpr std::size_t kMiscSizeField = 0xF4;
constexpr std::uint32_t kHashArrayMarker = 0x80809FBDU;
constexpr std::uint32_t kHashRowClass = 0x80809D02U;
constexpr std::size_t kHashRowStride = 16;
constexpr std::uint64_t kMaximumHashRows = 300000;
constexpr std::uint32_t kMaximumMiscSize = 16U * 1024U * 1024U;
constexpr std::size_t kBatchRows = 256;
constexpr std::size_t kSeenWords = 1024;

using SeenSet = std::array<std::uint64_t, kSeenWords>;

template <typename Value>
[[nodiscard]] Value field(std::span<const std::byte> bytes, std::size_t offset) noexcept {
    Value value{};
    if (offset <= bytes.size() && sizeof value <= bytes.size() - offset) {
        std::memcpy(&value, bytes.data() + offset, sizeof value);
    }
    return value;
}

[[nodiscard]] bool claim(SeenSet& seen, std::uint16_t packageId) noexcept {
    const std::uint64_t bit = std::uint64_t{1} << (packageId & 63U);
    std::uint64_t& word = seen[packageId >> 6U];
    if ((word & bit) != 0) {
        return false;
    }
    word |= bit;
    return true;
}

[[nodiscard]] bool search_pattern(std::wstring_view directory, Path& search) noexcept {
    const bool separated = !directory.empty() && directory.back() == L'\\';
    const int written = std::swprintf(search.chars.data(),
                                      search.chars.size(),
                                      separated ? L"%.*s*.pkg" : L"%.*s\\*.pkg",
                                      static_cast<int>(directory.size()),
                                      directory.data());
    return written > 0;
}

[[nodiscard]] bool package_family(std::wstring_view leaf, std::string_view wanted) noexcept {
    constexpr std::wstring_view prefix = L"w64_";
    constexpr std::wstring_view extension = L".pkg";
    if (!leaf.starts_with(prefix) || !leaf.ends_with(extension)) {
        return false;
    }
    leaf.remove_suffix(extension.size());
    const std::size_t patch = leaf.rfind(L'_');
    if (patch == std::wstring_view::npos) {
        return false;
    }
    leaf = leaf.substr(0, patch);
    const std::size_t identifier = leaf.rfind(L'_');
    if (identifier == std::wstring_view::npos || identifier <= prefix.size()) {
        return false;
    }
    const std::wstring_view family = leaf.substr(prefix.size(), identifier - prefix.size());
    if (family.size() != wanted.size()) {
        return false;
    }
    for (std::size_t index = 0; index < wanted.size(); ++index) {
        if (family[index] != static_cast<wchar_t>(wanted[index])) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool all_resolved(std::span<const Hash64Value> values) noexcept {
    return std::ranges::all_of(values, [](const Hash64Value& value) {
        return value.hash != 0 && value.resolved;
    });
}

[[nodiscard]] bool stop_requested(StopRequested stopped, void* context) noexcept {
    return stopped != nullptr && stopped(context);
}

[[nodiscard]] bool relative_target(std::size_t field,
                                   std::int64_t relative,
                                   std::size_t size,
                                   std::size_t& target) noexcept {
    if (field > static_cast<std::size_t>((std::numeric_limits<std::int64_t>::max)())) {
        return false;
    }
    const std::int64_t base = static_cast<std::int64_t>(field);
    if ((relative > 0 && base > (std::numeric_limits<std::int64_t>::max)() - relative)
        || (relative < 0 && base < (std::numeric_limits<std::int64_t>::min)() - relative)) {
        return false;
    }
    const std::int64_t value = base + relative;
    if (value < 0 || static_cast<std::uint64_t>(value) > size) {
        return false;
    }
    target = static_cast<std::size_t>(value);
    return true;
}

[[nodiscard]] bool scan_package(std::wstring_view directory,
                                std::uint16_t packageId,
                                std::span<Hash64Value> values,
                                Hash64ScanResult& result,
                                StopRequested stopped,
                                void* stopContext) noexcept {
    Path stem{};
    Path path{};
    std::uint32_t patchIndex = 0;
    std::array<std::byte, layout::kHeaderSize> headerBytes{};
    Header header{};
    if (!find_latest(directory, packageId, stem, patchIndex)
        || !build_path(stem, patchIndex, path) || !read_at(path, 0, headerBytes)
        || !parse_header(headerBytes, header) || header.packageId != packageId) {
        return false;
    }
    const std::uint32_t miscOffset = field<std::uint32_t>(headerBytes, kMiscOffsetField);
    const std::uint32_t miscSize = field<std::uint32_t>(headerBytes, kMiscSizeField);
    if (miscOffset == 0 || miscSize == 0) {
        return true;
    }
    if (miscSize > kMaximumMiscSize || header.entryCount > kEntryCapacity
        || stop_requested(stopped, stopContext)) {
        return false;
    }
    std::vector<std::byte> misc(miscSize);
    std::vector<layout::EntryRecord> entries(header.entryCount);
    if (!read_at(path, miscOffset, misc)
        || !read_at(path, header.entryTable, std::as_writable_bytes(std::span{entries}))) {
        return false;
    }
    ++result.packages;
    std::vector<std::size_t> seenTables;
    for (std::size_t descriptor = 0; descriptor + 16U <= misc.size(); descriptor += 8U) {
        if ((descriptor & 0x7FFU) == 0 && stop_requested(stopped, stopContext)) {
            return false;
        }
        const std::uint64_t count = field<std::uint64_t>(misc, descriptor);
        const std::int64_t relative = field<std::int64_t>(misc, descriptor + 8U);
        std::size_t headerOffset = 0;
        if (count == 0 || count > kMaximumHashRows
            || !relative_target(descriptor + 8U, relative, misc.size(), headerOffset)
            || headerOffset < 4U || headerOffset + 16U > misc.size()
            || field<std::uint32_t>(misc, headerOffset - 4U) != kHashArrayMarker
            || field<std::uint64_t>(misc, headerOffset) != count
            || field<std::uint32_t>(misc, headerOffset + 8U) != kHashRowClass
            || count > (misc.size() - headerOffset - 16U) / kHashRowStride
            || std::ranges::find(seenTables, headerOffset) != seenTables.end()) {
            continue;
        }
        seenTables.push_back(headerOffset);
        const std::size_t rows = headerOffset + 16U;
        result.rows += static_cast<std::size_t>(count);
        for (std::size_t row = 0; row < count; ++row) {
            if ((row % kBatchRows) == 0 && stop_requested(stopped, stopContext)) {
                return false;
            }
            const std::span<const std::byte> record(
                misc.data() + rows + row * kHashRowStride, kHashRowStride);
            const std::uint64_t hash = field<std::uint64_t>(record, 0);
            for (Hash64Value& requested : values) {
                if (requested.hash != hash || hash == 0) {
                    continue;
                }
                const std::uint32_t tag = field<std::uint32_t>(record, 8);
                const std::uint32_t classId = field<std::uint32_t>(record, 12);
                if (tag < layout::kTagBase || classId == 0) {
                    continue;
                }
                const std::uint32_t decoded = tag - layout::kTagBase;
                const std::uint32_t owner = decoded >> layout::kTagEntryBits;
                const std::size_t entry = decoded & layout::kTagEntryMask;
                if (owner != packageId || entry >= entries.size()
                    || entries[entry].reference != classId) {
                    continue;
                }
                if (requested.resolved
                    && (requested.tag != tag || requested.classId != classId)) {
                    return false;
                }
                if (!requested.resolved) {
                    requested.tag = tag;
                    requested.classId = classId;
                    requested.resolved = true;
                    ++result.resolved;
                }
            }
        }
    }
    return true;
}

[[nodiscard]] bool scan_family(std::wstring_view directory,
                               std::string_view family,
                               std::span<Hash64Value> values,
                               Hash64ScanResult& result,
                               StopRequested stopped,
                               void* stopContext) noexcept {
    Path pattern{};
    if (!search_pattern(directory, pattern)) {
        return false;
    }
    WIN32_FIND_DATAW found{};
    const HANDLE enumeration = FindFirstFileW(pattern.chars.data(), &found);
    if (enumeration == INVALID_HANDLE_VALUE) {
        return false;
    }
    SeenSet seen{};
    bool complete = true;
    do {
        if (stop_requested(stopped, stopContext)) {
            complete = false;
            break;
        }
        if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0
            || !package_family(found.cFileName, family)) {
            continue;
        }
        std::uint16_t packageId = 0;
        std::uint32_t patchIndex = 0;
        if (!parse_leaf(found.cFileName, packageId, patchIndex) || !claim(seen, packageId)) {
            continue;
        }
        if (!scan_package(
                directory, packageId, values, result, stopped, stopContext)) {
            complete = false;
            break;
        }
        if (all_resolved(values)) {
            break;
        }
    } while (FindNextFileW(enumeration, &found) != 0);
    if (complete && !all_resolved(values) && GetLastError() != ERROR_NO_MORE_FILES) {
        complete = false;
    }
    return FindClose(enumeration) != FALSE && complete;
}

} // namespace

bool resolve_hash64_scoped(std::wstring_view directory,
                           std::string_view contentFamily,
                           std::span<Hash64Value> values,
                           Hash64ScanResult& result,
                           StopRequested stopped,
                           void* stopContext) noexcept {
    result = {};
    try {
        if (directory.empty() || contentFamily.empty() || values.empty()) {
            return false;
        }
        for (std::size_t index = 0; index < values.size(); ++index) {
            if (values[index].hash == 0) {
                return false;
            }
            values[index].tag = 0;
            values[index].classId = 0;
            values[index].resolved = false;
            for (std::size_t prior = 0; prior < index; ++prior) {
                if (values[prior].hash == values[index].hash) {
                    return false;
                }
            }
        }
        constexpr std::array<std::string_view, 2> shared{"activities", "environments"};
        if (!scan_family(
                directory, contentFamily, values, result, stopped, stopContext)) {
            return false;
        }
        for (const std::string_view family : shared) {
            if (all_resolved(values)) {
                break;
            }
            if (!scan_family(directory, family, values, result, stopped, stopContext)) {
                return false;
            }
        }
        return !stop_requested(stopped, stopContext);
    } catch (...) {
        result = {};
        return false;
    }
}

} // namespace sunrise::middleware::content::packages::reader
