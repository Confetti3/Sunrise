#pragma once

#include <cstdint>

#include "../../../internal.h"
#include "../../../../../state/activity/entity_slots/definition.h"

namespace sunrise::server::bap::encrypted::push::activity::entity_slot_republish {

/** Exact manual request selected for one private-current roster publication. */
struct Selection final {
    state::activity::entity_slots::LeaseMask held{};
    state::activity::SessionBinding binding{};
    std::uint64_t bindingGeneration{};
    std::uint64_t token{};
};

/**
 * Selects the exact pending request and copies only its client-held lease mask.
 * Caller holds the BAP session lock so the newest-private revalidation is atomic with staging.
 */
[[nodiscard]] bool select(const Session& session, Selection& output) noexcept;

/** Records that both the authentic type-0 frame and its following roster were staged. */
void mark_staged(const Selection& selection) noexcept;
/** Pins the exact selection into the production `RosterPublication`. */
void stage_publication(Session& session, const Selection& selection) noexcept;
/** Counts/consumes a delivered publication without consulting a possibly rebound connection. */
void commit_staged_publication(Session& session) noexcept;
/** Counts a discarded publication while leaving the request pending for retry. */
void discard_staged_publication(Session& session) noexcept;
/** Records an atomic pair encode failure without consuming the request. */
void mark_encode_failed(std::uint64_t token) noexcept;
/** Counts caller delivery and compare-consumes only this exact token. */
void commit(const Selection& selection) noexcept;
/** Counts a discarded staged pair and deliberately leaves its exact token pending. */
void discard(const Selection& selection) noexcept;

#if defined(SUNRISE_TESTING)
/** Clears process-lifetime diagnostic state for a focused unit test only. */
void reset_for_test() noexcept;
#endif

} // namespace sunrise::server::bap::encrypted::push::activity::entity_slot_republish
