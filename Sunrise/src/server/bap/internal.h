#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "../../client/network/consumer.h"
#include "../../middleware/bap/activity_message/activity_patch_epoch_parser.h"
#include "../../middleware/bap/activity_message/sensor_auth_update.h"
#include "../../middleware/bap/frame.h"
#include "../../state/activity/bubble_authority/definition.h"
#include "../../state/activity/definition.h"
#include "../../state/build_data/scenarios/definition.h"
#include "../../state/runtime/runtime.h"
#include "encrypted/queuez/definition.h"

namespace sunrise::server::bap {

/** One session per transport peer slot, so a connection id indexes this array directly. */
inline constexpr std::size_t kSessionCount = client::network::kBapConnectionCount;
/** A delivered activity frame defers the next silence-prevention write by five seconds. */
inline constexpr std::uint64_t kActivityKeepaliveIntervalMs = 5'000;

/** Fixed scratch storage owned by the lock, kept off the Client thread's stack. */
struct Scratch {
    std::array<std::byte, client::network::kBapFrameCapacity> plaintext{};
    std::array<std::byte, client::network::kBapFrameCapacity> responseBody{};
    std::array<std::byte, client::network::kBapFrameCapacity> responsePayload{};
    std::array<std::byte, client::network::kBapFrameCapacity> sealed{};
    std::array<std::byte, client::network::kBapFrameCapacity> framed{};
    /** Roster groups the outbound body's slot spans point into, top-level and per-bubble alike. */
    std::array<state::build_data::scenarios::RosterGroup,
               middleware::bap::activity_message::sensor_auth_update::kGroupCapacity>
        rosterGroups{};
    /** Per-bubble sub-blocks the outbound body's field-1 span points into. */
    std::array<middleware::bap::activity_message::sensor_auth_update::BubbleSubBlock,
               state::build_data::scenarios::kBubbleCapacity>
        rosterSubBlocks{};
    /** Keys each sub-block carries, which its own span points into. */
    std::array<
        std::array<std::uint32_t, state::build_data::scenarios::kDestinationBubbleGroupCapacity>,
        state::build_data::scenarios::kBubbleCapacity>
        rosterSubBlockKeys{};
};

/**
 * What one staged roster body owes State, and the counters to put back if it is discarded.
 * A bubble is offered once, and the state byte rebuilds every object the roster owns. Both may
 * move only once the frame reaches the caller.
 */
struct RosterPublication {
    state::activity::bubble_authority::Grant grant{};
    /** ActivityClient generation that staged this grant and its roster counters. */
    std::uint64_t bindingGeneration{};
    std::uint32_t priorGroups{};
    std::uint8_t priorSends{};
    std::uint8_t priorState{};
    /** Set when the staged body carried a bubble grant that State has not recorded yet. */
    bool hasGrant{};
    /** Set while a roster body is staged and its outcome is undecided. */
    bool staged{};
};

/** ActivityClient role owned by one authenticated BAP link. */
enum class ActivityClientRole : std::uint8_t {
    none,
    privateCurrent,
    publicTarget,
};

/** Exact activity-session generations owned by one BAP link. */
struct ActivityClientBinding {
    /** Target/current session that every activity envelope on this link names. */
    state::activity::SessionBinding session{};
    /** Same as session for private links; advertised source for public targets. */
    state::activity::SessionBinding source{};
    std::uint64_t groupSessionId{};
    std::uint64_t hostGeneration{};
    /** Changes on every bind and rejoin, even when the session id stays the same. */
    std::uint64_t bindingGeneration{};
    /** Private: last citizen region. Public: immutable region captured by the host binding. */
    std::int32_t advertisedRegion{-1};
    ActivityClientRole role{ActivityClientRole::none};
};

/** Patch epoch tied to the exact ActivityClient binding that received it. */
struct BoundPatchEpoch {
    middleware::bap::activity_message::patch_epoch::PatchEpoch value{};
    std::uint64_t bindingGeneration{};
    bool seen{};
};

/** Host-session retain staged by one membership body until its frame is published. */
struct AdvertisementPublication {
    std::uint64_t hostGeneration{};
    bool staged{};
};

/** Compact world reward retained until an active Family-4 peer can publish it. */
enum class WorldRewardKind : std::uint8_t {
    item,
    profileItem,
};

struct WorldRewardRequest {
    std::int32_t quantity{};
    std::uint16_t itemDefinitionIndex{};
    WorldRewardKind kind{};
    std::uint8_t failures{};
};
static_assert(sizeof(WorldRewardRequest) == 8);

inline constexpr std::size_t kWorldRewardQueueCapacity = 64;

/** Mutable transport state owned by one BAP connection. */
struct Session {
    // Eight-byte scalars first avoid padding in every connection slot.
    std::uint64_t activityKeepaliveDueTick{};
    std::uint64_t activityMemberKey{};
    std::uint64_t activityCharacterSoid{};
    std::uint64_t activityRosterDueTick{};
    std::uint64_t activityMembershipSentGeneration{};
    std::uint64_t activityTransitionUntilTick{};
    std::uint64_t activityAdvertisementHostGeneration{};
    std::uint64_t family4RepushDueTick{};
    std::uint64_t family4RepushRoot{};
    std::uint64_t bannerRepushDueTick{};
    std::uint64_t bannerRepushRoot{};
    std::uint64_t acquisitionPresentationUntilTick{};
    std::uint64_t abilityRefreshDueTick{};

