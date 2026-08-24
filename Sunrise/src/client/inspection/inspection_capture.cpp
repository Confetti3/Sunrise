#include "inspection_capture.h"

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
#include "activity_graph_catalog.h"

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

[[nodiscard]] std::string
relationships_text(const std::vector<ActivityLogicRelationship>& relationships) {
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

[[nodiscard]] std::string bounds_provenance_text(const std::optional<Provenance>& provenance) {
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

[[nodiscard]] std::uint64_t structural_key(const InspectionDocument& document,
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
    mix(document.graph.breadcrumb(node.id));
    mix(node.source.packageName);
    mix(node.source.mapStem);
    return hash == 0 ? 1 : hash;
}

[[nodiscard]] std::uint32_t identity_epoch(const InspectionDocument&, const Node& node) noexcept {
    // The optional activity catalog is immutable browse metadata loaded once at startup.
    // It must not churn identity merely because the live activity producer epoch changes.
    return node.producer == Producer::activityCatalog
                   || node.producer == Producer::activityLogicCatalog
               ? 0U
               : node.key.producerEpoch;
}

[[nodiscard]] std::string make_identity(const InspectionDocument& document, const Node& node) {
    std::array<char, 160> text{};
    const int written =
        std::snprintf(text.data(),
                      text.size(),
                      "%s:%u:%016llX:%u",
                      producer_name(node.producer),
                      identity_epoch(document, node),
                      static_cast<unsigned long long>(structural_key(document, node)),
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
    std::ranges::copy(
        finalPath.chars.data(), finalPath.chars.data() + finalPath.length + 1, result.path.data());
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

void append_node_key_json(std::string& output, const NodeKey& key) {
    output.append("{\"producer\": ");
    append_json_string(output, producer_name(key.producer));
    output.append(", \"epoch\": ");
    append_number(output, key.producerEpoch);
    output.append(", \"kind\": ");
    append_json_string(output, kind_name(key.kind));
    output.append(", \"native_key\": ");
    append_number(output, key.nativeKey);
    output.append(", \"discriminator\": ");
    append_number(output, key.discriminator);
    output.push_back('}');
}

void append_property_value_json(std::string& output, const PropertyValue& property) {
    std::visit(
        [&output](const auto& value) {
            using Value = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Value, bool>) {
                output.append(value ? "true" : "false");
            } else if constexpr (std::is_same_v<Value, std::int64_t>) {
                append_signed(output, value);
            } else if constexpr (std::is_same_v<Value, std::uint64_t>) {
                append_number(output, value);
            } else if constexpr (std::is_same_v<Value, double>) {
                if (std::isfinite(value)) {
                    std::array<char, 48> text{};
                    const int written = std::snprintf(text.data(), text.size(), "%.17g", value);
                    if (written > 0) {
                        output.append(text.data(), static_cast<std::size_t>(written));
                    }
                } else {
                    output.append("null");
                }
            } else if constexpr (std::is_same_v<Value, std::string>) {
                append_json_string(output, value);
            } else {
                append_vector_json(output, value);
            }
        },
        property);
}

void append_properties_json(std::string& output, const std::vector<Property>& properties) {
    output.push_back('[');
    for (std::size_t index = 0; index < properties.size(); ++index) {
        if (index != 0) {
            output.append(", ");
        }
        const Property& property = properties[index];
        output.append("{\"key\": ");
        append_json_string(output, property.key);
        output.append(", \"label\": ");
        append_json_string(output, property.label);
        output.append(", \"value\": ");
        append_property_value_json(output, property.value);
        output.append(", \"provenance\": ");
        append_json_string(output, provenance_name(property.provenance));
        output.append(", \"searchable\": ");
        output.append(property.searchable ? "true" : "false");
        output.append(", \"trackable\": ");
        output.append(property.trackable ? "true" : "false");
        output.append(", \"visible\": ");
        output.append(property.visible ? "true}" : "false}");
    }
    output.push_back(']');
}

void append_relations_json(std::string& output, const std::vector<Relation>& relations) {
    output.push_back('[');
    for (std::size_t index = 0; index < relations.size(); ++index) {
        if (index != 0) {
            output.append(", ");
        }
        const Relation& relation = relations[index];
        output.append("{\"target\": ");
        append_node_key_json(output, relation.target);
        output.append(", \"kind\": ");
        append_number(output, static_cast<std::uint8_t>(relation.kind));
        output.append(", \"provenance\": ");
        append_json_string(output, provenance_name(relation.provenance));
        output.append(", \"occurrence_count\": ");
        append_number(output, relation.occurrenceCount);
        output.append(", \"outgoing\": ");
        output.append(relation.outgoing ? "true}" : "false}");
    }
    output.push_back(']');
}

void append_node_json(std::string& output, const InspectionDocument& document, const Node& node) {
    output.append("    {\"key\": ");
    append_node_key_json(output, node.key);
    output.append(", \"parent_key\": ");
    if (const Node* parent = document.graph.node(node.parent)) {
        append_node_key_json(output, parent->key);
    } else {
        output.append("null");
    }
    output.append(", \"producer\": ");
    append_json_string(output, producer_name(node.producer));
    output.append(", \"producer_epoch\": ");
    append_number(output, identity_epoch(document, node));
    output.append(", \"native_key\": ");
    append_number(output, structural_key(document, node));
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
    const SpatialEvidence spatial = spatial_evidence(node);
    output.append(", \"spatial_evidence\": {\"kind\": ");
    append_number(output, static_cast<std::uint8_t>(spatial.kind));
    output.append(", \"provenance\": ");
    append_json_string(output, provenance_name(spatial.provenance));
    output.append(", \"transform_present\": ");
    output.append(spatial.transform.has_value() ? "true" : "false");
    output.append(", \"bounds_present\": ");
    output.append(spatial.bounds.has_value() ? "true}" : "false}");
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
    output.append(", \"authored_rotation\": ");
    if (node.authoredRotation.has_value()) {
        output.push_back('[');
        for (std::size_t index = 0; index < node.authoredRotation->size(); ++index) {
            if (index != 0) {
                output.append(", ");
            }
            append_float(output, (*node.authoredRotation)[index]);
        }
        output.push_back(']');
    } else {
        output.append("null");
    }
    output.append(", \"properties\": ");
    append_properties_json(output, node.properties);
    output.append(", \"relations\": ");
    append_relations_json(output, node.relations);
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
    const InspectionDocument& document = snapshot.document;
    const WorldContext& context = document.context;
    std::string output;
    output.reserve(document.graph.nodes().size() * 512U + 2048U);
    output.append("{\n  \"schema_version\": ");
    append_number(output, kSchemaVersion);
    output.append(",\n  \"capture\": {\"captured_tick\": ");
    append_number(output, snapshot.capturedTick);
    output.append("},\n  \"image\": {\"client_build\": ");
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
    output.append("},\n  \"world_context\": {\"session_present\": ");
    output.append(context.sessionPresent ? "true" : "false");
    output.append(", \"stale\": ");
    output.append(context.stale ? "true" : "false");
    output.append(", \"package\": ");
    append_json_string(output, context.packageName);
    output.append(", \"map_stem\": ");
    append_json_string(output, context.mapStem);
    output.append(", \"activity_session\": ");
    append_number(output, context.activitySession);
    output.append(", \"activity_revision\": ");
    append_number(output, context.activityRevision);
    output.append(", \"activity_index\": ");
    append_signed(output, context.activityIndex);
    output.append(", \"region\": ");
    append_signed(output, context.region);
    output.append(", \"scenario_tag\": ");
    append_number(output, context.scenarioTag);
    output.append(", \"spawn_set_hash\": ");
    append_number(output, context.spawnSetHash);
    append_optional_integer(output, "bubble", context.bubble);
    append_optional_integer(output, "map_bubble", context.mapBubble);
    output.append("},\n  \"revisions\": {\"structure\": ");
    append_number(output, document.structureRevision);
    output.append(", \"value\": ");
    append_number(output, document.valueRevision);
    output.append("},\n  \"provider_reports\": [\n");
    for (std::size_t index = 0; index < document.providerReports.size(); ++index) {
        const ProviderReport& report = document.providerReports[index];
        output.append("    {\"producer\": ");
        append_json_string(output, producer_name(report.producer));
        output.append(", \"epoch\": ");
        append_number(output, report.epoch);
        output.append(", \"installed\": ");
        output.append(report.installed ? "true" : "false");
        output.append(", \"ready\": ");
        output.append(report.ready ? "true" : "false");
        output.append(", \"sequence\": ");
        append_number(output, report.sequence);
        output.append(", \"declared_count\": ");
        append_number(output, report.declaredCount);
        output.append(", \"copied_count\": ");
        append_number(output, report.copiedCount);
        output.append(", \"truncated\": ");
        output.append(report.truncated ? "true" : "false");
        output.append(", \"failure\": ");
        if (report.failure.empty()) {
            output.append("null");
        } else {
            append_json_string(output, report.failure);
        }
        output.append(index + 1 == document.providerReports.size() ? "}\n" : "},\n");
    }
    output.append("  ],\n  \"diagnostics\": [\n");
    for (std::size_t index = 0; index < document.diagnostics.size(); ++index) {
        const Diagnostic& diagnostic = document.diagnostics[index];
        output.append("    {\"severity\": ");
        append_number(output, static_cast<std::uint8_t>(diagnostic.severity));
        output.append(", \"message\": ");
        append_json_string(output, diagnostic.message);
        output.append(index + 1 == document.diagnostics.size() ? "}\n" : "},\n");
    }
    output.append("  ],\n  \"nodes\": [\n");
    const auto& nodes = document.graph.nodes();
    for (std::size_t index = 0; index < nodes.size(); ++index) {
        append_node_json(output, document, nodes[index]);
        output.append(index + 1 == nodes.size() ? "\n" : ",\n");
    }
    output.append("  ]");
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
    const InspectionDocument& document = snapshot.document;
    std::string output =
        "schema_version,captured_tick,image_sha256,image_verified,structure_revision,"
        "value_revision,key,parent_key,kind,producer,status,provenance,name,package,map_stem,"
        "scenario_tag,spawn_set_hash,activity_session,activity_index,bubble,spatial_kind,"
        "position_x,position_y,position_z,bounds_minimum,bounds_maximum,properties,relations,"
        "authored_metadata\r\n";
    output.reserve(document.graph.nodes().size() * 256U + output.size());

    for (const Node& node : document.graph.nodes()) {
        std::string key;
        append_node_key_json(key, node.key);
        std::string parentKey;
        if (const Node* parent = document.graph.node(node.parent)) {
            append_node_key_json(parentKey, parent->key);
        }
        std::string properties;
        append_properties_json(properties, node.properties);
        std::string relations;
        append_relations_json(relations, node.relations);
        std::string authored;
        if (node.authoredRotation.has_value()) {
            authored = "rotation=" + quaternion_text(*node.authoredRotation);
        }
        if (node.activityMetadata.has_value()) {
            if (!authored.empty()) {
                authored.push_back(';');
            }
            const ActivityMetadata& metadata = *node.activityMetadata;
            authored += "activity=" + std::to_string(metadata.activityHash)
                        + ";graph=" + std::to_string(metadata.graphHash)
                        + ";node=" + std::to_string(metadata.nodeHash);
        }
        if (node.activityLogicMetadata.has_value()) {
            if (!authored.empty()) {
                authored.push_back(';');
            }
            const ActivityLogicMetadata& metadata = *node.activityLogicMetadata;
            authored += "scenario=" + std::to_string(metadata.scenarioTag)
                        + ";definition=" + std::to_string(metadata.definitionTag)
                        + ";world_id=" + std::to_string(metadata.worldId);
        }

        append_number(output, kSchemaVersion);
        output.push_back(',');
        append_number(output, snapshot.capturedTick);
        output.push_back(',');
        append_csv(output, snapshot.imageSha256);
        output.push_back(',');
        output.append(snapshot.imageVerified ? "true," : "false,");
        append_number(output, document.structureRevision);
        output.push_back(',');
        append_number(output, document.valueRevision);
        output.push_back(',');
        append_csv(output, key);
        output.push_back(',');
        append_csv(output, parentKey);
        output.push_back(',');
        append_csv(output, kind_name(node.kind));
        output.push_back(',');
        append_csv(output, producer_name(node.producer));
        output.push_back(',');
        append_csv(output, status_name(node.status));
        output.push_back(',');
        append_csv(output, provenance_name(node.provenance));
        output.push_back(',');
        append_csv(output, node.name);
        output.push_back(',');
        append_csv(output, node.source.packageName);
        output.push_back(',');
        append_csv(output, node.source.mapStem);
        for (const std::string& value : {optional_text(node.source.scenarioTag),
                                         optional_text(node.source.spawnSetHash),
                                         optional_text(node.source.activitySession),
                                         optional_text(node.source.activityIndex),
                                         optional_text(node.source.bubble)}) {
            output.push_back(',');
            append_csv(output, value);
        }

        const SpatialEvidence spatial = spatial_evidence(node);
        output.push_back(',');
        append_number(output, static_cast<std::uint8_t>(spatial.kind));
        if (spatial.transform.has_value()) {
            for (const float lane : spatial.transform->position) {
                output.push_back(',');
                append_float(output, lane);
            }
        } else {
            output.append(",,,");
        }
        output.push_back(',');
        append_csv(output,
                   spatial.bounds.has_value() ? vector_text(spatial.bounds->minimum)
                                              : std::string{});
        output.push_back(',');
        append_csv(output,
                   spatial.bounds.has_value() ? vector_text(spatial.bounds->maximum)
                                              : std::string{});
        output.push_back(',');
        append_csv(output, properties);
        output.push_back(',');
        append_csv(output, relations);
        output.push_back(',');
        append_csv(output, authored);
        output.append("\r\n");
    }
    return output;
}
[[nodiscard]] std::string events_json(std::span<const ChangeEvent> events,
                                      const InspectionSnapshot& current) {
    const WorldContext& context = current.document.context;
    std::string output;
    output.reserve(events.size() * 256U + 512U);
    output.append("{\n  \"schema_version\": ");
    append_number(output, kSchemaVersion);
    output.append(",\n  \"capture\": {\"captured_tick\": ");
    append_number(output, current.capturedTick);
    output.append("},\n  \"image\": {\"client_build\": ");
    append_json_string(output, activity_catalog::kTargetContentBuildText);
    output.append(", \"image_sha256\": ");
    if (current.imageSha256.empty()) {
        output.append("null");
    } else {
        append_json_string(output, current.imageSha256);
    }
    output.append(", \"verified\": ");
    output.append(current.imageVerified ? "true}" : "false}");
    output.append(",\n  \"world_context\": {\"session_present\": ");
    output.append(context.sessionPresent ? "true" : "false");
    output.append(", \"stale\": ");
    output.append(context.stale ? "true" : "false");
    output.append(", \"package\": ");
    append_json_string(output, context.packageName);
    output.append(", \"map_stem\": ");
    append_json_string(output, context.mapStem);
    output.append(", \"activity_session\": ");
    append_number(output, context.activitySession);
    output.append(", \"activity_revision\": ");
    append_number(output, context.activityRevision);
    output.append(", \"activity_index\": ");
    append_signed(output, context.activityIndex);
    output.append(", \"region\": ");
    append_signed(output, context.region);
    output.append(", \"scenario_tag\": ");
    append_number(output, context.scenarioTag);
    output.append(", \"spawn_set_hash\": ");
    append_number(output, context.spawnSetHash);
    append_optional_integer(output, "bubble", context.bubble);
    append_optional_integer(output, "map_bubble", context.mapBubble);
    output.append("},\n  \"events\": [\n");
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

History::StateMap History::collect(const InspectionDocument& document) const {
    StateMap state;
    state.reserve(document.graph.nodes().size());
    for (const Node& node : document.graph.nodes()) {
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
        NodeState value{node, make_identity(document, node)};
        if (const Node* parent = document.graph.node(node.parent); parent != nullptr) {
            value.parentIdentity = make_identity(document, *parent);
        }
        value.childIdentities.reserve(node.children.size());
        for (const NodeId childId : node.children) {
            if (const Node* child = document.graph.node(childId); child != nullptr) {
                value.childIdentities.push_back(make_identity(document, *child));
            }
        }
        state.emplace(node.key, std::move(value));
    }
    return state;
}

void History::append(ChangeEvent event) {
    eventRing_.push(std::move(event));
    eventViewDirty_ = true;
}

[[nodiscard]] std::string property_text(const PropertyValue& property) {
    return std::visit(
        [](const auto& value) -> std::string {
            using Value = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Value, bool>) {
                return value ? "true" : "false";
            } else if constexpr (std::is_same_v<Value, std::string>) {
                return value;
            } else if constexpr (std::is_same_v<Value, std::array<float, 3>>) {
                return vector_text(value);
            } else {
                return std::to_string(value);
            }
        },
        property);
}

void History::set_recording(bool recording, const InspectionDocument& document) {
    if (recording == recording_) {
        return;
    }
    recording_ = recording;
    previous_ = recording ? collect(document) : StateMap{};
    lastObservedRevision_ = recording && document.valueRevision != 0
                                ? std::optional<std::uint64_t>{document.valueRevision}
                                : std::nullopt;
}

void History::set_options(ChangeTrackingOptions options, const InspectionDocument& document) {
    if (!std::isfinite(options.positionEpsilon) || options.positionEpsilon < 0.0F) {
        options.positionEpsilon = 0.05F;
    }
    if (options == options_) {
        return;
    }
    options_ = options;
    if (recording_) {
        // Changing scope/fields establishes a fresh baseline rather than manufacturing events.
        previous_ = collect(document);
        lastObservedRevision_ = document.valueRevision != 0
                                    ? std::optional<std::uint64_t>{document.valueRevision}
                                    : std::nullopt;
    }
}

void History::observe(const InspectionDocument& document) {
    if (!recording_) {
        return;
    }
    if (document.valueRevision != 0 && lastObservedRevision_.has_value()
        && *lastObservedRevision_ == document.valueRevision) {
        return;
    }
    StateMap current = collect(document);
    const std::uint64_t tick = GetTickCount64();
    for (const auto& [key, oldState] : previous_) {
        if (current.contains(key)) {
            continue;
        }
        ChangeEvent event{};
        event.sequence = ++sequence_;
        event.capturedTick = tick;
        event.nodeId = oldState.node.id.value;
        event.activityRevision = document.context.activityRevision;
        event.producerEpoch = oldState.node.key.producerEpoch;
        event.kind = ChangeKind::removed;
        event.identity = oldState.identity;
        event.nodeName = oldState.node.name;
        event.nodeKind = kind_name(oldState.node.kind);
        event.field = "node";
        event.before = oldState.node.name;
        event.provenance = provenance_name(oldState.node.provenance);
        append(std::move(event));
    }
    for (const auto& [key, newState] : current) {
        const auto found = previous_.find(key);
        if (found == previous_.end()) {
            ChangeEvent event{};
            event.sequence = ++sequence_;
            event.capturedTick = tick;
            event.nodeId = newState.node.id.value;
            event.activityRevision = document.context.activityRevision;
            event.producerEpoch = newState.node.key.producerEpoch;
            event.kind = ChangeKind::added;
            event.identity = newState.identity;
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
                                     document.context.activityRevision,
                                     identity_epoch(document, after),
                                     after,
                                     newState.identity,
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
        if (found->second.parentIdentity != newState.parentIdentity) {
            field_changed("parent", found->second.parentIdentity, newState.parentIdentity);
        }
        if (found->second.childIdentities != newState.childIdentities) {
            const auto children_text = [](const std::vector<std::string>& children) {
                std::string value;
                for (const std::string& child : children) {
                    if (!value.empty()) {
                        value.push_back(',');
                    }
                    value += child;
                }
                return value;
            };
            field_changed("children",
                          children_text(found->second.childIdentities),
                          children_text(newState.childIdentities));
        }
        if (before.status != after.status) {
            field_changed("status", status_name(before.status), status_name(after.status));
        }
        if (before.objectSystemType != after.objectSystemType) {
            field_changed("object_type",
                          optional_text(before.objectSystemType),
                          optional_text(after.objectSystemType));
        }
        for (const Property& property : after.properties) {
            if (!property.trackable) {
                continue;
            }
            const auto previous =
                std::ranges::find_if(before.properties, [&property](const Property& candidate) {
                    return candidate.key == property.key;
                });
            const std::string oldValue = previous == before.properties.end()
                                             ? std::string{}
                                             : property_text(previous->value);
            const std::string newValue = property_text(property.value);
            if (oldValue != newValue) {
                field_changed("property." + property.key, oldValue, newValue);
            }
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
                    field_changed(
                        "rotation",
                        before.transform->hasRotation ? vector_text(before.transform->rotation)
                                                      : "absent",
                        after.transform->hasRotation ? vector_text(after.transform->rotation)
                                                     : "absent");
                }
                if (before.transform->scale != after.transform->scale
                    || before.transform->hasScale != after.transform->hasScale) {
                    field_changed("scale",
                                  before.transform->hasScale ? vector_text(before.transform->scale)
                                                             : "absent",
                                  after.transform->hasScale ? vector_text(after.transform->scale)
                                                            : "absent");
                }
            }
            if (before.transformRuntime != after.transformRuntime) {
                field_changed("transform_runtime",
                              before.transformRuntime ? "true" : "false",
                              after.transformRuntime ? "true" : "false");
            }
            if (before.linearVelocity != after.linearVelocity) {
                field_changed("linear_velocity",
                              before.linearVelocity.has_value()
                                  ? vector_text(*before.linearVelocity)
                                  : "absent",
                              after.linearVelocity.has_value() ? vector_text(*after.linearVelocity)
                                                               : "absent");
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
            field_changed(
                "spatial_helper_state", spatial_state_text(before), spatial_state_text(after));
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
    previous_ = std::move(current);
    if (document.valueRevision != 0) {
        lastObservedRevision_ = document.valueRevision;
    }
}

void History::clear() noexcept {
    eventRing_.clear();
    eventView_.clear();
    eventViewDirty_ = false;
    lastObservedRevision_.reset();
    sequence_ = 0;
}

bool History::recording() const noexcept {
    return recording_;
}

ChangeTrackingOptions History::options() const noexcept {
    return options_;
}

std::span<const ChangeEvent> History::events() const noexcept {
    if (eventViewDirty_) {
        try {
            std::vector<ChangeEvent> next;
            next.reserve(eventRing_.size());
            eventRing_.for_each([&next](const ChangeEvent& event) { next.push_back(event); });
            eventView_ = std::move(next);
            eventViewDirty_ = false;
        } catch (const std::bad_alloc&) {
            // Preserve the most recent complete chronological view and retry on
            // the next call rather than allowing allocation to cross a noexcept UI path.
        }
    }
    return eventView_;
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

InspectionSnapshot make_snapshot(const InspectionDocument& document) {
    InspectionSnapshot result{};
    result.document = document;
    result.imageSha256 = g_imageHash;
    result.imageVerified = g_imageVerified;
    result.capturedTick = GetTickCount64();
    return result;
}

std::string stable_identity(const InspectionDocument& document, const Node& node) {
    return make_identity(document, node);
}

std::uint64_t stable_native_key(const InspectionDocument& document, const Node& node) noexcept {
    return structural_key(document, node);
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

std::string serialize_json(const InspectionSnapshot& snapshot) {
    return snapshot_json(snapshot);
}

std::string serialize_csv(const InspectionSnapshot& snapshot) {
    return snapshot_csv(snapshot);
}

ExportResult export_json(const InspectionSnapshot& snapshot) noexcept {
    ExportResult result{};
    try {
        const std::string document = serialize_json(snapshot);
        (void)write_atomic(g_jsonPath, document, result);
    } catch (...) {
        std::snprintf(result.error.data(), result.error.size(), "serialization allocation failed");
    }
    return result;
}

ExportResult export_csv(const InspectionSnapshot& snapshot) noexcept {
    ExportResult result{};
    try {
        const std::string document = serialize_csv(snapshot);
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
    history.set_options(ChangeTrackingOptions{false, true, 0.0F}, before.document);
    history.set_recording(true, before.document);
    history.observe(after.document);
    return {history.events().begin(), history.events().end()};
}

} // namespace sunrise::client::inspection::capture
