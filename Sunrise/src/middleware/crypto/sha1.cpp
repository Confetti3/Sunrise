#include "sha1.h"

#include <Windows.h>

#include <bcrypt.h>
#include <limits>

namespace sunrise::middleware::crypto::sha1 {
namespace {

/** @return True for a BCrypt status that reports success. */
[[nodiscard]] bool succeeded(NTSTATUS status) noexcept {
    return status >= 0;
}

} // namespace

/**
 * Hashes one buffer.
 * @param input Bytes to hash.
 * @param output Receives the digest only on success.
 * @return True when the Windows hash succeeded.
 */
bool hash(std::span<const std::byte> input, Digest& output) noexcept {
    if (input.size() > (std::numeric_limits<ULONG>::max)()) {
        return false;
    }
    return succeeded(BCryptHash(BCRYPT_SHA1_ALG_HANDLE,
                                nullptr,
                                0,
                                reinterpret_cast<PUCHAR>(const_cast<std::byte*>(input.data())),
                                static_cast<ULONG>(input.size()),
                                reinterpret_cast<PUCHAR>(output.data()),
                                static_cast<ULONG>(output.size())));
}

} // namespace sunrise::middleware::crypto::sha1
