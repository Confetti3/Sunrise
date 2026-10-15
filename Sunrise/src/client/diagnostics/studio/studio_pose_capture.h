#pragma once

#include <span>

#include "../../content/handles/handle_resolver.h"
#include "studio_pose_protocol.h"

namespace sunrise::client::diagnostics::studio {

/** Object pool storage addresses are copied while the registry permits observations. */
struct PoseObjectPool final {
    std::uintptr_t baseSlot{}, strideSlot{};
};

/** Both allocations and membership must survive the palette producer and the palette copy. */
struct PoseIdentity final {
    std::uint32_t owner{}, component{}, ownerGeneration{}, componentGeneration{};
    bool operator==(const PoseIdentity&) const = default;
};

/** This observation describes the current object, independently of any retained draw. */
struct PoseOwner final {
    std::uint32_t handle{}, generation{};
    bool operator==(const PoseOwner&) const = default;
};

/** Copies distinct models for one exact owner lifetime; time stays zero without a match. */
[[nodiscard]] std::size_t copy_pose_models(std::span<const pose_wire::Draw> draws,
                                           PoseOwner owner,
                                           std::uint64_t captureTime,
                                           std::span<std::uint32_t> models,
                                           std::uint64_t& time) noexcept;

/** A current full handle and stable generation qualify an unregistered player's draw. */
[[nodiscard]] bool describe_pose_owner(const content::handles::Source& source,
                                       PoseObjectPool pool,
                                       std::uint32_t handle,
                                       PoseOwner& output) noexcept;

/** Rejects retired components, replaced owners and components outside the owner's chain. */
[[nodiscard]] bool describe_pose_identity(const content::handles::Source& source,
                                          PoseObjectPool pool,
                                          std::uintptr_t draw,
                                          PoseIdentity& output) noexcept;

/**
 * Reads one finished primary draw while its producer still owns the frame records.
 * @param source Reader and handle tables for the model constants.
 * @param draw Native draw record.
 * @param pose Evaluated pose record the producer just filled.
 * @param output Receives the draw row; `first` is left for the caller to set.
 * @param palette Receives the address of the `output.count` palette vectors.
 * @return False when the draw has no owner, no admitted palette form or an unreadable record.
 */
[[nodiscard]] bool describe_draw(const content::handles::Source& source,
                                 std::uintptr_t draw,
                                 std::uintptr_t pose,
                                 pose_wire::Draw& output,
                                 std::uintptr_t& palette) noexcept;

} // namespace sunrise::client::diagnostics::studio
