#include "current_location_domain_cache.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <span>
#include <utility>
#include <vector>

#include "../../core/filesystem/path.h"
#include "../../middleware/gameplay/peer/join_messages.h"

namespace sunrise::client::inspection::current_location_domain_cache {
namespace {

constexpr std::array<std::byte, 8> kLogicMagic{std::byte{'S'}, std::byte{'L'}, std::byte{'O'},
                                               std::byte{'G'}, std::byte{'I'}, std::byte{'C'},
                                               std::byte{'0'}, std::byte{'1'}};
constexpr std::array<std::byte, 8> kBubbleMagic{std::byte{'S'}, std::byte{'B'}, std::byte{'B'},
                                                std::byte{'C'}, std::byte{'T'}, std::byte{'0'},
                                                std::byte{'0'}, std::byte{'1'}};
constexpr std::array<std::uint32_t, 5> kLogicStrides{28, 48, 4, 44, 16};
constexpr std::array<std::byte, 8> kGraphMagic{std::byte{'S'}, std::byte{'A'}, std::byte{'C'},
                                               std::byte{'A'}, std::byte{'T'}, std::byte{'0'},
                                               std::byte{'0'}, std::byte{'1'}};
constexpr std::array<std::uint32_t, 5> kGraphStrides{20, 20, 40, 4, 4};

template <typename Value>
void put(std::span<std::byte> output, std::size_t offset, const Value& value) noexcept {
    std::memcpy(output.data() + offset, &value, sizeof value);
}

[[nodiscard]] bool stale_schema(std::wstring_view path,
                                std::span<const std::byte, 8> magic,
                                std::uint32_t current) noexcept {
    const std::wstring owned(path);
    const HANDLE file = CreateFileW(owned.c_str(),
                                    GENERIC_READ,
                                    FILE_SHARE_READ,
                                    nullptr,
                                    OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    std::array<std::byte, 12> header{};
    DWORD read = 0;
    const bool complete = ReadFile(file,
                                   header.data(),
                                   static_cast<DWORD>(header.size()),
                                   &read,
                                   nullptr)
                              != FALSE
                          && read == header.size();
    const bool closed = CloseHandle(file) != FALSE;
    if (!complete || !closed || !std::equal(magic.begin(), magic.end(), header.begin())) {
        return false;
    }
    std::uint32_t schema = 0;
    std::memcpy(&schema, header.data() + 8U, sizeof schema);
    return schema != 0 && schema < current;
}

struct StringRef final {
    std::uint32_t offset{};
    std::uint32_t length{};
};

[[nodiscard]] bool add_string(std::vector<std::byte>& strings,
                              std::string_view value,
                              StringRef& output) {
    if (strings.size() > (std::numeric_limits<std::uint32_t>::max)()
        || value.size() > (std::numeric_limits<std::uint32_t>::max)()
        || value.size() > (std::numeric_limits<std::uint32_t>::max)() - strings.size()) {
        return false;
    }
    output.offset = static_cast<std::uint32_t>(strings.size());
    output.length = static_cast<std::uint32_t>(value.size());
    const auto* begin = reinterpret_cast<const std::byte*>(value.data());
    strings.insert(strings.end(), begin, begin + value.size());
    return true;
}

[[nodiscard]] bool encode_logic(const activity_logic_catalog::Catalog& catalog,
                                std::vector<std::byte>& output) {
    std::string error;
    if (!activity_logic_catalog::validate(catalog, error) || catalog.activities.size() != 1) {
        return false;
    }
    std::vector<std::uint32_t> references = catalog.activities.front().entityIndices;
    std::size_t placementCount = 0;
    for (const auto& entity : catalog.entities) {
        placementCount += entity.placements.size();
    }
    if (catalog.activities.size() > (std::numeric_limits<std::uint32_t>::max)()
        || catalog.entities.size() > (std::numeric_limits<std::uint32_t>::max)()
        || references.size() > (std::numeric_limits<std::uint32_t>::max)()
        || placementCount > (std::numeric_limits<std::uint32_t>::max)()
        || catalog.edges.size() > (std::numeric_limits<std::uint32_t>::max)()) {
        return false;
    }

    std::vector<std::byte> strings;
    StringRef activityName{};
    StringRef destination{};
    if (!add_string(strings, catalog.activities.front().name, activityName)
        || !add_string(strings, catalog.activities.front().destination, destination)) {
        return false;
    }
    struct EntityStrings final { StringRef name; StringRef label; StringRef localized; };
    std::vector<EntityStrings> entityStrings(catalog.entities.size());
    for (std::size_t index = 0; index < catalog.entities.size(); ++index) {
        const auto& entity = catalog.entities[index];
        if (!add_string(strings, entity.name, entityStrings[index].name)
            || !add_string(strings, entity.label, entityStrings[index].label)
            || !add_string(strings, entity.localizedText, entityStrings[index].localized)) {
            return false;
        }
    }

    const std::array<std::uint32_t, 5> counts{
        1U,
        static_cast<std::uint32_t>(catalog.entities.size()),
        static_cast<std::uint32_t>(references.size()),
        static_cast<std::uint32_t>(placementCount),
        static_cast<std::uint32_t>(catalog.edges.size())};
    std::array<std::uint32_t, 5> offsets{};
    std::uint64_t cursor = activity_logic_catalog::kHeaderSize;
    for (std::size_t index = 0; index < offsets.size(); ++index) {
        offsets[index] = static_cast<std::uint32_t>(cursor);
        cursor += static_cast<std::uint64_t>(counts[index]) * kLogicStrides[index];
    }
    const std::uint64_t stringOffset = cursor;
    cursor += strings.size();
    if (cursor > (std::numeric_limits<std::uint32_t>::max)()) {
        return false;
    }
    output.assign(static_cast<std::size_t>(cursor), std::byte{});
    std::copy(kLogicMagic.begin(), kLogicMagic.end(), output.begin());
    const auto bytes = std::span<std::byte>(output);
    put(bytes, 8, activity_logic_catalog::kSchemaVersion);
    put(bytes, 12, static_cast<std::uint32_t>(activity_logic_catalog::kHeaderSize));
    put(bytes, 16, static_cast<std::uint32_t>(output.size()));
    put(bytes, 20, static_cast<std::uint32_t>(stringOffset));
    put(bytes, 24, static_cast<std::uint32_t>(strings.size()));
    std::memcpy(output.data() + 28,
                catalog.provenance.contentFingerprint.data(),
                catalog.provenance.contentFingerprint.size());
    put(bytes, 60, catalog.provenance.collectorVersion);
    for (std::size_t index = 0; index < offsets.size(); ++index) {
        put(bytes, 64 + index * 12U, offsets[index]);
        put(bytes, 68 + index * 12U, counts[index]);
        put(bytes, 72 + index * 12U, kLogicStrides[index]);
    }
    put(bytes, 124, catalog.provenance.contentBuild);

    const auto& activity = catalog.activities.front();
    put(bytes, offsets[0], activity.scenarioTag);
    put(bytes, offsets[0] + 4U, activityName.offset);
    put(bytes, offsets[0] + 8U, activityName.length);
    put(bytes, offsets[0] + 12U, destination.offset);
    put(bytes, offsets[0] + 16U, destination.length);
    put(bytes, offsets[0] + 20U, std::uint32_t{0});
    put(bytes, offsets[0] + 24U, counts[2]);

    std::uint32_t firstPlacement = 0;
    for (std::size_t index = 0; index < catalog.entities.size(); ++index) {
        const auto& entity = catalog.entities[index];
        const std::size_t base = offsets[1] + index * kLogicStrides[1];
        put(bytes, base, entity.definitionTag);
        put(bytes, base + 4U, entity.classPrimary);
        put(bytes, base + 8U, entity.classSecondary);
        put(bytes, base + 12U, static_cast<std::uint8_t>(entity.role));
        put(bytes, base + 13U, static_cast<std::uint8_t>(entity.confidence));
        put(bytes, base + 14U, std::uint16_t{0});
        put(bytes, base + 16U, entityStrings[index].name.offset);
        put(bytes, base + 20U, entityStrings[index].name.length);
        put(bytes, base + 24U, entityStrings[index].label.offset);
        put(bytes, base + 28U, entityStrings[index].label.length);
        put(bytes, base + 32U, entityStrings[index].localized.offset);
        put(bytes, base + 36U, entityStrings[index].localized.length);
        put(bytes, base + 40U, firstPlacement);
        put(bytes, base + 44U, static_cast<std::uint32_t>(entity.placements.size()));
        for (const auto& placement : entity.placements) {
            const std::size_t placementBase = offsets[3] + firstPlacement * kLogicStrides[3];
            put(bytes, placementBase, placement.worldId);
            put(bytes, placementBase + 8U, placement.mapTableTag);
            put(bytes, placementBase + 12U, placement.placedEntityTag);
            std::memcpy(output.data() + placementBase + 16U,
                        placement.position.data(),
                        sizeof(float) * placement.position.size());
            std::memcpy(output.data() + placementBase + 28U,
                        placement.rotation.data(),
                        sizeof(float) * placement.rotation.size());
            ++firstPlacement;
        }
    }
    for (std::size_t index = 0; index < references.size(); ++index) {
        put(bytes, offsets[2] + index * kLogicStrides[2], references[index]);
    }
    for (std::size_t index = 0; index < catalog.edges.size(); ++index) {
        const auto& edge = catalog.edges[index];
        const std::size_t base = offsets[4] + index * kLogicStrides[4];
        put(bytes, base, edge.sourceEntityIndex);
        put(bytes, base + 4U, edge.targetEntityIndex);
        put(bytes, base + 8U, edge.nameHash);
        put(bytes, base + 12U, edge.occurrenceCount);
    }
    std::copy(strings.begin(), strings.end(), output.begin() + stringOffset);
    return true;
}

[[nodiscard]] bool encode_bubbles(const bubble_catalog::Catalog& catalog,
                                  std::vector<std::byte>& output) {
    std::string error;
    if (!bubble_catalog::validate(catalog, error)
        || catalog.schemaVersion != bubble_catalog::kSchemaVersion
        || catalog.bubbles.size() > (std::numeric_limits<std::uint32_t>::max)()) {
        return false;
    }
    output.assign(bubble_catalog::kHeaderSize
                      + catalog.bubbles.size() * bubble_catalog::kRecordSize,
                  std::byte{});
    std::copy(kBubbleMagic.begin(), kBubbleMagic.end(), output.begin());
    const auto bytes = std::span<std::byte>(output);
    put(bytes, 8, bubble_catalog::kSchemaVersion);
    put(bytes, 12, catalog.contentBuild);
    put(bytes, 16, static_cast<std::uint32_t>(catalog.bubbles.size()));
    for (std::size_t index = 0; index < catalog.bubbles.size(); ++index) {
        const auto& bubble = catalog.bubbles[index];
        if (bubble.family.empty() || bubble.family.size() >= bubble_catalog::kFamilyCapacity) {
            return false;
        }
        const std::size_t base = bubble_catalog::kHeaderSize + index * bubble_catalog::kRecordSize;
        put(bytes, base, bubble.tag);
        std::memcpy(output.data() + base + 8U, bubble.family.data(), bubble.family.size());
        std::memcpy(output.data() + base + 40U,
                    bubble.minimum.data(),
                    sizeof(float) * bubble.minimum.size());
        std::memcpy(output.data() + base + 52U,
                    bubble.maximum.data(),
                    sizeof(float) * bubble.maximum.size());
    }
    return true;
}

[[nodiscard]] bool encode_graph(const activity_catalog::Catalog& catalog,
                                std::vector<std::byte>& output) {
    std::string error;
    if (!activity_catalog::validate(catalog, error)) {
        return false;
    }
    std::size_t nodeCount = 0;
    std::size_t activityRefCount = 0;
    std::size_t linkedRefCount = 0;
    for (const auto& activity : catalog.activities) {
        activityRefCount += activity.graphHashes.size();
    }
    for (const auto& graph : catalog.graphs) {
        nodeCount += graph.nodes.size();
        linkedRefCount += graph.linkedGraphHashes.size();
        for (const auto& node : graph.nodes) {
            activityRefCount += node.stateValues.size();
            activityRefCount += node.activityHashes.size();
            linkedRefCount += node.linkedGraphHashes.size();
        }
    }
    const auto fits_u32 = [](std::size_t value) noexcept {
        return value <= (std::numeric_limits<std::uint32_t>::max)();
    };
    if (!fits_u32(catalog.activities.size()) || !fits_u32(catalog.graphs.size())
        || !fits_u32(nodeCount) || !fits_u32(activityRefCount) || !fits_u32(linkedRefCount)) {
        return false;
    }

    std::vector<std::byte> strings;
    std::vector<StringRef> activityNames(catalog.activities.size());
    for (std::size_t index = 0; index < catalog.activities.size(); ++index) {
        if (!add_string(strings, catalog.activities[index].name, activityNames[index])) {
            return false;
        }
    }
    const std::array<std::uint32_t, 5> counts{
        static_cast<std::uint32_t>(catalog.activities.size()),
        static_cast<std::uint32_t>(catalog.graphs.size()),
        static_cast<std::uint32_t>(nodeCount),
        static_cast<std::uint32_t>(activityRefCount),
        static_cast<std::uint32_t>(linkedRefCount)};
    std::array<std::uint32_t, 5> offsets{};
    std::uint64_t cursor = activity_catalog::kHeaderSize;
    for (std::size_t index = 0; index < offsets.size(); ++index) {
        if (cursor > (std::numeric_limits<std::uint32_t>::max)()) {
            return false;
        }
        offsets[index] = static_cast<std::uint32_t>(cursor);
        cursor += static_cast<std::uint64_t>(counts[index]) * kGraphStrides[index];
    }
    if (cursor > (std::numeric_limits<std::uint32_t>::max)()
        || strings.size() > (std::numeric_limits<std::uint32_t>::max)() - cursor) {
        return false;
    }
    const std::uint32_t stringOffset = static_cast<std::uint32_t>(cursor);
    cursor += strings.size();
    output.assign(static_cast<std::size_t>(cursor), std::byte{});
    std::copy(kGraphMagic.begin(), kGraphMagic.end(), output.begin());
    const auto bytes = std::span<std::byte>(output);
    put(bytes, 8, catalog.schemaVersion);
    put(bytes, 12, catalog.contentBuild);
    put(bytes, 16, static_cast<std::uint32_t>(activity_catalog::kHeaderSize));
    put(bytes, 20, static_cast<std::uint32_t>(output.size()));
    put(bytes, 24, catalog.collectorVersion);
    put(bytes, 28, catalog.scenarioTag);
    put(bytes, 32, stringOffset);
    put(bytes, 36, static_cast<std::uint32_t>(strings.size()));
    for (std::size_t index = 0; index < offsets.size(); ++index) {
        put(bytes, 40 + index * 12U, offsets[index]);
        put(bytes, 44 + index * 12U, counts[index]);
        put(bytes, 48 + index * 12U, kGraphStrides[index]);
    }
    std::memcpy(output.data() + 124,
                catalog.contentFingerprint.data(),
                catalog.contentFingerprint.size());

    std::uint32_t activityRef = 0;
    std::uint32_t linkedRef = 0;
    for (std::size_t index = 0; index < catalog.activities.size(); ++index) {
        const auto& activity = catalog.activities[index];
        const std::size_t base = offsets[0] + index * kGraphStrides[0];
        put(bytes, base, activity.hash);
        put(bytes, base + 4U, stringOffset + activityNames[index].offset);
        put(bytes, base + 8U, activityNames[index].length);
        put(bytes, base + 12U, activityRef);
        put(bytes, base + 16U, static_cast<std::uint32_t>(activity.graphHashes.size()));
        for (const std::uint32_t hash : activity.graphHashes) {
            put(bytes, offsets[3] + activityRef * kGraphStrides[3], hash);
            ++activityRef;
        }
    }
    std::uint32_t nodeIndex = 0;
    for (std::size_t graphIndex = 0; graphIndex < catalog.graphs.size(); ++graphIndex) {
        const auto& graph = catalog.graphs[graphIndex];
        const std::size_t graphBase = offsets[1] + graphIndex * kGraphStrides[1];
        put(bytes, graphBase, graph.hash);
        put(bytes, graphBase + 4U, nodeIndex);
        put(bytes, graphBase + 8U, static_cast<std::uint32_t>(graph.nodes.size()));
        put(bytes, graphBase + 12U, linkedRef);
        put(bytes, graphBase + 16U, static_cast<std::uint32_t>(graph.linkedGraphHashes.size()));
        for (const std::uint32_t hash : graph.linkedGraphHashes) {
            put(bytes, offsets[4] + linkedRef * kGraphStrides[4], hash);
            ++linkedRef;
        }
        for (const auto& node : graph.nodes) {
            const std::size_t nodeBase = offsets[2] + nodeIndex * kGraphStrides[2];
            put(bytes, nodeBase, node.graphHash);
            put(bytes, nodeBase + 4U, node.nodeHash);
            put(bytes, nodeBase + 8U, node.authoredX);
            put(bytes, nodeBase + 12U, node.authoredY);
            put(bytes, nodeBase + 16U, activityRef);
            put(bytes, nodeBase + 20U, static_cast<std::uint32_t>(node.stateValues.size()));
            for (const std::uint32_t value : node.stateValues) {
                put(bytes, offsets[3] + activityRef * kGraphStrides[3], value);
                ++activityRef;
            }
            put(bytes, nodeBase + 24U, activityRef);
            put(bytes, nodeBase + 28U, static_cast<std::uint32_t>(node.activityHashes.size()));
            for (const std::uint32_t hash : node.activityHashes) {
                put(bytes, offsets[3] + activityRef * kGraphStrides[3], hash);
                ++activityRef;
            }
            put(bytes, nodeBase + 32U, linkedRef);
            put(bytes, nodeBase + 36U, static_cast<std::uint32_t>(node.linkedGraphHashes.size()));
            for (const std::uint32_t hash : node.linkedGraphHashes) {
                put(bytes, offsets[4] + linkedRef * kGraphStrides[4], hash);
                ++linkedRef;
            }
            ++nodeIndex;
        }
    }
    std::copy(strings.begin(), strings.end(), output.begin() + stringOffset);
    return activityRef == counts[3] && linkedRef == counts[4] && nodeIndex == counts[2];
}

[[nodiscard]] bool write_file(std::wstring_view path, std::span<const std::byte> bytes) noexcept {
    const std::wstring owned(path);
    const HANDLE file = CreateFileW(owned.c_str(),
                                    GENERIC_WRITE,
                                    0,
                                    nullptr,
                                    CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
    if (file == INVALID_HANDLE_VALUE || bytes.size() > (std::numeric_limits<DWORD>::max)()) {
        if (file != INVALID_HANDLE_VALUE) {
            CloseHandle(file);
        }
        return false;
    }
    DWORD written = 0;
    bool complete = WriteFile(file,
                              bytes.data(),
                              static_cast<DWORD>(bytes.size()),
                              &written,
                              nullptr)
                        != FALSE
                    && written == bytes.size() && FlushFileBuffers(file) != FALSE;
    complete = CloseHandle(file) != FALSE && complete;
    return complete;
}

template <typename Catalog, typename Encode, typename Load>
[[nodiscard]] bool store_atomic(std::wstring_view path,
                                const Catalog& catalog,
                                Encode encode,
                                Load load,
                                std::string& diagnostic) {
    std::vector<std::byte> bytes;
    if (!encode(catalog, bytes)) {
        diagnostic = "The location catalogue did not pass serialization validation.";
        return false;
    }
    core::path::Buffer temporary{};
    if (!core::path::assign(temporary, path) || !core::path::append(temporary, L".tmp")) {
        diagnostic = "The location catalogue cache path is too long.";
        return false;
    }
    const std::wstring_view temporaryPath(temporary.chars.data(), temporary.length);
    if (!write_file(temporaryPath, bytes)) {
        DeleteFileW(temporary.chars.data());
        diagnostic = "The temporary location catalogue could not be written.";
        return false;
    }
    Catalog checked{};
    if (!load(temporaryPath, checked)) {
        DeleteFileW(temporary.chars.data());
        diagnostic = "The temporary location catalogue failed normal loader validation.";
        return false;
    }
    const std::wstring destination(path);
    if (MoveFileExW(temporary.chars.data(),
                    destination.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)
        == FALSE) {
        DeleteFileW(temporary.chars.data());
        diagnostic = "The location catalogue cache could not be replaced atomically.";
        return false;
    }
    diagnostic = "Cached the catalogue for this location.";
    return true;
}

} // namespace

LoadResult load_activity_graph(std::wstring_view path,
                               std::uint32_t scenarioTag,
                               std::span<const std::byte> contentFingerprint,
                               activity_catalog::Catalog& output) noexcept {
    if (stale_schema(path, kGraphMagic, activity_catalog::kSchemaVersion)) {
        output = {};
        return {LoadState::stale,
                "The cached Activity Graph schema is obsolete; catalogue this location again."};
    }
    const auto loaded = activity_catalog::load_file(path, output);
    if (loaded.compatibility == activity_catalog::Compatibility::missing) {
        return {LoadState::missing, "No cached Activity Graph catalogue for this location."};
    }
    activity_catalog::Digest expectedDigest{};
    if (contentFingerprint.size() == expectedDigest.size()) {
        std::transform(contentFingerprint.begin(),
                       contentFingerprint.end(),
                       expectedDigest.begin(),
                       [](std::byte value) { return static_cast<std::uint8_t>(value); });
    }
    if (loaded.compatibility != activity_catalog::Compatibility::compatible
        || contentFingerprint.size() != expectedDigest.size()
        || output.schemaVersion != activity_catalog::kSchemaVersion
        || output.collectorVersion != activity_catalog::kCollectorVersion
        || output.scenarioTag != scenarioTag || output.graphs.empty()
        || output.contentFingerprint != expectedDigest) {
        output = {};
        return {LoadState::rejected,
                "The cached Activity Graph catalogue does not match this location."};
    }
    return {LoadState::ready, "Loaded the cached Activity Graph catalogue for this location."};
}

bool store_activity_graph_atomic(std::wstring_view path,
                                 const activity_catalog::Catalog& catalog,
                                 std::uint32_t scenarioTag,
                                 std::span<const std::byte> contentFingerprint,
                                 std::string& diagnostic) noexcept {
    try {
        if (activity_catalog::compatibility(catalog)
                != activity_catalog::Compatibility::compatible
            || catalog.schemaVersion != activity_catalog::kSchemaVersion
            || catalog.graphs.empty()) {
            diagnostic = "The Activity Graph catalogue does not prove the current content build.";
            return false;
        }
        return store_atomic(
            path,
            catalog,
            &encode_graph,
            [scenarioTag, contentFingerprint](std::wstring_view candidate,
                                              activity_catalog::Catalog& checked) {
                return load_activity_graph(
                           candidate, scenarioTag, contentFingerprint, checked)
                           .state
                       == LoadState::ready;
            },
            diagnostic);
    } catch (...) {
        diagnostic = "The Activity Graph cache could not be allocated.";
        return false;
    }
}

LoadResult load_activity_logic(std::wstring_view path,
                               std::uint32_t scenarioTag,
                               std::span<const std::byte> contentFingerprint,
                               activity_logic_catalog::Catalog& output) noexcept {
    if (stale_schema(path, kLogicMagic, activity_logic_catalog::kSchemaVersion)) {
        output = {};
        return {LoadState::stale,
                "The cached Activity Logic schema is obsolete; catalogue this location again."};
    }
    const auto loaded = activity_logic_catalog::load_file(path, output);
    if (loaded.state == activity_logic_catalog::LoadState::missing) {
        return {LoadState::missing, "No cached Activity Logic catalogue for this location."};
    }
    activity_logic_catalog::Digest expected{};
    if (contentFingerprint.size() == expected.size()) {
        std::transform(contentFingerprint.begin(),
                       contentFingerprint.end(),
                       expected.begin(),
                       [](std::byte value) { return static_cast<std::uint8_t>(value); });
    }
    if (loaded.state != activity_logic_catalog::LoadState::ready || output.activities.size() != 1
        || output.activities.front().scenarioTag != scenarioTag
        || output.provenance.contentBuild != middleware::gameplay::peer::kHostBuild
        || output.provenance.collectorVersion != activity_logic_catalog::kCollectorVersion
        || contentFingerprint.size() != expected.size()
        || output.provenance.contentFingerprint != expected) {
        output = {};
        return {LoadState::rejected,
                "The cached Activity Logic catalogue does not match this location."};
    }
    return {LoadState::ready, "Loaded the cached Activity Logic catalogue for this location."};
}

bool store_activity_logic_atomic(std::wstring_view path,
                                 const activity_logic_catalog::Catalog& catalog,
                                 std::uint32_t scenarioTag,
                                 std::span<const std::byte> contentFingerprint,
                                 std::string& diagnostic) noexcept {
    try {
        if (catalog.provenance.contentBuild != middleware::gameplay::peer::kHostBuild
            || catalog.provenance.collectorVersion != activity_logic_catalog::kCollectorVersion) {
            diagnostic = "The Activity Logic catalogue does not prove the current content build.";
            return false;
        }
        return store_atomic(
            path,
            catalog,
            &encode_logic,
            [scenarioTag, contentFingerprint](std::wstring_view candidate,
                                              activity_logic_catalog::Catalog& checked) {
                return load_activity_logic(candidate,
                                           scenarioTag,
                                           contentFingerprint,
                                           checked).state
                       == LoadState::ready;
            },
            diagnostic);
    } catch (...) {
        diagnostic = "The Activity Logic cache could not be allocated.";
        return false;
    }
}

LoadResult load_bubble_bounds(std::wstring_view path,
                              std::string_view family,
                              bubble_catalog::Catalog& output) noexcept {
    const auto loaded = bubble_catalog::load_file(path, output);
    if (loaded.compatibility == bubble_catalog::Compatibility::missing) {
        return {LoadState::missing, "No cached bubble-bounds catalogue for this location."};
    }
    const bool matches = loaded.compatibility == bubble_catalog::Compatibility::compatible
                         && output.schemaVersion == bubble_catalog::kSchemaVersion
                         && !output.bubbles.empty()
                         && std::ranges::all_of(output.bubbles, [family](const auto& bubble) {
                                return bubble.family == family;
                            });
    if (!matches) {
        output = {};
        return {LoadState::rejected,
                "The cached bubble-bounds catalogue does not match this location."};
    }
    return {LoadState::ready, "Loaded cached bubble bounds for this location."};
}

bool store_bubble_bounds_atomic(std::wstring_view path,
                                const bubble_catalog::Catalog& catalog,
                                std::string_view family,
                                std::string& diagnostic) noexcept {
    try {
        return store_atomic(
            path,
            catalog,
            &encode_bubbles,
            [family](std::wstring_view candidate, bubble_catalog::Catalog& checked) {
                return load_bubble_bounds(candidate, family, checked).state == LoadState::ready;
            },
            diagnostic);
    } catch (...) {
        diagnostic = "The bubble-bounds cache could not be allocated.";
        return false;
    }
}

} // namespace sunrise::client::inspection::current_location_domain_cache