    state::matchmaking::ContextHandle matchmakingContext{};
    AdvertisementPublication activityAdvertisementStaged{};
    BoundPatchEpoch activityPatchEpoch{};
    std::array<encrypted::queuez::AcquisitionPresentationRow,
               encrypted::queuez::kAcquisitionPresentationRowCapacity>
        acquisitionPresentationRows{};
    ActivityClientBinding activity{};
    encrypted::queuez::SessionState queuez{};

    std::uint32_t id{};
    std::uint32_t activityRosterGroups{};
    std::int32_t pendingSeasonalExperienceAmount{};
    std::uint32_t pendingSeasonalExperienceMutationSerial{};
    std::uint8_t pendingSeasonalExperienceFailures{};

    bool authenticated{};
    std::uint8_t activityRosterSends{};
    std::uint8_t activityRosterState{};
    std::uint8_t activityRosterReason{};
    bool family4RepushArmed{};
    bool bannerRepushArmed{};
    bool accountMutationPublished{};
    bool accountResyncArmed{};
    std::uint8_t acquisitionPresentationRowCount{};
    bool abilityRefreshArmed{};
    std::array<std::byte, state::kBapNonceSize> sendNonce{};
    std::array<std::byte, state::kBapNonceSize> receiveNonce{};

    /** What one staged roster body owes if publication succeeds or fails. */
    RosterPublication activityRosterStaged{};
};

/** Arms every other Family-4 peer after the origin publishes a complete account mutation. */
void arm_account_resync_elsewhere(Session& origin) noexcept;

/** Arms every Family-4 peer, including the origin, for a full account resync. */
void arm_account_resync_everywhere() noexcept;

/** Holds this peer's full Family-4 refreshes until its acquisition flyout has finished. */
void arm_acquisition_presentation_hold(Session& session) noexcept;

/** Queues one character item for normal acquisition feedback. */
[[nodiscard]] bool arm_world_item_acquisition(std::uint16_t itemDefinitionIndex) noexcept;

/** Queues one profile material for normal acquisition feedback. */
[[nodiscard]] bool arm_world_profile_item_acquisition(std::uint16_t itemDefinitionIndex,
                                                      std::int32_t quantity) noexcept;

/** Reads the oldest queued world reward without removing it. */
[[nodiscard]] bool current_world_reward(WorldRewardRequest& request) noexcept;

/** Removes the world reward returned by current_world_reward. */
void complete_world_reward() noexcept;

/** Records one failed publication and settles a repeatedly failing reward. */
void fail_world_reward_attempt() noexcept;

/** Queues one transient XP item update so the native HUD presents a seasonal XP gain. */
[[nodiscard]] bool arm_seasonal_experience_presentation(std::int32_t amount) noexcept;

namespace plaintext {

/**
 * Handles plaintext bootstrap services, arms encryption after service 25, and routes the rest.
 * @param session Auth and nonce state owned by the connection.
 * @param scratch Transform buffers owned by the lock, kept off the Client thread stack.
 * @param outer Parsed outer frame carrying the service id and its body.
 * @param response Whole-frame storage owned by the caller.
 * @param written Gets the encoded response size in bytes.
 * @return True when the service owes no reply, or its response is encoded.
 */
[[nodiscard]] bool consume(Session& session,
                           Scratch& scratch,
                           const middleware::bap::OuterFrame& outer,
                           std::span<std::byte> response,
                           std::size_t& written) noexcept;

} // namespace plaintext

namespace encrypted {

/**
 * Authenticates and routes one encrypted post-bootstrap service frame.
 * @param session Auth and nonce state owned by the connection.
 * @param scratch Transform buffers owned by the lock, kept off the Client thread stack.
 * @param outer Validated encrypted outer frame.
 * @param response Whole-frame storage owned by the caller.
 * @param written Gets the encoded response size in bytes.
 * @return True when routing works, any response fits, State commits and the nonce is published.
 */
[[nodiscard]] bool consume(Session& session,
                           Scratch& scratch,
                           const middleware::bap::OuterFrame& outer,
                           std::span<std::byte> response,
                           std::size_t& written) noexcept;

/**
 * Sends the owed Family-4 re-push once its delay has passed.
 * @param session Auth, nonce and queuez state owned by the connection.
 * @param scratch Transform buffers owned by the lock, kept off the Client thread stack.
 * @param response Whole-frame storage owned by the caller.
 * @param written Gets the encoded notification size in bytes.
 * @param touchesScratch Set before any scratch buffer is used.
 * @return True when a whole Family-4 notification is published.
 */
[[nodiscard]] bool consume_deferred(Session& session,
                                    Scratch& scratch,
                                    std::span<std::byte> response,
                                    std::size_t& written,
                                    bool& touchesScratch) noexcept;

} // namespace encrypted

} // namespace sunrise::server::bap
