#pragma once

#include <array>
#include <cstddef>

#include "../../inspection/world_inspection_model.h"

namespace sunrise::client::ui::world_inspector::debug_primitives {

using Vector = std::array<float, 3>;

struct ScreenPoint final {
    float x{};
    float y{};
};

struct ProjectionContext final {
    Vector position{};
    Vector forward{1.0F, 0.0F, 0.0F};
    Vector up{0.0F, 0.0F, 1.0F};
    float fov{};
    float viewportX{};
    float viewportY{};
    float viewportWidth{};
    float viewportHeight{};
    float nearPlane{0.01F};
};

struct ProjectedPoint final {
    ScreenPoint screen{};
    float depth{};
};

struct ProjectedSegment final {
    ScreenPoint start{};
    ScreenPoint end{};
    float startDepth{};
    float endDepth{};
};

struct Capsule final {
    Vector first{};
    Vector second{};
    float radius{};
};

inline constexpr std::size_t kBoxEdgeCount = 12;
inline constexpr std::size_t kCapsuleSegmentCapacity = 192;
using ProjectedBox = std::array<ProjectedSegment, kBoxEdgeCount>;
using ProjectedCapsule = std::array<ProjectedSegment, kCapsuleSegmentCapacity>;

[[nodiscard]] bool valid_projection(const ProjectionContext& context) noexcept;
[[nodiscard]] bool project_point(const ProjectionContext& context,
                                 const Vector& point,
                                 ProjectedPoint& projected) noexcept;
[[nodiscard]] bool project_segment(const ProjectionContext& context,
                                   const Vector& start,
                                   const Vector& end,
                                   ProjectedSegment& projected) noexcept;
[[nodiscard]] std::size_t project_aabb(const ProjectionContext& context,
                                       const inspection::Bounds& bounds,
                                       ProjectedBox& projected) noexcept;
[[nodiscard]] std::size_t project_box(const ProjectionContext& context,
                                      const std::array<Vector, 8>& corners,
                                      ProjectedBox& projected) noexcept;
[[nodiscard]] std::size_t project_capsule(const ProjectionContext& context,
                                          const Capsule& capsule,
                                          ProjectedCapsule& projected) noexcept;
[[nodiscard]] float distance_squared(ScreenPoint point, ScreenPoint start, ScreenPoint end) noexcept;

} // namespace sunrise::client::ui::world_inspector::debug_primitives
