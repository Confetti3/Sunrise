#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "world_inspection_model.h"

namespace sunrise::client::inspection {

struct ProviderReport final {
    Producer producer{Producer::graph};
    std::string failure;
    std::uint64_t sequence{};
    std::uint64_t declaredCount{};
    std::uint64_t copiedCount{};
    std::uint32_t epoch{1};
    bool installed{};
    bool ready{};
    bool truncated{};
};

struct WorldContext final {
    std::string packageName;
    std::string mapStem;
    std::uint64_t activitySession{};
    std::uint64_t activityRevision{};
    std::uint32_t scenarioTag{};
    std::uint32_t spawnSetHash{};
    std::int32_t activityIndex{-1};
    std::int32_t region{-1};
    std::optional<std::uint16_t> bubble;
    std::optional<std::uint16_t> mapBubble;
    bool sessionPresent{};
    bool stale{};
};

/** One coherent, pointer-free Inspector document copied from independent producers. */
struct InspectionDocument final {
    Graph graph;
    std::vector<Diagnostic> diagnostics;
    WorldContext context;
    std::vector<ProviderReport> providerReports;
    std::uint64_t structureRevision{};
    std::uint64_t valueRevision{};
};

} // namespace sunrise::client::inspection
