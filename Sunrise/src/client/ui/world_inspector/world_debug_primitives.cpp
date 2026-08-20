#include "world_debug_primitives.h"

#include <algorithm>
#include <cmath>

namespace sunrise::client::ui::world_inspector::debug_primitives {
namespace {

constexpr float kMinimumVectorLength = 1.0e-5F;
constexpr float kPi = 3.14159265358979323846F;
constexpr std::size_t kCapsuleAround = 16;
constexpr std::size_t kCapsuleRings = 4;

[[nodiscard]] float dot(const Vector& left, const Vector& right) noexcept {
    return left[0] * right[0] + left[1] * right[1] + left[2] * right[2];
}

[[nodiscard]] Vector subtract(const Vector& left, const Vector& right) noexcept {
    return {left[0] - right[0], left[1] - right[1], left[2] - right[2]};
}

[[nodiscard]] Vector add(const Vector& left, const Vector& right) noexcept {
    return {left[0] + right[0], left[1] + right[1], left[2] + right[2]};
}

[[nodiscard]] Vector multiply(const Vector& value, float scalar) noexcept {
    return {value[0] * scalar, value[1] * scalar, value[2] * scalar};
}

[[nodiscard]] Vector cross(const Vector& left, const Vector& right) noexcept {
    return {left[1] * right[2] - left[2] * right[1],
            left[2] * right[0] - left[0] * right[2],
            left[0] * right[1] - left[1] * right[0]};
}

[[nodiscard]] float length_squared(const Vector& value) noexcept {
    return dot(value, value);
}

[[nodiscard]] bool finite(const Vector& value) noexcept {
    for (const float lane : value) {
        if (!std::isfinite(lane)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool normalize(Vector& value) noexcept {
    const float squared = length_squared(value);
    if (!std::isfinite(squared) || squared <= kMinimumVectorLength * kMinimumVectorLength) {
        return false;
    }
    const float inverse = 1.0F / std::sqrt(squared);
    value = multiply(value, inverse);
    return finite(value);
}

struct Basis final {
    Vector forward{};
    Vector right{};
    Vector up{};
};

[[nodiscard]] bool make_basis(const ProjectionContext& context, Basis& basis) noexcept {
    basis.forward = context.forward;
    if (!normalize(basis.forward)) {
        return false;
    }
    basis.right = cross(basis.forward, context.up);
    if (!normalize(basis.right)) {
        return false;
    }
    basis.up = cross(basis.right, basis.forward);
    return normalize(basis.up);
}

[[nodiscard]] float half_fov(float fov) noexcept {
    return fov <= kPi + 0.1F ? fov * 0.5F : fov * (kPi / 360.0F);
}

[[nodiscard]] Vector lerp(const Vector& start, const Vector& end, float amount) noexcept {
    return {start[0] + (end[0] - start[0]) * amount,
            start[1] + (end[1] - start[1]) * amount,
            start[2] + (end[2] - start[2]) * amount};
}

[[nodiscard]] Vector capsule_point(const Capsule& capsule,
                                   const Vector& axis,
                                   const Vector& radial,
                                   std::size_t angleIndex,
                                   std::size_t ringIndex,
                                   bool second) noexcept {
    const float angle = static_cast<float>(angleIndex) * (2.0F * kPi)
                        / static_cast<float>(kCapsuleAround);
    const Vector around = add(multiply(radial, std::cos(angle)),
                              multiply(cross(axis, radial), std::sin(angle)));
    const float phi = static_cast<float>(ringIndex) * (0.5F * kPi)
                      / static_cast<float>(kCapsuleRings);
    const float axial = std::cos(phi) * capsule.radius;
    const float radialDistance = std::sin(phi) * capsule.radius;
    const Vector center = second ? capsule.second : capsule.first;
    const Vector axialOffset = multiply(axis, (second ? 1.0F : -1.0F) * axial);
    return add(add(center, axialOffset), multiply(around, radialDistance));
}

} // namespace

bool valid_projection(const ProjectionContext& context) noexcept {
    return finite(context.position) && finite(context.forward) && finite(context.up)
           && std::isfinite(context.fov) && context.fov > 0.05F && context.fov < 179.0F
           && std::isfinite(context.viewportX) && std::isfinite(context.viewportY)
           && std::isfinite(context.viewportWidth) && std::isfinite(context.viewportHeight)
           && context.viewportWidth > 0.0F && context.viewportHeight > 0.0F
           && std::isfinite(context.nearPlane) && context.nearPlane > 0.0F;
}

bool project_point(const ProjectionContext& context,
                   const Vector& point,
                   ProjectedPoint& projected) noexcept {
    if (!valid_projection(context) || !finite(point)) {
        return false;
    }
    Basis basis{};
    if (!make_basis(context, basis)) {
        return false;
    }
    const Vector relative = subtract(point, context.position);
    const float depth = dot(relative, basis.forward);
    if (!std::isfinite(depth) || depth < context.nearPlane) {
        return false;
    }
    const float tangent = std::tan(half_fov(context.fov));
    const float aspect = context.viewportWidth / context.viewportHeight;
    const float verticalTangent = tangent / aspect;
    if (!std::isfinite(tangent) || tangent <= 0.0F || !std::isfinite(verticalTangent)
        || verticalTangent <= 0.0F || !std::isfinite(aspect) || aspect <= 0.0F) {
        return false;
    }
    const float horizontal = dot(relative, basis.right) / (depth * tangent);
    const float vertical = dot(relative, basis.up) / (depth * verticalTangent);
    if (!std::isfinite(horizontal) || !std::isfinite(vertical)) {
        return false;
    }
    projected.screen.x = context.viewportX
                        + (horizontal * 0.5F + 0.5F) * context.viewportWidth;
    projected.screen.y = context.viewportY
                        + (0.5F - vertical * 0.5F) * context.viewportHeight;
    projected.depth = depth;
    return std::isfinite(projected.screen.x) && std::isfinite(projected.screen.y);
}

bool project_segment(const ProjectionContext& context,
                     const Vector& start,
                     const Vector& end,
                     ProjectedSegment& projected) noexcept {
    if (!valid_projection(context) || !finite(start) || !finite(end)) {
        return false;
    }
    Basis basis{};
    if (!make_basis(context, basis)) {
        return false;
    }
    const float startDepth = dot(subtract(start, context.position), basis.forward);
    const float endDepth = dot(subtract(end, context.position), basis.forward);
    if (!std::isfinite(startDepth) || !std::isfinite(endDepth)
        || (startDepth < context.nearPlane && endDepth < context.nearPlane)) {
        return false;
    }

    Vector clippedStart = start;
    Vector clippedEnd = end;
    const float clipDepth = context.nearPlane
                            + (std::max)(1.0e-4F, context.nearPlane * 1.0e-4F);
    if (startDepth < context.nearPlane) {
        const float amount = (clipDepth - startDepth) / (endDepth - startDepth);
        if (!std::isfinite(amount)) {
            return false;
        }
        clippedStart = lerp(start, end, std::clamp(amount, 0.0F, 1.0F));
    }
    if (endDepth < context.nearPlane) {
        const float amount = (clipDepth - startDepth) / (endDepth - startDepth);
        if (!std::isfinite(amount)) {
            return false;
        }
        clippedEnd = lerp(start, end, std::clamp(amount, 0.0F, 1.0F));
    }

    ProjectedPoint first{};
    ProjectedPoint second{};
    if (!project_point(context, clippedStart, first)
        || !project_point(context, clippedEnd, second)) {
        return false;
    }
    projected.start = first.screen;
    projected.end = second.screen;
    projected.startDepth = first.depth;
    projected.endDepth = second.depth;
    return true;
}

std::size_t project_box(const ProjectionContext& context,
                        const std::array<Vector, 8>& corners,
                        ProjectedBox& projected) noexcept {
    constexpr std::array<std::array<std::size_t, 2>, kBoxEdgeCount> edges{
        std::array<std::size_t, 2>{0, 1},
        std::array<std::size_t, 2>{1, 2},
        std::array<std::size_t, 2>{2, 3},
        std::array<std::size_t, 2>{3, 0},
        std::array<std::size_t, 2>{4, 5},
        std::array<std::size_t, 2>{5, 6},
        std::array<std::size_t, 2>{6, 7},
        std::array<std::size_t, 2>{7, 4},
        std::array<std::size_t, 2>{0, 4},
        std::array<std::size_t, 2>{1, 5},
        std::array<std::size_t, 2>{2, 6},
        std::array<std::size_t, 2>{3, 7}};
    std::size_t count = 0;
    for (const auto& edge : edges) {
        ProjectedSegment line{};
        if (project_segment(context, corners[edge[0]], corners[edge[1]], line)) {
            projected[count++] = line;
        }
    }
    return count;
}

std::size_t project_aabb(const ProjectionContext& context,
                         const inspection::Bounds& bounds,
                         ProjectedBox& projected) noexcept {
    if (!inspection::bounds_valid(bounds)) {
        return 0;
    }
    return project_box(context, inspection::bounds_corners(bounds), projected);
}

std::size_t project_capsule(const ProjectionContext& context,
                            const Capsule& capsule,
                            ProjectedCapsule& projected) noexcept {
    if (!valid_projection(context) || !finite(capsule.first) || !finite(capsule.second)
        || !std::isfinite(capsule.radius) || capsule.radius <= 0.0F) {
        return 0;
    }
    Vector axis = subtract(capsule.second, capsule.first);
    if (!normalize(axis)) {
        return 0;
    }
    Vector helper = std::abs(axis[2]) < 0.9F ? Vector{0.0F, 0.0F, 1.0F}
                                             : Vector{0.0F, 1.0F, 0.0F};
    Vector radial = cross(axis, helper);
    if (!normalize(radial)) {
        return 0;
    }

    std::size_t count = 0;
    auto append = [&](const Vector& start, const Vector& end) {
        if (count >= projected.size()) {
            return;
        }
        ProjectedSegment line{};
        if (project_segment(context, start, end, line)) {
            projected[count++] = line;
        }
    };
    for (std::size_t angle = 0; angle < kCapsuleAround; ++angle) {
        const std::size_t next = (angle + 1U) % kCapsuleAround;
        for (std::size_t ring = 0; ring < kCapsuleRings; ++ring) {
            append(capsule_point(capsule, axis, radial, angle, ring, false),
                   capsule_point(capsule, axis, radial, angle, ring + 1U, false));
            append(capsule_point(capsule, axis, radial, angle, ring, true),
                   capsule_point(capsule, axis, radial, angle, ring + 1U, true));
        }
        append(capsule_point(capsule, axis, radial, angle, kCapsuleRings, false),
               capsule_point(capsule, axis, radial, angle, kCapsuleRings, true));
        append(capsule_point(capsule, axis, radial, angle, kCapsuleRings, false),
               capsule_point(capsule, axis, radial, next, kCapsuleRings, false));
        append(capsule_point(capsule, axis, radial, angle, kCapsuleRings, true),
               capsule_point(capsule, axis, radial, next, kCapsuleRings, true));
    }
    return count;
}

float distance_squared(ScreenPoint point, ScreenPoint start, ScreenPoint end) noexcept {
    const float dx = end.x - start.x;
    const float dy = end.y - start.y;
    const float length = dx * dx + dy * dy;
    if (!std::isfinite(length) || length <= 0.0F) {
        const float x = point.x - start.x;
        const float y = point.y - start.y;
        return x * x + y * y;
    }
    const float amount = std::clamp(((point.x - start.x) * dx + (point.y - start.y) * dy) / length,
                                     0.0F,
                                     1.0F);
    const float closestX = start.x + amount * dx;
    const float closestY = start.y + amount * dy;
    const float x = point.x - closestX;
    const float y = point.y - closestY;
    return x * x + y * y;
}

} // namespace sunrise::client::ui::world_inspector::debug_primitives
