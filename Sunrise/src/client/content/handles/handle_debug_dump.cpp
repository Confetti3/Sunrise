#include "handle_debug_dump.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <limits>
#include <cstring>

#include "../../../core/logging/log.h"

namespace sunrise::client::content::handles::debug {
namespace {

constexpr std::size_t kMaximumWindowBytes = 64 * 1024;
constexpr std::size_t kMaximumChunkBytes = 64;
constexpr std::size_t kMaximumLabelBytes = 48;
constexpr std::size_t kScanChunkBytes = 16 * 1024;
constexpr std::size_t kMaximumSchemaProbes = 16;
constexpr std::size_t kMaximumSchemaHits = 64;

struct DefinitionPrefix final {
    std::uint32_t length{};
    std::uint32_t opaque04{};
    std::uint32_t definitionHash{};
    std::uint32_t opaque0C{};
    std::uint32_t baseType{};
    std::uint32_t structureBytes{};
};

template <typename Value>
[[nodiscard]] bool read_value(const Source& source,
                              std::uintptr_t address,
                              Value& value) noexcept {
    return source.read != nullptr
           && source.read(source.context,
                          address,
                          std::span(reinterpret_cast<std::byte*>(&value), sizeof value));
}

void report_status(std::uint32_t handle,
                   std::string_view label,
                   const char* result,
                   std::uintptr_t address = 0) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=handle_dump label=%.*s handle=0x%08X result=%s "
                                      "address=0x%llX",
                                      static_cast<int>(label.size()),
                                      label.data(),
                                      handle,
                                      result,
                                      static_cast<unsigned long long>(address));
    if (written > 0) {
        core::log::write(core::log::Channel::state,
                         result[0] == 'o' ? core::log::Level::info : core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

void report_binding(std::uint32_t handle,
                    std::string_view label,
                    const char* result,
                    std::uintptr_t slot = 0,
                    std::uintptr_t record = 0,
                    const DefinitionPrefix* prefix = nullptr) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int written = std::snprintf(
        line.data(),
        line.size(),
        "ev=schema_binding label=%.*s handle=0x%08X result=%s slot=0x%llX record=0x%llX "
        "length=%u hash=0x%08X base_type=0x%08X struct_bytes=%u",
        static_cast<int>(label.size()),
        label.data(),
        handle,
        result,
        static_cast<unsigned long long>(slot),
        static_cast<unsigned long long>(record),
        prefix != nullptr ? prefix->length : 0,
        prefix != nullptr ? prefix->definitionHash : 0,
        prefix != nullptr ? prefix->baseType : 0,
        prefix != nullptr ? prefix->structureBytes : 0);
    if (written > 0) {
        core::log::write(core::log::Channel::state,
                         result[0] == 'o' ? core::log::Level::info : core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

[[nodiscard]] bool valid_prefix(const DefinitionPrefix& prefix) noexcept {
    constexpr std::uint32_t kMinimumDefinitionBytes = 0x58;
    constexpr std::uint32_t kMaximumDefinitionBytes = 16 * 1024 * 1024;
    return prefix.length >= kMinimumDefinitionBytes
           && prefix.length <= kMaximumDefinitionBytes
           && prefix.definitionHash != 0
           && (prefix.baseType == 0xFFFFFFFFU || (prefix.baseType & 0x80000000U) != 0);
}

} // namespace

bool dump(const Source& source,
          std::uint32_t handle,
          std::string_view label,
          Window window) noexcept {
    if (label.empty() || label.size() > kMaximumLabelBytes || window.chunkBytes == 0
        || window.chunkBytes > kMaximumChunkBytes
        || window.bytesBefore > kMaximumWindowBytes
        || window.bytesAfter > kMaximumWindowBytes - window.bytesBefore) {
        report_status(handle, label, "invalid_request");
        return false;
    }
    std::uintptr_t resolved = 0;
    if (!resolve(source, handle, resolved)) {
        report_status(handle, label, "resolve_fail");
        return false;
    }
    const std::size_t before = (std::min)(window.bytesBefore, resolved);
    const std::uintptr_t start = resolved - before;
    if (window.bytesAfter > (std::numeric_limits<std::uintptr_t>::max)() - resolved) {
        report_status(handle, label, "range_overflow", resolved);
        return false;
    }
    const std::size_t total = before + window.bytesAfter;
    report_status(handle, label, "ok", resolved);

    std::array<std::byte, kMaximumChunkBytes> bytes{};
    bool readAny = false;
    for (std::size_t offset = 0; offset < total; offset += window.chunkBytes) {
        const std::size_t count = (std::min)(window.chunkBytes, total - offset);
        const std::uintptr_t address = start + offset;
        const bool readable = source.read != nullptr
                              && source.read(source.context, address, std::span(bytes).first(count));
        std::array<char, core::log::kLineCapacity> line{};
        int written = std::snprintf(line.data(),
                                    line.size(),
                                    "ev=handle_dump label=%.*s handle=0x%08X base=0x%llX "
                                    "relative=%lld address=0x%llX bytes=%zu result=%s",
                                    static_cast<int>(label.size()),
                                    label.data(),
                                    handle,
                                    static_cast<unsigned long long>(resolved),
                                    static_cast<long long>(address) - static_cast<long long>(resolved),
                                    static_cast<unsigned long long>(address),
                                    count,
                                    readable ? "ok hex=" : "unreadable");
        if (readable) {
            readAny = true;
            for (std::size_t index = 0;
                 written > 0 && static_cast<std::size_t>(written) + 2 < line.size()
                 && index < count;
                 ++index) {
                written += std::snprintf(line.data() + written,
                                         line.size() - static_cast<std::size_t>(written),
                                         "%02X",
                                         std::to_integer<unsigned>(bytes[index]));
            }
        }
        if (written > 0) {
            core::log::write(core::log::Channel::state,
                             readable ? core::log::Level::info : core::log::Level::warn,
                             {line.data(), static_cast<std::size_t>(written)});
        }
    }
    return readAny;
}

std::size_t dump_schema_bindings(const Source& source,
                                 Range image,
                                 std::span<const SchemaProbe> probes,
                                 Window window) noexcept {
    if (source.read == nullptr || image.base == 0 || image.bytes < sizeof(std::uint32_t)
        || probes.empty() || probes.size() > kMaximumSchemaProbes
        || image.bytes > (std::numeric_limits<std::uintptr_t>::max)() - image.base) {
        return 0;
    }
    for (const SchemaProbe probe : probes) {
        if (probe.label.empty() || probe.label.size() > kMaximumLabelBytes) {
            report_binding(probe.handle, probe.label, "invalid_request");
            return 0;
        }
    }

    std::array<std::byte, kScanChunkBytes> bytes{};
    std::array<bool, kMaximumSchemaProbes> found{};
    std::size_t hitCount = 0;
    for (std::size_t offset = 0; offset < image.bytes && hitCount < kMaximumSchemaHits;) {
        const std::size_t count = (std::min)(bytes.size(), image.bytes - offset);
        const std::uintptr_t chunkAddress = image.base + offset;
        if (!source.read(source.context, chunkAddress, std::span(bytes).first(count))) {
            offset += count;
            continue;
        }
        for (std::size_t byteIndex = 0; byteIndex + sizeof(std::uint32_t) <= count; ++byteIndex) {
            std::uint32_t candidate = 0;
            std::memcpy(&candidate, bytes.data() + byteIndex, sizeof candidate);
            for (std::size_t probeIndex = 0; probeIndex < probes.size(); ++probeIndex) {
                const SchemaProbe probe = probes[probeIndex];
                if (candidate != probe.handle) {
                    continue;
                }
                const std::uintptr_t slot = chunkAddress + byteIndex;
                std::uintptr_t record = 0;
                DefinitionPrefix prefix{};
                if (!read_value(source, slot + 8, record) || record == 0
                    || !read_value(source, record, prefix) || !valid_prefix(prefix)) {
                    continue;
                }
                found[probeIndex] = true;
                ++hitCount;
                report_binding(probe.handle, probe.label, "ok", slot, record, &prefix);
                (void)dump(source, probe.handle, probe.label, window);
                // dump() uses the generic content resolver. Log the actual slot record separately.
                Window recordWindow = window;
                const std::size_t before = (std::min)(recordWindow.bytesBefore, record);
                const std::uintptr_t start = record - before;
                const std::size_t total = before + recordWindow.bytesAfter;
                std::array<std::byte, kMaximumChunkBytes> recordBytes{};
                for (std::size_t relative = 0; relative < total; relative += recordWindow.chunkBytes) {
                    const std::size_t part =
                        (std::min)(recordWindow.chunkBytes, total - relative);
                    const std::uintptr_t address = start + relative;
                    const bool readable = source.read(source.context,
                                                      address,
                                                      std::span(recordBytes).first(part));
                    std::array<char, core::log::kLineCapacity> line{};
                    int written = std::snprintf(
                        line.data(),
                        line.size(),
                        "ev=schema_record label=%.*s handle=0x%08X base=0x%llX relative=%lld "
                        "address=0x%llX bytes=%zu result=%s",
                        static_cast<int>(probe.label.size()),
                        probe.label.data(),
                        probe.handle,
                        static_cast<unsigned long long>(record),
                        static_cast<long long>(address) - static_cast<long long>(record),
                        static_cast<unsigned long long>(address),
                        part,
                        readable ? "ok hex=" : "unreadable");
                    if (readable) {
                        for (std::size_t index = 0;
                             written > 0 && static_cast<std::size_t>(written) + 2 < line.size()
                             && index < part;
                             ++index) {
                            written += std::snprintf(
                                line.data() + written,
                                line.size() - static_cast<std::size_t>(written),
                                "%02X",
                                std::to_integer<unsigned>(recordBytes[index]));
                        }
                    }
                    if (written > 0) {
                        core::log::write(core::log::Channel::state,
                                         readable ? core::log::Level::info
                                                  : core::log::Level::warn,
                                         {line.data(), static_cast<std::size_t>(written)});
                    }
                }
            }
        }
        offset += count;
    }
    for (std::size_t index = 0; index < probes.size(); ++index) {
        if (!found[index]) {
            report_binding(probes[index].handle, probes[index].label, "missing");
        }
    }
    return hitCount;
}

} // namespace sunrise::client::content::handles::debug
