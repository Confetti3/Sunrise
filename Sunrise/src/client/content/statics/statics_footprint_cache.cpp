#include "statics_footprint_cache.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

#include "../../../core/filesystem/path.h"

namespace sunrise::client::content::statics::cache {
namespace {

constexpr std::array<std::byte, 8> kMagic{std::byte{'S'},
                                          std::byte{'S'},
                                          std::byte{'T'},
                                          std::byte{'C'},
                                          std::byte{'A'},
                                          std::byte{'T'},
                                          std::byte{'0'},
                                          std::byte{'1'}};
constexpr std::size_t kHeaderSize = 144;
constexpr std::size_t kRecordSize = 40;
constexpr std::size_t kFamilyCapacity = 32;
constexpr std::size_t kPackageCapacity = 8;
constexpr std::size_t kMaximumFileSize = kHeaderSize + kRecordSize * kFootprintCapacity;

template <typename Value>
void put(std::span<std::byte> output, std::size_t offset, const Value& value) noexcept {
    std::memcpy(output.data() + offset, &value, sizeof value);
}

template <typename Value>
[[nodiscard]] Value get(std::span<const std::byte> input, std::size_t offset) noexcept {
    Value value{};
    std::memcpy(&value, input.data() + offset, sizeof value);
    return value;
}

[[nodiscard]] bool valid_key(const Key& key) noexcept {
    if (key.scenarioTag == 0 || key.mapFamily.empty() || key.mapFamily.size() >= kFamilyCapacity
        || key.packageCount == 0 || key.packageCount > kPackageCapacity) {
        return false;
    }
    for (std::size_t index = 0; index < key.packageCount; ++index) {
        if (key.packageIds[index] == 0
            || (index != 0 && key.packageIds[index - 1] >= key.packageIds[index])) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool valid_rows(std::span<const Footprint> rows) noexcept {
    if (rows.size() > kFootprintCapacity) {
        return false;
    }
    std::uint32_t previous = 0;
    for (const Footprint& row : rows) {
        if (row.tag == 0 || row.instanceCount == 0 || row.meshCount == 0
            || (previous != 0 && row.tag <= previous)) {
            return false;
        }
        for (std::size_t lane = 0; lane < 3; ++lane) {
            if (!std::isfinite(row.minimum[lane]) || !std::isfinite(row.maximum[lane])
                || row.minimum[lane] > row.maximum[lane]) {
                return false;
            }
        }
        previous = row.tag;
    }
    return true;
}

[[nodiscard]] bool encode(const Key& key,
                          std::span<const Footprint> rows,
                          const Progress& progress,
                          std::vector<std::byte>& output) {
    if (!valid_key(key) || !valid_rows(rows)
        || rows.size() > (std::numeric_limits<std::uint32_t>::max)()) {
        return false;
    }
    output.assign(kHeaderSize + rows.size() * kRecordSize, std::byte{});
    std::copy(kMagic.begin(), kMagic.end(), output.begin());
    const auto bytes = std::span<std::byte>(output);
    put(bytes, 8, kSchemaVersion);
    put(bytes, 12, static_cast<std::uint32_t>(kHeaderSize));
    put(bytes, 16, static_cast<std::uint32_t>(output.size()));
    put(bytes, 20, key.scenarioTag);
    std::copy(key.contentFingerprint.begin(), key.contentFingerprint.end(), output.begin() + 24);
    put(bytes, 56, static_cast<std::uint32_t>(key.mapFamily.size()));
    put(bytes, 60, static_cast<std::uint32_t>(key.packageCount));
    put(bytes, 64, static_cast<std::uint32_t>(rows.size()));
    put(bytes, 68, static_cast<std::uint32_t>(kRecordSize));
    put(bytes, 72, static_cast<std::uint32_t>(progress.collections));
    put(bytes, 76, static_cast<std::uint32_t>(progress.rejected));
    put(bytes, 80, static_cast<std::uint32_t>(progress.truncated));
    std::memcpy(output.data() + 88, key.mapFamily.data(), key.mapFamily.size());
    for (std::size_t index = 0; index < key.packageCount; ++index) {
        put(bytes, 120 + index * sizeof(std::uint16_t), key.packageIds[index]);
    }
    for (std::size_t index = 0; index < rows.size(); ++index) {
        const Footprint& row = rows[index];
        const std::size_t base = kHeaderSize + index * kRecordSize;
        put(bytes, base, row.tag);
        std::memcpy(output.data() + base + 4, row.minimum.data(), sizeof(float) * 3);
        std::memcpy(output.data() + base + 16, row.maximum.data(), sizeof(float) * 3);
        put(bytes, base + 28, row.instanceCount);
        put(bytes, base + 32, row.meshCount);
    }
    return true;
}

[[nodiscard]] bool read_file(std::wstring_view path,
                             std::vector<std::byte>& bytes,
                             LoadState& state) {
    state = LoadState::missing;
    const HANDLE file = CreateFileW(std::wstring(path).c_str(),
                                    GENERIC_READ,
                                    FILE_SHARE_READ,
                                    nullptr,
                                    OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    state = LoadState::rejected;
    LARGE_INTEGER size{};
    bool complete = GetFileSizeEx(file, &size) != FALSE && size.QuadPart >= kHeaderSize
                    && size.QuadPart <= static_cast<LONGLONG>(kMaximumFileSize);
    if (complete) {
        bytes.resize(static_cast<std::size_t>(size.QuadPart));
        DWORD read = 0;
        complete = ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr)
                       != FALSE
                   && read == bytes.size();
    }
    complete = CloseHandle(file) != FALSE && complete;
    return complete;
}

[[nodiscard]] bool write_file(std::wstring_view path, std::span<const std::byte> bytes) noexcept {
    const HANDLE file = CreateFileW(std::wstring(path).c_str(),
                                    GENERIC_WRITE,
                                    0,
                                    nullptr,
                                    CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD written = 0;
    bool complete = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr)
                        != FALSE
                    && written == bytes.size() && FlushFileBuffers(file) != FALSE;
    complete = CloseHandle(file) != FALSE && complete;
    return complete;
}

} // namespace

LoadResult load(std::wstring_view path,
                const Key& key,
                std::vector<Footprint>& rows,
                Progress& progress) noexcept {
    rows.clear();
    progress = {};
    try {
        std::vector<std::byte> bytes;
        LoadState fileState{};
        if (!read_file(path, bytes, fileState)) {
            return {fileState,
                    fileState == LoadState::missing ? "No cached statics catalogue for this location."
                                                    : "The cached statics catalogue could not be read."};
        }
        const auto input = std::span<const std::byte>(bytes);
        if (!valid_key(key) || !std::equal(kMagic.begin(), kMagic.end(), input.begin())
            || get<std::uint32_t>(input, 8) != kSchemaVersion
            || get<std::uint32_t>(input, 12) != kHeaderSize
            || get<std::uint32_t>(input, 16) != bytes.size()
            || get<std::uint32_t>(input, 20) != key.scenarioTag
            || !std::equal(key.contentFingerprint.begin(),
                           key.contentFingerprint.end(),
                           input.begin() + 24)
            || get<std::uint32_t>(input, 56) != key.mapFamily.size()
            || get<std::uint32_t>(input, 60) != key.packageCount
            || get<std::uint32_t>(input, 68) != kRecordSize
            || std::memcmp(input.data() + 88, key.mapFamily.data(), key.mapFamily.size()) != 0) {
            return {LoadState::rejected, "The cached statics catalogue does not match this location."};
        }
        for (std::size_t index = 0; index < key.packageCount; ++index) {
            if (get<std::uint16_t>(input, 120 + index * sizeof(std::uint16_t))
                != key.packageIds[index]) {
                return {LoadState::rejected,
                        "The cached statics catalogue package set does not match this location."};
            }
        }
        const std::size_t count = get<std::uint32_t>(input, 64);
        if (count > kFootprintCapacity || kHeaderSize + count * kRecordSize != bytes.size()) {
            return {LoadState::rejected, "The cached statics catalogue has invalid bounds."};
        }
        rows.resize(count);
        for (std::size_t index = 0; index < count; ++index) {
            Footprint& row = rows[index];
            const std::size_t base = kHeaderSize + index * kRecordSize;
            row.tag = get<std::uint32_t>(input, base);
            std::memcpy(row.minimum.data(), input.data() + base + 4, sizeof(float) * 3);
            std::memcpy(row.maximum.data(), input.data() + base + 16, sizeof(float) * 3);
            row.instanceCount = get<std::uint32_t>(input, base + 28);
            row.meshCount = get<std::uint32_t>(input, base + 32);
        }
        if (!valid_rows(rows)) {
            rows.clear();
            return {LoadState::rejected, "The cached statics catalogue contains invalid rows."};
        }
        progress.collections = get<std::uint32_t>(input, 72);
        progress.published = rows.size();
        progress.rejected = get<std::uint32_t>(input, 76);
        progress.truncated = get<std::uint32_t>(input, 80);
        return {LoadState::ready, "Loaded the cached statics catalogue for this location."};
    } catch (...) {
        rows.clear();
        progress = {};
        return {LoadState::rejected, "The cached statics catalogue could not be allocated."};
    }
}

bool store_atomic(std::wstring_view path,
                  const Key& key,
                  std::span<const Footprint> rows,
                  const Progress& progress,
                  std::string& diagnostic) noexcept {
    try {
        std::vector<std::byte> bytes;
        if (!encode(key, rows, progress, bytes)) {
            diagnostic = "The statics catalogue did not pass cache validation.";
            return false;
        }
        core::path::Buffer temporary{};
        if (!core::path::assign(temporary, path) || !core::path::append(temporary, L".tmp")) {
            diagnostic = "The statics catalogue cache path is too long.";
            return false;
        }
        if (!write_file(std::wstring_view(temporary.chars.data(), temporary.length), bytes)) {
            (void)DeleteFileW(temporary.chars.data());
            diagnostic = "The temporary statics catalogue could not be written.";
            return false;
        }
        std::vector<Footprint> validated;
        Progress validatedProgress{};
        const LoadResult checked = load(
            std::wstring_view(temporary.chars.data(), temporary.length), key, validated, validatedProgress);
        if (checked.state != LoadState::ready
            || MoveFileExW(temporary.chars.data(),
                           std::wstring(path).c_str(),
                           MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)
                   == FALSE) {
            (void)DeleteFileW(temporary.chars.data());
            diagnostic = checked.state == LoadState::ready
                             ? "The statics catalogue cache could not be replaced atomically."
                             : checked.diagnostic;
            return false;
        }
        diagnostic = "Cached the statics catalogue for this location.";
        return true;
    } catch (...) {
        diagnostic = "The statics catalogue cache could not be allocated.";
        return false;
    }
}

} // namespace sunrise::client::content::statics::cache
