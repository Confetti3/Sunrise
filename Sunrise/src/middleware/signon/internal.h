#pragma once

#include <cstddef>
#include <span>
#include <string_view>

#include "../protobuf/codec.h"

namespace sunrise::middleware::signon {

/** Every SignOn sub-message is a plain protobuf message. */
using Writer = protobuf::Writer;

/** @param text Null-free ASCII field value. @return Byte view for a length-delimited field. */
[[nodiscard]] inline std::span<const std::byte> text_bytes(std::string_view text) noexcept {
    return std::as_bytes(std::span<const char>(text.data(), text.size()));
}

} // namespace sunrise::middleware::signon
