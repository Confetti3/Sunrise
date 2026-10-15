#include <algorithm>

#include "runtime.h"

namespace sunrise::state::activity_sdk {

/** Every catalog identity lane must match before borrowed rows may survive a reopen. */
bool same_identity(const Snapshot& left, const Snapshot& right) noexcept {
    if (left == nullptr || right == nullptr) {
        return false;
    }
    if (left == right) {
        return true;
    }
    const auto same = [](std::span<const std::byte> first,
                         std::span<const std::byte> second) noexcept {
        // SDK identities use SHA-256.
        constexpr std::size_t kDigestBytes = std::tuple_size_v<identity::Digest>;
        return first.size() == kDigestBytes && second.size() == kDigestBytes
               && std::equal(first.begin(), first.end(), second.begin());
    };
    return same(left->sdk_build_sha256(), right->sdk_build_sha256())
           && same(left->payload_sha256(), right->payload_sha256())
           && same(left->content_key_sha256(), right->content_key_sha256())
           && same(left->logical_ir_sha256(), right->logical_ir_sha256());
}

} // namespace sunrise::state::activity_sdk
