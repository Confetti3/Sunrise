#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "inspection_document.h"
#include "inspection_event_ring.h"

namespace sunrise::client::inspection::capture {

inline constexpr std::uint32_t kSchemaVersion = 1;
inline constexpr std::size_t kEventCapacity = 4096;

enum class ChangeKind : std::uint8_t {
    added,
    removed,
    changed,
};

struct InspectionSnapshot final {
    InspectionDocument document;
    std::string imageSha256;
    std::uint64_t capturedTick{};
    bool imageVerified{};
};

struct ChangeEvent final {
    std::uint64_t sequence{};
    std::uint64_t capturedTick{};
    std::uint64_t nodeId{};
    std::uint64_t activityRevision{};
    std::uint32_t producerEpoch{};
    ChangeKind kind{ChangeKind::changed};
    std::string identity;
    std::string nodeName;
    std::string nodeKind;
    std::string field;
    std::string before;
    std::string after;
    std::string provenance;
};

struct ExportResult final {
    std::array<wchar_t, 32768> path{};
    std::array<char, 160> error{};
    bool success{};
};

struct RouteCaptureMetadata final {
    std::string pathName;
    std::string keyframeLabel;
    std::array<float, 3> position{};
    std::uint64_t cameraSession{};
    std::uint64_t captureSequence{};
    std::size_t keyframeIndex{};
    float yaw{};
    float pitch{};
    float fov{};
};

struct ChangeTrackingOptions final {
    // Runtime-only is the useful interactive default. Full snapshot Compare overrides this.
    bool runtimeOnly{true};
    // High-frequency transforms are opt-in so movement cannot drown structural/state changes.
    bool trackTransforms{};
    float positionEpsilon{0.05F};

    [[nodiscard]] friend bool operator==(const ChangeTrackingOptions&,
                                         const ChangeTrackingOptions&) noexcept = default;
};

class History final {
public:
    void set_recording(bool recording, const InspectionDocument& document);
    void set_options(ChangeTrackingOptions options, const InspectionDocument& document);
    void observe(const InspectionDocument& document);
    void clear() noexcept;

    [[nodiscard]] bool recording() const noexcept;
    [[nodiscard]] ChangeTrackingOptions options() const noexcept;
    [[nodiscard]] std::span<const ChangeEvent> events() const noexcept;

private:
    struct NodeState final {
        Node node;
        std::string identity;
        std::string parentIdentity;
        std::vector<std::string> childIdentities;
    };

    using StateMap = std::unordered_map<NodeKey, NodeState, NodeKeyHash>;

    [[nodiscard]] StateMap collect(const InspectionDocument& document) const;
    void append(ChangeEvent event);

    StateMap previous_;
    ChronologicalRing<ChangeEvent, kEventCapacity> eventRing_{};
    // Contiguous chronological view retained for the existing export/UI span API.
    mutable std::vector<ChangeEvent> eventView_;
    mutable bool eventViewDirty_{};
    std::optional<std::uint64_t> lastObservedRevision_;
    std::uint64_t sequence_{};
    ChangeTrackingOptions options_{};
    bool recording_{};
};

void initialize(void* module) noexcept;
void shutdown() noexcept;

[[nodiscard]] InspectionSnapshot make_snapshot(const InspectionDocument& document);
[[nodiscard]] std::string stable_identity(const InspectionDocument& document, const Node& node);
[[nodiscard]] std::uint64_t stable_native_key(const InspectionDocument& document,
                                              const Node& node) noexcept;
[[nodiscard]] const char* change_kind_name(ChangeKind kind) noexcept;

/** Serializes an inspection snapshot with export schema version 1. */
[[nodiscard]] std::string serialize_json(const InspectionSnapshot& snapshot);
/** Serializes one CSV row per node with export schema version 1. */
[[nodiscard]] std::string serialize_csv(const InspectionSnapshot& snapshot);

[[nodiscard]] ExportResult export_json(const InspectionSnapshot& snapshot) noexcept;
[[nodiscard]] ExportResult export_csv(const InspectionSnapshot& snapshot) noexcept;
[[nodiscard]] ExportResult export_route_json(const InspectionSnapshot& snapshot,
                                             const RouteCaptureMetadata& metadata) noexcept;
[[nodiscard]] ExportResult export_events(std::span<const ChangeEvent> events,
                                         const InspectionSnapshot& current) noexcept;
[[nodiscard]] std::vector<ChangeEvent> compare(const InspectionSnapshot& before,
                                               const InspectionSnapshot& after);

} // namespace sunrise::client::inspection::capture
