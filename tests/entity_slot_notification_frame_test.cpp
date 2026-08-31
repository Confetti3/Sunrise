#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "core/logging/log.h"
#include "middleware/bap/activity_message/entity_slots.h"
#include "middleware/bap/frame.h"
#include "middleware/encoding/byte_order.h"
#include "middleware/secure_channel/runtime.h"
#include "server/bap/encrypted/push/activity/activity_message_push.h"
#include "server/bap/encrypted/push/activity/activity_roster_atomic.h"

namespace push = sunrise::server::bap::encrypted::push::activity;
namespace bap = sunrise::middleware::bap;
namespace slots = sunrise::middleware::bap::activity_message::entity_slots;
namespace encoding = sunrise::middleware::encoding;
namespace secure = sunrise::middleware::secure_channel;

namespace sunrise::core::log {
void write(Channel, Level, std::string_view) noexcept {}
}

int main() {
    static sunrise::server::bap::Scratch scratch{};
    std::array<std::byte, sunrise::state::kAesKeySize> key{};
    std::array<std::byte, sunrise::state::kBapNonceSize> nonce{};
    for (std::size_t index = 0; index < key.size(); ++index) {
        key[index] = static_cast<std::byte>(index * 11U + 3U);
    }
    nonce[0] = std::byte{7};
    const auto sealingNonce = nonce;
    slots::EntitySlotMask mask{};
    for (std::size_t index = 0; index < mask.size(); ++index) {
        mask[index] = static_cast<std::byte>((index * 29U + 5U) & 0xFFU);
    }
    static std::array<std::byte, sunrise::client::network::kBapFrameCapacity> response{};
    std::size_t written = 0;
    constexpr std::uint64_t kSession = 0x1020304050607080ULL;
    assert(push::append_entity_slot_notification(
        scratch, kSession, mask, key, nonce, response, written));
    assert(written != 0 && nonce[0] == std::byte{8});
    for (std::size_t index = 1; index < nonce.size(); ++index) {
        assert(nonce[index] == sealingNonce[index]);
    }

    bap::OuterFrame outer{};
    assert(bap::parse_frame(std::span(response).first(written), outer));
    assert(outer.frameType == bap::FrameType::encrypted);
    static std::array<std::byte, sunrise::client::network::kBapFrameCapacity> plaintext{};
    std::size_t plaintextSize = 0;
    assert(secure::open_frame(key, sealingNonce, outer.payload, plaintext, plaintextSize));
    const std::span<const std::byte> body(plaintext.data(), plaintextSize);
    constexpr std::size_t kNotificationHeader = 6;
    constexpr std::size_t kDiscriminator = kNotificationHeader;
    constexpr std::size_t kSessionOffset = kDiscriminator + 1;
    constexpr std::size_t kTypeOffset = kSessionOffset + 8;
    constexpr std::size_t kLengthOffset = kTypeOffset + 4;
    constexpr std::size_t kBodyOffset = kLengthOffset + 4;
    assert(body.size() == kBodyOffset + slots::kEncodedSize);
    assert(encoding::read_u16_be(std::span<const std::byte, 2>(body.data(), 2))
           == static_cast<std::uint16_t>(bap::NotificationService::activityMessage));
    assert(encoding::read_u32_be(std::span<const std::byte, 4>(body.data() + 2, 4)) == 0);
    assert(body[kDiscriminator] == std::byte{1});
    assert(encoding::read_u64_be(std::span<const std::byte, 8>(body.data() + kSessionOffset, 8)) == kSession);
    assert(encoding::read_u32_be(std::span<const std::byte, 4>(body.data() + kTypeOffset, 4))
           == slots::kNotificationMessageType);
    assert(encoding::read_u32_be(std::span<const std::byte, 4>(body.data() + kLengthOffset, 4)) == slots::kEncodedSize);
    assert(std::equal(mask.begin(), mask.end(), body.begin() + kBodyOffset));

    // Production append failure restores the caller's byte count and nonce.
    auto failedNonce = sealingNonce;
    std::array<std::byte, 8> tooSmall{};
    tooSmall[0] = std::byte{0xAA};
    std::size_t failedWritten = 1;
    assert(!push::append_entity_slot_notification(
        scratch, kSession, mask, key, failedNonce, tooSmall, failedWritten));
    assert(failedWritten == 1 && failedNonce == sealingNonce
           && tooSmall[0] == std::byte{0xAA});

    // A real framed type-0 followed by a failed roster rolls the complete pair back.
    static std::array<std::byte, sunrise::client::network::kBapFrameCapacity> pairResponse{};
    pairResponse[0] = std::byte{0xA1};
    pairResponse[1] = std::byte{0xA2};
    pairResponse[2] = std::byte{0xA3};
    std::size_t pairWritten = 3;
    auto pairNonce = sealingNonce;
    const auto pairInitialNonce = pairNonce;
    bool discarded = false;
    assert(!push::detail::append_entity_slot_roster_pair(
        pairNonce,
        pairResponse,
        pairWritten,
        [&]() noexcept {
            return push::append_entity_slot_notification(
                scratch, kSession, mask, key, pairNonce, pairResponse, pairWritten);
        },
        [&]() noexcept {
            pairResponse[pairWritten++] = std::byte{5};
            secure::advance_nonce(pairNonce);
            return false;
        },
        [&]() noexcept { discarded = true; }));
    assert(discarded && pairWritten == 3 && pairNonce == pairInitialNonce);
    assert(pairResponse[0] == std::byte{0xA1} && pairResponse[1] == std::byte{0xA2}
           && pairResponse[2] == std::byte{0xA3});
    assert(std::all_of(pairResponse.begin() + 3, pairResponse.end(), [](std::byte value) {
        return value == std::byte{};
    }));
    return 0;
}
