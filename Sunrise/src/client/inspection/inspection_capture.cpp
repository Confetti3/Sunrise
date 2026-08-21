#include "inspection_capture.h"
#include "activity_graph_catalog.h"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <limits>
#include <string_view>
#include <type_traits>
#include <utility>

#include "../../core/filesystem/path.h"
#include "../../core/filesystem/temporary_sibling.h"
#include "../../middleware/crypto/sha256.h"
#include "../hooks/noclip/runtime.h"
#include "../hooks/viewer_audio/viewer_audio.h"
#include "../hooks/viewer_objects/viewer_objects.h"
#include "../hooks/viewer_triggers/viewer_triggers.h"

namespace sunrise::client::inspection::capture {
namespace {

constexpr std::string_view kBuildHash =
    "36A04F76B8A7D0778EB2A45CF05688A26817175C67D7DD43375743CA016B65A4";
constexpr std::wstring_view kJsonSuffix = L"\\viewer-inspection.json";
constexpr std::wstring_view kCsvSuffix = L"\\viewer-inspection.csv";
constexpr std::wstring_view kEventsSuffix = L"\\viewer-events.json";

core::path::Buffer g_jsonPath{};
core::path::Buffer g_rootPath{};
core::path::Buffer g_csvPath{};
core::path::Buffer g_eventsPath{};
bool g_pathsReady{};
std::string g_imageHash;
bool g_imageVerified{};

void capture_image_hash() noexcept {
    g_imageHash.clear();
    g_imageVerified = false;
    std::array<wchar_t, 32768> path{};
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) {
        return;
    }
    const HANDLE file = CreateFileW(path.data(),
                                    GENERIC_READ,
                                    FILE_SHARE_READ | FILE_SHARE_DELETE,
                                    nullptr,
                                    OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    LARGE_INTEGER size{};
    HANDLE mapping = nullptr;
    const void* view = nullptr;
    if (GetFileSizeEx(file, &size) != FALSE && size.QuadPart > 0
        && static_cast<unsigned long long>(size.QuadPart) <= (std::numeric_limits<ULONG>::max)()) {
        mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (mapping != nullptr) {
            view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
        }
    }
    middleware::crypto::sha256::Digest digest{};
    const bool digestReady =
        view != nullptr
        && middleware::crypto::sha256::hash(
            {static_cast<const std::byte*>(view), static_cast<std::size_t>(size.QuadPart)}, digest);
    std::array<char, middleware::crypto::sha256::kDigestSize * 2U + 1U> encoded{};
    if (digestReady) {
        constexpr char kHex[] = "0123456789ABCDEF";
        std::size_t offset = 0;
        for (const std::byte value : digest) {
            const unsigned byte = std::to_integer<unsigned>(value);
            encoded[offset++] = kHex[byte >> 4U];
            encoded[offset++] = kHex[byte & 0x0FU];
        }
    }
    if (view != nullptr) {
        UnmapViewOfFile(view);
    }
    if (mapping != nullptr) {
        CloseHandle(mapping);
    }
    CloseHandle(file);
    if (digestReady) {
        try {
            g_imageHash.assign(encoded.data(), encoded.size() - 1U);
            g_imageVerified = g_imageHash == kBuildHash;
        } catch (...) {
            g_imageHash.clear();
            g_imageVerified = false;
        }
    }
}

void append_json_string(std::string& output, std::string_view value) {
    constexpr char kHex[] = "0123456789ABCDEF";
    output.push_back('"');
    for (const unsigned char byte : value) {
        switch (byte) {
        case '"':
            output.append("\\\"");
            break;
        case '\\':
            output.append("\\\\");
            break;
        case '\b':
            output.append("\\b");
            break;
        case '\f':
            output.append("\\f");
            break;
        case '\n':
            output.append("\\n");
            break;
        case '\r':
            output.append("\\r");
            break;
        case '\t':
            output.append("\\t");
            break;
        default:
            if (byte < 0x20U) {
                output.append("\\u00");
                output.push_back(kHex[byte >> 4U]);
                output.push_back(kHex[byte & 0x0FU]);
            } else {
                output.push_back(static_cast<char>(byte));
            }
            break;
        }
    }
    output.push_back('"');
}

void append_csv(std::string& output, std::string_view value) {
    output.push_back('"');
    for (const char character : value) {
        if (character == '"') {
            output.append("\"\"");
        } else {
            output.push_back(character);
        }
    }
    output.push_back('"');
}

void append_number(std::string& output, std::uint64_t value) {
    std::array<char, 32> text{};
    const int written =
        std::snprintf(text.data(), text.size(), "%llu", static_cast<unsigned long long>(value));
    if (written > 0) {
        output.append(text.data(), static_cast<std::size_t>(written));
    }
}

void append_signed(std::string& output, std::int64_t value) {
    std::array<char, 32> text{};
    const int written =
        std::snprintf(text.data(), text.size(), "%lld", static_cast<long long>(value));
    if (written > 0) {
        output.append(text.data(), static_cast<std::size_t>(written));
    }
}

void append_float(std::string& output, float value) {
    if (!std::isfinite(value)) {
        output.append("null");
        return;
    }
    std::array<char, 48> text{};
    const int written = std::snprintf(text.data(), text.size(), "%.9g", static_cast<double>(value));
    if (written > 0) {
        output.append(text.data(), static_cast<std::size_t>(written));
    }
}

void append_vector_json(std::string& output, const std::array<float, 3>& value) {
    output.push_back('[');
    append_float(output, value[0]);
    output.append(", ");
    append_float(output, value[1]);
    output.append(", ");
    append_float(output, value[2]);
    output.push_back(']');
}

[[nodiscard]] std::string vector_text(const std::array<float, 3>& value) {
    std::array<char, 128> text{};
    const int written = std::snprintf(text.data(),
                                      text.size(),
                                      "%.4f, %.4f, %.4f",
                                      static_cast<double>(value[0]),
                                      static_cast<double>(value[1]),
                                      static_cast<double>(value[2]));
    return written > 0 && static_cast<std::size_t>(written) < text.size()
               ? std::string(text.data(), static_cast<std::size_t>(written))
               : std::string{};
}

[[nodiscard]] std::string quaternion_text(const std::array<float, 4>& value) {
    std::array<char, 160> text{};
    const int written = std::snprintf(text.data(),
                                      text.size(),
                                      "%.4f, %.4f, %.4f, %.4f",
                                      static_cast<double>(value[0]),
                                      static_cast<double>(value[1]),
                                      static_cast<double>(value[2]),
                                      static_cast<double>(value[3]));
    return written > 0 && static_cast<std::size_t>(written) < text.size()
               ? std::string(text.data(), static_cast<std::size_t>(written))
               : std::string{};
}

[[nodiscard]] std::string relationships_text(const std::vector<ActivityLogicRelationship>& relationships) {
    std::string output;
    for (const ActivityLogicRelationship& relationship : relationships) {
        if (!output.empty()) {
            output.push_back(';');
        }
        output += relationship.outgoing ? "outgoing" : "incoming";
        output.push_back(':');
        output += std::to_string(relationship.definitionTag);
        output.push_back(':');
        output += std::to_string(relationship.nameHash);
        output.push_back(':');
        output += std::to_string(relationship.occurrenceCount);
    }
    return output;
}

[[nodiscard]] bool position_changed(const std::array<float, 3>& left,
                                    const std::array<float, 3>& right,
                                    float epsilon) noexcept {
    if (epsilon <= 0.0F) {
        return left != right;
    }
    float squared = 0.0F;
    for (std::size_t lane = 0; lane < left.size(); ++lane) {
        const float delta = right[lane] - left[lane];
        if (!std::isfinite(delta)) {
            return left != right;
        }
        squared += delta * delta;
    }
    return squared > epsilon * epsilon;
}

template <typename Value>
[[nodiscard]] std::string optional_text(const std::optional<Value>& value) {
    if (!value.has_value()) {
        return "absent";
    }
    if constexpr (std::is_same_v<Value, bool>) {
        return *value ? "true" : "false";
    } else {
        return std::to_string(*value);
    }
}

[[nodiscard]] std::string source_text(const Source& source) {
    std::string result = source.packageName + "|" + source.mapStem;
    result += "|" + optional_text(source.scenarioTag);
    result += "|" + optional_text(source.spawnSetHash);
    result += "|" + optional_text(source.activitySession);
    result += "|" + optional_text(source.activityIndex);
    result += "|" + optional_text(source.bubble);
    return result;
}

[[nodiscard]] std::string bounds_text(const std::optional<Bounds>& bounds) {
    return bounds.has_value() ? vector_text(bounds->minimum) + " | " + vector_text(bounds->maximum)
                              : "absent";
}

[[nodiscard]] std::string bounds_provenance_text(
    const std::optional<Provenance>& provenance) {
    return provenance.has_value() ? std::string(provenance_name(*provenance)) : "absent";
}
[[nodiscard]] std::string spatial_state_text(const Node& node) {
    if (node.bounds.has_value() && bounds_valid(*node.bounds)) {
        return "known AABB / " + bounds_provenance_text(node.boundsProvenance);
    }
    if (node.kind == NodeKind::trigger && node.transform.has_value()) {
        return "shape unavailable; center observation only";
    }
    return "absent";
}

[[nodiscard]] std::uint64_t structural_key(const providers::WorldSnapshot& snapshot,
                                           const Node& node) noexcept {
    if (node.nativeKey != 0) {
        return node.nativeKey;
    }
    constexpr std::uint64_t kOffset = 14695981039346656037ULL;
    constexpr std::uint64_t kPrime = 1099511628211ULL;
    std::uint64_t hash = kOffset;
    const auto mix = [&hash](std::string_view value) noexcept {
        for (const unsigned char byte : value) {
            hash = (hash ^ byte) * kPrime;
        }
        hash = (hash ^ 0xFFU) * kPrime;
    };
    mix(kind_name(node.kind));
    mix(snapshot.graph.breadcrumb(node.id));
    mix(node.source.packageName);
    mix(node.source.mapStem);
    return hash == 0 ? 1 : hash;
}

[[nodiscard]] std::uint32_t identity_epoch(const providers::WorldSnapshot& snapshot,
                                                  const Node& node) noexcept {
    // The optional activity catalog is immutable browse metadata loaded once at startup.
    // It must not churn identity merely because the live activity producer epoch changes.
    return node.producer == Producer::activityCatalog
                   || node.producer == Producer::activityLogicCatalog
               ? 0U
               : snapshot.producerEpoch;
}

[[nodiscard]] std::string make_identity(const providers::WorldSnapshot& snapshot,
                                        const Node& node) {
    std::array<char, 160> text{};
    const int written =
        std::snprintf(text.data(),
                      text.size(),
                      "%s:%u:%016llX:%u",
                      producer_name(node.producer),
                      identity_epoch(snapshot, node),
                      static_cast<unsigned long long>(structural_key(snapshot, node)),
                      static_cast<unsigned>(node.kind));
    return written > 0 && static_cast<std::size_t>(written) < text.size()
               ? std::string(text.data(), static_cast<std::size_t>(written))
               : std::string{};
}

[[nodiscard]] ChangeEvent changed_event(std::uint64_t sequence,
                                        std::uint64_t tick,
                                        std::uint64_t activityRevision,
                                        std::uint32_t producerEpoch,
                                        const Node& after,
                                        std::string identity,
                                        std::string field,
                                        std::string oldValue,
                                        std::string newValue) {
    ChangeEvent event{};
    event.sequence = sequence;
    event.capturedTick = tick;
    event.nodeId = after.id.value;
    event.activityRevision = activityRevision;
    event.producerEpoch = producerEpoch;
    event.kind = ChangeKind::changed;
    event.identity = std::move(identity);
    event.nodeName = after.name;
    event.nodeKind = kind_name(after.kind);
    event.field = std::move(field);
    event.before = std::move(oldValue);
    event.after = std::move(newValue);
    event.provenance = provenance_name(after.provenance);
    return event;
}

[[nodiscard]] bool write_atomic(const core::path::Buffer& finalPath,
                                std::string_view document,
                                ExportResult& result) noexcept {
    result = {};
    if (!g_pathsReady || finalPath.length == 0
        || document.size() > (std::numeric_limits<DWORD>::max)()) {
        std::snprintf(result.error.data(), result.error.size(), "export path or size unavailable");
        return false;
    }
    static volatile LONG sequence{};
    core::path::Buffer temporary{};
    HANDLE file = INVALID_HANDLE_VALUE;
    for (unsigned attempt = 0; attempt < 8 && file == INVALID_HANDLE_VALUE; ++attempt) {
        temporary = finalPath;
        std::array<wchar_t, 96> suffix{};
        const int written = std::swprintf(suffix.data(),
                                          suffix.size(),
                                          L".%lu.%lu.%ld.tmp",
                                          GetCurrentProcessId(),
                                          GetCurrentThreadId(),
                                          InterlockedIncrement(&sequence));
        if (written <= 0 || !core::path::append(temporary, suffix.data())) {
            std::snprintf(result.error.data(), result.error.size(), "temporary path is too long");
            return false;
        }
        file = CreateFileW(temporary.chars.data(),
                           GENERIC_WRITE,
                           0,
                           nullptr,
                           CREATE_NEW,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                           nullptr);
        if (file == INVALID_HANDLE_VALUE && GetLastError() != ERROR_FILE_EXISTS) {
            break;
        }
    }
    if (file == INVALID_HANDLE_VALUE) {
        std::snprintf(
            result.error.data(), result.error.size(), "open failed (%lu)", GetLastError());
        return false;
    }
    DWORD written = 0;
    bool complete =
        WriteFile(file, document.data(), static_cast<DWORD>(document.size()), &written, nullptr)
            != FALSE
        && written == document.size() && FlushFileBuffers(file) != FALSE;
    complete = CloseHandle(file) != FALSE && complete;
    if (complete) {
        complete = MoveFileExW(temporary.chars.data(),
                               finalPath.chars.data(),
                               MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)
                   != FALSE;
    }
    if (!complete) {
        const DWORD error = GetLastError();
        (void)DeleteFileW(temporary.chars.data());
        std::snprintf(result.error.data(), result.error.size(), "write failed (%lu)", error);
        return false;
    }
    std::ranges::copy_n(finalPath.chars.begin(), finalPath.length + 1, result.path.begin());
    result.success = true;
    return true;
}

void append_optional_u64(std::string& output,
                         std::string_view name,
                         const std::optional<std::uint64_t>& value) {
    output.append(", \"");
    output.append(name);
    output.append("\": ");
    if (value.has_value()) {
        append_number(output, *value);
    } else {
        output.append("null");
    }
}

template <typename Integer>
void append_optional_integer(std::string& output,
                             std::string_view name,
                             const std::optional<Integer>& value) {
    output.append(", \"");
    output.append(name);
    output.append("\": ");
    if (value.has_value()) {
        if constexpr (std::is_signed_v<Integer>) {
            append_signed(output, static_cast<std::int64_t>(*value));
        } else {
            append_number(output, static_cast<std::uint64_t>(*value));
        }
    } else {
        output.append("null");
    }
}

void append_optional_bool(std::string& output,
                          std::string_view name,
                          const std::optional<bool>& value) {
    output.append(", \"");
    output.append(name);
    output.append("\": ");
    output.append(value.has_value() ? (*value ? "true" : "false") : "null");
}

void append_node_json(std::string& output,
                      const providers::WorldSnapshot& snapshot,
                      const Node& node) {
    output.append("    {\"id\": ");
    append_number(output, node.id.value);
    output.append(", \"parent\": ");
    append_number(output, node.parent.value);
    output.append(", \"identity\": ");
    append_json_string(output, make_identity(snapshot, node));
    output.append(", \"producer\": ");
    append_json_string(output, producer_name(node.producer));
    output.append(", \"producer_epoch\": ");
    append_number(output, identity_epoch(snapshot, node));
    output.append(", \"native_key\": ");
    append_number(output, structural_key(snapshot, node));
    output.append(", \"provenance\": ");
    append_json_string(output, provenance_name(node.provenance));
    output.append(", \"name\": ");
    append_json_string(output, node.name);
    output.append(", \"search_text\": ");
    append_json_string(output, node.searchText);
    output.append(", \"kind\": ");
    append_json_string(output, kind_name(node.kind));
    output.append(", \"status\": ");
    append_json_string(output, status_name(node.status));
    append_optional_u64(output, "runtime_entity", node.runtimeEntity);
    append_optional_u64(output, "observation_id", node.observationId);
    append_optional_u64(output, "world_id", node.worldId);
    append_optional_integer(output, "object_system_type", node.objectSystemType);
    append_optional_integer(output, "trigger_selector", node.triggerSelector);
    append_optional_integer(output, "trigger_source_hash", node.triggerSourceHash);
    append_optional_integer(output, "trigger_overlap_count", node.triggerOverlapCount);
    append_optional_bool(output, "trigger_enabled", node.triggerEnabled);
    append_optional_bool(output, "trigger_active", node.triggerActive);
    append_optional_integer(output, "tag", node.tag);
    append_optional_integer(output, "class_hash", node.classHash);
    append_optional_integer(output, "name_hash", node.nameHash);
    output.append(", \"source\": {\"package\": ");
    append_json_string(output, node.source.packageName);
    output.append(", \"map_stem\": ");
    append_json_string(output, node.source.mapStem);
    append_optional_integer(output, "scenario_tag", node.source.scenarioTag);
    append_optional_integer(output, "spawn_set_hash", node.source.spawnSetHash);
    append_optional_integer(output, "activity_session", node.source.activitySession);
    append_optional_integer(output, "activity_index", node.source.activityIndex);
    append_optional_integer(output, "bubble", node.source.bubble);
    output.append("}");
    output.append(", \"transform\": ");
    if (!node.transform.has_value()) {
        output.append("null");
    } else {
        output.append("{\"position\": ");
        append_vector_json(output, node.transform->position);
        output.append(", \"rotation\": ");
        if (node.transform->hasRotation) {
            append_vector_json(output, node.transform->rotation);
        } else {
            output.append("null");
        }
        output.append(", \"scale\": ");
        if (node.transform->hasScale) {
            append_vector_json(output, node.transform->scale);
        } else {
            output.append("null");
        }
        output.push_back('}');
    }
    output.append(", \"bounds\": ");
    if (!node.bounds.has_value()) {
        output.append("null");
    } else {
        output.append("{\"minimum\": ");
        append_vector_json(output, node.bounds->minimum);
        output.append(", \"maximum\": ");
        append_vector_json(output, node.bounds->maximum);
        output.push_back('}');
    }
    output.append(", \"bounds_provenance\": ");
    if (node.boundsProvenance.has_value()) {
        append_json_string(output, provenance_name(*node.boundsProvenance));
    } else {
        output.append("null");
    }
    output.append(", \"spatial_helper_state\": ");
    append_json_string(output, spatial_state_text(node));
    output.append(", \"activity_metadata\": ");
    if (!node.activityMetadata.has_value()) {
        output.append("null");
    } else {
        const ActivityMetadata& metadata = *node.activityMetadata;
        output.append("{\"activity_hash\": ");
        append_number(output, metadata.activityHash);
        output.append(", \"graph_hash\": ");
        append_number(output, metadata.graphHash);
        output.append(", \"node_hash\": ");
        append_number(output, metadata.nodeHash);
        output.append(", \"state_hash\": ");
        append_number(output, metadata.stateHash);
        output.append(", \"style_hash\": ");
        append_number(output, metadata.styleHash);
        output.append(", \"authored_position\": [");
        append_float(output, metadata.authoredPosition[0]);
        output.append(", ");
        append_float(output, metadata.authoredPosition[1]);
        output.append("], \"release_count\": ");
        append_number(output, metadata.releaseCount);
        output.append(", \"reference_count\": ");
        append_number(output, metadata.referenceCount);
        output.append(", \"catalog_build\": ");
        append_number(output, metadata.catalogBuild);
        output.append(", \"catalog_version\": ");
        append_json_string(output, metadata.catalogVersion);
        output.append(", \"linked_graphs\": [");
        for (std::size_t index = 0; index < metadata.linkedGraphHashes.size(); ++index) {
            if (index != 0) {
                output.append(", ");
            }
            append_number(output, metadata.linkedGraphHashes[index]);
        }
        output.append("]");
        output.append(", \"build_match\": ");
        output.append(metadata.buildMatch ? "true" : "false");
        output.append(", \"browse_only\": ");
        output.append(metadata.browseOnly ? "true}" : "false}");
    }
    output.append(", \"activity_logic_metadata\": ");
    if (!node.activityLogicMetadata.has_value()) {
        output.append("null");
    } else {
        const ActivityLogicMetadata& metadata = *node.activityLogicMetadata;
        output.append("{\"scenario_tag\": ");
        append_number(output, metadata.scenarioTag);
        output.append(", \"definition_tag\": ");
        append_number(output, metadata.definitionTag);
        output.append(", \"class_primary\": ");
        append_number(output, metadata.classPrimary);
        output.append(", \"class_secondary\": ");
        append_number(output, metadata.classSecondary);
        output.append(", \"role\": ");
        append_number(output, metadata.role);
        output.append(", \"role_name\": ");
        append_json_string(output, metadata.roleName);
        output.append(", \"confidence\": ");
        append_number(output, metadata.confidence);
        output.append(", \"confidence_name\": ");
        append_json_string(output, metadata.confidenceName);
        output.append(", \"label\": ");
        append_json_string(output, metadata.label);
        output.append(", \"localized_text\": ");
        append_json_string(output, metadata.localizedText);
        output.append(", \"placement_count\": ");
        append_number(output, metadata.placementCount);
        output.append(", \"has_placement\": ");
        output.append(metadata.hasPlacement ? "true" : "false");
        output.append(", \"world_id\": ");
        append_number(output, metadata.worldId);
        output.append(", \"map_table_tag\": ");
        append_number(output, metadata.mapTableTag);
        output.append(", \"placed_entity_tag\": ");
        append_number(output, metadata.placedEntityTag);
        output.append(", \"authored_rotation\": [");
        for (std::size_t index = 0; index < metadata.authoredRotation.size(); ++index) {
            if (index != 0) {
                output.append(", ");
            }
            append_float(output, metadata.authoredRotation[index]);
        }
        output.append("], \"relationships\": [");
        for (std::size_t index = 0; index < metadata.relationships.size(); ++index) {
            if (index != 0) {
                output.append(", ");
            }
            const ActivityLogicRelationship& relationship = metadata.relationships[index];
            output.append("{\"direction\": ");
            append_json_string(output, relationship.outgoing ? "outgoing" : "incoming");
            output.append(", \"definition_tag\": ");
            append_number(output, relationship.definitionTag);
            output.append(", \"name_hash\": ");
            append_number(output, relationship.nameHash);
            output.append(", \"occurrence_count\": ");
            append_number(output, relationship.occurrenceCount);
            output.push_back('}');
        }
        output.append("]}");
    }
    output.append(", \"transform_runtime\": ");
    output.append(node.transformRuntime ? "true" : "false");
    output.append(", \"linear_velocity\": ");
    if (node.linearVelocity.has_value()) {
        append_vector_json(output, *node.linearVelocity);
    } else {
        output.append("null");
    }
    output.append(", \"actions\": ");
    append_number(output, static_cast<std::uint32_t>(node.actions));
    output.append(", \"children\": [");
    for (std::size_t index = 0; index < node.children.size(); ++index) {
        if (index != 0) {
            output.append(", ");
        }
        append_number(output, node.children[index].value);
    }
    output.append("]}");
}

[[nodiscard]] std::string snapshot_json(const InspectionSnapshot& snapshot,
                                        const RouteCaptureMetadata* route = nullptr) {
    const auto& world = snapshot.world;
    std::string output;
    output.reserve(world.graph.nodes().size() * 384U + 2048U);
    output.append("{\n  \"schema_version\": ");
    append_number(output, snapshot.schemaVersion);
    output.append(",\n  \"build\": {\"client\": ");
    append_json_string(output, activity_catalog::kTargetContentBuildText);
    output.append(", \"expected_sha256\": ");
    append_json_string(output, kBuildHash);
    output.append(", \"image_sha256\": ");
    if (snapshot.imageSha256.empty()) {
        output.append("null");
    } else {
        append_json_string(output, snapshot.imageSha256);
    }
    output.append(", \"verified\": ");
    output.append(snapshot.imageVerified ? "true" : "false");
    output.append("},\n  \"captured_tick\": ");
    append_number(output, snapshot.capturedTick);
    output.append(",\n  \"activity\": {\"present\": ");
    output.append(world.sessionPresent ? "true" : "false");
    output.append(", \"session\": ");
    append_number(output, world.activitySession);
    output.append(", \"revision\": ");
    append_number(output, world.activityRevision);
    output.append(", \"index\": ");
    append_signed(output, world.activityIndex);
    output.append(", \"package\": ");
    append_json_string(output, world.packageName);
    output.append(", \"map_stem\": ");
    append_json_string(output, world.mapStem);
    output.append("},\n  \"activity_catalog\": {\"present\": ");
    output.append(world.activityCatalogPresent ? "true" : "false");
    output.append(", \"build\": ");
    append_number(output, world.activityCatalogBuild);
    output.append(", \"version\": ");
    append_json_string(output, world.activityCatalogVersion);
    output.append(", \"build_match\": ");
    output.append(world.activityCatalogBuildMatch ? "true" : "false");
    output.append(", \"diagnostic\": ");
    append_json_string(output, world.activityCatalogDiagnostic);
    output.append("},\n  \"coverage\": {\"graph_generation\": ");
    append_number(output, world.graph.generation());
    output.append(", \"runtime_objects_ready\": ");
    output.append(world.runtimeObjectsPresent ? "true" : "false");
    output.append(", \"runtime_objects_declared\": ");
    append_number(output, world.runtimeObjectDeclaredCount);
    output.append(", \"runtime_objects_copied\": ");
    append_number(output, world.runtimeObjectCount);
    output.append(", \"runtime_objects_truncated\": ");
    output.append(world.runtimeObjectsTruncated ? "true" : "false");
    output.append(", \"triggers_ready\": ");
    output.append(world.triggersPresent ? "true" : "false");
    output.append(", \"triggers_copied\": ");
    append_number(output, world.triggerCount);
    output.append(", \"triggers_truncated\": ");
    output.append(world.triggersTruncated ? "true" : "false");
    output.append(", \"audio_listener_ready\": ");
    output.append(world.audioListenerPresent ? "true" : "false");
    output.append(", \"physics_ready\": ");
    output.append(world.physicsPresent ? "true" : "false");
    output.append(", \"physics_declared\": ");
    append_number(output, world.physicsDeclaredSlots);
    output.append(", \"physics_copied\": ");
    append_number(output, world.physicsBodyCount);
    output.append(", \"physics_truncated\": ");
    output.append(world.physicsTruncated ? "true" : "false");
    output.append("},\n  \"nodes\": [\n");
    const auto& nodes = world.graph.nodes();
    for (std::size_t index = 0; index < nodes.size(); ++index) {
        append_node_json(output, world, nodes[index]);
        output.append(index + 1 == nodes.size() ? "\n" : ",\n");
    }
    output.append("  ],\n  \"diagnostics\": [\n");
    for (std::size_t index = 0; index < world.diagnostics.size(); ++index) {
        const Diagnostic& diagnostic = world.diagnostics[index];
        output.append("    {\"severity\": ");
        append_number(output, static_cast<std::uint8_t>(diagnostic.severity));
        output.append(", \"message\": ");
        append_json_string(output, diagnostic.message);
        output.append(index + 1 == world.diagnostics.size() ? "}\n" : "},\n");
    }
    output.append("  ],\n  \"producers\": [\n");
    for (std::size_t index = 0; index < snapshot.producers.size(); ++index) {
        const InspectionSnapshot::ProducerState& producerState = snapshot.producers[index];
        output.append("    {\"name\": ");
        append_json_string(output, producerState.name);
        output.append(", \"version\": 1, \"epoch\": ");
        append_number(output, producerState.epoch);
        output.append(", \"installed\": ");
        output.append(producerState.installed ? "true" : "false");
        output.append(", \"ready\": ");
        output.append(producerState.ready ? "true" : "false");
        output.append(", \"sequence\": ");
        append_number(output, producerState.sequence);
        output.append(", \"declared_count\": ");
        append_number(output, producerState.declaredCount);
        output.append(", \"copied_count\": ");
        append_number(output, producerState.copiedCount);
        output.append(", \"truncated\": ");
        output.append(producerState.truncated ? "true" : "false");
        output.append(", \"failure\": ");
        if (producerState.failure.empty()) {
            output.append("null");
        } else {
            append_json_string(output, producerState.failure);
        }
        output.append(index + 1 == snapshot.producers.size() ? "}\n" : "},\n");
    }
    output.append("  ],\n  \"producer_versions\": {\"catalog\": 1, \"activity-catalog\": 1, "
                  "\"local-player\": 1, \"object-system\": 1, \"trigger\": 1, "
                  "\"audio-listener\": 1, \"physics\": 1},\n  \"unavailable_producers\": ["
                  "\"audio-emitter\", \"navigation\", \"light\", \"terrain\", \"entity-bounds\"]");
    if (route != nullptr) {
        output.append(",\n  \"route_capture\": {\"path\": ");
        append_json_string(output, route->pathName);
        output.append(", \"keyframe_label\": ");
        append_json_string(output, route->keyframeLabel);
        output.append(", \"keyframe_index\": ");
        append_number(output, route->keyframeIndex);
        output.append(", \"camera_session\": ");
        append_number(output, route->cameraSession);
        output.append(", \"capture_sequence\": ");
        append_number(output, route->captureSequence);
        output.append(", \"pose\": {\"position\": ");
        append_vector_json(output, route->position);
        output.append(", \"yaw\": ");
        append_float(output, route->yaw);
        output.append(", \"pitch\": ");
        append_float(output, route->pitch);
        output.append(", \"fov\": ");
        append_float(output, route->fov);
        output.append("}}");
    }
    output.append("\n}\n");
    return output;
}
[[nodiscard]] std::string snapshot_csv(const InspectionSnapshot& snapshot) {
    std::string output =
        "schema_version,expected_image_sha256,image_sha256,image_verified,activity_revision,"
        "activity_catalog_build,activity_catalog_version,activity_catalog_build_match,"
        "producer_status,"
        "identity,producer,producer_epoch,native_key,id,parent,name,search_text,kind,status,"
        "provenance,package,map_stem,source_scenario_tag,source_spawn_set_hash,source_session,"
        "source_activity,source_bubble,runtime_entity,observation_id,object_type,world_id,tag,"
        "class_hash,name_hash,trigger_selector,trigger_source_hash,trigger_overlap_count,"
        "trigger_enabled,trigger_active,position_x,position_y,position_z,rotation,scale,"
        "transform_runtime,velocity,bounds,bounds_provenance,spatial_helper_state,"
        "activity_graph_hash,activity_node_hash,activity_hash,activity_x,activity_y,"
        "activity_catalog_build,activity_catalog_version,activity_build_match,"
        "activity_logic_scenario_tag,activity_logic_definition_tag,activity_logic_role,"
        "activity_logic_role_name,activity_logic_confidence,activity_logic_confidence_name,"
        "activity_logic_label,activity_logic_localized_text,activity_logic_placement_count,"
        "activity_logic_has_placement,activity_logic_world_id,activity_logic_map_table_tag,"
        "activity_logic_placed_entity_tag,activity_logic_authored_rotation,"
        "activity_logic_relationships,actions\r\n";
    output.reserve(snapshot.world.graph.nodes().size() * 192U + output.size());
    std::string producerStatus;
    for (const InspectionSnapshot::ProducerState& state : snapshot.producers) {
        if (!producerStatus.empty()) {
            producerStatus.push_back(';');
        }
        producerStatus +=
            state.name + ":version=1:installed=" + (state.installed ? "1" : "0") + ":ready="
            + (state.ready ? "1" : "0") + ":truncated=" + (state.truncated ? "1" : "0") + ":epoch="
            + std::to_string(state.epoch) + ":sequence=" + std::to_string(state.sequence)
            + ":failure=" + (state.failure.empty() ? "none" : state.failure) + ":count="
            + std::to_string(state.copiedCount) + "/" + std::to_string(state.declaredCount);
    }
    for (const Node& node : snapshot.world.graph.nodes()) {
        append_number(output, snapshot.schemaVersion);
        output.push_back(',');
        append_csv(output, kBuildHash);
        output.push_back(',');
        append_csv(output, snapshot.imageSha256);
        output.push_back(',');
        output.append(snapshot.imageVerified ? "true," : "false,");
        append_number(output, snapshot.world.activityRevision);
        output.push_back(',');
        append_number(output, snapshot.world.activityCatalogBuild);
        output.push_back(',');
        append_csv(output, snapshot.world.activityCatalogVersion);
        output.push_back(',');
        output.append(snapshot.world.activityCatalogBuildMatch ? "true," : "false,");
        append_csv(output, producerStatus);
        output.push_back(',');
        append_csv(output, make_identity(snapshot.world, node));
        output.push_back(',');
        append_csv(output, producer_name(node.producer));
        output.push_back(',');
        append_number(output, identity_epoch(snapshot.world, node));
        output.push_back(',');
        append_number(output, structural_key(snapshot.world, node));
        output.push_back(',');
        append_number(output, node.id.value);
        output.push_back(',');
        append_number(output, node.parent.value);
        output.push_back(',');
        append_csv(output, node.name);
        output.push_back(',');
        append_csv(output, node.searchText);
        output.push_back(',');
        append_csv(output, kind_name(node.kind));
        output.push_back(',');
        append_csv(output, status_name(node.status));
        output.push_back(',');
        append_csv(output, provenance_name(node.provenance));
        output.push_back(',');
        append_csv(output, node.source.packageName);
        output.push_back(',');
        append_csv(output, node.source.mapStem);
        for (const std::string value : {optional_text(node.source.scenarioTag),
                                        optional_text(node.source.spawnSetHash),
                                        optional_text(node.source.activitySession),
                                        optional_text(node.source.activityIndex),
                                        optional_text(node.source.bubble),
                                        optional_text(node.runtimeEntity),
                                        optional_text(node.observationId),
                                        optional_text(node.objectSystemType),
                                        optional_text(node.worldId),
                                        optional_text(node.tag),
                                        optional_text(node.classHash),
                                        optional_text(node.nameHash),
                                        optional_text(node.triggerSelector),
                                        optional_text(node.triggerSourceHash),
                                        optional_text(node.triggerOverlapCount),
                                        optional_text(node.triggerEnabled),
                                        optional_text(node.triggerActive)}) {
            output.push_back(',');
            append_csv(output, value);
        }
        if (node.transform.has_value()) {
            for (const float lane : node.transform->position) {
                output.push_back(',');
                append_float(output, lane);
            }
        } else {
            output.append(",,,");
        }
        output.push_back(',');
        append_csv(output,
                   node.transform.has_value() && node.transform->hasRotation
                       ? vector_text(node.transform->rotation)
                       : "absent");
        output.push_back(',');
        append_csv(output,
                   node.transform.has_value() && node.transform->hasScale
                       ? vector_text(node.transform->scale)
                       : "absent");
        output.push_back(',');
        output.append(node.transformRuntime ? "true" : "false");
        output.push_back(',');
        append_csv(output,
                   node.linearVelocity.has_value() ? vector_text(*node.linearVelocity) : "absent");
        output.push_back(',');
        append_csv(output, bounds_text(node.bounds));
        output.push_back(',');
        append_csv(output, bounds_provenance_text(node.boundsProvenance));
        output.push_back(',');
        append_csv(output, spatial_state_text(node));
        output.push_back(',');
        if (node.activityMetadata.has_value()) {
            const ActivityMetadata& metadata = *node.activityMetadata;
            append_number(output, metadata.graphHash);
            output.push_back(',');
            append_number(output, metadata.nodeHash);
            output.push_back(',');
            append_number(output, metadata.activityHash);
            output.push_back(',');
            append_float(output, metadata.authoredPosition[0]);
            output.push_back(',');
            append_float(output, metadata.authoredPosition[1]);
            output.push_back(',');
            append_number(output, metadata.catalogBuild);
            output.push_back(',');
            append_csv(output, metadata.catalogVersion);
            output.push_back(',');
            output.append(metadata.buildMatch ? "true," : "false,");
        } else {
            output.append(",,,,,,,");
        }
        if (node.activityLogicMetadata.has_value()) {
            const ActivityLogicMetadata& metadata = *node.activityLogicMetadata;
            append_number(output, metadata.scenarioTag);
            output.push_back(',');
            append_number(output, metadata.definitionTag);
            output.push_back(',');
            append_number(output, metadata.role);
            output.push_back(',');
            append_csv(output, metadata.roleName);
            output.push_back(',');
            append_number(output, metadata.confidence);
            output.push_back(',');
            append_csv(output, metadata.confidenceName);
            output.push_back(',');
            append_csv(output, metadata.label);
            output.push_back(',');
            append_csv(output, metadata.localizedText);
            output.push_back(',');
            append_number(output, metadata.placementCount);
            output.push_back(',');
            output.append(metadata.hasPlacement ? "true," : "false,");
            append_number(output, metadata.worldId);
            output.push_back(',');
            append_number(output, metadata.mapTableTag);
            output.push_back(',');
            append_number(output, metadata.placedEntityTag);
            output.push_back(',');
            append_csv(output, quaternion_text(metadata.authoredRotation));
            output.push_back(',');
            append_csv(output, relationships_text(metadata.relationships));
            output.push_back(',');
        } else {
            output.append(",,,,,,,,,,,,,,,");
        }
        append_number(output, static_cast<std::uint32_t>(node.actions));
        output.append("\r\n");
    }
    return output;
}

[[nodiscard]] std::string events_json(std::span<const ChangeEvent> events,
                                      const InspectionSnapshot& current) {
    std::string output;
    output.reserve(events.size() * 256U + 128U);
    output.append("{\n  \"schema_version\": ");
    append_number(output, kSchemaVersion);
    output.append(",\n  \"build\": {\"client\": ");
    append_json_string(output, activity_catalog::kTargetContentBuildText);
    output.append(", \"expected_sha256\": ");
    append_json_string(output, kBuildHash);
    output.append(", \"image_sha256\": ");
    if (current.imageSha256.empty()) {
        output.append("null");
    } else {
        append_json_string(output, current.imageSha256);
    }
    output.append(", \"verified\": ");
    output.append(current.imageVerified ? "true" : "false");
    output.append("},\n  \"activity_revision\": ");
    append_number(output, current.world.activityRevision);
    output.append(",\n  \"activity_catalog\": {\"present\": ");
    output.append(current.world.activityCatalogPresent ? "true" : "false");
    output.append(", \"build\": ");
    append_number(output, current.world.activityCatalogBuild);
    output.append(", \"version\": ");
    append_json_string(output, current.world.activityCatalogVersion);
    output.append(", \"build_match\": ");
    output.append(current.world.activityCatalogBuildMatch ? "true" : "false");
    output.append("}");
    output.append(",\n  \"producer_epoch\": ");
    append_number(output, current.world.producerEpoch);
    output.append(",\n  \"producer_status\": [");
    for (std::size_t index = 0; index < current.producers.size(); ++index) {
        if (index != 0) {
            output.append(", ");
        }
        output.append("{\"name\": ");
        append_json_string(output, current.producers[index].name);
        output.append(", \"version\": 1, \"installed\": ");
        output.append(current.producers[index].installed ? "true" : "false");
        output.append(", \"ready\": ");
        output.append(current.producers[index].ready ? "true" : "false");
        output.append(", \"truncated\": ");
        output.append(current.producers[index].truncated ? "true" : "false");
        output.append(", \"sequence\": ");
        append_number(output, current.producers[index].sequence);
        output.append(", \"epoch\": ");
        append_number(output, current.producers[index].epoch);
        output.append(", \"declared_count\": ");
        append_number(output, current.producers[index].declaredCount);
        output.append(", \"copied_count\": ");
        append_number(output, current.producers[index].copiedCount);
        output.append(", \"failure\": ");
        if (current.producers[index].failure.empty()) {
            output.append("null}");
        } else {
            append_json_string(output, current.producers[index].failure);
            output.push_back('}');
        }
    }
    output.append("],\n  \"events\": [\n");
    for (std::size_t index = 0; index < events.size(); ++index) {
        const ChangeEvent& event = events[index];
        output.append("    {\"sequence\": ");
        append_number(output, event.sequence);
        output.append(", \"captured_tick\": ");
        append_number(output, event.capturedTick);
        output.append(", \"activity_revision\": ");
        append_number(output, event.activityRevision);
        output.append(", \"producer_epoch\": ");
        append_number(output, event.producerEpoch);
        output.append(", \"kind\": ");
        append_json_string(output, change_kind_name(event.kind));
        output.append(", \"identity\": ");
        append_json_string(output, event.identity);
        output.append(", \"node_name\": ");
        append_json_string(output, event.nodeName);
        output.append(", \"node_kind\": ");
        append_json_string(output, event.nodeKind);
        output.append(", \"field\": ");
        append_json_string(output, event.field);
        output.append(", \"before\": ");
        append_json_string(output, event.before);
        output.append(", \"after\": ");
        append_json_string(output, event.after);
        output.append(", \"provenance\": ");
        append_json_string(output, event.provenance);
        output.append("}");
        output.append(index + 1 == events.size() ? "\n" : ",\n");
    }
    output.append("  ]\n}\n");
    return output;
}

} // namespace

History::StateMap History::collect(const providers::WorldSnapshot& snapshot) const {
    StateMap state;
    state.reserve(snapshot.graph.nodes().size());
    for (const Node& node : snapshot.graph.nodes()) {
        // Havok array slots are observations, not durable body identities. Export them, but do not
        // claim continuity for change history until a producer can prove a lifetime key.
        // Browse-only activity metadata is immutable and would only waste the bounded event bank.
        if (node.producer == Producer::physics || node.producer == Producer::activityCatalog
            || node.producer == Producer::activityLogicCatalog) {
            continue;
        }
        if (options_.runtimeOnly && node.provenance != Provenance::runtime) {
            continue;
        }
        NodeState value{node, make_identity(snapshot, node)};
        state.insert_or_assign(value.identity, std::move(value));
    }
    return state;
}

void History::append(ChangeEvent event) {
    if (events_.size() == kEventCapacity) {
        events_.erase(events_.begin());
    }
    events_.push_back(std::move(event));
}

void History::set_recording(bool recording, const providers::WorldSnapshot& snapshot) {
    if (recording == recording_) {
        return;
    }
    recording_ = recording;
    previous_ = recording ? collect(snapshot) : StateMap{};
}

void History::set_options(ChangeTrackingOptions options,
                          const providers::WorldSnapshot& snapshot) {
    if (!std::isfinite(options.positionEpsilon) || options.positionEpsilon < 0.0F) {
        options.positionEpsilon = 0.05F;
    }
    if (options == options_) {
        return;
    }
    options_ = options;
    if (recording_) {
        // Changing scope/fields establishes a fresh baseline rather than manufacturing events.
        previous_ = collect(snapshot);
    }
}

void History::observe(const providers::WorldSnapshot& snapshot) {
    if (!recording_) {
        return;
    }
    StateMap current = collect(snapshot);
    const std::uint64_t tick = GetTickCount64();
    for (const auto& [identity, oldState] : previous_) {
        if (current.contains(identity)) {
            continue;
        }
        ChangeEvent event{};
        event.sequence = ++sequence_;
        event.capturedTick = tick;
        event.nodeId = oldState.node.id.value;
        event.activityRevision = snapshot.activityRevision;
        event.producerEpoch = snapshot.producerEpoch;
        event.kind = ChangeKind::removed;
        event.identity = identity;
        event.nodeName = oldState.node.name;
        event.nodeKind = kind_name(oldState.node.kind);
        event.field = "node";
        event.before = oldState.node.name;
        event.provenance = provenance_name(oldState.node.provenance);
        append(std::move(event));
    }
    for (const auto& [identity, newState] : current) {
        const auto found = previous_.find(identity);
        if (found == previous_.end()) {
            ChangeEvent event{};
            event.sequence = ++sequence_;
            event.capturedTick = tick;
            event.nodeId = newState.node.id.value;
            event.activityRevision = snapshot.activityRevision;
            event.producerEpoch = snapshot.producerEpoch;
            event.kind = ChangeKind::added;
            event.identity = identity;
            event.nodeName = newState.node.name;
            event.nodeKind = kind_name(newState.node.kind);
            event.field = "node";
            event.after = newState.node.name;
            event.provenance = provenance_name(newState.node.provenance);
            append(std::move(event));
            continue;
        }
        const Node& before = found->second.node;
        const Node& after = newState.node;
        const auto field_changed =
            [&](std::string field, std::string oldValue, std::string newValue) {
                append(changed_event(++sequence_,
                                     tick,
                                     snapshot.activityRevision,
                                     identity_epoch(snapshot, after),
                                     after,
                                     identity,
                                     std::move(field),
                                     std::move(oldValue),
                                     std::move(newValue)));
            };
        if (before.name != after.name) {
            field_changed("name", before.name, after.name);
        }
        if (before.searchText != after.searchText) {
            field_changed("search_text", before.searchText, after.searchText);
        }
        if (before.provenance != after.provenance) {
            field_changed("provenance",
                          provenance_name(before.provenance),
                          provenance_name(after.provenance));
        }
        if (before.parent != after.parent) {
            field_changed(
                "parent", std::to_string(before.parent.value), std::to_string(after.parent.value));
        }
        if (before.children != after.children) {
            const auto children_text = [](const std::vector<NodeId>& children) {
                std::string value;
                for (const NodeId child : children) {
                    if (!value.empty()) {
                        value.push_back(',');
                    }
                    value += std::to_string(child.value);
                }
                return value;
            };
            field_changed(
                "children", children_text(before.children), children_text(after.children));
        }
        if (before.status != after.status) {
            field_changed("status", status_name(before.status), status_name(after.status));
        }
        if (before.objectSystemType != after.objectSystemType) {
            field_changed("object_type",
                          optional_text(before.objectSystemType),
                          optional_text(after.objectSystemType));
        }
        if (options_.trackTransforms) {
            if (before.transform.has_value() != after.transform.has_value()) {
                field_changed("transform",
                              before.transform.has_value() ? "present" : "absent",
                              after.transform.has_value() ? "present" : "absent");
            } else if (before.transform.has_value()
                       && position_changed(before.transform->position,
                                           after.transform->position,
                                           options_.positionEpsilon)) {
                field_changed("position",
                              vector_text(before.transform->position),
                              vector_text(after.transform->position));
            }
            if (before.transform.has_value() && after.transform.has_value()) {
                if (before.transform->rotation != after.transform->rotation
                    || before.transform->hasRotation != after.transform->hasRotation) {
                    field_changed("rotation",
                                  before.transform->hasRotation
                                      ? vector_text(before.transform->rotation)
                                      : "absent",
                                  after.transform->hasRotation
                                      ? vector_text(after.transform->rotation)
                                      : "absent");
                }
                if (before.transform->scale != after.transform->scale
                    || before.transform->hasScale != after.transform->hasScale) {
                    field_changed(
                        "scale",
                        before.transform->hasScale ? vector_text(before.transform->scale) : "absent",
                        after.transform->hasScale ? vector_text(after.transform->scale) : "absent");
                }
            }
            if (before.transformRuntime != after.transformRuntime) {
                field_changed("transform_runtime",
                              before.transformRuntime ? "true" : "false",
                              after.transformRuntime ? "true" : "false");
            }
            if (before.linearVelocity != after.linearVelocity) {
                field_changed(
                    "linear_velocity",
                    before.linearVelocity.has_value() ? vector_text(*before.linearVelocity) : "absent",
                    after.linearVelocity.has_value() ? vector_text(*after.linearVelocity) : "absent");
            }
        }
        if (before.bounds != after.bounds) {
            field_changed("bounds", bounds_text(before.bounds), bounds_text(after.bounds));
        }
        if (before.boundsProvenance != after.boundsProvenance) {
            field_changed("bounds_provenance",
                          bounds_provenance_text(before.boundsProvenance),
                          bounds_provenance_text(after.boundsProvenance));
        }
        if (spatial_state_text(before) != spatial_state_text(after)) {
            field_changed("spatial_helper_state",
                          spatial_state_text(before),
                          spatial_state_text(after));
        }
        if (before.runtimeEntity != after.runtimeEntity) {
            field_changed("runtime_entity",
                          optional_text(before.runtimeEntity),
                          optional_text(after.runtimeEntity));
        }
        if (before.observationId != after.observationId) {
            field_changed("observation_id",
                          optional_text(before.observationId),
                          optional_text(after.observationId));
        }
        if (before.worldId != after.worldId) {
            field_changed("world_id", optional_text(before.worldId), optional_text(after.worldId));
        }
        if (before.tag != after.tag || before.classHash != after.classHash
            || before.nameHash != after.nameHash) {
            field_changed("catalog_keys",
                          optional_text(before.tag) + "/" + optional_text(before.classHash) + "/"
                              + optional_text(before.nameHash),
                          optional_text(after.tag) + "/" + optional_text(after.classHash) + "/"
                              + optional_text(after.nameHash));
        }
        if (source_text(before.source) != source_text(after.source)) {
            field_changed("source", source_text(before.source), source_text(after.source));
        }
        if (before.actions != after.actions) {
            field_changed("actions",
                          std::to_string(static_cast<std::uint32_t>(before.actions)),
                          std::to_string(static_cast<std::uint32_t>(after.actions)));
        }
        if (before.triggerActive != after.triggerActive) {
            field_changed("trigger_active",
                          optional_text(before.triggerActive),
                          optional_text(after.triggerActive));
        }
        if (before.triggerOverlapCount != after.triggerOverlapCount) {
            field_changed("trigger_overlap_count",
                          optional_text(before.triggerOverlapCount),
                          optional_text(after.triggerOverlapCount));
        }
        if (before.triggerEnabled != after.triggerEnabled) {
            field_changed("trigger_enabled",
                          optional_text(before.triggerEnabled),
                          optional_text(after.triggerEnabled));
        }
        if (before.triggerSelector != after.triggerSelector) {
            field_changed("trigger_selector",
                          optional_text(before.triggerSelector),
                          optional_text(after.triggerSelector));
        }
        if (before.triggerSourceHash != after.triggerSourceHash) {
            field_changed("trigger_source_hash",
                          optional_text(before.triggerSourceHash),
                          optional_text(after.triggerSourceHash));
        }
    }
    if (events_.size() > kEventCapacity) {
        events_.erase(events_.begin(),
                      events_.begin()
                          + static_cast<std::ptrdiff_t>(events_.size() - kEventCapacity));
    }
    previous_ = std::move(current);
}

void History::clear() noexcept {
    events_.clear();
    sequence_ = 0;
}

bool History::recording() const noexcept {
    return recording_;
}

ChangeTrackingOptions History::options() const noexcept {
    return options_;
}

std::span<const ChangeEvent> History::events() const noexcept {
    return events_;
}

void initialize(void* module) noexcept {
    capture_image_hash();
    core::path::Buffer root{};
    g_pathsReady = core::path::artifact_directory(module, root);
    if (!g_pathsReady) {
        return;
    }
    g_jsonPath = root;
    g_rootPath = root;
    g_csvPath = root;
    g_eventsPath = root;
    g_pathsReady = core::path::append(g_jsonPath, kJsonSuffix)
                   && core::path::append(g_csvPath, kCsvSuffix)
                   && core::path::append(g_eventsPath, kEventsSuffix);
}

void shutdown() noexcept {
    g_jsonPath = {};
    g_rootPath = {};
    g_csvPath = {};
    g_eventsPath = {};
    g_pathsReady = false;
    g_imageHash.clear();
    g_imageVerified = false;
}

InspectionSnapshot make_snapshot(const providers::WorldSnapshot& snapshot) {
    InspectionSnapshot result{};
    result.world = snapshot;
    result.imageSha256 = g_imageHash;
    result.imageVerified = g_imageVerified;
    result.capturedTick = GetTickCount64();
    result.schemaVersion = kSchemaVersion;
    const auto add = [&result, epoch = snapshot.producerEpoch](std::string name,
                                                               bool installed,
                                                               bool ready,
                                                               std::uint64_t sequence,
                                                               std::uint64_t declared,
                                                               std::uint64_t copied,
                                                               bool truncated,
                                                               std::string failure = {}) {
        result.producers.push_back({std::move(name),
                                    std::move(failure),
                                    sequence,
                                    declared,
                                    copied,
                                    epoch,
                                    installed,
                                    ready,
                                    truncated});
    };
    add("catalog",
        true,
        snapshot.scenarioCatalogReady && snapshot.spawnCatalogReady,
        0,
        snapshot.placedObjectCount + snapshot.placedObjectSlotCount,
        snapshot.placedObjectCount + snapshot.placedObjectSlotCount,
        snapshot.scenarioTruncated || snapshot.placedObjectSlotsTruncated);
    add("activity-catalog",
        snapshot.activityCatalogPresent,
        snapshot.activityCatalogPresent,
        0,
        snapshot.activityCatalogPresent ? 1 : 0,
        snapshot.activityCatalogPresent ? 1 : 0,
        false,
        snapshot.activityCatalogDiagnostic);
    add("activity-logic-catalog",
        snapshot.activityLogicPresent,
        snapshot.activityLogicMatched,
        0,
        snapshot.activityLogicDefinitionCount,
        snapshot.activityLogicDefinitionCount,
        false,
        snapshot.activityLogicDiagnostic);
    add("local-player",
        true,
        snapshot.localPlayerPresent,
        0,
        snapshot.localPlayerPresent ? 1 : 0,
        snapshot.localPlayerPresent ? 1 : 0,
        false);

    client::viewer::objects::Snapshot objects{};
    (void)client::viewer::objects::snapshot(objects);
    add("object-system",
        client::viewer::objects::installed(),
        objects.present,
        objects.sequence,
        objects.declaredCount,
        objects.objectCount,
        objects.truncated,
        client::viewer::objects::installed() ? std::string{} : "not installed");
    client::viewer::triggers::Snapshot triggers{};
    (void)client::viewer::triggers::snapshot(triggers);
    add("trigger",
        client::viewer::triggers::installed(),
        triggers.present,
        triggers.sequence,
        triggers.triggerCount,
        triggers.triggerCount,
        triggers.truncated,
        client::viewer::triggers::installed() ? std::string{} : "not installed");
    add("audio-listener",
        client::viewer::audio::installed(),
        snapshot.audioListenerPresent,
        0,
        snapshot.audioListenerPresent ? 1 : 0,
        snapshot.audioListenerPresent ? 1 : 0,
        false,
        client::viewer::audio::installed() ? std::string{} : "not installed");
    client::hooks::noclip::PhysicsObservationSnapshot physics{};
    const bool physicsReady = client::hooks::noclip::physics_observation_snapshot(physics);
    add("physics",
        client::hooks::noclip::installed(),
        physicsReady,
        physics.sequence,
        physics.declaredSlots,
        physics.bodyCount,
        physics.truncated,
        client::hooks::noclip::installed() ? std::string{} : "not installed");
    for (const char* unavailable :
         {"audio-emitter", "navigation", "light", "terrain", "entity-bounds"}) {
        add(unavailable, false, false, 0, 0, 0, false, "research gate not satisfied");
    }
    return result;
}

std::string stable_identity(const providers::WorldSnapshot& snapshot, const Node& node) {
    return make_identity(snapshot, node);
}

std::uint64_t stable_native_key(const providers::WorldSnapshot& snapshot,
                                const Node& node) noexcept {
    return structural_key(snapshot, node);
}

const char* change_kind_name(ChangeKind kind) noexcept {
    switch (kind) {
    case ChangeKind::added:
        return "added";
    case ChangeKind::removed:
        return "removed";
    case ChangeKind::changed:
        return "changed";
    }
    return "changed";
}

ExportResult export_json(const InspectionSnapshot& snapshot) noexcept {
    ExportResult result{};
    try {
        const std::string document = snapshot_json(snapshot);
        (void)write_atomic(g_jsonPath, document, result);
    } catch (...) {
        std::snprintf(result.error.data(), result.error.size(), "serialization allocation failed");
    }
    return result;
}

ExportResult export_csv(const InspectionSnapshot& snapshot) noexcept {
    ExportResult result{};
    try {
        const std::string document = snapshot_csv(snapshot);
        (void)write_atomic(g_csvPath, document, result);
    } catch (...) {
        std::snprintf(result.error.data(), result.error.size(), "serialization allocation failed");
    }
    return result;
}

ExportResult export_route_json(const InspectionSnapshot& snapshot,
                               const RouteCaptureMetadata& metadata) noexcept {
    ExportResult result{};
    try {
        core::path::Buffer path = g_rootPath;
        std::array<wchar_t, 128> suffix{};
        const int written = std::swprintf(suffix.data(),
                                          suffix.size(),
                                          L"\\viewer-route-%lu-%llu-%llu-%llu.json",
                                          GetCurrentProcessId(),
                                          static_cast<unsigned long long>(metadata.cameraSession),
                                          static_cast<unsigned long long>(metadata.captureSequence),
                                          static_cast<unsigned long long>(snapshot.capturedTick));
        if (written <= 0 || !core::path::append(path, suffix.data())) {
            std::snprintf(
                result.error.data(), result.error.size(), "route export path is too long");
            return result;
        }
        const std::string document = snapshot_json(snapshot, &metadata);
        (void)write_atomic(path, document, result);
    } catch (...) {
        std::snprintf(result.error.data(), result.error.size(), "serialization allocation failed");
    }
    return result;
}

ExportResult export_events(std::span<const ChangeEvent> events,
                           const InspectionSnapshot& current) noexcept {
    ExportResult result{};
    try {
        const std::string document = events_json(events, current);
        (void)write_atomic(g_eventsPath, document, result);
    } catch (...) {
        std::snprintf(result.error.data(), result.error.size(), "serialization allocation failed");
    }
    return result;
}

std::vector<ChangeEvent> compare(const InspectionSnapshot& before,
                                 const InspectionSnapshot& after) {
    History history;
    history.set_options(ChangeTrackingOptions{false, true, 0.0F}, before.world);
    history.set_recording(true, before.world);
    history.observe(after.world);
    return {history.events().begin(), history.events().end()};
}

} // namespace sunrise::client::inspection::capture
