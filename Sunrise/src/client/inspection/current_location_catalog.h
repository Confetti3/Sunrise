#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include "../../state/build_data/scenarios/definition.h"

namespace sunrise::client::inspection::current_location_catalog {

enum class DomainState : std::uint8_t { idle, cached, collecting, ready, unavailable, failed };

struct DomainStatus final {
    DomainState state{DomainState::idle};
    std::size_t records{};
    std::string diagnostic;
};

struct Scope final {
    std::uint64_t activitySession{};
    std::uint32_t scenarioTag{};
    std::string packageName;
    std::string mapFamily;
    std::array<std::uint16_t, state::build_data::scenarios::kDestinationPackageCapacity>
        packageIds{};
    std::size_t packageCount{};
};

struct ActivitySelection final {
    Scope scope;
    std::string displayName;
};

struct PreviewStatus final {
    bool active{};
    std::string displayName;
    std::string mapFamily;
    std::uint32_t scenarioTag{};
    DomainStatus activityGraph;
    DomainStatus activityLogic;
};

struct Status final {
    bool scopeAvailable{};
    bool canCollect{};
    DomainStatus activityGraph;
    DomainStatus activityLogic;
    DomainStatus bubbleBounds;
    DomainStatus statics;
};

void initialize(void* module) noexcept;
void cancel() noexcept;
void shutdown() noexcept;

/**
 * Selects the live location, activates a matching cache, and consumes completed worker output.
 * @return True when provider publication changed and the Inspector document must rebuild.
 */
[[nodiscard]] bool refresh(const Scope& scope) noexcept;

/** Queues an explicit collection for the selected live location. */
[[nodiscard]] bool request(const Scope& scope) noexcept;

/** Selects an authored activity, activating caches or collecting Graph and Logic on a miss. */
[[nodiscard]] bool select_activity(const ActivitySelection& selection) noexcept;

/** Clears the authored preview and restores matching live Graph and Logic caches. */
[[nodiscard]] bool clear_activity_preview() noexcept;

[[nodiscard]] PreviewStatus preview_status() noexcept;

[[nodiscard]] Status status() noexcept;
[[nodiscard]] const char* state_name(DomainState state) noexcept;

} // namespace sunrise::client::inspection::current_location_catalog
