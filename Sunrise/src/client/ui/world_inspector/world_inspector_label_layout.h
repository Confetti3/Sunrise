#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace sunrise::client::ui::world_inspector::label_layout {

struct Rect final {
    float minimumX{};
    float minimumY{};
    float maximumX{};
    float maximumY{};
    [[nodiscard]] friend bool operator==(const Rect&, const Rect&) noexcept = default;
};

struct Candidate final {
    float desiredX{};
    float desiredY{};
    float width{};
    float height{};
    float depth{};
    std::uint64_t id{};
    int priority{};
};

struct Placement final {
    std::size_t candidate{};
    Rect rect{};
    [[nodiscard]] friend bool operator==(const Placement&, const Placement&) noexcept = default;
};

struct Result final {
    std::size_t attempted{};
    std::size_t placed{};
    std::size_t collisionOmitted{};
};

/**
 * Places every candidate in deterministic priority order.  The 32px grid is
 * only an acceleration structure: collisions are always checked against the
 * exact rectangles, so it does not change label visibility semantics.
 * Ordering is selected/hover priority, nearest depth, then stable ID.
 */
Result place(std::span<const Candidate> candidates,
             Rect viewport,
             float gap,
             float cellSize,
             std::vector<Placement>& output);

} // namespace sunrise::client::ui::world_inspector::label_layout
