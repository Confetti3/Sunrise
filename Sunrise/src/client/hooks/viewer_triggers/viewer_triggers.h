#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace sunrise::client::viewer::triggers {

inline constexpr std::size_t kObservationCapacity = 256;

enum class Kind : std::uint8_t {
    event,
    volume,
};

struct Observation final {
    Kind kind{Kind::event};
    std::uint64_t observationId{};
    std::uint16_t objectHandle{};
    std::uint32_t sourceHash{};
    std::int32_t selector{};
    std::array<float, 3> position{};
    std::uint32_t overlapCount{};
    bool positionPresent{};
    bool enabled{};
    bool active{};
};

struct Snapshot final {
    std::array<Observation, kObservationCapacity> triggers{};
    std::uint16_t triggerCount{};
    std::uint64_t sequence{};
    bool present{};
    bool truncated{};
};

/** Observes live native trigger-event components through their exact-image tick method. */
[[nodiscard]] bool install() noexcept;

/** Detaches the trigger observer without retaining native component pointers. */
[[nodiscard]] bool uninstall() noexcept;

/** Copies the current bounded, pointer-free trigger observations. */
[[nodiscard]] bool snapshot(Snapshot& output) noexcept;
[[nodiscard]] bool installed() noexcept;

} // namespace sunrise::client::viewer::triggers
