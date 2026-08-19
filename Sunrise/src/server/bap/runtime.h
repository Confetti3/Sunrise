#pragma once

#include <cstdint>

#include "../../client/network/consumer.h"
#include "../../state/activity/definition.h"

namespace sunrise::server::bap {

/** Caller-owned identity of the private ActivityClient binding. */
struct ActivitySnapshot {
    state::activity::SessionBinding binding{};
    std::uint64_t bindingGeneration{};
    std::int32_t advertisedRegion{-1};
};

/** Copies the newest private ActivityClient binding under the BAP session lock. */
[[nodiscard]] bool snapshot_private_activity(ActivitySnapshot& output) noexcept;

/** Applies one connection-scoped BAP lifecycle event. */
[[nodiscard]] bool consume(const client::network::BapRequest& request,
                           client::network::BapResponse& response) noexcept;

/** Wipes every connection-owned nonce and transform buffer. */
void shutdown() noexcept;

} // namespace sunrise::server::bap
