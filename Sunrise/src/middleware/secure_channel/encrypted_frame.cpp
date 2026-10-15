#include "../crypto/aes_gcm_encrypt.h"
#include "runtime.h"

namespace sunrise::middleware::secure_channel {

static_assert(kFrameTagSize == crypto::aes_gcm::kTagSize);
static_assert(state::kAesKeySize == crypto::aes_gcm::kKeySize);
static_assert(state::kBapNonceSize == crypto::aes_gcm::kNonceSize);

/** Seals one frame payload as tag followed by ciphertext. */
bool seal_frame(std::span<const std::byte, state::kAesKeySize> key,
                std::span<const std::byte, state::kBapNonceSize> nonce,
                std::span<const std::byte> plaintext,
                std::span<std::byte> output,
                std::size_t& written) noexcept {
    written = 0;
    if (output.size() < kFrameTagSize + plaintext.size()) {
        return false;
    }
    if (!crypto::aes_gcm::encrypt(
            key, nonce, plaintext, output.subspan(kFrameTagSize), output.first<kFrameTagSize>())) {
        return false;
    }
    written = kFrameTagSize + plaintext.size();
    return true;
}

/** Authenticates and opens one tag-prefixed encrypted frame payload. */
bool open_frame(std::span<const std::byte, state::kAesKeySize> key,
                std::span<const std::byte, state::kBapNonceSize> nonce,
                std::span<const std::byte> payload,
                std::span<std::byte> output,
                std::size_t& written) noexcept {
    written = 0;
    if (payload.size() < kFrameTagSize || output.size() < payload.size() - kFrameTagSize) {
        return false;
    }
    if (!crypto::aes_gcm::decrypt(
            key, nonce, payload.subspan(kFrameTagSize), payload.first<kFrameTagSize>(), output)) {
        return false;
    }
    written = payload.size() - kFrameTagSize;
    return true;
}

/** Increments a BAP nonce as a little-endian fixed-width counter. */
void advance_nonce(std::span<std::byte, state::kBapNonceSize> nonce) noexcept {
    for (std::byte& value : nonce) {
        value = static_cast<std::byte>(std::to_integer<std::uint8_t>(value) + 1);
        if (value != std::byte{0}) {
            return;
        }
    }
}

} // namespace sunrise::middleware::secure_channel
