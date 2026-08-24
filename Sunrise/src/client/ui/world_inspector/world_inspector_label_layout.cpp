#include "world_inspector_label_layout.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <unordered_map>
#include <vector>

namespace sunrise::client::ui::world_inspector::label_layout {
namespace {

struct Cell final {
    int x{};
    int y{};
    [[nodiscard]] friend bool operator==(const Cell&, const Cell&) noexcept = default;
};

struct CellHash final {
    [[nodiscard]] std::size_t operator()(Cell value) const noexcept {
        const auto ux = static_cast<std::uint32_t>(value.x);
        const auto uy = static_cast<std::uint32_t>(value.y);
        return static_cast<std::size_t>(ux * 0x9e3779b9U
                                        ^ (uy + 0x85ebca6bU + (ux << 6U) + (ux >> 2U)));
    }
};

[[nodiscard]] bool overlaps(const Rect& left, const Rect& right, float gap) noexcept {
    return left.minimumX < right.maximumX + gap && left.maximumX + gap > right.minimumX
           && left.minimumY < right.maximumY + gap && left.maximumY + gap > right.minimumY;
}

[[nodiscard]] int cell_index(float value, float minimum, float cellSize) noexcept {
    return static_cast<int>(std::floor((value - minimum) / cellSize));
}

} // namespace

Result place(std::span<const Candidate> candidates,
             Rect viewport,
             float gap,
             float cellSize,
             std::vector<Placement>& output) {
    Result result{};
    output.clear();
    if (candidates.empty() || viewport.maximumX <= viewport.minimumX
        || viewport.maximumY <= viewport.minimumY || !std::isfinite(cellSize) || cellSize <= 0.0F) {
        return result;
    }
    output.reserve((std::max)(output.capacity(), candidates.size()));

    std::unordered_map<Cell, std::vector<std::size_t>, CellHash> grid;
    grid.reserve(candidates.size() * 2U + 1U);
    std::vector<std::uint32_t> marks;
    marks.reserve(candidates.size());
    std::vector<std::size_t> colliders;
    colliders.reserve(64);
    std::vector<float> yCandidates;
    yCandidates.reserve(64);
    std::vector<std::size_t> order(candidates.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::ranges::stable_sort(order, [&candidates](std::size_t left, std::size_t right) {
        const Candidate& first = candidates[left];
        const Candidate& second = candidates[right];
        if (first.priority != second.priority) {
            return first.priority > second.priority;
        }
        if (first.depth != second.depth) {
            return first.depth < second.depth;
        }
        return first.id < second.id;
    });
    std::uint32_t collectStamp = 0U;

    const auto collect = [&](const Rect& rect) {
        colliders.clear();
        const int minX = cell_index(rect.minimumX - gap, viewport.minimumX, cellSize);
        const int maxX = cell_index(rect.maximumX + gap, viewport.minimumX, cellSize);
        const int minY = cell_index(rect.minimumY - gap, viewport.minimumY, cellSize);
        const int maxY = cell_index(rect.maximumY + gap, viewport.minimumY, cellSize);
        ++collectStamp;
        if (collectStamp == 0U) {
            std::ranges::fill(marks, 0U);
            collectStamp = 1U;
        }
        const std::uint32_t stamp = collectStamp;
        if (marks.size() < output.size()) {
            marks.resize(output.size(), 0U);
        }
        for (int x = minX; x <= maxX; ++x) {
            for (int y = minY; y <= maxY; ++y) {
                const auto found = grid.find(Cell{x, y});
                if (found == grid.end()) {
                    continue;
                }
                for (const std::size_t index : found->second) {
                    if (index >= marks.size() || marks[index] == stamp) {
                        continue;
                    }
                    marks[index] = stamp;
                    colliders.push_back(index);
                }
            }
        }
    };

    const auto register_rect = [&](const Rect& rect, std::size_t index) {
        const int minX = cell_index(rect.minimumX - gap, viewport.minimumX, cellSize);
        const int maxX = cell_index(rect.maximumX + gap, viewport.minimumX, cellSize);
        const int minY = cell_index(rect.minimumY - gap, viewport.minimumY, cellSize);
        const int maxY = cell_index(rect.maximumY + gap, viewport.minimumY, cellSize);
        for (int x = minX; x <= maxX; ++x) {
            for (int y = minY; y <= maxY; ++y) {
                grid[Cell{x, y}].push_back(index);
            }
        }
    };

    for (const std::size_t candidateIndex : order) {
        ++result.attempted;
        const Candidate& candidate = candidates[candidateIndex];
        const float width = (std::max)(0.0F, candidate.width);
        const float height = (std::max)(0.0F, candidate.height);
        if (width > viewport.maximumX - viewport.minimumX
            || height > viewport.maximumY - viewport.minimumY) {
            ++result.collisionOmitted;
            continue;
        }
        const float maximumX = (std::max)(viewport.minimumX, viewport.maximumX - width);
        const float maximumY = (std::max)(viewport.minimumY, viewport.maximumY - height);
        const float x = std::clamp(candidate.desiredX, viewport.minimumX, maximumX);
        const float desiredY = std::clamp(candidate.desiredY, viewport.minimumY, maximumY);
        const Rect base{x, desiredY, x + width, desiredY + height};
        // Query the vertical strip, not just the desired cell. This remains local
        // in X while exposing every exact above/below boundary that can yield a
        // valid in-viewport placement.
        collect(Rect{x, viewport.minimumY, x + width, viewport.maximumY});
        yCandidates.clear();
        yCandidates.push_back(desiredY);
        for (const std::size_t index : colliders) {
            const Rect& other = output[index].rect;
            if (base.minimumX >= other.maximumX + gap || base.maximumX + gap <= other.minimumX) {
                continue;
            }
            yCandidates.push_back(other.maximumY + gap);
            yCandidates.push_back(other.minimumY - height - gap);
        }
        std::ranges::sort(yCandidates, [desiredY](float left, float right) {
            const float leftDistance = std::abs(left - desiredY);
            const float rightDistance = std::abs(right - desiredY);
            return leftDistance == rightDistance ? left < right : leftDistance < rightDistance;
        });
        bool placed = false;
        for (const float y : yCandidates) {
            if (y < viewport.minimumY || y > maximumY) {
                continue;
            }
            const Rect rect{x, y, x + width, y + height};
            collect(rect);
            if (std::ranges::none_of(colliders, [&](std::size_t index) {
                    return overlaps(rect, output[index].rect, gap);
                })) {
                output.push_back(Placement{candidateIndex, rect});
                register_rect(rect, output.size() - 1U);
                ++result.placed;
                placed = true;
                break;
            }
        }
        if (!placed) {
            ++result.collisionOmitted;
        }
    }
    return result;
}

} // namespace sunrise::client::ui::world_inspector::label_layout
