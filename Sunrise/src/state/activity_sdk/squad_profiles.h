#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>

#include "format.h"

namespace sunrise::state::activity_sdk {

/** Field-5 lanes use bias-one widths 2, 3, 2 and 3; authored profiles are nonnegative. */
[[nodiscard]] inline bool valid_spawn_profile(const std::array<std::int8_t, 4>& profile) noexcept {
    // Bias-one wire widths bound each logical profile lane.
    constexpr std::array<std::int8_t, 4> kMaximumLogical{2, 6, 2, 6};
    for (std::size_t index = 0; index < profile.size(); ++index) {
        if (profile[index] < 0 || profile[index] > kMaximumLogical[index]) {
            return false;
        }
    }
    return true;
}

/**
 * A profile certificate never supplies or replaces an actor identity.
 * @param member Generated member and its all-candidate certificate.
 * @param actors Exact actor definitions retained by the same catalog.
 * @return False for invalid certificates or conflicting single-class evidence.
 */
[[nodiscard]] inline bool
valid_member_spawn_profile(const format::SquadMember& member,
                           std::span<const format::ActorClass> actors) noexcept {
    if ((member.flags & ~format::kSquadMemberFlagMask) != 0) {
        return false;
    }
    const bool exactActor = (member.flags & format::kSquadMemberActorClassExact) != 0;
    if (exactActor ? member.actorClassIndex >= actors.size()
                   : member.actorClassIndex != format::kAbsentIndex) {
        return false;
    }
    if ((member.flags & format::kSquadMemberSpawnProfileExact) == 0) {
        return member.authoredSpawnProfile == std::array<std::int8_t, 4>{};
    }
    // Certification requires every candidate to exist and be resolved.
    constexpr auto kCandidateProof =
        format::kSquadMemberCandidateCountsComplete | format::kSquadMemberNoNullCandidates;
    return (member.flags & kCandidateProof) == kCandidateProof
           && std::any_of(member.candidateCounts.begin(),
                          member.candidateCounts.end(),
                          [](auto count) { return count != 0; })
           && valid_spawn_profile(member.authoredSpawnProfile)
           && (!exactActor
               || member.authoredSpawnProfile
                      == actors[member.actorClassIndex].authoredSpawnProfile);
}

/**
 * A zero-count retirement retains the same profile proof as its source.
 * @param members Authored member lanes in their source order.
 * @param actors Exact catalog actor definitions for single-class members.
 * @param requestedCounts Requested count for each member lane.
 * @param output Receives the common profile; cleared on failure.
 * @return True only when every selected member has the same proved profile.
 */
[[nodiscard]] inline bool squad_spawn_profile(std::span<const format::SquadMember> members,
                                              std::span<const format::ActorClass> actors,
                                              std::span<const std::int32_t> requestedCounts,
                                              std::array<std::int8_t, 4>& output) noexcept {
    output = {};
    if (members.empty() || members.size() != requestedCounts.size()
        || std::any_of(
            requestedCounts.begin(), requestedCounts.end(), [](auto count) { return count < 0; })) {
        return false;
    }
    const bool loose = std::any_of(
        requestedCounts.begin(), requestedCounts.end(), [](auto count) { return count > 0; });
    bool found = false;
    std::array<std::int8_t, 4> common{};
    for (std::size_t index = 0; index < members.size(); ++index) {
        if (loose && requestedCounts[index] == 0) {
            continue;
        }
        const auto& member = members[index];
        if (!valid_member_spawn_profile(member, actors)) {
            return false;
        }
        const bool certified = (member.flags & format::kSquadMemberSpawnProfileExact) != 0;
        if (!certified && (member.flags & format::kSquadMemberActorClassExact) == 0) {
            return false;
        }
        const auto& profile = certified ? member.authoredSpawnProfile
                                        : actors[member.actorClassIndex].authoredSpawnProfile;
        if (!valid_spawn_profile(profile) || (found && profile != common)) {
            return false;
        }
        common = profile;
        found = true;
    }
    output = common;
    return found;
}

} // namespace sunrise::state::activity_sdk
