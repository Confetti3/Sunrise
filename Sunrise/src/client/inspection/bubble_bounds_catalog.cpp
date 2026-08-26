#include "bubble_bounds_catalog.h"

#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace sunrise::client::inspection::bubble_catalog {
namespace {

constexpr std::size_t kMaximumFileSize = 1U << 20U;
constexpr char kMagic[8] = {'S', 'B', 'B', 'C', 'T', '0', '0', '1'};

[[nodiscard]] std::uint32_t read_u32(const std::byte* bytes) noexcept {
    std::uint32_t value = 0;
    std::memcpy(&value, bytes, sizeof(value));
    return value;
}

[[nodiscard]] std::uint64_t read_u64(const std::byte* bytes) noexcept {
    std::uint64_t value = 0;
    std::memcpy(&value, bytes, sizeof(value));
    return value;
}

[[nodiscard]] float read_f32(const std::byte* bytes) noexcept {
    float value = 0.0F;
    std::memcpy(&value, bytes, sizeof(value));
    return value;
}

[[nodiscard]] std::string read_family(const std::byte* bytes) {
    const char* start = reinterpret_cast<const char*>(bytes);
    const std::size_t length = strnlen(start, kFamilyCapacity);
    return std::string(start, length);
}

} // namespace

bool validate(const Catalog& catalog, std::string& error) {
    if (catalog.bubbles.empty()) {
        error = "bubble catalog holds no bubbles";
        return false;
    }
    if (catalog.bubbles.size() > kMaximumBubbles) {
        error = "bubble catalog exceeds the bubble budget";
        return false;
    }
    for (const Bubble& bubble : catalog.bubbles) {
        if (bubble.tag == 0) {
            error = "bubble catalog entry has no tag";
            return false;
        }
        bool lanesValid = true;
        for (std::size_t lane = 0; lane < 3; ++lane) {
            lanesValid = lanesValid && std::isfinite(bubble.minimum[lane])
                         && std::isfinite(bubble.maximum[lane])
                         && bubble.minimum[lane] <= bubble.maximum[lane];
        }
        if (!lanesValid) {
            error = "bubble catalog entry holds an invalid footprint";
            return false;
        }
    }
    return true;
}

bool load(std::span<const std::byte> bytes, Catalog& catalog, std::string& error) {
    catalog = {};
    if (bytes.size() < kHeaderSize) {
        error = "bubble catalog header is truncated";
        return false;
    }
    if (std::memcmp(bytes.data(), kMagic, sizeof(kMagic)) != 0) {
        error = "bubble catalog magic does not match";
        return false;
    }
    const std::uint32_t schema = read_u32(bytes.data() + 8);
    if (schema != kSchemaVersion) {
        error = "bubble catalog schema version is unsupported";
        return false;
    }
    catalog.schemaVersion = schema;
    catalog.contentBuild = read_u32(bytes.data() + 12);
    const std::uint32_t count = read_u32(bytes.data() + 16);
    const std::uint32_t reserved = read_u32(bytes.data() + 20);
    if (reserved != 0 || count == 0 || count > kMaximumBubbles) {
        error = "bubble catalog header fields are invalid";
        return false;
    }
    const std::uint64_t expected =
        static_cast<std::uint64_t>(kHeaderSize) + static_cast<std::uint64_t>(count) * kRecordSize;
    if (bytes.size() != expected) {
        error = "bubble catalog size does not match its bubble count";
        return false;
    }
    catalog.bubbles.reserve(count);
    const std::byte* cursor = bytes.data() + kHeaderSize;
    for (std::uint32_t index = 0; index < count; ++index, cursor += kRecordSize) {
        Bubble bubble{};
        bubble.tag = read_u64(cursor);
        bubble.minimum = {read_f32(cursor + 40), read_f32(cursor + 44), read_f32(cursor + 48)};
        bubble.maximum = {read_f32(cursor + 52), read_f32(cursor + 56), read_f32(cursor + 60)};
        bubble.family = read_family(cursor + 8);
        if (bubble.family.empty()) {
            error = "bubble catalog entry has an empty family";
            return false;
        }
        catalog.bubbles.push_back(std::move(bubble));
    }
    if (!validate(catalog, error)) {
        return false;
    }
    error.clear();
    return true;
}

LoadResult load_file(std::wstring_view path, Catalog& catalog) noexcept {
    LoadResult result{};
    catalog = {};
    try {
        const std::filesystem::path file(path);
        std::ifstream input(file, std::ios::binary | std::ios::ate);
        if (!input) {
            result.compatibility = Compatibility::missing;
            result.diagnostic = "bubble-bounds cache is absent";
            return result;
        }
        const std::streampos end = input.tellg();
        if (end <= 0 || static_cast<std::uint64_t>(end) > kMaximumFileSize) {
            result.compatibility = Compatibility::malformed;
            result.diagnostic = "bubble catalog file size is invalid";
            return result;
        }
        std::vector<std::byte> bytes(static_cast<std::size_t>(end));
        input.seekg(0, std::ios::beg);
        input.read(reinterpret_cast<char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
        if (!input) {
            result.compatibility = Compatibility::malformed;
            result.diagnostic = "bubble catalog read failed";
            return result;
        }
        std::string error;
        if (!load(bytes, catalog, error)) {
            result.compatibility = Compatibility::malformed;
            result.diagnostic = error;
            return result;
        }
    } catch (...) {
        result.compatibility = Compatibility::malformed;
        result.diagnostic = "bubble catalog load threw";
        return result;
    }
    result.compatibility = compatibility(catalog);
    if (result.compatibility == Compatibility::buildMismatch) {
        result.diagnostic = "bubble catalog was built for another content build";
    }
    return result;
}

Compatibility compatibility(const Catalog& catalog) noexcept {
    if (catalog.bubbles.empty()) {
        return Compatibility::missing;
    }
    return catalog.contentBuild == kTargetContentBuild ? Compatibility::compatible
                                                       : Compatibility::buildMismatch;
}

} // namespace sunrise::client::inspection::bubble_catalog
